#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "buffer-pool.h"
#include "client.h"
#include "parser.h"
#include "reference-count.h"
#include "uri.h"
#include "utils.h"

namespace http
{

static const string_case_equals _case_equals;

static int _requester_count_ = 0;

class _requester : public parser
{
    define_reference_count(_requester)

    friend class client;

    struct _write_req : public uv_write_t
    {
        std::string data;
    };

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
    uv_stream_t* socket_ = nullptr;

    _requester(std::shared_ptr<buffer_pool> buffer_pool)
        : parser(false, buffer_pool)
    {
        _requester_count_++;
    }

    virtual ~_requester()
    {
        close_socket();

        printf("alive %d responsers\n", --_requester_count_);
    }

    void close_socket()
    {
        uv_stream_t* tcp = socket_;
        socket_ = nullptr;
        if (tcp != nullptr)
        {
            uv_handle_set_data((uv_handle_t*)tcp, nullptr);
            uv_close((uv_handle_t*)tcp, on_closed_and_delete_cb);
            // printf("%p:%p socket closed\n", this, tcp);
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
            on_end(r, false);
        return r;
    }

    int on_connected()
    {
        headers& headers = request_.headers;
        size_t content_length = request_.body.size();

        std::string request;
        request.reserve(content_length + 4096);

        request.append(request_.method);
        request.append(" ");
        request.append(uri::encode(uri_.path));
        request.append(" HTTP/1.1\r\nHost: ", 17);
        request.append(uri_.host);
        request.append("\r\n", 2);

        if (content_length != 0)
        {
            char sz[64] = {};
            ::snprintf(sz, sizeof(sz), "%zu", content_length);
            headers[HEADER_CONTENT_LENGTH] = sz;
        }
        if (!request_.has_header(HEADER_USER_AGENT))
            headers[HEADER_USER_AGENT] = "libhttp";
        if (!request_.has_header(HEADER_ACCEPT_ENCODING))
            headers[HEADER_ACCEPT_ENCODING] = "identity";

        for (auto it = headers.cbegin(); it != headers.cend(); it++)
        {
            request.append(it->first);
            request.append(": ", 2);
            request.append(it->second);
            request.append("\r\n", 2);
        }
        request.append("\r\n", 2);

        if (content_length != 0)
            request.append(request_.body);

        _write_req* req = new _write_req{};
        req->data = request; // to keep reference of the request
        uv_req_set_data((uv_req_t*)req, this);

        uv_buf_t buf = uv_buf_init(const_cast<char*>(request.c_str()), request.size());
        int r = uv_write(req, socket_, &buf, 1, on_written_cb);
        if (r != 0)
            delete req;
        return r;
    }

    virtual request* on_get_request()
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
                    keep_alive_ = _case_equals(host, uri_.host)
                                && (p = response_.headers.find(HEADER_CONNECTION)) != end
                                    && _case_equals(p->second, "Keep-Alive");  
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

    virtual void on_read_done(int error_code)
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

    int on_written()
    {
        return start_read(socket_);
    }

    void on_end(int error_code, bool release_this = true)
    {
        if (error_code < 0 && error_code != UV_E_USER_CANCELLED)
        {
            printf("%p:%p end: %s, %s, %d\n", this, socket_, uv_err_name(error_code), request_.url.c_str(), ref_count_);
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
        delete handle;

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

    static void on_written_cb(uv_write_t* req, int status)
    {
        _requester* p_this = (_requester*)uv_req_get_data((uv_req_t*)req);
        delete req;

        if (status == 0)
            status = p_this->on_written();
        if (status < 0)
            p_this->on_end(status);
    }
};

client::client(uv_loop_t* loop)
{
    buffer_pool_ = std::make_shared<buffer_pool>();
    loop_ = loop != nullptr ? loop : uv_default_loop();
}

bool client::fetch(const request& request,
                on_response on_response,
                on_content on_content,
                on_redirect on_redirect,
                on_error on_error)
{
    _requester* requester = new _requester(buffer_pool_);
    if (!requester->uri_.parse(request.url))
    {
        delete requester;
        return false;
    }

    requester->loop_ = loop_;
    requester->request_ = request;
    requester->on_response_ = std::move(on_response);
    requester->on_content_ = std::move(on_content);
    requester->on_redirect_ = std::move(on_redirect);
    requester->on_error_ = std::move(on_error);

    int ret = requester->resolve();
    if (ret != 0)
    {
        delete requester;
        return false;
    }
    return true;
}

bool client::fetch(const request& request,
                on_content_body on_body,
                on_response on_response,
                on_redirect on_redirect)
{
    std::string* body = new std::string();
    return fetch(request, on_response ? on_response :
        [=](const response& res) {
            body->reserve(res.content_length.value_or(4096));
            return res.is_ok();
        },
        [=](const char* data, size_t size, bool final) {
            body->append(data, size);
            if (final)
            {
                on_body(*body, 0);
                delete body;
            }
            return true;
        },
        on_redirect,
        [=](int code) {
            on_body(*body, code);
            delete body;
        }
    );
}

int client::run_loop()
{
    return uv_run(loop_, UV_RUN_DEFAULT);
}

} // namespace http