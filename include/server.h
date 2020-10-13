#ifndef _http_server_h_
#define _http_server_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include <regex>
#include "common.h"
#include "loop.h"

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

class server : public loop
{
public:
    static string_map mime_types;
    static std::unordered_map<int, std::string> status_messages;

public:
    server(bool use_default = true);
    ~server();

    void serve(const std::string& pattern, on_router&& on_route);
    void serve(const std::string& pattern, router router);

    bool serve_file(const std::string& path, const request2& req, response2& res);

    bool listen(const std::string& address, int port, int socket_type = 0);
    inline int port() const { return port_; }

    // can call in other threads
    bool remove_cache(const std::string& path);
    void remove_cache(const std::vector<std::string>& paths);

private:
    void on_connection(uv_stream_t* socket);
    static void on_connection_cb(uv_stream_t* socket, int status);

private:
    int port_;
    uv_stream_t* socket_;
    std::shared_ptr<class buffer_pool> buffer_pool_;
    std::unordered_map<std::string, std::shared_ptr<class file_map>> file_cache_;
    std::unordered_map<std::string, router> router_map_;
    std::list<std::pair<std::regex, router>> router_list_;
};

} // namespace http

#endif // _http_server_h_