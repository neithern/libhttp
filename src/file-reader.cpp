#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "buffer-pool.h"
#include "file-reader.h"

namespace http
{

file_reader::file_reader(uv_loop_t* loop, const std::string& path, std::shared_ptr<buffer_pool> buffer_pool)
{
    loop_ = loop;
    buffer_pool_ = buffer_pool;

    read_req_ = new read_req{};
    if (read_req_ != nullptr)
        uv_req_set_data((uv_req_t*)read_req_, this);

    uv_fs_t open_req{};
    fd_ = uv_fs_open(loop_, &open_req, path.c_str(), UV_FS_O_RDONLY | UV_FS_O_SEQUENTIAL, 0, nullptr);
    uv_fs_req_cleanup(&open_req);
}

file_reader::~file_reader()
{
    if (reading_)
    {
        if (uv_cancel((uv_req_t*)read_req_) == 0)
        {
            buffer_pool_->recycle_buffer(read_req_->buf);
            delete read_req_;
        }
        else
            uv_req_set_data((uv_req_t*)&read_req_, nullptr);
    }
    else
    {
        buffer_pool_->recycle_buffer(read_req_->buf);
        delete read_req_;
    }

    uv_fs_t* close_req = new uv_fs_t{};
    uv_fs_close(loop_, close_req, fd_, [](uv_fs_t* req) {
        uv_fs_req_cleanup(req);
        delete req;
    });
}

int file_reader::request_chunk(int64_t offset, size_t size, content_sink sink)
{
    sink_ = std::move(sink);

    if (reading_)
        return UV_EAGAIN;

    if (read_req_ == nullptr || !buffer_pool_->get_buffer(size, read_req_->buf))
        return UV_ENOMEM;

    reading_ = true;
    int r = uv_fs_read(loop_, (uv_fs_t*)read_req_, fd_, &read_req_->buf, 1, offset, on_read_cb);
    if (r < 0)
    {
        buffer_pool_->recycle_buffer(read_req_->buf);
        reading_ = false;
    }
    return r;
}

void file_reader::on_read()
{
    uv_buf_t buf = read_req_->buf;
    ssize_t len = uv_fs_get_result(read_req_);
    read_req_->buf.base = nullptr;
    read_req_->buf.len = 0;

    reading_ = false;

    auto done = [pool = buffer_pool_, buf]() {
        pool->recycle_buffer((uv_buf_t&)buf);
    };

    if (sink_)
        sink_(buf.base, len, done);
    else
        done();
}

void file_reader::on_read_cb(uv_fs_t* req)
{
    file_reader* p_this = (file_reader*)uv_req_get_data((uv_req_t*)req);
    if (p_this != nullptr)
        p_this->on_read();
    else
        buffer_pool::free_buffer(((read_req*)req)->buf.base);
}

} // namespace http