#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "buffer-pool.h"
#include "client.h"
#include "content-writer.h"
#include "parser.h"
#include "reference-count.h"
#include "trace.h"
#include "uri.h"
#include "utils.h"

namespace http
{

static int _socket_checker_count_ = 0;

class _socket_checker
{
    define_reference_count(_socket_checker)

public:
    _socket_checker(uv_stream_t* socket, std::string key, std::shared_ptr<std::unordered_multimap<std::string, class _socket_checker*>> socket_cache)
    {
        socket_ = socket;
        uv_handle_set_data((uv_handle_t*)socket_, this);
        key_ = key;
        socket_cache_ = socket_cache;

        _socket_checker_count_++;
    }

    ~_socket_checker()
    {
        if (socket_ != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)socket_, nullptr);
            uv_close((uv_handle_t*)socket_, parser::on_closed_and_delete_cb);

            auto range = socket_cache_->equal_range(key_);
            for (auto it = range.first; it != range.second; it++)
            {
                if (it->second == this)
                {
                    socket_cache_->erase(it);
                    break;
                }
            }
        }

        _socket_checker_count_--;
        trace("%d living socket checkers\n", _socket_checker_count_);
    }

    int start()
    {
        return socket_ != nullptr ? uv_read_start(socket_, on_alloc_cb, on_read_cb) : -1;
    }

    uv_stream_t* stop()
    {
        uv_stream_t* socket = socket_;
        socket_ = nullptr;
        if (socket != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)socket, nullptr);
            socket_cache_->erase(key_);
        }
       return socket;
    }

protected:
    void on_read(ssize_t status)
    {
        release();
    }

    static void on_alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf)
    {
        buf->base = (char*)malloc(64);
        buf->len = buf->base != nullptr ? 64 : 0;
    }

    static void on_read_cb(uv_stream_t* socket, ssize_t nread, const uv_buf_t* buf)
    {
        free(buf->base);

        uv_read_stop(socket);
        _socket_checker* p_this = (_socket_checker*)uv_handle_get_data((uv_handle_t*)socket);
        if (p_this != NULL)
            p_this->on_read(nread);
    }

private:
    uv_stream_t* socket_;
    std::string key_;
    std::shared_ptr<std::unordered_multimap<std::string, class _socket_checker*>> socket_cache_;
};

static int _requester_count_ = 0;

class _requester : public parser, public content_writer
{
    define_reference_count(_requester)

    friend class client;

protected:
    // for request
    uri uri_;
    request request_;

    // for response
    response response_;

    // for callbacks
    on_response on_response_;
    on_redirect on_redirect_;
    on_content on_content_;
    on_error on_error_;

    bool keep_alive_ = false;
    bool redirecting_ = false;
    int last_error_ = 0;

    std::shared_ptr<std::unordered_multimap<std::string, _socket_checker*>> socket_cache_;

    _requester(uv_loop_t* loop, std::shared_ptr<buffer_pool> buffer_pool, std::shared_ptr<std::unordered_multimap<std::string, _socket_checker*>> socket_cache) :
        parser(false, buffer_pool),
        content_writer(loop),
        socket_cache_(socket_cache)
    {
        _requester_count_++;
    }

    ~_requester()
    {
        close_socket();

        _requester_count_--;
        trace("%d living requesters\n", _requester_count_);
    }

    void close_socket()
    {
        uv_stream_t* socket = socket_;
        bool keep_alive = keep_alive_;
        socket_ = nullptr;
        keep_alive_ = false;
        if (socket != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)socket, nullptr);
            if (keep_alive && last_error_ == 0)
            {
                std::string key = uri_.host + ':' + uri_.port;
                auto checker = new _socket_checker(socket, key, socket_cache_);
                if (checker->start() == 0)
                {
                    socket_cache_->emplace(key, checker);
                    return;
                }
                checker->release();
            }
            uv_close((uv_handle_t*)socket, on_closed_and_delete_cb);
            // trace("%p:%p socket closed\n", this, socket);
        }
    }

    int resolve()
    {
        std::string key = uri_.host + ':' + uri_.port;
        auto range = socket_cache_->equal_range(key);
        if (range.first != range.second)
        {
            _socket_checker* checker = range.first->second;
            socket_cache_->erase(range.first);
            if (checker != nullptr)
            {
                socket_ = checker->stop();
                checker->release();
                if (socket_ != nullptr)
                {
                    uv_handle_set_data((uv_handle_t*)socket_, this);
                    return on_connected();
                }
            }
        }

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
        if (!headers.count(HEADER_CONNECTION))
            headers[HEADER_CONNECTION] = "Keep-Alive";

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
                if (!redirecting_)
                    delete async;
                set_read_done();
                return true;
            }
        }

        p = response_.headers.find(HEADER_CONNECTION);
        keep_alive_ = p != end && case_equals(p->second, "Keep-Alive");

        response_.content_length = content_length;
        return on_response_ ? on_response_(response_) : true;
    }

    virtual bool on_content_received(const char* data, size_t size)
    {
        return on_content_ ? on_content_(data, size, is_read_done()) : true;
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
        last_error_ = error_code;
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
        p_this->close_socket();
        int status = p_this->resolve();
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

client::client(bool use_default) : loop(use_default)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
    socket_cache_ = std::make_shared<std::unordered_multimap<std::string, _socket_checker*>>();
}

client::~client()
{
    for (auto& p : (*socket_cache_))
        p.second->release();
    socket_cache_->clear();
}

void client::fetch(const request& request,
                on_response&& on_response,
                on_content&& on_content,
                on_redirect&& on_redirect,
                on_error&& on_error)
{
    _requester* requester = new _requester(loop_, buffer_pool_, socket_cache_);
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

    requester->request_ = request;
    requester->on_response_ = std::move(on_response);
    requester->on_content_ = std::move(on_content);
    requester->on_redirect_ = std::move(on_redirect);
    requester->on_error_ = std::move(on_error);

    if ((void*)uv_thread_self() == loop_thread_)
        requester->resolve();
    else
    {
        int r = async([=]() {
            requester->resolve();
        });
        if (r != 0)
        {
            delete requester;
            if (on_error)
                on_error(r);
        }
    }
}

void client::fetch(const request& request,
                on_content_body&& on_body,
                on_response&& on_response,
                on_redirect&& on_redirect)
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

    fetch(request, on_response ? std::move(on_response) :
        [=](const response& res) {
            auto length = res.content_length.value_or(4096);
            if (length > 0)
                p_body->reserve(length);
            return res.is_ok();
        },
        [=](const char* data, size_t size, bool end) {
            p_body->append(data, size);
            if (end)
                on_end(0);
            return true;
        },
        std::move(on_redirect),
        std::move(on_end)
    );
}

} // namespace http