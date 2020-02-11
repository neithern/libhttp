#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "file-cache.h"

namespace http
{

file_cache::file_cache(uv_loop_t* loop, int fd, const uv_stat_t& stat)
{
    fd_ = fd;
    loop_ = loop;
    time_ = stat.st_mtim;

    chunk_count_ = (stat.st_size + _chunk_size - 1) / _chunk_size;
    chunks_ = (chunk**)calloc(chunk_count_, sizeof(chunk*));

    chunk* p_first = chunks_[0] = new chunk(0, chunk_count_ == 1 ? stat.st_size : _chunk_size);
    request_chunk(p_first); // request first chunk
}

file_cache::~file_cache()
{
    for (int i = chunk_count_ - 1; i >= 0; i--)
    {
        auto p_chunk = chunks_[i];
        if (p_chunk != nullptr)
        {
            if (p_chunk->reading_)
            {
                p_chunk->reading_ = false;
                uv_cancel((uv_req_t*)p_chunk);
            }
            p_chunk->release();
        }
    }
    free(chunks_);

    uv_fs_t* close_req = new uv_fs_t;
    uv_fs_close(loop_, close_req, fd_, [](uv_fs_t* req) {
        uv_fs_req_cleanup(req);
        delete req;
    });
}

chunk* file_cache::get_chunk(int64_t offset)
{
    size_t index = offset / _chunk_size;
    if (index >= chunk_count_)
        return nullptr;

    offset = index * _chunk_size; // round to _chunk_size

    chunk*& p_chunk = chunks_[index];
    if (p_chunk == nullptr)
    {
        p_chunk = new chunk(offset);
        if (p_chunk == nullptr)
            return nullptr;
        request_chunk(p_chunk);
    }
    else if (p_chunk->size() == 0 && !p_chunk->reading_)
    {
        request_chunk(p_chunk);
    }
    else if (index < chunk_count_ - 1)
    {
        chunk* p_next = get_chunk(offset + _chunk_size); // request next chunk
        if (p_next != nullptr)
            p_next->release();
    }
    return p_chunk->aquire();
}

void file_cache::request_chunk(chunk* p_chunk)
{
    p_chunk->reading_ = true;
    uv_fs_read(loop_, p_chunk, fd_, &p_chunk->buffer_, 1, p_chunk->offset_, on_read_cb);
}

void file_cache::on_read_cb(uv_fs_t* req)
{
    uv_fs_req_cleanup(req);
    chunk* p_chunk = (chunk*)req;
    p_chunk->size_ = uv_fs_get_result(req);
    p_chunk->reading_ = false;
}

} // namespace http