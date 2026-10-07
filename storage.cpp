#include "storage.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr std::size_t max_field_size = 1024 * 1024;
constexpr std::size_t max_line_size = max_field_size * 4 + 128;
constexpr char hex_digits[] = "0123456789abcdef";

std::runtime_error io_error(const std::string &operation) {
  return std::runtime_error(operation + ": " + std::strerror(errno));
}

std::int64_t current_time_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void validate_entry(const StorageEntry &entry) {
  if (entry.key.size() > max_field_size || entry.value.size() > max_field_size) {
    throw std::invalid_argument("Key and value must not exceed 1 MiB each");
  }
  if (entry.expires_at_ms < 0) {
    throw std::invalid_argument("Expiration must be a nonnegative Unix time");
  }
}

std::uint64_t parse_unsigned(std::string_view text) {
  if (text.empty() || (text.size() > 1 && text.front() == '0')) {
    throw std::invalid_argument("Invalid unsigned integer");
  }
  std::uint64_t value = 0;
  for (char digit : text) {
    if (digit < '0' || digit > '9') {
      throw std::invalid_argument("Invalid unsigned integer");
    }
    const auto number = static_cast<std::uint64_t>(digit - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - number) / 10) {
      throw std::invalid_argument("Unsigned integer overflow");
    }
    value = value * 10 + number;
  }
  return value;
}

template <std::size_t count>
std::array<std::string_view, count> split_fields(std::string_view line) {
  std::array<std::string_view, count> fields;
  std::size_t start = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto end = line.find(' ', start);
    if (index + 1 == count) {
      if (end != std::string_view::npos || start >= line.size()) {
        throw std::invalid_argument("Invalid field count");
      }
      fields[index] = line.substr(start);
    } else {
      if (end == std::string_view::npos || end == start) {
        throw std::invalid_argument("Invalid field count");
      }
      fields[index] = line.substr(start, end - start);
      start = end + 1;
    }
  }
  return fields;
}

unsigned hex_digit(char digit) {
  if (digit >= '0' && digit <= '9') {
    return static_cast<unsigned>(digit - '0');
  }
  if (digit >= 'a' && digit <= 'f') {
    return static_cast<unsigned>(digit - 'a' + 10);
  }
  throw std::invalid_argument("Invalid hexadecimal encoding");
}

std::string encode_hex(const std::string &value) {
  if (value.empty()) {
    return "-";
  }
  std::string encoded(value.size() * 2, '0');
  for (std::size_t index = 0; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    encoded[index * 2] = hex_digits[byte >> 4];
    encoded[index * 2 + 1] = hex_digits[byte & 15];
  }
  return encoded;
}

std::string decode_hex(std::string_view encoded) {
  if (encoded == "-") {
    return {};
  }
  if (encoded.empty() || encoded.size() % 2 != 0 ||
      encoded.size() / 2 > max_field_size) {
    throw std::invalid_argument("Invalid hexadecimal field size");
  }
  std::string value(encoded.size() / 2, '\0');
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<char>(hex_digit(encoded[index * 2]) * 16 +
                                     hex_digit(encoded[index * 2 + 1]));
  }
  return value;
}

std::uint64_t checksum(std::string_view data) {
  std::uint64_t value = 14695981039346656037ULL;
  for (unsigned char byte : data) {
    value ^= byte;
    value *= 1099511628211ULL;
  }
  return value;
}

std::string encode_checksum(std::uint64_t value) {
  std::string encoded(16, '0');
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    encoded[15 - index] = hex_digits[value & 15];
    value >>= 4;
  }
  return encoded;
}

void write_all(int fd, const std::string &data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto written = write(fd, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw io_error("Cannot write AOF");
    }
    if (written == 0) {
      throw std::runtime_error("Cannot write AOF: zero-byte write");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void sync_file(int fd) {
  while (fsync(fd) < 0) {
    if (errno != EINTR) {
      throw io_error("Cannot synchronize AOF");
    }
  }
}

void sync_directory(const std::string &path) {
  const auto slash = path.rfind('/');
  const std::string directory =
      slash == std::string::npos ? "." : (slash == 0 ? "/" : path.substr(0, slash));
  const int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    throw io_error("Cannot open AOF directory");
  }
  try {
    sync_file(fd);
  } catch (...) {
    close(fd);
    throw;
  }
  close(fd);
}

int open_or_create(const std::string &path, bool &created) {
  int fd;
  do {
    fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  } while (fd < 0 && errno == EINTR);
  if (fd >= 0) {
    created = true;
    return fd;
  }
  if (errno != EEXIST) {
    throw io_error("Cannot create " + path);
  }
  created = false;
  do {
    fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  } while (fd < 0 && errno == EINTR);
  if (fd < 0) {
    throw io_error("Cannot open " + path);
  }
  return fd;
}

}

std::string encode_mutation(const StorageMutation &mutation) {
  validate_entry(mutation.entry);
  std::string payload =
      "UPDATE " + std::to_string(mutation.sequence) + " " +
      std::to_string(mutation.entry.expires_at_ms) + " " +
      encode_hex(mutation.entry.key) + " " + encode_hex(mutation.entry.value);
  return payload + " " + encode_checksum(checksum(payload)) + "\n";
}

StorageMutation decode_mutation(const std::string &line) {
  if (line.size() > max_line_size) {
    throw std::invalid_argument("Mutation line exceeds maximum size");
  }
  std::string_view data(line);
  if (!data.empty() && data.back() == '\n') {
    data.remove_suffix(1);
  }
  const auto fields = split_fields<6>(data);
  if (fields[0] != "UPDATE") {
    throw std::invalid_argument("Unknown mutation type");
  }
  if (fields[5].size() != 16) {
    throw std::invalid_argument("Invalid checksum size");
  }
  std::uint64_t expected_checksum = 0;
  for (char digit : fields[5]) {
    expected_checksum = expected_checksum * 16 + hex_digit(digit);
  }
  const auto payload = data.substr(0, data.size() - fields[5].size() - 1);
  if (checksum(payload) != expected_checksum) {
    throw std::invalid_argument("Mutation checksum mismatch");
  }
  StorageMutation mutation;
  mutation.sequence = parse_unsigned(fields[1]);
  const auto expiration = parse_unsigned(fields[2]);
  if (expiration > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
    throw std::invalid_argument("Expiration integer overflow");
  }
  mutation.entry.expires_at_ms = static_cast<std::int64_t>(expiration);
  mutation.entry.key = decode_hex(fields[3]);
  mutation.entry.value = decode_hex(fields[4]);
  return mutation;
}

StorageEngine::StorageEngine(const std::string &aof_path) : aof_path_(aof_path) {
  if (aof_path_.empty()) {
    throw std::invalid_argument("AOF path must not be empty");
  }
  try {
    bool lock_created = false;
    lock_fd_ = open_or_create(aof_path_ + ".lock", lock_created);
    while (flock(lock_fd_, LOCK_EX | LOCK_NB) < 0) {
      if (errno != EINTR) {
        throw io_error("Cannot lock AOF; another server may be using it");
      }
    }
    if (lock_created) {
      sync_file(lock_fd_);
      sync_directory(aof_path_);
    }
    bool aof_created = false;
    aof_fd_ = open_or_create(aof_path_, aof_created);
    if (aof_created) {
      write_all(aof_fd_, "VCAOF1 0 0\n");
      sync_file(aof_fd_);
      sync_directory(aof_path_);
    } else {
      load();
    }
  } catch (...) {
    if (aof_fd_ >= 0) {
      close(aof_fd_);
    }
    if (lock_fd_ >= 0) {
      close(lock_fd_);
    }
    throw;
  }
}

StorageEngine::~StorageEngine() {
  if (aof_fd_ >= 0) {
    close(aof_fd_);
  }
  if (lock_fd_ >= 0) {
    close(lock_fd_);
  }
}

void StorageEngine::load() {
  bool header_loaded = false;
  std::uint64_t snapshot_remaining = 0;
  off_t offset = 0;
  off_t valid_offset = 0;
  std::size_t line_number = 0;
  std::string line;
  bool oversized = false;
  char buffer[65536];

  const auto process_line = [&]() {
    ++line_number;
    if (oversized) {
      throw std::runtime_error("AOF line exceeds maximum size at line " +
                               std::to_string(line_number));
    }
    try {
      if (!header_loaded) {
        const auto fields = split_fields<3>(line);
        if (fields[0] != "VCAOF1") {
          throw std::invalid_argument("Unsupported AOF header");
        }
        sequence_ = parse_unsigned(fields[1]);
        snapshot_remaining = parse_unsigned(fields[2]);
        header_loaded = true;
      } else {
        auto mutation = decode_mutation(line);
        if (snapshot_remaining > 0) {
          if (mutation.sequence != sequence_) {
            throw std::invalid_argument("Snapshot sequence mismatch");
          }
          if (!db_.emplace(std::move(mutation.entry.key),
                           StoredValue{std::move(mutation.entry.value),
                                       mutation.entry.expires_at_ms})
                   .second) {
            throw std::invalid_argument("Duplicate snapshot key");
          }
          --snapshot_remaining;
        } else {
          if (sequence_ == std::numeric_limits<std::uint64_t>::max() ||
              mutation.sequence != sequence_ + 1) {
            throw std::invalid_argument("AOF sequence gap");
          }
          db_[mutation.entry.key] =
              StoredValue{std::move(mutation.entry.value),
                          mutation.entry.expires_at_ms};
          sequence_ = mutation.sequence;
        }
      }
    } catch (const std::invalid_argument &error) {
      throw std::runtime_error("Corrupted AOF at line " +
                               std::to_string(line_number) + ": " + error.what());
    }
    valid_offset = offset;
  };

  while (true) {
    const auto received = read(aof_fd_, buffer, sizeof(buffer));
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw io_error("Cannot read AOF");
    }
    if (received == 0) {
      break;
    }
    for (ssize_t index = 0; index < received; ++index) {
      ++offset;
      if (buffer[index] == '\n') {
        process_line();
        line.clear();
        oversized = false;
      } else if (line.size() < max_line_size) {
        line.push_back(buffer[index]);
      } else {
        oversized = true;
      }
    }
  }
  if (!header_loaded || snapshot_remaining != 0) {
    throw std::runtime_error("Incomplete AOF header or snapshot");
  }
  if (valid_offset != offset) {
    while (ftruncate(aof_fd_, valid_offset) < 0) {
      if (errno != EINTR) {
        throw io_error("Cannot truncate incomplete AOF tail");
      }
    }
    sync_file(aof_fd_);
  }
  if (lseek(aof_fd_, 0, SEEK_END) < 0) {
    throw io_error("Cannot seek AOF");
  }
  purge_expired();
}

void StorageEngine::require_writable() const {
  if (!writable_) {
    throw std::runtime_error("Storage is unavailable after an AOF failure");
  }
}

void StorageEngine::commit_mutation(const StorageMutation &mutation) {
  require_writable();
  if (sequence_ == std::numeric_limits<std::uint64_t>::max() ||
      mutation.sequence != sequence_ + 1) {
    throw std::invalid_argument("Mutation sequence must follow current sequence");
  }
  const auto encoded = encode_mutation(mutation);
  StoredValue staged{mutation.entry.value, mutation.entry.expires_at_ms};
  auto entry = db_.find(mutation.entry.key);
  const bool inserted = entry == db_.end();
  if (inserted) {
    entry = db_.emplace(mutation.entry.key, std::move(staged)).first;
  } else {
    entry->second.value.swap(staged.value);
    std::swap(entry->second.expires_at_ms, staged.expires_at_ms);
  }
  try {
    write_all(aof_fd_, encoded);
    sync_file(aof_fd_);
  } catch (...) {
    writable_ = false;
    if (inserted) {
      db_.erase(entry);
    } else {
      entry->second.value.swap(staged.value);
      std::swap(entry->second.expires_at_ms, staged.expires_at_ms);
    }
    throw;
  }
  sequence_ = mutation.sequence;
}

StorageMutation StorageEngine::set(
    const std::string &key, const std::string &value,
    std::optional<std::int64_t> ttl_seconds) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  require_writable();
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("Mutation sequence exhausted");
  }
  StorageMutation mutation{sequence_ + 1, StorageEntry{key, value, 0}};
  if (ttl_seconds) {
    const auto now = current_time_ms();
    if (*ttl_seconds <= 0 || now < 0 ||
        *ttl_seconds > (std::numeric_limits<std::int64_t>::max() - now) / 1000) {
      throw std::invalid_argument("TTL must be positive and fit in Unix time");
    }
    mutation.entry.expires_at_ms = now + *ttl_seconds * 1000;
  }
  commit_mutation(mutation);
  return mutation;
}

std::optional<std::string> StorageEngine::get(const std::string &key) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  const auto entry = db_.find(key);
  if (entry == db_.end()) {
    return std::nullopt;
  }
  if (entry->second.expires_at_ms != 0 &&
      entry->second.expires_at_ms <= current_time_ms()) {
    db_.erase(entry);
    return std::nullopt;
  }
  return entry->second.value;
}

StorageSnapshot StorageEngine::snapshot() {
  std::lock_guard<std::mutex> lock(db_mutex_);
  purge_expired();
  StorageSnapshot result;
  result.sequence = sequence_;
  result.entries.reserve(db_.size());
  for (const auto &entry : db_) {
    result.entries.push_back(StorageEntry{entry.first, entry.second.value,
                                         entry.second.expires_at_ms});
  }
  return result;
}

void StorageEngine::apply_replication(const StorageMutation &mutation) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  commit_mutation(mutation);
}

void StorageEngine::replace_snapshot(const StorageSnapshot &snapshot) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  require_writable();
  std::unordered_map<std::string, StoredValue> staged;
  staged.reserve(snapshot.entries.size());
  for (const auto &entry : snapshot.entries) {
    validate_entry(entry);
    if (!staged.emplace(entry.key, StoredValue{entry.value, entry.expires_at_ms})
             .second) {
      throw std::invalid_argument("Duplicate snapshot key");
    }
  }
  const std::string header = "VCAOF1 " + std::to_string(snapshot.sequence) + " " +
                             std::to_string(snapshot.entries.size()) + "\n";
  std::string temporary = aof_path_ + ".tmp.XXXXXX";
  std::vector<char> name(temporary.begin(), temporary.end());
  name.push_back('\0');
  int temporary_fd = -1;
  bool temporary_created = false;
  bool renamed = false;
  try {
    temporary_fd = mkstemp(name.data());
    if (temporary_fd < 0) {
      throw io_error("Cannot create snapshot AOF");
    }
    temporary_created = true;
    temporary.assign(name.data());
    if (fcntl(temporary_fd, F_SETFD, FD_CLOEXEC) < 0) {
      throw io_error("Cannot set snapshot descriptor flags");
    }
    write_all(temporary_fd, header);
    for (const auto &entry : snapshot.entries) {
      write_all(temporary_fd,
                encode_mutation(StorageMutation{snapshot.sequence, entry}));
    }
    sync_file(temporary_fd);
    while (rename(temporary.c_str(), aof_path_.c_str()) < 0) {
      if (errno != EINTR) {
        throw io_error("Cannot replace snapshot AOF");
      }
    }
    renamed = true;
    sync_directory(aof_path_);
    const int previous_fd = aof_fd_;
    aof_fd_ = temporary_fd;
    temporary_fd = -1;
    db_.swap(staged);
    sequence_ = snapshot.sequence;
    close(previous_fd);
  } catch (...) {
    writable_ = false;
    if (temporary_fd >= 0) {
      close(temporary_fd);
    }
    if (temporary_created && !renamed) {
      unlink(name.data());
    }
    throw;
  }
}

void StorageEngine::purge_expired() {
  const auto now = current_time_ms();
  for (auto entry = db_.begin(); entry != db_.end();) {
    if (entry->second.expires_at_ms != 0 && entry->second.expires_at_ms <= now) {
      entry = db_.erase(entry);
    } else {
      ++entry;
    }
  }
}

void StorageEngine::clean_expired() {
  std::lock_guard<std::mutex> lock(db_mutex_);
  purge_expired();
}
