#ifndef _http_server_h_
#define _http_server_h_

#include <stdlib.h>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <regex>
#include "common.h"

typedef struct uv_async_s uv_async_t;
typedef struct uv_loop_s uv_loop_t;
typedef struct uv_stream_s uv_stream_t;

namespace http
{

struct request2 : public request_base
{
    string_map queries;
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
    server(bool default_loop = true);
    ~server();

    void serve(const std::string& pattern, on_router&& on_route);
    void serve(const std::string& pattern, router router);

    bool serve_file(const std::string& path, const request2& req, response2& res);

    bool listen(const std::string& address, int port);

    // can call in other threads
    bool remove_cache(const std::string& path);

    inline uv_loop_t* get_loop() const { return loop_; }

    int run_loop();
    void stop_loop();

private:
    void on_connection(uv_stream_t* socket);
    static void on_connection_cb(uv_stream_t* socket, int status);

    void on_async();
    static void on_async_cb(uv_async_t* handle);

private:
    uv_loop_t* loop_;
    uv_stream_t* socket_;
    void* server_thread_;
    std::shared_ptr<class buffer_pool> buffer_pool_;
    std::unordered_map<std::string, std::shared_ptr<class file_map>> file_cache_;
    std::unordered_map<std::string, router> router_map_;
    std::list<std::pair<std::regex, router>> router_list_;
    std::mutex list_mutex_;
    std::list<std::string> to_remove_list_;
};

} // namespace http

#endif // _http_server_h_