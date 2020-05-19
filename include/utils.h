#ifndef _http_utils_h_
#define _http_utils_h_

#include <optional.hpp>
#include <string>

namespace http
{

int decode_hex(int ch);

std::string file_extension(const std::string& path);

bool from_hex_to_i(const std::string& s, size_t i, size_t cnt, int& val);

bool parse_range(const std::string& s, std::optional<int64_t>& begin, std::optional<int64_t>& end);

size_t to_utf8(int code, char* buf);

} // namespace http

#endif // _http_utils_h_