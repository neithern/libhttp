#ifndef _http_client_h_
#define _http_client_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include "common.h"
#include "loop.h"

namespace http
{

using on_response = std::function<bool(const response& res)>;
using on_content = std::function<bool(const char* data, size_t size, bool end)>;
using on_content_body = std::function<void(const std::string& body, int error)>;
using on_redirect = std::function<bool(std::string& url)>;
using on_error = std::function<void(int code)>;

class client : public loop
{
public:
    client(bool use_default = true);
    ~client();

    int fetch(const request& request,
                on_response&& on_response,
                on_content&& on_content,
                on_redirect&& on_redirect = nullptr,
                on_error&& on_error = nullptr);

    int fetch(const request& request,
                on_content_body&& on_body,
                on_response&& on_response = nullptr,
                on_redirect&& on_redirect = [](std::string& url) { return true; });

private:
    std::shared_ptr<class buffer_pool> buffer_pool_;
    std::shared_ptr<std::unordered_multimap<std::string, class _socket_checker*>> socket_cache_;
};

} // namespace http

#endif // _http_client_h_