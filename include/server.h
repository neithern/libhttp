#ifndef _http_server_h_
#define _http_server_h_

#include <stdlib.h>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include "common.h"

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_stream_s uv_stream_t;

namespace http
{

using recycler = std::function<void(void* ptr)>;

struct content_provider
{
    // either reader or referer
    std::function<size_t(char* buffer, size_t size, int64_t offset)> reader;
    std::function<void*(char*& data, size_t& size, int64_t offset, recycler& recycler)> referer;
    std::function<void()> releaser;

    void clear()
    {
        reader = nullptr;
        referer = nullptr;
        releaser = nullptr;
    }
};

struct response2 : public response
{
    content_provider content_provider;
    int64_t range_begin = 0;
    int64_t range_end = 0;
};

using on_router = std::function<void(const request& req, response2& res)>;

class server
{
public:
    server(uv_loop_t* loop = nullptr);
    ~server();

    void serve(const std::string& pattern, on_router router);

    bool serve_file(const std::string& path, const request& req, response2& res);

    bool listen(const std::string& address, int port);

    int run_loop();

private:
    void on_connection(uv_stream_t* socket);

    static void on_connection_cb(uv_stream_t* socket, int status);

private:
    uv_loop_t* loop_;
    uv_stream_t* socket_;
    std::shared_ptr<struct buffer_pool> buffer_pool_;
    std::unordered_map<std::string, on_router> router_map_;
    std::vector<std::pair<std::regex, on_router>> router_list_;
};

} // namespace http

#endif // _http_server_h_