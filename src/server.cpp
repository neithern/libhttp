#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "utils.h"
#include "buffer_pool.h"
#include "server.h"
#include "parser.h"

namespace http
{

static const size_t _max_request_body_ = 8 * 1024 * 1024;

struct _write_head_req : public uv_write_t
{
    uv_buf_t buf = {};
    // for response header
    std::string data;
};

struct _write_data_req : public uv_write_t
{
    uv_buf_t buf = {};
    // for content_provider.getter
    size_t capacity = 0;
    // for content_provider.setter
    void* ptr = nullptr;
    std::function<void(void* ptr)> recycler;
};

class _responser : public parser
{
    friend class server;

protected:
    int ref_count = 1;

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

    _responser(std::shared_ptr<buffer_pool> buffer_pool, const std::unordered_map<std::string, on_router>& router_map, const std::vector<std::pair<std::regex, on_router>>& router_list)
        : parser(true, buffer_pool), router_map_(router_map), router_list_(router_list)
    {
        uv_handle_set_data((uv_handle_t*)&write_head_req_, this);
        uv_handle_set_data((uv_handle_t*)&write_data_req_, this);
    }

    ~_responser()
    {
        close_socket();
    }

    void start()
    {
        uv_handle_set_data((uv_handle_t*)socket_, this);
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

        response_.range_begin = 0;
        response_.range_end = 0;
        p = request_.headers.find(HEADER_RANGE);
        if (p != end)
            parse_range(p->second, response_.range_begin, response_.range_end);

        request_.body.clear();
        request_.body.reserve(4096);
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
        response_.status_code = 404;
        response_.status_msg = "not found";
        response_.content_length.reset();
        response_.headers.clear();
        response_.content_provider.clear();

        if (router != nullptr)
        {
            router(request_, response_);
            if (keep_alive_ && response_.headers.find(HEADER_CONNECTION) == response_.headers.cend())
                response_.headers[HEADER_CONNECTION] = "Keep-Alive";
        }
        else
            response_.content_length = 0;

        start_write();
    }

    inline bool is_write_done() { return content_written_ >= content_to_write_; }
    inline void set_write_done() { content_to_write_ = 0; }

    void start_write()
    {
        headers& headers = response_.headers;
        int status_code = response_.status_code;

        if (!response_.content_length && (status_code < 200 || status_code >= 299))
            response_.content_length = 0;

        if (response_.status_msg.empty())
            response_.status_msg = ".";

        char sz[64] = {};
        ::snprintf(sz, 64, "%d", status_code);

        std::string res_text;
        res_text.reserve(4096);
        res_text += "HTTP/1.1 ";
        res_text += sz;
        res_text += " ";
        res_text += response_.status_msg;
        res_text += " \r\n";
        if (response_.content_length && headers.find(HEADER_CONTENT_LENGTH) == headers.cend())
        {
            ::snprintf(sz, 64, "%lld", response_.content_length.value());
            headers[HEADER_CONTENT_LENGTH] = sz;
        }
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

        if (string_case_equals().operator()(request_.method, "HEAD"))
        {
            content_to_write_ = 0; // to be done
        }
        else if (response_.content_length)
        {
            int64_t length = response_.content_length.value();
            if (response_.range_end > length - 1)
                response_.range_end = length - 1;
            content_to_write_ = response_.range_end > response_.range_begin
                ? response_.range_end + 1 - response_.range_begin
                : length - response_.range_begin;
        }
        else
        {
            content_to_write_ = INT64_MAX;
        }
        content_written_ = 0; // ignore the headers
        uv_write(&head_req, socket_, &head_req.buf, 1, on_written_cb);
    }

    int start_next_write()
    {
        if (socket_ == nullptr)
            return UV_ESHUTDOWN;

        const content_provider& provider = response_.content_provider;
        _write_data_req& data_req = write_data_req_;
        data_req.buf.len = 0;

        if (provider.getter)
        {
            if (data_req.capacity == 0)
            {
                if (!buffer_pool_->get_buffer(65536, data_req.buf))
                    return UV_ENOMEM;
                data_req.capacity = data_req.buf.len;
            }
            data_req.buf.len = provider.getter(data_req.buf.base, data_req.capacity, content_written_);
        }
        else if (provider.setter)
        {
            assert(data_req.recycler == nullptr);
            assert(data_req.ptr == nullptr);
            data_req.ptr = provider.setter(data_req.buf.base, data_req.buf.len, content_written_, data_req.recycler);
        }
        if (data_req.buf.len <= 0)
            return data_req.buf.len;

        content_written_ += data_req.buf.len;
        return uv_write(&data_req, socket_, &data_req.buf, 1, on_written_cb);
    }

    int on_written()
    {
        _write_data_req& req = write_data_req_;
        if (req.ptr != nullptr && req.recycler)
        {
            req.recycler(req.ptr);
            req.recycler = nullptr;
            req.ptr = nullptr;
        }

        if (is_write_done())
            return 0; // UV_E_USER_CANCELED;
        return start_next_write();
    }

    void on_end(int error_code, bool release_this = true)
    {
        const content_provider& provider = response_.content_provider;
        if (provider.releaser)
            provider.releaser();

        if (error_code == 0 && keep_alive_)
        {
            ++ref_count;
            reset_status();
        }

        release();
    }

    void release()
    {
        if (--ref_count == 0)
            delete this;
    }

    void close_socket()
    {
        uv_stream_t* tcp = socket_;
        socket_ = nullptr;
        if (tcp != nullptr)
        {
            printf("%p closing socket %p\n", this, tcp);
            uv_handle_set_data((uv_handle_t*)tcp, nullptr);
            uv_close((uv_handle_t*)tcp, on_closed_and_delete_cb);
        }

        uv_cancel((uv_req_t*)&write_head_req_);
        uv_cancel((uv_req_t*)&write_data_req_);
        uv_handle_set_data((uv_handle_t*)&write_head_req_, nullptr);
        uv_handle_set_data((uv_handle_t*)&write_data_req_, nullptr);

        _write_data_req& req = write_data_req_;
        if (req.capacity > 0)
        {
            req.capacity = 0;
            buffer_pool_->recycle_buffer(req.buf);
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
            r = p_this->on_written();
            if (r < 0 && r != UV_E_USER_CANCELED)
                printf("handle written error: %s\n", uv_strerror(r));
        }
        else
            printf("on_written error: %s\n", uv_strerror(status));

        if (r == UV_EOF && !p_this->is_write_done())
            p_this->set_write_done();

        bool done = p_this->is_write_done();
        if (r < 0 || done)
            p_this->on_end(r);
    }
};

server::server(uv_loop_t* loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
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
        router_list_.push_back(std::pair<std::regex, on_router>(std::regex(pattern), router));
}

bool server::serve_file(const std::string& path, const request& req, response2& res)
{
    uv_fs_t fs_req;
    int r = uv_fs_stat(loop_, &fs_req, path.c_str(), nullptr);
    if (r != 0 || fs_req.result != 0 || !(fs_req.statbuf.st_mode & S_IFREG))
        return false;

    uint64_t length = fs_req.statbuf.st_size;
    uv_fs_req_cleanup(&fs_req);

    bool has_range = res.range_begin > 0 || res.range_begin < res.range_end;
    if (res.range_end > length - 1)
        res.range_end = length - 1;

    int flags = UV_FS_O_RDONLY;
    flags |= has_range ? UV_FS_O_RANDOM : UV_FS_O_SEQUENTIAL;

    uv_fs_t open_req;
    uv_file fs_file = uv_fs_open(loop_, &open_req, path.c_str(), flags, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
    if (fs_file == -1)
        return false;

    if (has_range)
    {
        if (res.range_end == 0)
            res.range_end = length - 1;
        char sz[256] = {};
        ::snprintf(sz, 256, "bytes %lld-%lld/%lld", res.range_begin, res.range_end, length);
        res.headers[HEADER_CONTENT_RANGE] = sz;
    }
    res.headers[HEADER_ACCEPT_RANGES] = "bytes";

    int64_t read_begin = res.range_begin;
    int64_t read_end = res.range_end == 0 ? length : res.range_end + 1; // range end in headers is length-1

    res.content_length = length;
    res.content_provider.releaser = [=]() {
        uv_fs_t close_req;
        uv_fs_close(nullptr, &close_req, fs_file, nullptr);
        uv_fs_req_cleanup(&close_req);
    };
    res.content_provider.getter = [=](char* buffer, size_t size, int64_t offset) {
        uv_fs_t read_req;
        offset += read_begin;
        if (offset + size > read_end)
            size = read_end > offset ? read_end - offset : 0;
        uv_buf_t buf = uv_buf_init(buffer, size);
        size = uv_fs_read(nullptr, &read_req, fs_file, &buf, 1, offset, nullptr);
        // printf("read at: %lld, %zu bytes\n", offset, size);
        return size;
    };
    // or
    // res.content_provider.setter = [=](char*& data, size_t& size, int64_t offset, recycler& recycler) {
    //     uv_fs_t read_req;
    //     uv_buf_t* buf = new uv_buf_t;
    //     if (buffer_pool_->get_buffer(65536, *buf))
    //     {
    //         offset += read_begin;
    //         if (offset + size > read_end)
    //             size = read_end > offset ? read_end - offset : 0;
    //         data = buf->base;
    //         size = uv_fs_read(nullptr, &read_req, fs_file, buf, 1, offset, nullptr);
    //         recycler = [=](void* ptr) {
    //             uv_buf_t* p_buf = (uv_buf_t*)ptr;
    //             buffer_pool_->recycle_buffer(*p_buf);
    //             delete p_buf;
    //         };
    //         return buf;
    //     }
    //     else
    //     {
    //         data = nullptr;
    //         size = 0;
    //         return (uv_buf_t*)nullptr;
    //     }
    // };

    res.status_code = 200;
    res.status_msg = "ok";
    return true;
}

void server::on_connection(uv_stream_t* socket)
{
    uv_tcp_t* tcp = new uv_tcp_t;
    uv_tcp_init(loop_, tcp);
    if (uv_accept(socket, (uv_stream_t*)tcp) == 0)
    {
        _responser* responser = new _responser(buffer_pool_, router_map_, router_list_);
        responser->buffer_pool_ = buffer_pool_;
        responser->loop_ = loop_;
        responser->socket_ = (uv_stream_t*)tcp;
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