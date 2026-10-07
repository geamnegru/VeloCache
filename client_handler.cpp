#include "client_handler.h"
#include "resp.h"

#include <cstdint>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

bool ascii_equal(std::string_view value, std::string_view expected) {
  if (value.size() != expected.size()) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char byte = value[index];
    const char upper = byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
    if (upper != expected[index]) {
      return false;
    }
  }
  return true;
}

std::optional<std::int64_t> parse_ttl(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  std::int64_t seconds = 0;
  for (char byte : value) {
    if (byte < '0' || byte > '9') {
      return std::nullopt;
    }
    const auto digit = static_cast<std::int64_t>(byte - '0');
    if (seconds > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    seconds = seconds * 10 + digit;
  }
  return seconds > 0 ? std::optional<std::int64_t>(seconds) : std::nullopt;
}

}

CommandResult handle_command(const std::string &command, StorageEngine &storage,
                             bool read_only, bool ready) {
  std::stringstream ss(command);
  std::string directive, key, value;
  ss >> directive;

  if (directive == "PING") {
    return {"PONG\n", std::nullopt};
  }

  if (directive == "SET") {
    if (read_only) {
      return {"ERR Read only replica\n", std::nullopt};
    }
    if (!(ss >> key)) {
      return {"ERR Missing key\n", std::nullopt};
    }
    std::getline(ss >> std::ws, value);
    if (!value.empty() && value.back() == '\n')
      value.pop_back();
    if (!value.empty() && value.back() == '\r')
      value.pop_back();

    std::optional<std::int64_t> ttl;
    auto position = value.rfind(" EX ");
    if (position != std::string::npos) {
      std::stringstream ttl_stream(value.substr(position + 4));
      std::int64_t seconds;
      if (!(ttl_stream >> seconds) || seconds <= 0) {
        return {"ERR Invalid expiry\n", std::nullopt};
      }
      ttl_stream >> std::ws;
      if (!ttl_stream.eof()) {
        return {"ERR Invalid expiry\n", std::nullopt};
      }
      ttl = seconds;
      value.resize(position);
    }

    try {
      auto mutation = storage.set(key, value, ttl);
      return {"OK\n", std::move(mutation)};
    } catch (const std::invalid_argument &) {
      return {"ERR Invalid key, value or expiry\n", std::nullopt};
    }
  }

  if (directive == "GET") {
    if (!ready) {
      return {"ERR Replica syncing\n", std::nullopt};
    }
    if (!(ss >> key)) {
      return {"ERR Missing key\n", std::nullopt};
    }
    if (!key.empty() && key.back() == '\n')
      key.pop_back();
    if (!key.empty() && key.back() == '\r')
      key.pop_back();
    auto stored_value = storage.get(key);
    return {stored_value ? *stored_value + "\n" : "(nil)\n", std::nullopt};
  }

  return {"ERR Unknown directive. Use SET, GET or PING\n", std::nullopt};
}

CommandResult handle_resp_command(const std::vector<std::string> &arguments,
                                 StorageEngine &storage, bool read_only,
                                 bool ready) {
  if (arguments.empty()) {
    return {resp_error("ERR empty command"), std::nullopt};
  }

  if (ascii_equal(arguments[0], "PING")) {
    if (arguments.size() == 1) {
      return {resp_simple("PONG"), std::nullopt};
    }
    if (arguments.size() == 2) {
      return {resp_bulk(arguments[1]), std::nullopt};
    }
    return {resp_error("ERR wrong number of arguments for PING"), std::nullopt};
  }

  if (ascii_equal(arguments[0], "SET")) {
    if (read_only) {
      return {resp_error("READONLY replica only accepts reads"), std::nullopt};
    }
    if (arguments.size() != 3 && arguments.size() != 5) {
      return {resp_error("ERR wrong number of arguments for SET"), std::nullopt};
    }
    std::optional<std::int64_t> ttl;
    if (arguments.size() == 5) {
      if (!ascii_equal(arguments[3], "EX")) {
        return {resp_error("ERR syntax error"), std::nullopt};
      }
      ttl = parse_ttl(arguments[4]);
      if (!ttl) {
        return {resp_error("ERR invalid expire time in SET"), std::nullopt};
      }
    }
    try {
      auto mutation = storage.set(arguments[1], arguments[2], ttl);
      return {resp_simple("OK"), std::move(mutation)};
    } catch (const std::invalid_argument &) {
      return {resp_error("ERR invalid key, value or expiry"), std::nullopt};
    }
  }

  if (ascii_equal(arguments[0], "GET")) {
    if (arguments.size() != 2) {
      return {resp_error("ERR wrong number of arguments for GET"), std::nullopt};
    }
    if (!ready) {
      return {resp_error("LOADING replica is synchronizing"), std::nullopt};
    }
    const auto value = storage.get(arguments[1]);
    return {value ? resp_bulk(*value) : resp_null(), std::nullopt};
  }

  return {resp_error("ERR unknown command"), std::nullopt};
}
