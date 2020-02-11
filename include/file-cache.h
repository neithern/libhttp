#ifndef _file_cache_h_
#define _file_cache_h_

#include "reference-count.h"

namespace http
{

static const size_t _chunk_size = 64 * 1024;

class chunk : public uv_fs_t
{
    define_reference_count(chunk)

    friend class file_cache;

public:
    chunk(int64_t offset, size_t capacity = _chunk_size)
    {
        buffer_.base = (char*)::malloc(capacity);
        buffer_.len = capacity;
        offset_ = offset;
    }

    ~chunk()
    {
        if (buffer_.base != nullptr)
            ::free(buffer_.base);
    }

    inline char* buffer() const { return buffer_.base; }
    inline size_t size() const { return size_; }
    inline int64_t offset() const { return offset_; }

protected:
    uv_buf_t buffer_;
    int64_t offset_;
    size_t size_ = 0;
    bool reading_ = false;
};

class file_cache
{
public:
    file_cache(uv_loop_t* loop, uv_file fd, const uv_stat_t& stat);
    ~file_cache();

    inline bool is_modified(const uv_timespec_t& mtime) const
    {
        return time_.tv_sec != mtime.tv_sec || time_.tv_nsec != mtime.tv_nsec;
    }

    chunk* get_chunk(int64_t offset);

protected:
    void request_chunk(chunk* p_chunk);

    static void on_read_cb(uv_fs_t* req);

private:
    uv_loop_t* loop_;
    uv_file fd_;
    uv_timespec_t time_;
    size_t chunk_count_;
    chunk** chunks_;
};

} // namespace http

#endif // _file_cache_h_