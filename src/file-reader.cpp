#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "buffer-pool.h"
#include "file-reader.h"

namespace http
{

static const size_t _buffer_size = 64 * 1024;

file_reader::file_reader(uv_loop_t* loop, const std::string& path, std::shared_ptr<buffer_pool> buffer_pool)
{
    loop_ = loop;
    buffer_pool_ = buffer_pool;
    buffer_pool_->get_buffer(_buffer_size, buffer_);

    reading_ = false;
    read_req_ = new uv_fs_t{};
    uv_req_set_data((uv_req_t*)read_req_, this);

    int flags = UV_FS_O_RDONLY | UV_FS_O_SEQUENTIAL;
    uv_fs_t open_req{};
    fd_ = uv_fs_open(loop_, &open_req, path.c_str(), flags, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
}

file_reader::~file_reader()
{
    if (reading_)
        uv_cancel((uv_req_t*)read_req_);
    else
        delete read_req_;
    uv_req_set_data((uv_req_t*)&read_req_, nullptr);

    uv_fs_t* close_req = new uv_fs_t{};
    uv_fs_close(loop_, close_req, fd_, [](uv_fs_t* req) {
        uv_fs_req_cleanup(req);
        delete req;
    });
    buffer_pool_->recycle_buffer(buffer_);
}

int file_reader::request_chunk(int64_t offset, size_t size, content_sink sink)
{
    if (buffer_.base == nullptr)
        return UV_ENOMEM;

    sink_ = std::move(sink);

    if (reading_)
        return UV_EAGAIN;

    buffer_.len = size;
    reading_ = true;
    int r = uv_fs_read(loop_, read_req_, fd_, &buffer_, 1, offset, on_read_cb);
    if (r < 0)
        reading_ = false;
    return r;
}

void file_reader::on_read(uv_fs_t* req)
{
    reading_ = false;
    if (sink_)
        sink_(buffer_.base, uv_fs_get_result(req), nullptr);
}

void file_reader::on_read_cb(uv_fs_t* req)
{
    file_reader* p_this = (file_reader*)uv_req_get_data((uv_req_t*)req);
    if (p_this != nullptr)
        p_this->on_read(req);
    else
        delete req;
}

} // namespace http