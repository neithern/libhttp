#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "buffer-pool.h"
#include "client.h"
#include "content-writer.h"
#include "file-reader.h"
#include "parser.h"
#include "reference-count.h"
#include "trace.h"
#include "uri.h"
#include "utils.h"

namespace http
{

static int _requester_count_ = 0;

class _requester : public parser, public content_writer
{
    define_reference_count(_requester)

    friend class client;

protected:
    uv_loop_t* loop_;

    // for request
    uri uri_;
    request request_;

    // for response
    response response_;

    // for callbacks
    on_response on_response_;
    on_redirect on_redirect_;
    on_content on_content_;
    on_error on_error_ = nullptr;

    bool keep_alive_ = false;
    bool redirecting_ = false;

    _requester(uv_loop_t* loop, std::shared_ptr<buffer_pool> buffer_pool) :
        parser(false, buffer_pool),
        content_writer(loop)
    {
        _requester_count_++;
    }

    virtual ~_requester()
    {
        close_socket();

        trace("%d living requesters\n", --_requester_count_);
    }

    void close_socket()
    {
        uv_stream_t* tcp = socket_;
        socket_ = nullptr;
        if (tcp != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)tcp, nullptr);
            uv_close((uv_handle_t*)tcp, on_closed_and_delete_cb);
            // trace("%p:%p socket closed\n", this, tcp);
        }
    }

    int resolve()
    {
        addrinfo hints = {};
        hints.ai_family = PF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = 0;

        uv_getaddrinfo_t* req = new uv_getaddrinfo_t{};
        uv_req_set_data((uv_req_t*)req, this);
        int r = uv_getaddrinfo(loop_, req, on_resolved_cb, uri_.host.c_str(), uri_.port.c_str(), &hints);
        if (r != 0)
            on_end(r);
        return r;
    }

    int on_connected()
    {
        string_map& headers = request_.headers;

        auto pstr = std::make_shared<std::string>();
        std::string& str = *pstr.get();
        str.reserve(buffer_pool::buffer_size);

        str.append(request_.method);
        str.append(" ");
        str.append(uri::encode(uri_.path));
        str.append(" HTTP/1.1\r\nHost: ", 17);
        str.append(uri_.host);
        str.append("\r\n", 2);

        if (!headers.count(HEADER_USER_AGENT))
            headers[HEADER_USER_AGENT] = LIBHTTP_TAG;
        if (!headers.count(HEADER_ACCEPT_ENCODING))
            headers[HEADER_ACCEPT_ENCODING] = "identity";

        for (auto& p : headers)
        {
            str.append(p.first);
            str.append(": ", 2);
            str.append(p.second);
            str.append("\r\n", 2);
        }
        str.append("\r\n", 2);
        return content_writer::start_write(pstr, request_.provider);
    }

    virtual request_base* on_get_request()
    {
        assert(!"should not be called!");
        return &request_;
    }

    virtual response* on_get_response()
    {
        return &response_;
    }

    virtual bool on_headers_parsed(std::optional<int64_t> content_length)
    {
        auto end = response_.headers.cend();
        auto p = end;

        if (response_.is_redirect()
            && on_redirect_
            && (p = response_.headers.find(HEADER_LOCATION)) != end)
        {
            std::string host = uri_.host;
            std::string location = p->second;
            if (on_redirect_ && on_redirect_(location) && uri_.parse(location))
            {
                uv_async_t* async = new uv_async_t{};
                uv_async_init(loop_, async, on_redirect_cb);
                uv_handle_set_data((uv_handle_t*)async, this);
                int r = uv_async_send(async);
                redirecting_ = r == 0;
                if (redirecting_)
                    keep_alive_ = case_equals(host, uri_.host)
                                && (p = response_.headers.find(HEADER_CONNECTION)) != end
                                    && case_equals(p->second, "Keep-Alive");  
                else
                    delete async;
                set_read_done();
                return true;
            }
        }

        response_.content_length = content_length;
        return on_response_(response_);
    }

    virtual bool on_content_received(const char* data, size_t size)
    {
        if (on_content_)
            return on_content_(data, size, is_read_done());

        set_read_done();
        return false;
    }

    virtual void on_read_end(int error_code)
    {
        if (error_code < 0 && socket_ != nullptr)
            uv_read_stop(socket_);

        if (!redirecting_)
            on_end(error_code);
    }

    int on_resolved(addrinfo* res)
    {
        uv_tcp_t* socket = new uv_tcp_t{};
        uv_tcp_init(loop_, socket);
        uv_handle_set_data((uv_handle_t*)socket, this);

        uv_connect_t* req = new uv_connect_t{};
        uv_req_set_data((uv_req_t*)req, this);
        int r = uv_tcp_connect(req, socket, res->ai_addr, on_connected_cb);
        if (r == 0)
            socket_ = (uv_stream_t*)socket;
        else
            uv_close((uv_handle_t*)socket, on_closed_and_delete_cb);
        return r;
    }

    virtual void on_write_end(int error_code)
    {
        if (error_code == 0)
            start_read(socket_);
        else if (error_code < 0)
            on_end(error_code);
    }

    void on_end(int error_code)
    {
        if (error_code < 0/* && error_code != UV_E_USER_CANCELLED*/)
        {
            trace("%p:%p end: %s, %s, %d\n", this, socket_, uv_err_name(error_code), request_.url.c_str(), ref_count_);
            if (on_error_)
                on_error_(error_code);
        }
        release();
    }

private:
    static void on_connected_cb(uv_connect_t* req, int status)
    {
        _requester* p_this = (_requester*)uv_req_get_data((uv_req_t*)req);
        delete req;

        if (status == 0)
            status = p_this->on_connected();
        if (status < 0)
            p_this->on_end(status);
    }

    static void on_redirect_cb(uv_async_t* handle)
    {
        _requester* p_this = (_requester*)uv_handle_get_data((uv_handle_t*)handle);
        uv_close((uv_handle_t*)handle, on_closed_and_delete_cb);

        p_this->redirecting_ = false;

        int status;
        if (p_this->keep_alive_)
        {
            p_this->keep_alive_ = false;
            status = p_this->on_connected();
        }
        else
        {
            p_this->close_socket();
            status = p_this->resolve();
        }
        if (status < 0)
            p_this->on_end(status);
    }

    static void on_resolved_cb(uv_getaddrinfo_t* req, int status, addrinfo* res)
    {
        _requester* p_this = (_requester*)uv_req_get_data((uv_req_t*)req);
        delete req;

        if (status == 0)
            status = p_this->on_resolved(res);
        if (status < 0)
            p_this->on_end(status);
    }
};

class file_puller
{
    define_reference_count(file_puller)

    friend class client;

protected:
    uv_loop_t* loop_;
    uv_async_t async_;
    uint64_t length_;
    uint64_t offset_;

    std::shared_ptr<file_reader> reader_;
    on_content on_content_;
    on_error on_error_ = nullptr;

public:
    file_puller(uv_loop_t* loop, std::shared_ptr<file_reader> reader, uint64_t length)
    {
        loop_ = loop;
        reader_ = reader;
        length_ = length;
        offset_ = 0;

        uv_async_init(loop_, &async_, on_async_cb);
        uv_handle_set_data((uv_handle_t*)&async_, this);
    }

    ~file_puller()
    {
        uv_handle_set_data((uv_handle_t*)&async_, nullptr);
        uv_close((uv_handle_t*)&async_, nullptr);
    }

    void start()
    {
        on_async();
    }

protected:
    void on_async()
    {
        int r = reader_->request_chunk(offset_, 64 * 1024, sink_);
        if (r < 0 && r != UV_EAGAIN)
            on_end(r);
    }

    void on_end(int error_code)
    {
        if (error_code < 0 && on_error_)
            on_error_(error_code);
        release();
    }

    content_sink sink_ = [this](const char* data, size_t size, content_done done)
    {
        if ((int)size > 0)
            offset_ += size;

        int r = (int)size;
        bool final_call = r < 0 || offset_ >= length_;
        if (!on_content_(data, size, final_call))
        {
            r = UV_E_USER_CANCELLED;
            final_call = true;
        }
        if (done)
            done();

        if (final_call)
            on_end(r >= 0 ? 0 : r);
        else
            uv_async_send(&async_);
    };

    static void on_async_cb(uv_async_t* handle)
    {
        file_puller* p_this = (file_puller*)uv_handle_get_data((uv_handle_t*)handle);
        if (p_this != nullptr)
            p_this->on_async();
    }
};

client::client(bool default_loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
    loop_ = default_loop ? uv_default_loop() : uv_loop_new();
    client_thread_ = uv_thread_self();
}

client::~client()
{
    if (loop_ != nullptr && loop_ != uv_default_loop())
    {
        uv_loop_close(loop_);
        uv_loop_delete(loop_);
    }
}

void client::fetch(const request& request,
                on_response on_response,
                on_content on_content,
                on_redirect on_redirect,
                on_error on_error)
{
    _requester* requester = new _requester(loop_, buffer_pool_);
    if (requester == nullptr)
    {
        if (on_error)
            on_error(UV_ENOMEM);
        return;
    }

    if (!requester->uri_.parse(request.url))
    {
        delete requester;
        if (on_error)
            on_error(UV_EINVAL);
        return;
    }

    requester->loop_ = loop_;
    requester->request_ = request;
    requester->on_response_ = std::move(on_response);
    requester->on_content_ = std::move(on_content);
    requester->on_redirect_ = std::move(on_redirect);
    requester->on_error_ = std::move(on_error);

    if (uv_thread_self() == client_thread_)
        requester->resolve();
    else
    {
        std::lock_guard lock(list_mutex_);
        requester_list_.push_back(requester);

        uv_async_t* async = new uv_async_t{};
        uv_async_init(loop_, async, on_async_cb);
        uv_handle_set_data((uv_handle_t*)async, this);
        int r = uv_async_send(async);
        if (r == 0)
            return;

        delete async;
        if (on_error)
            on_error(r);
    }
}

void client::fetch(const request& request,
                on_content_body on_body,
                on_response on_response,
                on_redirect on_redirect)
{
    std::string* p_body = new std::string();
    if (p_body == nullptr)
    {
        on_body("", UV_ENOMEM);
        return;
    }

    auto on_end = [=](int code) {
        on_body(*p_body, code);
        delete p_body;
    };

    fetch(request, on_response ? on_response :
        [=](const response& res) {
            p_body->reserve((size_t)res.content_length.value_or(4096));
            return res.is_ok();
        },
        [=](const char* data, size_t size, bool final) {
            p_body->append(data, size);
            if (final)
                on_end(0);
            return true;
        },
        on_redirect,
        on_end
    );
}

void client::pull(const std::string& path,
                on_content on_content,
                on_error on_error)
{
    uv_fs_t fs_req{};
    int r = uv_fs_stat(loop_, &fs_req, path.c_str(), nullptr);
    uint64_t length = fs_req.statbuf.st_size;
    uv_fs_req_cleanup(&fs_req);
    if (r != 0 || fs_req.result != 0 || !(fs_req.statbuf.st_mode & S_IFREG) || length == 0)
    {
        if (on_error)
            on_error(UV_EINVAL);
        return;
    }

    std::shared_ptr<file_reader> reader = std::make_shared<file_reader>(loop_, path, buffer_pool_);
    if (reader == nullptr)
    {
        if (on_error)
            on_error(UV_ENOMEM);
        return;
    }

    if (reader->get_fd() < 0)
    {
        if (on_error)
            on_error(UV_EACCES);
        return;
    }

    file_puller* puller = new file_puller(loop_, reader, length);
    if (puller == nullptr)
    {
        if (on_error)
            on_error(UV_ENOMEM);
        return;
    }

    puller->on_content_ = std::move(on_content);
    puller->on_error_ = std::move(on_error);
    puller->start();
}

int client::run_loop()
{
    client_thread_ = uv_thread_self();
    return uv_run(loop_, UV_RUN_DEFAULT);
}

void client::on_async()
{
    size_t count = 0;
    do
    {
        _requester* requester = nullptr;
        {
            std::lock_guard lock(list_mutex_);
            count = requester_list_.size();
            if (count > 0)
            {
                requester = requester_list_.front();
                requester_list_.pop_front();
            }
        }
        if (requester != nullptr)
            requester->resolve();
    } while (count > 0);
}

void client::on_async_cb(uv_async_t* handle)
{
    client* p_this = (client*)uv_handle_get_data((uv_handle_t*)handle);
    uv_close((uv_handle_t*)handle, parser::on_closed_and_delete_cb);
    p_this->on_async();
}

} // namespace http