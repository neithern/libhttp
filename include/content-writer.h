#ifndef _content_writer_h_
#define _content_writer_h_

#include <list>
#include <memory>

namespace http
{

class content_writer
{
protected:
    struct write_req : public uv_write_t
    {
        uv_buf_t buf;
        content_done done_;

        write_req(const char* data = nullptr, size_t size = 0, content_done d = nullptr);
        ~write_req();
    };

public:
    content_writer(uv_loop_t* loop);
    virtual ~content_writer();

    int start_write(std::shared_ptr<std::string> headers, content_provider provider);

protected:
    virtual void on_write_end(int error_code) = 0;

    inline bool is_write_done() { return content_written_ >= content_to_write_; }
    inline void set_write_done() { content_to_write_ = 0; }

    void prepare_next();

    int write_content(std::shared_ptr<write_req> req);
    int write_next();

    static void on_written_cb(uv_write_t* req, int status);

protected:
    int64_t content_written_;
    int64_t content_to_write_;
    uv_stream_t* socket_;

private:
    uv_loop_t* loop_;

    content_sink content_sink_;
    content_provider content_provider_;

    bool headers_written_ = false;
    int last_socket_error_ = 0;
    std::shared_ptr<write_req> writing_req_;
    std::list<std::shared_ptr<write_req>> req_list_;
};

} // namespace http

#endif // _content_writer_h_