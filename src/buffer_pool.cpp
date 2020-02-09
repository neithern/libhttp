#include <stdio.h>
#include <stdlib.h>
#include "uv.h"
#include "buffer_pool.h"

namespace http
{

#define _ENABLE_CACHE_

struct buffer
{
    size_t size;
    struct buffer* next;
};

buffer_pool::buffer_pool(size_t max_size)
{
    max_size_ = max_size;
    alloc_count_ = 0;
    hit_count_ = 0;
    header_ = nullptr;
    tailer_ = nullptr;
}

buffer_pool::~buffer_pool()
{
    clear();
#ifdef _ENABLE_CACHE_
    printf("buffer pool status: alloc %zu, hit %zu\n", alloc_count_, hit_count_);
#endif
}

bool buffer_pool::get_buffer(size_t size, uv_buf_t& buf)
{
#ifdef _ENABLE_CACHE_
    buffer* p_buf;
    if (header_ != nullptr)
    {
        p_buf = header_;
        header_ = header_->next;
        if (header_ == nullptr)
            tailer_ = nullptr;
        hit_count_++;
    }
    else
    {
        p_buf = alloc_buffer(size);
        if (p_buf == nullptr)
            return false;
        alloc_count_++;
    }

    buf.base = (char*)p_buf + sizeof(buffer);
    buf.len = p_buf->size > size ? p_buf->size : size;
    p_buf->next = nullptr;
#else
    buf.base = (char*)malloc(size);
    buf.len = size;
#endif
    return true;
}

void buffer_pool::recycle_buffer(uv_buf_t& buf)
{
    if (buf.base == nullptr)
        return;

#ifdef _ENABLE_CACHE_
    buffer* p_buf = (buffer*)(buf.base - sizeof(buffer));
    if (tailer_ == nullptr)
    {
        header_ = tailer_ = p_buf;
    }
    else
    {
        tailer_->next = p_buf;
        tailer_ = p_buf;
    }
#else
    free(buf.base);
#endif
    buf.base = nullptr;
    buf.len = 0;
}

void buffer_pool::clear()
{
    while (header_ != nullptr)
    {
        buffer* next = header_->next;
        free(next);
        header_ = next;
    }
    tailer_ = nullptr;
}

buffer* buffer_pool::alloc_buffer(size_t size)
{
    if (size < max_size_)
        size = max_size_;

    buffer* buf = (buffer*)malloc(sizeof(buffer) + size);
    if (buf == nullptr)
        return nullptr;

    buf->size = size;
    buf->next = nullptr;
    return buf;
}

void buffer_pool::free_buffer(buffer* buf)
{
    free(buf);
}

} // namespace http