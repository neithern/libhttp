#ifndef _http_common_h_
#define _http_common_h_

#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>

namespace http
{

#define HEADER_ACCEPT_ENCODING      "Accept-Encoding"
#define HEADER_ACCEPT_RANGES        "Accept-Ranges"
#define HEADER_CONNECTION           "Connection"
#define HEADER_CONTENT_LENGTH       "Content-Length"
#define HEADER_CONTENT_RANGE        "Content-Range"
#define HEADER_CONTENT_TYPE         "Content-Type"
#define HEADER_LOCATION             "Location"
#define HEADER_RANGE                "Range"
#define HEADER_TRANSFER_ENCODING    "Transfer-Encoding"
#define HEADER_USER_AGENT           "User-Agent"

struct string_case_hash : public std::hash<std::string>
{
    size_t operator()(const std::string& v) const
    {
        std::string low = v;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        return std::hash<std::string>::operator()(low);
    }
};

struct string_case_equals : public std::equal_to<std::string>
{
    bool operator()(const std::string& x, const std::string& y) const
    {
        if (x.size() != y.size())
            return false;

        for (std::string::const_iterator c1 = x.begin(), c2 = y.begin(); c1 != x.end(); c1++, c2++)
        {
            if (::tolower(*c1) != ::tolower(*c2))
                return false;
        }
        return true;
    }
};

using string_map = std::unordered_map<std::string, std::string, string_case_hash, string_case_equals>;
using headers = string_map;

struct request
{
    std::string method = "GET";
    std::string url;
    headers headers;
    std::string body;
};

struct response
{
    int status_code = 0;
    std::string status_msg;
    std::optional<int64_t> content_length;
    headers headers;

    inline bool is_ok() const { return status_code >= 200 && status_code <= 299; }
    inline bool is_redirect() const { return status_code >= 300 && status_code <= 310; }
};

} // namespace http

#endif // _http_common_h_