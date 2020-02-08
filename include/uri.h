#ifndef _http_uri_h_
#define _http_uri_h_

#include <string>

namespace http
{

struct uri
{
    std::string host;
    std::string port;
    std::string path;
    bool secure;

    bool parse(const std::string& url);

    static std::string decode(const std::string& s);
    static std::string encode(const std::string& s);
};

} // namespace http

#endif // _http_uri_h_