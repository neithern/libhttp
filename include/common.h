#ifndef _http_common_h_
#define _http_common_h_

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace http
{

static const std::string HEADER_ACCEPT_ENCODING     = "Accept-Encoding";
static const std::string HEADER_ACCEPT_RANGES       = "Accept-Ranges";
static const std::string HEADER_CONNECTION          = "Connection";
static const std::string HEADER_CONTENT_LENGTH      = "Content-Length";
static const std::string HEADER_CONTENT_RANGE       = "Content-Range";
static const std::string HEADER_CONTENT_TYPE        = "Content-Type";
static const std::string HEADER_LOCATION            = "Location";
static const std::string HEADER_RANGE               = "Range";
static const std::string HEADER_REMOTE_ADDRESS      = "Remote-Address";
static const std::string HEADER_TRANSFER_ENCODING   = "Transfer-Encoding";
static const std::string HEADER_USER_AGENT          = "User-Agent";

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

static const string_case_hash   case_hash;
static const string_case_equals case_equals;

using string_map = std::unordered_map<std::string, std::string, string_case_hash, string_case_equals>;

using content_done = std::function<void()>;
using content_sink = std::function<void(const char* data, size_t size, content_done done)>;
using content_provider = std::function<void(int64_t offset, int64_t length, content_sink sink)>;

struct request_base
{
    std::string method = "GET";
    std::string url;
    string_map headers;
    content_provider provider;
};

struct request : public request_base
{
    content_provider provider;
};

struct response
{
    int status_code = 0;
    std::string status_msg;
    std::optional<int64_t> content_length;
    string_map headers;

    inline bool is_ok() const { return status_code >= 200 && status_code <= 299; }
    inline bool is_redirect() const { return status_code >= 300 && status_code <= 310; }
};

} // namespace http

#endif // _http_common_h_