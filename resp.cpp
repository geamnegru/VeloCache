#include "resp.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

struct LengthResult {
  RespParseStatus status;
  std::size_t next = 0;
  std::size_t value = 0;
  const char *error = "";
};

LengthResult parse_length(std::string_view input, std::size_t position,
                          char marker, std::size_t maximum) {
  const auto start = position;
  if (position >= resp_max_frame_size) {
    return {RespParseStatus::Error, 0, 0, "RESP request exceeds maximum size"};
  }
  if (position >= input.size()) {
    return {RespParseStatus::Incomplete};
  }
  if (input[position] != marker) {
    return {RespParseStatus::Error, 0, 0, "Expected RESP array of bulk strings"};
  }
  ++position;
  std::size_t value = 0;
  bool has_digits = false;
  while (position < input.size()) {
    if (position - start >= resp_max_header_size) {
      return {RespParseStatus::Error, 0, 0, "RESP length header exceeds maximum size"};
    }
    if (position >= resp_max_frame_size) {
      return {RespParseStatus::Error, 0, 0, "RESP request exceeds maximum size"};
    }
    const char byte = input[position];
    if (byte == '\r') {
      if (!has_digits) {
        return {RespParseStatus::Error, 0, 0, "Invalid RESP length"};
      }
      if (position - start + 2 > resp_max_header_size) {
        return {RespParseStatus::Error, 0, 0,
                "RESP length header exceeds maximum size"};
      }
      if (position + 2 > resp_max_frame_size) {
        return {RespParseStatus::Error, 0, 0, "RESP request exceeds maximum size"};
      }
      if (position + 1 >= input.size()) {
        return {RespParseStatus::Incomplete};
      }
      if (input[position + 1] != '\n') {
        return {RespParseStatus::Error, 0, 0, "Expected CRLF after RESP length"};
      }
      return {RespParseStatus::Complete, position + 2, value};
    }
    if (byte < '0' || byte > '9') {
      return {RespParseStatus::Error, 0, 0, "Invalid RESP length"};
    }
    const auto digit = static_cast<std::size_t>(byte - '0');
    if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
      return {RespParseStatus::Error, 0, 0, "RESP length overflow"};
    }
    value = value * 10 + digit;
    if (value > maximum) {
      return {RespParseStatus::Error, 0, 0, "RESP length exceeds maximum size"};
    }
    has_digits = true;
    ++position;
  }
  if (position - start >= resp_max_header_size) {
    return {RespParseStatus::Error, 0, 0, "RESP length header exceeds maximum size"};
  }
  if (position >= resp_max_frame_size) {
    return {RespParseStatus::Error, 0, 0, "RESP request exceeds maximum size"};
  }
  return {RespParseStatus::Incomplete};
}

RespParseResult parse_error(const char *message) {
  return {RespParseStatus::Error, 0, {}, message};
}

void validate_line(const std::string &value) {
  if (value.find_first_of("\r\n") != std::string::npos) {
    throw std::invalid_argument("RESP line must not contain CR or LF");
  }
}

}

RespParseResult parse_resp_request(std::string_view input) {
  const auto array = parse_length(input, 0, '*', 64);
  if (array.status == RespParseStatus::Error) {
    return parse_error(array.error);
  }
  if (array.status == RespParseStatus::Incomplete) {
    return {RespParseStatus::Incomplete, 0, {}, {}};
  }
  if (array.value == 0) {
    return parse_error("RESP command must contain at least one argument");
  }

  std::array<std::pair<std::size_t, std::size_t>, 64> arguments;
  std::size_t position = array.next;
  for (std::size_t index = 0; index < array.value; ++index) {
    const auto bulk = parse_length(input, position, '$', resp_max_bulk_size);
    if (bulk.status == RespParseStatus::Error) {
      return parse_error(bulk.error);
    }
    if (bulk.status == RespParseStatus::Incomplete) {
      return {RespParseStatus::Incomplete, 0, {}, {}};
    }
    if (bulk.next > resp_max_frame_size - 2 ||
        bulk.value > resp_max_frame_size - bulk.next - 2) {
      return parse_error("RESP request exceeds maximum size");
    }
    const auto data_end = bulk.next + bulk.value;
    if (data_end >= input.size()) {
      return {RespParseStatus::Incomplete, 0, {}, {}};
    }
    if (input[data_end] != '\r') {
      return parse_error("Expected CRLF after RESP bulk string");
    }
    if (data_end + 1 >= input.size()) {
      return {RespParseStatus::Incomplete, 0, {}, {}};
    }
    if (input[data_end + 1] != '\n') {
      return parse_error("Expected CRLF after RESP bulk string");
    }
    arguments[index] = {bulk.next, bulk.value};
    position = data_end + 2;
  }

  RespParseResult result{RespParseStatus::Complete, position, {}, {}};
  result.arguments.reserve(array.value);
  for (std::size_t index = 0; index < array.value; ++index) {
    result.arguments.emplace_back(
        input.substr(arguments[index].first, arguments[index].second));
  }
  return result;
}

std::string resp_simple(const std::string &value) {
  validate_line(value);
  return "+" + value + "\r\n";
}

std::string resp_error(const std::string &value) {
  validate_line(value);
  return "-" + value + "\r\n";
}

std::string resp_bulk(const std::string &value) {
  return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
}

std::string resp_null() { return "$-1\r\n"; }
