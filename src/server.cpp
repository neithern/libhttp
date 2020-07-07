#include <stdio.h>
#include <stdlib.h>
#include <list>
#include <uv.h>
#include "buffer-pool.h"
#include "common.h"
#include "content-writer.h"
#include "file-map.h"
#include "file-reader.h"
#include "parser.h"
#include "reference-count.h"
#include "server.h"
#include "uri.h"
#include "utils.h"

#ifdef _MSC_VER
#define ssize_t intptr_t
#undef min
#endif

#define _ENABLE_KEEP_ALIVE_

namespace http
{

static const size_t _max_request_body_ = 8 * 1024 * 1024;

static int _responser_count_ = 0;

class _responser : public parser, public content_writer
{
    define_reference_count(_responser)

    friend class server;

    enum _end_reason
    {
        reason_start_failed,
        reason_read_done,
        reason_write_done
    };

private:
    const std::unordered_map<std::string, router>& router_map_;
    const std::vector<std::pair<std::regex, router>> router_list_;

    // for input
    request2 request_;
    std::string peer_address_;

    // for output
    response2 response_;
    router router_ = {};

    bool keep_alive_ = false;

protected:
    _responser(uv_loop_t* loop, uv_stream_t* socket, std::shared_ptr<buffer_pool> buffer_pool,
            const std::unordered_map<std::string, router>& router_map, const std::vector<std::pair<std::regex, router>>& router_list) :
        parser(true, buffer_pool), router_map_(router_map), router_list_(router_list),
        content_writer(loop)
    {
        socket_ = socket;
        uv_handle_set_data((uv_handle_t*)socket, this);

        sockaddr_in addr = {};
        int len = sizeof(addr);
        int r = uv_tcp_getpeername((uv_tcp_t*)socket, (sockaddr*)&addr, &len);
        if (r == 0)
        {
            char name[256] = {};
            r = uv_inet_ntop(addr.sin_family, &addr.sin_addr, name, sizeof(name));
            peer_address_ = name;
        }

        _responser_count_++;
    }

    virtual ~_responser()
    {
        printf("%d alive responsers\n", --_responser_count_);
    }

    void start()
    {
        int r = start_read(socket_);
        if (r != 0)
            on_end(r, reason_start_failed);
    }

    virtual request_base* on_get_request()
    {
        return &request_;
    }

    virtual response* on_get_response()
    {
        assert(!"should not be called!");
        return &response_;
    }

    virtual bool on_headers_parsed(std::optional<int64_t> content_length)
    {
        auto end = request_.headers.cend();
        auto p = end;

        p = request_.headers.find(HEADER_CONNECTION);
        keep_alive_ = p != end && case_equals(p->second, "Keep-Alive");

        request_.range_begin.reset();
        request_.range_end.reset();
        p = request_.headers.find(HEADER_RANGE);
        if (p != end)
            parse_range(p->second, request_.range_begin, request_.range_end);

        // split url and queries
        request_.queries.clear();
        auto pos = request_.url.find('?');
        if (pos != std::string::npos)
        {
            std::string query = request_.url.substr(pos + 1);
            request_.url = request_.url.substr(0, pos);

            std::regex re("&");
            std::vector<std::string> vec(std::sregex_token_iterator(query.begin(), query.end(), re, -1), std::sregex_token_iterator());
            for (auto& s : vec)
            {
                auto pos2 = s.find('=');
                if (pos2 != std::string::npos)
                    request_.queries[s.substr(0, pos2)] = uri::decode(s.substr(pos2 + 1));
                else
                    request_.queries[s] = "";
            }
        }

        auto p2 = router_map_.find(request_.url);
        if (p2 != router_map_.cend())
        {
            router_ = p2->second;
        }
        else for (auto& p3 : router_list_)
        {
            if (std::regex_match(request_.url, p3.first))
            {
                router_ = p3.second;
                break;
            }
        }

        printf("%p:%p begin: %s\n", this, socket_, request_.url.c_str());
        return !router_.on_start || router_.on_start(request_);
    }

    virtual bool on_content_received(const char* data, size_t size)
    {
        return !router_.on_data || router_.on_data(data, size);
    }

    virtual void on_read_end(int error_code)
    {
        uv_read_stop(socket_);

        if (state_ == state_parsed && error_code >= 0)
            on_route();
        else if (state_ != state_outputing)
            on_end(error_code, reason_read_done);
    }

    void on_route()
    {
        // set default status
        response_.status_msg.clear();
        response_.headers.clear();
        response_.content_length.reset();
        response_.provider = nullptr;
        response_.releaser = nullptr;

        if (router_.on_route)
        {
            request_.headers[HEADER_REMOTE_ADDRESS] = peer_address_;
            response_.status_code = 200;
            router_.on_route(request_, response_);
            if (!response_.provider)
                response_.content_length = 0;
        }
        else
        {
            response_.content_length = 0;
            response_.status_code = 404;
        }
        start_write();
    }

    void start_write()
    {
        string_map& headers = response_.headers;

        if (case_equals(request_.method, "HEAD"))
        {
            content_written_ = content_to_write_ = 0; // to be done
            response_.content_length = 0;
        }
        else if (response_.content_length)
        {
            int64_t length = response_.content_length.value();
            int64_t length_1 = length - 1;
            if (request_.has_range())
            {
                int64_t range_end = std::min(request_.range_end.value_or(length_1), length_1);
                headers[HEADER_CONTENT_RANGE] = std::string("bytes ") + std::to_string(request_.range_begin.value()) + '-' + std::to_string(range_end) + '/' + std::to_string(length);
                response_.status_code = 206;
            }

            int64_t read_end = request_.range_end.value_or(length_1) + 1;
            content_written_ = request_.range_begin.value_or(0);
            content_to_write_ = std::min(read_end, length);
            response_.content_length = content_to_write_ - content_written_;
        }
        else
        {
            content_written_ = 0;
            content_to_write_ = INT64_MAX;
        }

        if (response_.content_length)
        {
            if (!headers.count(HEADER_CONTENT_LENGTH))
                headers[HEADER_CONTENT_LENGTH] = std::to_string(response_.content_length.value());
            if (!headers.count(HEADER_ACCEPT_RANGES))
                headers[HEADER_ACCEPT_RANGES] = "bytes";
        }

        response_.headers[HEADER_SERVER] = LIBHTTP_TAG;
#ifdef _ENABLE_KEEP_ALIVE_
        if (!response_.headers.count(HEADER_CONNECTION))
            response_.headers[HEADER_CONNECTION] = keep_alive_ ? "Keep-Alive" : "Close";
#endif

        if (response_.status_msg.empty())
        {
            auto p = server::status_messages.find(response_.status_code);
            if (p != server::status_messages.cend())
                response_.status_msg = p->second;
            else
                response_.status_msg = "Done";
        }

        auto pstr = std::make_shared<std::string>();
        std::string& str = *pstr.get();
        str.reserve(buffer_pool::buffer_size);

        str.append("HTTP/1.1 ", 9);
        str.append(std::to_string(response_.status_code));
        str.append(" ", 1);
        str.append(response_.status_msg);
        str.append(" \r\n", 3);
        for (auto& p : headers)
        {
            str.append(p.first);
            str.append(": ", 2);
            str.append(p.second);
            str.append("\r\n", 2);
        }
        str.append("\r\n", 2);

        state_ = state_outputing;
        content_writer::start_write(pstr, response_.provider);
    }

    virtual void on_write_end(int error_code)
    {
        on_end(error_code, reason_write_done);
    }

    void on_end(int error_code, _end_reason reason)
    {
        if (response_.releaser)
        {
            response_.releaser();
            response_.releaser = nullptr;
        }

        if (error_code != 0)
            keep_alive_ = false;
#ifdef _ENABLE_KEEP_ALIVE_
        if (keep_alive_)
        {
            printf("%p:%p alive%d: %s, %s, %d\n", this, socket_, reason, error_code == 0 ? "DONE" : uv_err_name(error_code), request_.url.c_str(), ref_count_);

            if (start_read(socket_) == 0)
                return;
        }
#endif
        printf("%p:%p end%d: %s, %s, %d\n", this, socket_, reason, error_code == 0 ? "DONE" : uv_err_name(error_code), request_.url.c_str(), ref_count_);

        state_ = state_none;
        assert(ref_count_ == 1);
        release();
    }
};

server::server(uv_loop_t* loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
    loop_ = loop != nullptr ? loop : uv_default_loop();
    socket_ = new uv_stream_t{};
    uv_tcp_init(loop_, (uv_tcp_t*)socket_);
}

server::~server()
{
    uv_close((uv_handle_t*)socket_, [](uv_handle_t* handle) {
        delete handle;
    });
}

bool server::listen(const std::string& address, int port)
{
    sockaddr_in addr;
    uv_ip4_addr(address.c_str(), port, &addr);

    uv_tcp_bind((uv_tcp_t*)socket_, (const sockaddr*)&addr, 0);
    uv_handle_set_data((uv_handle_t*)socket_, this);
    int r = uv_listen(socket_, 128, on_connection_cb);
    return r == 0;
}

void server::serve(const std::string& pattern, on_router on_route)
{
    router router = {};
    router.on_route = on_route;
    serve(pattern, router);
}

void server::serve(const std::string& pattern, router router)
{
    auto p = router_map_.find(pattern);
    if (p == router_map_.cend())
    {
        router_map_.emplace(pattern, router);
        router_list_.push_back(std::make_pair(std::regex(pattern), router));
    }
}

bool server::serve_file(const std::string& path, const request2& req, response2& res)
{
    uv_fs_t fs_req{};
    int r = uv_fs_stat(loop_, &fs_req, path.c_str(), nullptr);
    uint64_t length = fs_req.statbuf.st_size;
    uv_fs_req_cleanup(&fs_req);
    if (r != 0 || fs_req.result != 0 || !(fs_req.statbuf.st_mode & S_IFREG) || length == 0)
    {
        res.status_code = 404;
        return false;
    }

    std::shared_ptr<file_map> fmap;
    if (length <= INT32_MAX)
    {
        long modified_time = fs_req.statbuf.st_mtim.tv_sec;
        auto p = file_cache_.find(path);
        if (p != file_cache_.cend() && p->second->modified_time() == modified_time)
            fmap = p->second;
        else
            file_cache_[path] = fmap = std::make_shared<file_map>(path, (size_t)length, modified_time);
    }

    if (fmap && fmap->ptr() != nullptr)
    {
        res.content_length = fmap->size();
        res.provider = [fmap](int64_t offset, int64_t length, content_sink sink) {
            size_t size = std::min(buffer_pool::buffer_size, (size_t)(length - offset));
            int r = fmap->read_chunk(offset, size, sink);
            if (r < 0 && r != UV_EAGAIN)
                sink(nullptr, r, nullptr);
        };
    }
    else
    {
        auto reader = std::make_shared<file_reader>(loop_, path, buffer_pool_);
        if (reader->get_fd() < 0)
        {
            res.status_code = 403;
            return false;
        }
        res.content_length = length;
        res.provider = [reader](int64_t offset, int64_t length, content_sink sink) {
            size_t size = std::min(buffer_pool::buffer_size, (size_t)(length - offset));
            int r = reader->request_chunk(offset, size, sink);
            if (r < 0 && r != UV_EAGAIN)
                sink(nullptr, r, nullptr);
        };
    }

    std::string ext = file_extension(path);
    auto p = mime_types.find(ext);
    if (p != mime_types.cend())
        res.headers[http::HEADER_CONTENT_TYPE] = p->second;
    return true;
}

int server::run_loop()
{
    return uv_run(loop_, UV_RUN_DEFAULT);
}

void server::stop_loop()
{
    uv_stop(loop_);
}

void server::on_connection(uv_stream_t* socket)
{
    uv_tcp_t* tcp = new uv_tcp_t{};
    uv_tcp_init(loop_, tcp);
    if (uv_accept(socket, (uv_stream_t*)tcp) == 0)
    {
        uv_tcp_keepalive(tcp, 1, 30); // in seconds
        (new _responser(loop_, (uv_stream_t*)tcp, buffer_pool_, router_map_, router_list_))->start();
    }
    else
    {
        uv_close((uv_handle_t*)tcp, parser::on_closed_and_delete_cb);
    }
}

void server::on_connection_cb(uv_stream_t* socket, int status)
{
    server* p_this = (server*)uv_handle_get_data((uv_handle_t*)socket);
    if (status == 0)
        p_this->on_connection(socket);
    else
        printf("accept error: %d\n", status);
}

string_map server::mime_types =
{
    { "txt", "text/plain" },
    { "htm", "text/html" },
    { "html","text/html" },
    { "css", "text/css" },
    { "gif", "image/gif" },
    { "jpg", "image/jpg" },
    { "php", "application/x-httpd-php" },
    { "png", "image/png" },
    { "svg", "image/svg+xml" },
    { "flv", "video/x-flv" },
    { "3gp", "video/3gpp" },
    { "m3u8","application/vnd.apple.mpegURL" },
    { "mov", "video/quicktime" },
    { "mp4", "video/mp4" },
    { "ts",  "video/MP2T" },
    { "js",  "application/javascript" },
    { "json","application/json" },
    { "pdf", "application/pdf" },
    { "wasm","application/wasm" },
    { "xml", "application/xml" },
};

std::unordered_map<int, std::string> server::status_messages =
{
    { 200, "OK" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 206, "Partial Content" },
    { 301, "Moved Permanently" },
    { 302, "Found" },
    { 303, "See Other" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 413, "Payload Too Large" },
    { 414, "Request-URI Too Long" },
    { 415, "Unsupported Media Type" },
    { 416, "Range Not Satisfiable" },
    { 500, "Internal Server Error" },
    { 503, "Service Unavailable" },
};

} // namespace http