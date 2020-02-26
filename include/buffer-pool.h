#ifndef _buffer_pool_h_
#define _buffer_pool_h_

#include <stdlib.h>

namespace http
{

class buffer_pool
{
public:
    static constexpr size_t buffer_size = 64 * 1024;

    buffer_pool(size_t min_size = buffer_size);
    ~buffer_pool();

    void* get_buffer(size_t size);
    void recycle_buffer(void* ptr);

    bool get_buffer(size_t size, struct uv_buf_t& buf);
    void recycle_buffer(struct uv_buf_t& buf);

    void clear();

protected:
    struct buffer* alloc_buffer(size_t size);
    void free_buffer(struct buffer* buf);

private:
    size_t min_size_;
    size_t alloc_count_;
    size_t hit_count_;
    struct buffer* header_;
    struct buffer* tailer_;
};

} // namespace http

#endif // _buffer_pool_h_