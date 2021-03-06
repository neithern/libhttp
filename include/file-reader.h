#ifndef _file_reader_h_
#define _file_reader_h_

#include "common.h"

namespace http
{

class file_reader
{
    struct read_req : public uv_fs_t
    {
        uv_buf_t buf;
    };

public:
    file_reader(uv_loop_t* loop, const std::string& path, std::shared_ptr<buffer_pool> buffer_pool);
    ~file_reader();

    inline uv_file get_fd() const { return fd_; }

    // read only once every time call
    int request_chunk(int64_t offset, size_t size, content_sink sink);

protected:
    void on_read();

    static void on_read_cb(uv_fs_t* req);

private:
    uv_loop_t* loop_;
    uv_file fd_;
    bool reading_ = false;
    read_req* read_req_;
    content_sink sink_;
    std::shared_ptr<buffer_pool> buffer_pool_;
};

} // namespace http

#endif // _file_reader_h_