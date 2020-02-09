#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "chunked_decoder.h"
#include "parser.h"
#include "pico/picohttpparser.h"

namespace http
{

parser::parser(bool request_mode, std::shared_ptr<buffer_pool> buffer_pool)
{
    request_mode_ = request_mode;
    buffer_pool_ = buffer_pool;
}

parser::~parser()
{
    if (chunked_decoder_ != nullptr)
        delete chunked_decoder_;
}

int parser::start_read(uv_stream_t* socket)
{
    assert(uv_handle_get_data((uv_handle_t*)socket) == this);
    reset_status();
    return uv_read_start(socket, on_alloc_cb, on_read_cb);
}

void parser::reset_status()
{
    content_received_ = 0;
    content_to_receive_ = 0;
    headers_parsed_ = false;
    received_cache_.clear();

    if (chunked_decoder_ != nullptr)
    {
        delete chunked_decoder_;
        chunked_decoder_ = nullptr;
    }
}

int parser::on_content_read(const char* data, size_t size)
{
    if (chunked_decoder_ != nullptr)
    {
        int r = chunked_decoder_->decode(data, size, [&](const char* data, size_t size) {
            if (size > 0)
                    content_received_ += size;
                else
                    set_read_done(); // complete
            return on_content_received(data, size);
        });
        if (r == -1)
            r = UV_E_HTTP_CHUNKED;
        return r;
    }

    content_received_ += size;
    return on_content_received(data, size) ? 0 : UV_E_USER_CANCELED;
}

int parser::on_socket_read(ssize_t nread, const uv_buf_t* buf)
{
    if (headers_parsed_)
        return on_content_read(buf->base, nread);

    size_t last_size = received_cache_.size();
    if (last_size > 0)
        received_cache_.append(buf->base, nread);

    const char* data = last_size > 0 ? received_cache_.c_str() : buf->base;
    size_t size = last_size + nread;

    int r = 0;
    headers* headers = nullptr;
    if (request_mode_)
    {
        request* req = on_get_request();
        req->body.clear();
        headers = &req->headers;
        headers->clear();
        r = parse_request(data, size, last_size, *req);
    }
    else
    {
        response* res = on_get_response();
        res->content_length.reset();
        headers = &res->headers;
        headers->clear();
        r = parse_response(data, size, last_size, *res);
    }
    headers_parsed_ = r > 0;

    if (headers_parsed_)
    {
        auto end = headers->cend();
        auto p = end;

        p = headers->find(HEADER_TRANSFER_ENCODING);
        if (chunked_decoder_ != nullptr) delete chunked_decoder_;
        chunked_decoder_ = p != end && string_case_equals().operator()(p->second, "chunked") ? new chunked_decoder : nullptr;

        std::optional<int64_t> content_length;
        p = headers->find(HEADER_CONTENT_LENGTH);
        content_length = p != end ? ::atoll(p->second.c_str()) : std::optional<int64_t>();
        content_to_receive_ = content_length.value_or(request_mode_ ? 0 : INT64_MAX);

        if (!on_headers_parsed(content_length))
            return UV_E_USER_CANCELED;

        if (request_mode_ && !content_length)
            set_read_done();

        if (r < size)
            return on_content_read(data + r, size - r);
    }
    else if (r == -2)
    {
        if (last_size == 0)
            received_cache_.append(buf->base, nread);
        return 0;
    }
    return r < 0 ? UV_E_HTTP_HEADERS : 0;
}

void parser::on_alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf)
{
    parser* p_this = (parser*)uv_handle_get_data((uv_handle_t*)handle);
    p_this->buffer_pool_->get_buffer(size, *buf);
}

void parser::on_closed_and_delete_cb(uv_handle_t* handle)
{
    delete handle;
}

void parser::on_read_cb(uv_stream_t* socket, ssize_t nread, const uv_buf_t* buf)
{
    parser* p_this = (parser*)uv_handle_get_data((uv_handle_t*)socket);
    if (p_this == nullptr)
        return;

    int r = nread;
    if (nread >= 0)
    {
        r = p_this->on_socket_read(nread, buf);
        if (r < 0 && r != UV_E_USER_CANCELED)
            printf("%p:%p read socket: %s\n", p_this, socket, uv_err_name(r));
    }
    else
        printf("%p:%p on_read_cb: %s\n", p_this, socket, uv_err_name(r));
    p_this->buffer_pool_->recycle_buffer(const_cast<uv_buf_t&>(*buf));

    if (r == UV_EOF && !p_this->is_read_done())
        p_this->set_read_done();

    bool done = p_this->is_read_done();
    if (r < 0 || done)
        p_this->on_read_done(r);
}

static int check_is_http(const char* data, size_t size, size_t last_size, int r)
{
    if (r == -2 && last_size == 0 && size > 0 && data[0] != 'H')
        r = -1;
    return r;
}

int parser::parse_request(const char* data, size_t size, size_t last_size, request& req)
{
    int minor_version = 0;
    const char* method = nullptr;
    const char* path = nullptr;
    size_t method_len = 0;
    size_t path_len = 0;
    size_t num_headers = 100;
    phr_header phr_headers[num_headers];

    int r = phr_parse_request(data, size, &method, &method_len, &path, &path_len, &minor_version,
                                phr_headers, &num_headers, last_size);
    if (r > 0)
    {
        if (method != nullptr)
            req.method = std::string(method, method_len);
        if (path != nullptr)
            req.url = std::string(path, path_len);

        for (size_t i = 0; i < num_headers; i++)
        {
            const phr_header& header = phr_headers[i];
            std::string name = std::string(header.name, header.name_len);
            std::string value = std::string(header.value, header.value_len);
            req.headers[name] = value;
        }
    }
    else if (r == -2)
    {
        r = check_is_http(data, size, last_size, r);
    }
    return r;
}

int parser::parse_response(const char* data, size_t size, size_t last_size, response& res)
{
    int minor_version = 0;
    const char* msg = nullptr;
    size_t msg_len = 0;
    size_t num_headers = 100;
    phr_header phr_headers[num_headers];

    int r = phr_parse_response(data, size, &minor_version, &res.status_code, &msg, &msg_len,
                                phr_headers, &num_headers, last_size);
    if (r > 0)
    {
        if (msg != nullptr)
            res.status_msg = std::string(msg, msg_len);

        for (size_t i = 0; i < num_headers; i++)
        {
            const phr_header& header = phr_headers[i];
            std::string name = std::string(header.name, header.name_len);
            std::string value = std::string(header.value, header.value_len);
            res.headers[name] = value;
        }
    }
    else if (r == -2)
    {
        r = check_is_http(data, size, last_size, r);
    }
    return r;
}

} // namespace http