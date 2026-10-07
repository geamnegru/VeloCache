#ifndef RESP_H
#define RESP_H

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

inline constexpr std::size_t resp_max_bulk_size = 1024 * 1024;
inline constexpr std::size_t resp_max_frame_size = 4 * 1024 * 1024 + 256;
inline constexpr std::size_t resp_max_header_size = 32;

enum class RespParseStatus { Incomplete, Complete, Error };

struct RespParseResult {
  RespParseStatus status;
  std::size_t consumed = 0;
  std::vector<std::string> arguments;
  std::string error;
};

RespParseResult parse_resp_request(std::string_view input);
std::string resp_simple(const std::string &value);
std::string resp_error(const std::string &value);
std::string resp_bulk(const std::string &value);
std::string resp_null();

#endif
