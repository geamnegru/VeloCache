#include "client_handler.h"
#include "resp.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string request(const std::vector<std::string> &arguments) {
  std::string result = "*" + std::to_string(arguments.size()) + "\r\n";
  for (const auto &argument : arguments) {
    result += "$" + std::to_string(argument.size()) + "\r\n";
    result += argument;
    result += "\r\n";
  }
  return result;
}

void expect_error(const std::string &frame) {
  const auto parsed = parse_resp_request(frame);
  require(parsed.status == RespParseStatus::Error,
          "Invalid RESP frame was accepted or left incomplete");
  require(!parsed.error.empty(), "Protocol error has no diagnostic");
}

void expect_prefixes(const std::vector<std::string> &arguments) {
  const auto frame = request(arguments);
  for (std::size_t size = 0; size < frame.size(); ++size) {
    const auto parsed =
        parse_resp_request(std::string_view(frame.data(), size));
    require(parsed.status == RespParseStatus::Incomplete,
            "Valid partial frame rejected at byte " + std::to_string(size));
  }
  const auto parsed = parse_resp_request(frame);
  require(parsed.status == RespParseStatus::Complete,
          "Complete RESP request was not parsed");
  require(parsed.arguments == arguments, "Bulk bytes changed during parsing");
  require(parsed.consumed == frame.size(), "Frame size reported incorrectly");
}

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    for (int attempt = 0; attempt < 100; ++attempt) {
      path = std::filesystem::temp_directory_path() /
             ("velocache-resp-" + std::to_string(token) + "-" +
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

void test_parser() {
  expect_prefixes({"PING"});
  expect_prefixes({"SET", "", ""});
  const std::string binary = std::string("\0value\r\n", 8) + "salut și 世界";
  expect_prefixes({"SET", "key\r\nwith spaces", binary, "EX", "20"});

  const auto first = request({"PING"});
  const auto second = request({"GET", "pipeline"});
  const auto combined = first + second;
  const auto parsed_first = parse_resp_request(combined);
  require(parsed_first.status == RespParseStatus::Complete &&
              parsed_first.arguments == std::vector<std::string>{"PING"} &&
              parsed_first.consumed == first.size(),
          "Parser consumed subsequent pipelined request");
  const auto parsed_second = parse_resp_request(
      std::string_view(combined).substr(parsed_first.consumed));
  require(parsed_second.status == RespParseStatus::Complete &&
              parsed_second.arguments ==
                  std::vector<std::string>{"GET", "pipeline"} &&
              parsed_second.consumed == second.size(),
          "Second pipelined request was not preserved");

  for (const auto &frame : {
           "*0\r\n", "*-1\r\n", "*65\r\n", "*+1\r\n", "*x\r\n",
           "*184467440737095516160\r\n", "*1\r\n$-1\r\n",
           "*1\r\n$-2\r\n", "*1\r\n$+1\r\n", "*1\r\n$no\r\n",
           "*1\r\n$1048577\r\n", "*1\r\n$184467440737095516160\r\n",
           "*1\r\n+PING\r\n", "*1\r\n$4\r\nPINGxx",
           "*1\r\n$3\r\nPING\r\n", "*1\r\n$4\nPING\r\n",
       }) {
    expect_error(frame);
  }
  const auto maximal = request({std::string(resp_max_bulk_size, 'x')});
  const auto parsed_maximal = parse_resp_request(maximal);
  require(parsed_maximal.status == RespParseStatus::Complete &&
              parsed_maximal.arguments.front().size() == resp_max_bulk_size,
          "Maximum supported bulk string was rejected");
  expect_error(request(std::vector<std::string>(5,
                                                std::string(resp_max_bulk_size, 'x'))));
  expect_error("*1\r\n$" + std::string(40, '0') + "1\r\nx\r\n");
  const auto boundary_header = "*1\r\n$" + std::string(28, '0') + "1\r\nx\r\n";
  const auto parsed_boundary = parse_resp_request(boundary_header);
  require(parsed_boundary.status == RespParseStatus::Complete &&
              parsed_boundary.arguments == std::vector<std::string>{"x"},
          "Maximum supported length header was rejected");
  expect_error("*1\r\n$" + std::string(29, '0') + "1\r\nx\r\n");
  std::cout << "PASS RESP parser fragmentation, binary fields, limits and pipelines\n";
}

void test_responses() {
  require(resp_simple("OK") == "+OK\r\n", "Simple response encoding failed");
  require(resp_error("ERR failed") == "-ERR failed\r\n",
          "Error response encoding failed");
  require(resp_null() == "$-1\r\n", "Null bulk response encoding failed");
  require(resp_bulk("") == "$0\r\n\r\n", "Empty bulk response encoding failed");
  const std::string binary("\0a\r\nb", 5);
  require(resp_bulk(binary) == "$5\r\n" + binary + "\r\n",
          "Binary bulk response encoding failed");
  for (const auto &text : {"safe\r\n+injected", "safe\ninjected"}) {
    bool simple_rejected = false;
    bool error_rejected = false;
    try {
      resp_simple(text);
    } catch (const std::invalid_argument &) {
      simple_rejected = true;
    }
    try {
      resp_error(text);
    } catch (const std::invalid_argument &) {
      error_rejected = true;
    }
    require(simple_rejected && error_rejected,
            "Line response allowed protocol injection");
  }
  std::cout << "PASS RESP simple, error, null and binary bulk encoding\n";
}

void test_commands(const std::filesystem::path &path) {
  StorageEngine storage(path.string());
  require(handle_resp_command({"PING"}, storage, false).response ==
              resp_simple("PONG"),
          "PING command failed");
  require(handle_resp_command({"pInG", "hello\r\nworld"}, storage, false)
                  .response == resp_bulk("hello\r\nworld"),
          "PING message command failed");
  require(handle_resp_command({"GET", "missing"}, storage, false).response ==
              resp_null(),
          "Missing GET did not produce null bulk");
  const std::string binary("\0value\r\n", 8);
  const auto set = handle_resp_command({"sEt", "", binary}, storage, false);
  require(set.response == resp_simple("OK") && set.mutation &&
              set.mutation->entry.key.empty() &&
              set.mutation->entry.value == binary,
          "SET lost binary key/value or mutation");
  require(handle_resp_command({"get", ""}, storage, false).response ==
              resp_bulk(binary),
          "GET lost binary bytes");
  require(handle_resp_command({"SET", "empty", ""}, storage, false).response ==
              resp_simple("OK") &&
              handle_resp_command({"GET", "empty"}, storage, false).response ==
                  resp_bulk(""),
          "Empty value was confused with missing value");
  require(handle_resp_command({"SET", "ttl", "value", "ex", "2"}, storage,
                              false)
                  .response == resp_simple("OK"),
          "Positive TTL was rejected");
  for (const auto &arguments : std::vector<std::vector<std::string>>{
           {}, {"PING", "a", "b"}, {"GET"}, {"GET", "a", "b"},
           {"SET", "key"}, {"SET", "key", "value", "EX"},
           {"SET", "key", "value", "NX", "1"},
           {"SET", "key", "value", "EX", "0"},
           {"SET", "key", "value", "EX", "-1"},
           {"SET", "key", "value", "EX", "abc"},
           {"SET", "key", "value", "EX", "1x"},
           {"SET", "key", "value", "EX", "99999999999999999999"},
           {"UNKNOWN"},
       }) {
    const auto result = handle_resp_command(arguments, storage, false);
    require(!result.response.empty() && result.response.front() == '-' &&
                !result.mutation,
            "Invalid command succeeded or created a mutation");
  }
  const auto read_only =
      handle_resp_command({"SET", "", "changed"}, storage, true);
  require(read_only.response.front() == '-' && !read_only.mutation &&
              storage.get("") == binary,
          "Read-only node accepted SET");
  require(handle_resp_command({"GET", ""}, storage, true, false)
                  .response.front() == '-',
          "Unsynchronized replica served GET");
  require(handle_resp_command({"PING"}, storage, true, false).response ==
              resp_simple("PONG"),
          "Unsynchronized replica rejected health check");
  std::cout << "PASS RESP commands, validation, binary storage and replica state\n";
}

}

int main() {
  std::ios_base::sync_with_stdio(false);
  std::cin.tie(nullptr);

  try {
    TemporaryDirectory directory;
    test_parser();
    test_responses();
    test_commands(directory.path / "commands.aof");
    std::cout << "All RESP unit tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "RESP test failed: " << error.what() << '\n';
    return 1;
  }
}
