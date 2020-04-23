#ifndef _http_server_h_
#define _http_server_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include <regex>
#include "common.h"

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_stream_s uv_stream_t;

namespace http
{

struct request2 : public request_base
{
    std::optional<int64_t> range_begin;
    std::optional<int64_t> range_end;

    inline bool has_range() const { return range_begin.has_value(); }
};

struct response2 : public response
{
    content_provider provider;
    content_done releaser;
};

using on_request_start = std::function<bool(const request2& req)>;
using on_request_data = std::function<bool(const char* data, size_t size)>;
using on_router = std::function<void(const request2& req, response2& res)>;

struct router
{
    on_request_start on_start;
    on_request_data on_data;
    on_router on_route;
};

class server
{
public:
    static string_map mime_types;
    static std::unordered_map<int, std::string> status_messages;

public:
    server(uv_loop_t* loop = nullptr);
    ~server();

    void serve(const std::string& pattern, on_router on_route);
    void serve(const std::string& pattern, router router);

    bool serve_file(const std::string& path, const request2& req, response2& res);

    bool listen(const std::string& address, int port);

    int run_loop();
    void stop_loop();

private:
    void on_connection(uv_stream_t* socket);

    static void on_connection_cb(uv_stream_t* socket, int status);

private:
    uv_loop_t* loop_;
    uv_stream_t* socket_;
    std::shared_ptr<class buffer_pool> buffer_pool_;
    std::unordered_map<std::string, std::shared_ptr<class file_map>> file_cache_;
    std::unordered_map<std::string, router> router_map_;
    std::vector<std::pair<std::regex, router>> router_list_;
};

} // namespace http

#endif // _http_server_h_