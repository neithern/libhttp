#ifndef _http_parser_h_
#define _http_parser_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include "buffer-pool.h"
#include "common.h"

namespace http
{

#define UV_E_USER_CANCELLED (UV_ERRNO_MAX - 1)
#define UV_E_HTTP_HEADERS   (UV_ERRNO_MAX - 2)
#define UV_E_HTTP_CHUNKED   (UV_ERRNO_MAX - 3)

class parser
{
protected:
    enum state
    {
        state_none,
        state_parsing,
        state_parsed,
        state_outputing
    };

public:
    parser(bool request_mode, std::shared_ptr<buffer_pool> buffer_pool);
    virtual ~parser();

    int start_read(uv_stream_t* socket);

    static void on_closed_and_free_cb(uv_handle_t* handle);

protected:
    inline bool is_read_done() { return content_received_ >= content_to_receive_; }
    void set_read_done() { content_to_receive_ = 0; }

    void reset_status();

    int on_content_read(const char* data, size_t size);
    int on_socket_read(ssize_t nread, const uv_buf_t* buf);

    virtual request_base* on_get_request() = 0;
    virtual response* on_get_response() = 0;
    virtual bool on_headers_parsed(std::optional<int64_t> content_length) = 0;
    virtual bool on_content_received(const char* data, size_t size) = 0;
    virtual void on_read_end(int error_code) = 0;

    static void on_alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf);
    static void on_read_cb(uv_stream_t* socket, ssize_t nread, const uv_buf_t* buf);

    static int parse_request(const char* data, size_t size, size_t last_size, request_base& req);
    static int parse_response(const char* data, size_t size, size_t last_size, response& res);

protected:
    bool request_mode_;
    state state_ = state_none;
    int64_t content_received_ = 0;
    int64_t content_to_receive_ = 0;
    std::string received_cache_;
    std::shared_ptr<buffer_pool> buffer_pool_;
    class chunked_decoder* chunked_decoder_ = nullptr;
    std::function<bool(const char* data, size_t size)> chunked_sink_;
};

} // namespace http

#endif // _http_parser_h_