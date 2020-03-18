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
static const std::string HEADER_SERVER              = "Server";
static const std::string HEADER_TRANSFER_ENCODING   = "Transfer-Encoding";
static const std::string HEADER_USER_AGENT          = "User-Agent";

static const std::string LIBHTTP_TAG = "libhttp/0.1";

struct string_case_hash : public std::hash<std::string>
{
    static inline std::uint32_t get_chars(unsigned char const* p)
    {
        return p[0] | (p[1] <<  8) | (p[2] << 16) | (p[3] << 24);
    }

    // from https://github.com/boostorg/beast/blob/develop/include/boost/beast/http/impl/field.ipp
    inline size_t operator()(const std::string& s) const
    {
        size_t r = 0;
        size_t n = s.size();
        auto p = reinterpret_cast<unsigned char const*>(s.data());
        // consume N characters at a time
        // VFALCO Can we do 8 on 64-bit systems?
        while (n >= 4)
        {
            auto const v = get_chars(p);
            r = (r * 5 + (v | 0x20202020)); // convert to lower
            p += 4;
            n -= 4;
        }
        // handle remaining characters
        while (n > 0)
        {
            r = r * 5 + (*p | 0x20);
            ++p;
            --n;
        }
        return r;
    }
};

struct string_case_equals : public std::equal_to<std::string>
{
    // from https://github.com/boostorg/beast/blob/develop/include/boost/beast/http/impl/field.ipp
    inline bool operator()(const std::string& lhs, const std::string& rhs) const
    {
        auto n = lhs.size();
        if (n != rhs.size())
            return false;
        auto p1 = reinterpret_cast<unsigned char const*>(lhs.data());
        auto p2 = reinterpret_cast<unsigned char const*>(rhs.data());
        auto constexpr s = sizeof(std::uint32_t);
        auto constexpr mask = static_cast<std::uint32_t>(0xDFDFDFDFDFDFDFDF & ~std::uint32_t{0});
        for (; n >= s; p1 += s, p2 += s, n -= s)
        {
            std::uint32_t const v1 = string_case_hash::get_chars(p1);
            std::uint32_t const v2 = string_case_hash::get_chars(p2);
            if ((v1 ^ v2) & mask)
                return false;
        }
        for (; n; ++p1, ++p2, --n)
            if(( *p1 ^ *p2) & 0xDF)
                return false;
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