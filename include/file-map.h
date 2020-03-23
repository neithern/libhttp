#ifndef _file_map_h_
#define _file_map_h_

#include <memory>
#include "common.h"

namespace http
{

class file_map : public std::enable_shared_from_this<file_map>
{
public:
    file_map(const std::string& path, size_t length, long modified_time);
    ~file_map();

    inline const char* ptr() const { return ptr_; }
    inline size_t size() const { return size_; }
    inline long modified_time() const { return modified_time_; }

    int read_chunk(int64_t offset, size_t size, content_sink sink);

private:
    char* ptr_;
    size_t size_;
    long modified_time_;
};

} // namespace http

#endif // _file_map_h_