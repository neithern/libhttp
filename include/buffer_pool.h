#ifndef _buffer_pool_h_
#define _buffer_pool_h_

#include <stdlib.h>

namespace http
{

class buffer_pool
{
public:
    buffer_pool(size_t max_size = 64 * 1024);

    ~buffer_pool();

    bool get_buffer(size_t size, struct uv_buf_t& buf);

    void recycle_buffer(struct uv_buf_t& buf);

    void clear();

protected:
    struct buffer* alloc_buffer(size_t size);

    void free_buffer(struct buffer* buf);

private:
    size_t max_size_;
    size_t alloc_count_;
    size_t hit_count_;
    struct buffer* header_;
    struct buffer* tailer_;
};

} // namespace http

#endif // _buffer_pool_h_