#ifndef _http_parser_h_
#define _http_parser_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include "buffer_pool.h"
#include "common.h"

namespace http
{

#define UV_E_USER_CANCELED  (UV_ERRNO_MAX - 1)
#define UV_E_HTTP_HEADERS   (UV_ERRNO_MAX - 2)
#define UV_E_HTTP_CHUNKED   (UV_ERRNO_MAX - 3)

class parser
{
public:
    parser(bool request_mode, std::shared_ptr<buffer_pool> buffer_pool);
    virtual ~parser();

    int start_read(uv_stream_t* socket);

protected:
    inline bool is_read_done() { return content_received_ >= content_to_receive_; }
    void reset_status();
    void set_read_done() { content_to_receive_ = 0; }

    int on_content_read(const char* data, size_t size);
    int on_socket_read(ssize_t nread, const uv_buf_t* buf);

    virtual request* on_get_request() = 0;
    virtual response* on_get_response() = 0;
    virtual bool on_headers_parsed(std::optional<int64_t> content_length) = 0;
    virtual bool on_content_received(const char* data, size_t size) = 0;
    virtual void on_read_done(int error_code) = 0;

    static void on_alloc_cb(uv_handle_t* handle, size_t size, uv_buf_t* buf);
    static void on_closed_and_delete_cb(uv_handle_t* handle);
    static void on_read_cb(uv_stream_t* socket, ssize_t nread, const uv_buf_t* buf);

    static int parse_request(const char* data, size_t size, size_t last_size, request& req);
    static int parse_response(const char* data, size_t size, size_t last_size, response& res);

protected:
    std::shared_ptr<buffer_pool> buffer_pool_;

private:
    bool request_mode_;
    int64_t content_received_ = 0;
    int64_t content_to_receive_ = 0;
    bool headers_parsed_ = false;
    std::string received_cache_;
    class chunked_decoder* chunked_decoder_ = nullptr;
};

} // namespace http

#endif // _http_parser_h_