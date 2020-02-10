#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "utils.h"
#include "buffer_pool.h"
#include "server.h"
#include "parser.h"

namespace http
{

#define _ENABLE_KEEP_ALIVE_

static const size_t _buffer_size = 64 * 1024;
static const size_t _max_request_body_ = 8 * 1024 * 1024;

static const char* status_message(int status)
{
    switch (status)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 414: return "Request-URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 503: return "Service Unavailable";

    default:
    case 500: return "Internal Server Error";
    }
}

struct _write_head_req : public uv_write_t
{
    uv_buf_t buf = {nullptr, 0};
    // for response header
    std::string data;
};

struct _write_data_req : public uv_write_t
{
    uv_buf_t buf = {nullptr, 0};
    // for content_provider.getter
    size_t capacity = 0;
    // for content_provider.setter
    void* ptr = nullptr;
    recycler recycler;

    void recycle()
    {
        if (recycler && ptr != nullptr)
            recycler(ptr);
        recycler = nullptr;
        ptr = nullptr;
    }
};

class _responser : public parser
{
    friend class server;

private:
    int ref_count_ = 1;

    uv_loop_t* loop_;

    const std::unordered_map<std::string, on_router>& router_map_;
    const std::vector<std::pair<std::regex, on_router>> router_list_;

    // for input
    request request_;

    // for output
    int64_t content_written_ = 0;
    int64_t content_to_write_ = 0;
    response2 response_;
    _write_head_req write_head_req_;
    _write_data_req write_data_req_;

    bool keep_alive_ = false;
    uv_stream_t* socket_ = nullptr;
    uv_timer_t* write_timer_ = nullptr;

protected:
    _responser(uv_loop_t* loop, uv_stream_t* socket, std::shared_ptr<buffer_pool> buffer_pool,
            const std::unordered_map<std::string, on_router>& router_map, const std::vector<std::pair<std::regex, on_router>>& router_list)
        : parser(true, buffer_pool), router_map_(router_map), router_list_(router_list)
    {
        loop_ = loop;
        socket_ = socket;
        uv_handle_set_data((uv_handle_t*)socket_, this);

        uv_handle_set_data((uv_handle_t*)&write_head_req_, this);
        uv_handle_set_data((uv_handle_t*)&write_data_req_, this);
    }

    ~_responser()
    {
        close_socket();
    }

    void start()
    {
        int r = start_read(socket_);
        if (r != 0)
            on_end(r);
    }

    virtual request* on_get_request()
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
        keep_alive_ = p != end && string_case_equals().operator()(p->second, "Keep-Alive");

        request_.range_begin.reset();
        request_.range_end.reset();
        p = request_.headers.find(HEADER_RANGE);
        if (p != end)
            parse_range(p->second, request_.range_begin, request_.range_end);

        request_.body.clear();
        request_.body.reserve(4096);

        printf("%p:%p begin: %s\n", this, socket_, request_.url.c_str());
        return true;
    }

    virtual bool on_content_received(const char* data, size_t size)
    {
        request_.body.append(data, size);
        return request_.body.size() < _max_request_body_;
    }

    virtual void on_read_done(int error_code)
    {
        if (error_code < 0)
            on_end(error_code);
        else
            on_route();
    }

    void on_route()
    {
        on_router router = nullptr;
        auto p = router_map_.find(request_.url);
        if (p != router_map_.cend())
        {
            router = p->second;
        }
        else
        {
            for (auto it = router_list_.cbegin(); it != router_list_.cend(); it++)
            {
                if (std::regex_match(request_.url, it->first))
                {
                    router = it->second;
                    break;
                }
            }
        }

        // set default status
        response_.status_msg.clear();
        response_.headers.clear();
        response_.content_length.reset();
        response_.content_provider.clear();

        if (router != nullptr)
        {
            response_.status_code = 200;
            router(request_, response_);
            if (!response_.is_ok())
                response_.content_length = 0;
#ifdef _ENABLE_KEEP_ALIVE_
            if (keep_alive_ && !response_.has_header(HEADER_CONNECTION))
                response_.headers[HEADER_CONNECTION] = "Keep-Alive";
#endif
        }
        else
        {
            response_.content_length = 0;
            response_.status_code = 404;
        }
        if (response_.status_msg.empty())
            response_.status_msg = status_message(response_.status_code);
        start_write();
    }

    inline bool is_write_done() { return content_written_ >= content_to_write_; }
    inline void set_write_done() { content_to_write_ = 0; }

    void start_write()
    {
        if (!response_.content_length && (response_.status_code < 200 || response_.status_code >= 299))
            response_.content_length = 0;

        char sz[256] = {0};
        headers& headers = response_.headers;

        if (string_case_equals().operator()(request_.method, "HEAD"))
        {
            content_written_ = content_to_write_ = 0; // to be done
        }
        else if (response_.content_length)
        {
            if (!response_.has_header(HEADER_CONTENT_LENGTH))
            {
                ::snprintf(sz, 256, "%lld", response_.content_length.value());
                headers[HEADER_CONTENT_LENGTH] = sz;
                headers[HEADER_ACCEPT_RANGES] = "bytes";
            }

            int64_t length = response_.content_length.value();
            int64_t length_1 = length - 1;
            if (request_.has_range())
            {
                int64_t range_end = std::min(request_.range_end.value_or(length_1), length_1);
                ::snprintf(sz, 256, "bytes %lld-%lld/%lld", request_.range_begin.value(), range_end, length);
                headers[HEADER_CONTENT_RANGE] = sz;
                response_.status_code = 206;
                printf("%p:%p range: %s\n", this, socket_, sz);
            }

            int64_t read_end = request_.range_end.value_or(length_1) + 1;
            content_written_ = request_.range_begin.value_or(0);
            content_to_write_ = std::min(read_end, length);
        }
        else
        {
            content_written_ = 0;
            content_to_write_ = INT64_MAX;
        }

        ::snprintf(sz, 64, "%d", response_.status_code);
        if (response_.status_msg.empty())
            response_.status_msg = "abc";

        std::string res_text;
        res_text.reserve(4096);
        res_text += "HTTP/1.1 ";
        res_text += sz;
        res_text += " ";
        res_text += response_.status_msg;
        res_text += " \r\n";

        for (auto it = headers.cbegin(); it != headers.cend(); it++)
        {
            res_text += it->first;
            res_text += ": ";
            res_text += it->second;
            res_text += "\r\n";
        }
        res_text += "\r\n";

        size_t size = res_text.size();
        _write_head_req& head_req = write_head_req_;
        head_req.data = res_text; // hold the buffer
        head_req.buf.base = const_cast<char*>(res_text.c_str());
        head_req.buf.len = size;
        uv_write(&head_req, socket_, &head_req.buf, 1, on_written_cb);
    }

    int start_next_write()
    {
        if (socket_ == nullptr)
            return UV_ESHUTDOWN;

        size_t max_read = content_to_write_ - content_written_;
        if ((ssize_t)max_read <= 0)
            return 0; // write done

        const content_provider& provider = response_.content_provider;
        _write_data_req& data_req = write_data_req_;
        size_t read_size = 0; 

        if (provider.reader)
        {
            if (data_req.capacity == 0)
            {
                if (!buffer_pool_->get_buffer(_buffer_size, data_req.buf))
                    return UV_ENOMEM;
                data_req.capacity = data_req.buf.len;
            }
            read_size = provider.reader(data_req.buf.base, std::min(max_read, data_req.capacity), content_written_);
        }
        else if (provider.referer)
        {
            data_req.buf.base = nullptr;
            data_req.recycler = nullptr;
            data_req.ptr = provider.referer(data_req.buf.base, read_size, content_written_, data_req.recycler);
        }
        else
            return content_to_write_ == 0 ? 0 : UV_E_USER_CANCELED;

        if ((ssize_t)read_size < 0)
        {
            printf("%p:%p read content: %s\n", this, socket, uv_err_name(read_size));
            return read_size;
        }
        else if (read_size == 0)
        {
            if (write_timer_ == nullptr)
            {
                write_timer_ = new uv_timer_t;
                uv_timer_init(loop_, write_timer_);
                uv_handle_set_data((uv_handle_t*)write_timer_, this);
            }
            uv_timer_start(write_timer_, on_write_timer_cb, 10, 0);
            return 0;
        }

        if (read_size > max_read)
            read_size = max_read;
        content_written_ += read_size;
        data_req.buf.len = read_size;
        return uv_write(&data_req, socket_, &data_req.buf, 1, on_written_cb);
    }

    int on_written(uv_write_t* req)
    {
        if (req == &write_data_req_)
            write_data_req_.recycle();
        return start_next_write();
    }

    void on_end(int error_code, bool release_this = true)
    {
        if (error_code != 0)
            keep_alive_ = false;
        printf("%p:%p end: %s, %s, %s, %d\n", this, socket_, error_code == 0 ? "ok" : uv_err_name(error_code), request_.url.c_str(), keep_alive_ ? "alive" : "close", ref_count_);

        content_provider& provider = response_.content_provider;
        if (provider.releaser)
        {
            provider.releaser();
            provider.clear();
        }

#ifdef _ENABLE_KEEP_ALIVE_
        if (keep_alive_)
        {
            reset_status();
            return;
        }
#endif

        release();
    }

    void release()
    {
        if (--ref_count_ == 0)
            delete this;
    }

    void close_socket()
    {
        uv_timer_t* timer = write_timer_;
        write_timer_ = nullptr;
        if (timer != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)timer, nullptr);
            uv_timer_stop(timer);
            delete timer;
        }

        uv_stream_t* tcp = socket_;
        socket_ = nullptr;
        if (tcp != nullptr)
        {
            uv_cancel((uv_req_t*)&write_head_req_);
            uv_cancel((uv_req_t*)&write_data_req_);
            uv_handle_set_data((uv_handle_t*)&write_head_req_, nullptr);
            uv_handle_set_data((uv_handle_t*)&write_data_req_, nullptr);

            _write_data_req& req = write_data_req_;
            if (req.capacity > 0)
            {
                buffer_pool_->recycle_buffer(req.buf);
                req.capacity = 0;
            }
            else
                req.recycle();

            // printf("%p:%p closing socket\n", this, tcp);
            uv_handle_set_data((uv_handle_t*)tcp, nullptr);
            uv_close((uv_handle_t*)tcp, on_closed_and_delete_cb);
        }
    }

private:
    static void on_written_cb(uv_write_t* req, int status)
    {
        _responser* p_this = (_responser*)uv_req_get_data((uv_req_t*)req);
        if (p_this == nullptr)
            return;

        int r = status;
        if (status >= 0)
        {
            r = p_this->on_written(req);
            if (r < 0 && r != UV_E_USER_CANCELED)
                printf("%p:%p write socket: %s\n", p_this, p_this->socket_, uv_err_name(r));
        }
        else
            printf("%p:%p on_written_cb: %s\n", p_this, p_this->socket_, uv_err_name(r));

        if (r == UV_EOF && !p_this->is_write_done())
            p_this->set_write_done();

        bool done = p_this->is_write_done();
        if (r < 0 || done)
            p_this->on_end(r);
    }

    static void on_write_timer_cb(uv_timer_t* timer)
    {
        _responser* p_this = (_responser*)uv_req_get_data((uv_req_t*)timer);
        uv_timer_stop(timer);
        if (p_this == nullptr)
            return;

        on_written_cb(&p_this->write_data_req_, 0);
    }
};

server::server(uv_loop_t* loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>(_buffer_size);
    loop_ = loop != nullptr ? loop : uv_default_loop();
    socket_ = new uv_stream_t;
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

void server::serve(const std::string& pattern, on_router router)
{
    auto p = router_map_.insert_or_assign(pattern, router);
    if (p.second)
        router_list_.push_back(std::make_pair(std::regex(pattern), router));
}

bool server::serve_file(const std::string& path, const request& req, response2& res) const
{
    uv_fs_t fs_req;
    int r = uv_fs_stat(loop_, &fs_req, path.c_str(), nullptr);
    uint64_t length = fs_req.statbuf.st_size;
    uv_fs_req_cleanup(&fs_req);
    if (r != 0 || fs_req.result != 0 || !(fs_req.statbuf.st_mode & S_IFREG) || length == 0)
    {
        res.status_code = 404;
        return false;
    }

    uv_fs_t open_req;
    int flags = UV_FS_O_RDONLY | (req.has_range() ? UV_FS_O_RANDOM : UV_FS_O_SEQUENTIAL);
    uv_file fs_file = uv_fs_open(loop_, &open_req, path.c_str(), flags, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
    if (fs_file < 0)
    {
        printf("open file: %s, %s\n", path.c_str(), uv_err_name(fs_file));
        res.status_code = 404;
        return false;
    }

    res.content_length = length;
    res.content_provider.releaser = [=]() {
        uv_fs_t close_req;
        int nread = uv_fs_close(nullptr, &close_req, fs_file, nullptr);
        uv_fs_req_cleanup(&close_req);
        if (nread < 0)
            printf("close file %d: %s, %s\n", fs_file, path.c_str(), uv_err_name(nread));
    };
    res.content_provider.reader = [=](char* buffer, size_t size, int64_t offset) {
        uv_fs_t read_req;
        uv_buf_t buf = uv_buf_init(buffer, size);
        int nread = uv_fs_read(nullptr, &read_req, fs_file, &buf, 1, offset, nullptr);
        uv_fs_req_cleanup(&read_req);
        if (nread < 0)
            printf("read file %d: %s, %s\n", fs_file, path.c_str(), uv_err_name(nread));
        return nread;
    };
    /* or
    res.content_provider.referer = [=](char*& data, size_t& size, int64_t offset, recycler& recycler) {
        size_t capacity = _buffer_size;
        uv_buf_t* p_buf = (uv_buf_t*)buffer_pool_->get_buffer(sizeof(uv_buf_t) + capacity);
        if (p_buf == nullptr)
        {
            size = UV_ENOMEM;
            return (uv_buf_t*)nullptr;
        }

        uv_fs_t read_req;
        recycler = [=](void* ptr) { buffer_pool_->recycle_buffer(ptr); };
        data = p_buf->base = (char*)p_buf + sizeof(uv_buf_t);
        p_buf->len = capacity;
        size = uv_fs_read(nullptr, &read_req, fs_file, p_buf, 1, offset, nullptr);
        uv_fs_req_cleanup(&read_req);
        return p_buf;
    };*/
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
    uv_tcp_t* tcp = new uv_tcp_t;
    uv_tcp_init(loop_, tcp);
    if (uv_accept(socket, (uv_stream_t*)tcp) == 0)
    {
        _responser* responser = new _responser(loop_, (uv_stream_t*)tcp, buffer_pool_, router_map_, router_list_);
        responser->start();
    }
    else
    {
        uv_close((uv_handle_t*)tcp, _responser::on_closed_and_delete_cb);
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

} // namespace http