#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "file-map.h"
#ifdef _WIN32
#else
#include <unistd.h>
#include <sys/mman.h>
#endif

namespace http
{

file_map::file_map(const std::string& path, size_t length, long modified_time)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd != -1)
    {
        size_ = length;
        ptr_ = (char*)::mmap(NULL, size_, PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr_ == (char*)-1LL)
            ptr_ = nullptr;
        ::close(fd);
    }
    else
    {
        ptr_ = nullptr;
        size_ = 0;
    }
    modified_time_ = modified_time;
}

file_map::~file_map()
{
    if (ptr_ != nullptr)
        ::munmap(ptr_, size_);
}

int file_map::read_chunk(int64_t offset, size_t size, content_sink sink)
{
    if (ptr_ != nullptr && offset < size_)
    {
        size_t max_size = size_ - offset;
        if (size > max_size)
            size = max_size;
        sink(ptr_ + offset, size, [p_this = shared_from_this()]() {});
        return 0;
    }
    else
    {
        return EOF;
    }
}

} // namespace http