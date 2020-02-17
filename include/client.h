#ifndef _http_client_h_
#define _http_client_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include "common.h"

typedef struct uv_loop_s uv_loop_t;

namespace http
{

using on_response = std::function<bool(const response& res)>;
using on_content = std::function<bool(const char* data, size_t size, bool final)>;
using on_content_body = std::function<void(const std::string& body, int error)>;
using on_redirect = std::function<bool(std::string& url)>;
using on_error = std::function<void(int code)>;

class client
{
public:
    client(uv_loop_t* loop = nullptr);

    bool fetch(const request& request,
                on_response on_response,
                on_content on_content,
                on_redirect on_redirect = nullptr,
                on_error on_error = nullptr);

    bool fetch(const request& request,
                on_content_body on_body,
                on_response on_response = nullptr,
                on_redirect on_redirect = [](std::string& url) { return true; });

    int run_loop();

private:
    uv_loop_t* loop_;
    std::shared_ptr<struct buffer_pool> buffer_pool_;
};

} // namespace http

#endif // _http_client_h_