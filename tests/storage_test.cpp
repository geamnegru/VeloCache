#include "storage.h"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Exception, typename Action>
void expect_exception(Action action, const std::string &message) {
  try {
    action();
  } catch (const Exception &) {
    return;
  }
  throw std::runtime_error(message);
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path = std::filesystem::temp_directory_path() /
             ("velocache-storage-" + std::to_string(token) + "-" +
              std::to_string(attempt));
      if (std::filesystem::create_directory(path)) {
        return;
      }
    }
    throw std::runtime_error("Cannot create temporary directory");
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }

  std::filesystem::path path;
};

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path &path, const std::string &data,
                bool append = false) {
  std::ofstream output(path, std::ios::binary |
                                 (append ? std::ios::app : std::ios::trunc));
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  require(output.good(), "Cannot write test AOF");
}

void test_encoding() {
  StorageMutation mutation{7, {"key with spaces", "value\r\nwith spaces", 12345}};
  const std::string encoded = encode_mutation(mutation);
  StorageMutation decoded = decode_mutation(encoded);
  require(decoded.sequence == mutation.sequence, "Sequence roundtrip failed");
  require(decoded.entry.key == mutation.entry.key, "Key roundtrip failed");
  require(decoded.entry.value == mutation.entry.value, "Value roundtrip failed");
  require(decoded.entry.expires_at_ms == mutation.entry.expires_at_ms,
          "Expiry roundtrip failed");
  StorageMutation empty{8, {"empty", "", 0}};
  require(decode_mutation(encode_mutation(empty)).entry.value.empty(),
          "Empty value roundtrip failed");
  std::string corrupt = encoded;
  std::size_t checksum = corrupt.find_last_not_of("\r\n");
  require(checksum != std::string::npos, "Missing encoded checksum");
  corrupt[checksum] = corrupt[checksum] == '0' ? '1' : '0';
  expect_exception<std::invalid_argument>(
      [&] { decode_mutation(corrupt); }, "Corrupt checksum accepted");
  expect_exception<std::invalid_argument>(
      [] { decode_mutation("UPDATE invalid 0 61 62 0\n"); },
      "Malformed mutation accepted");
}

void test_storage(const std::filesystem::path &path) {
  {
    StorageEngine storage(path.string());
    require(!storage.get("missing"), "Missing key returned a value");
    StorageMutation first = storage.set("basic", "value");
    require(first.sequence == 1, "First mutation sequence must be one");
    require(storage.get("basic") == std::optional<std::string>("value"),
            "Basic GET failed");
    storage.set("empty", "");
    require(storage.get("empty") == std::optional<std::string>(""),
            "Empty value lost");
    storage.set("spaces", "one  two   three");
    storage.set("reset", "temporary", 1);
    StorageMutation reset = storage.set("reset", "forever");
    require(reset.entry.expires_at_ms == 0, "SET did not clear previous TTL");
    expect_exception<std::invalid_argument>(
        [&] { storage.set("basic", "invalid", 0); }, "Zero TTL accepted");
    expect_exception<std::invalid_argument>(
        [&] { storage.set("basic", "invalid", -1); }, "Negative TTL accepted");
    require(storage.get("basic") == std::optional<std::string>("value"),
            "Invalid TTL changed existing value");
    storage.set("expire", "temporary", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    require(!storage.get("expire"), "GET returned expired value");
    require(storage.get("reset") == std::optional<std::string>("forever"),
            "Cleared TTL still expired");
    expect_exception<std::runtime_error>(
        [&] { StorageEngine conflict(path.string()); },
        "Two engines acquired the same AOF lock");
  }
  {
    StorageEngine recovered(path.string());
    require(recovered.get("basic") == std::optional<std::string>("value"),
            "AOF recovery lost acknowledged value");
    require(recovered.get("empty") == std::optional<std::string>(""),
            "AOF recovery lost empty value");
    require(!recovered.get("expire"), "AOF recovery resurrected expired value");
    require(recovered.get("reset") == std::optional<std::string>("forever"),
            "AOF recovery restored cleared TTL");
  }
}

void test_replication(const std::filesystem::path &path) {
  {
    StorageEngine replica(path.string());
    replica.set("old", "old snapshot");
    StorageSnapshot snapshot{42, {{"new", "snapshot value", 0},
                                  {"empty", "", 0},
                                  {"expired", "expired", now_ms() - 1}}};
    replica.replace_snapshot(snapshot);
    require(!replica.get("old"), "Snapshot retained an old key");
    require(replica.get("new") == std::optional<std::string>("snapshot value"),
            "Snapshot did not install new value");
    require(!replica.get("expired"), "Snapshot exposed expired data");
    const std::string before_snapshot = read_file(path);
    StorageSnapshot invalid{70, {{"duplicate", "first", 0},
                                 {"duplicate", "second", 0}}};
    expect_exception<std::invalid_argument>(
        [&] { replica.replace_snapshot(invalid); },
        "Snapshot with duplicate keys accepted");
    require(read_file(path) == before_snapshot,
            "Invalid snapshot changed durable state");
    require(replica.snapshot().sequence == 42 &&
                replica.get("new") ==
                    std::optional<std::string>("snapshot value"),
            "Invalid snapshot changed memory or sequence");
    replica.apply_replication({43, {"update", "ordered", 0}});
    const std::string before_gap = read_file(path);
    expect_exception<std::invalid_argument>(
        [&] { replica.apply_replication({45, {"gap", "invalid", 0}}); },
        "Replication sequence gap accepted");
    expect_exception<std::invalid_argument>(
        [&] { replica.apply_replication({43, {"duplicate", "invalid", 0}}); },
        "Duplicate replication sequence accepted");
    require(read_file(path) == before_gap, "Rejected replication changed AOF");
    require(!replica.get("gap") && !replica.get("duplicate"),
            "Rejected replication changed memory");
    replica.apply_replication({44, {"update", "next", 0}});
    require(replica.snapshot().sequence == 44, "Replica sequence incorrect");
  }
  {
    StorageEngine recovered(path.string());
    require(recovered.snapshot().sequence == 44,
            "Snapshot recovery lost sequence");
    require(recovered.get("update") == std::optional<std::string>("next"),
            "Snapshot recovery lost subsequent update");
    require(!recovered.get("old") && !recovered.get("expired"),
            "Snapshot recovery resurrected discarded data");
  }
}

void test_recovery_errors(const std::filesystem::path &path) {
  {
    StorageEngine storage(path.string());
    storage.set("durable", "retained");
  }
  const std::string valid = read_file(path);
  write_file(path, "UPDATE 999 0 616263", true);
  {
    StorageEngine recovered(path.string());
    require(recovered.get("durable") == std::optional<std::string>("retained"),
            "Truncated tail damaged valid records");
    require(read_file(path) == valid, "Truncated tail was not repaired");
    recovered.set("after_tail", "still writable");
  }
  std::string corrupt = read_file(path);
  std::size_t record = corrupt.find("UPDATE ");
  require(record != std::string::npos,
          "Cannot locate mutation for checksum corruption");
  std::size_t checksum = corrupt.find('\n', record);
  require(checksum != std::string::npos && checksum > 0,
          "Cannot locate complete AOF record");
  --checksum;
  corrupt[checksum] = corrupt[checksum] == '0' ? '1' : '0';
  write_file(path, corrupt);
  expect_exception<std::runtime_error>(
      [&] { StorageEngine broken(path.string()); },
      "Complete record with corrupt checksum recovered successfully");
}

}

int main() {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);

  try {
    TemporaryDirectory temp;
    test_encoding();
    test_storage(temp.path / "storage.aof");
    test_replication(temp.path / "replica.aof");
    test_recovery_errors(temp.path / "errors.aof");
    std::cout << "All storage tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "Storage test failed: " << error.what() << '\n';
    return 1;
  }
}
