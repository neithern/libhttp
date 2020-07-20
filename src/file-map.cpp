#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include "file-map.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

namespace http
{

file_map::file_map(const std::string& path, size_t length, long modified_time)
{
    const char* psz = path.c_str();
    modified_time_ = modified_time;
    size_ = 0;
    ptr_ = nullptr;

#ifdef _WIN32
    int path_len = ::MultiByteToWideChar(CP_UTF8, 0, psz, -1, NULL, 0);
    WCHAR* pwsz = new WCHAR[path_len];
    ::MultiByteToWideChar(CP_UTF8, 0, psz, -1, pwsz, path_len);
    HANDLE h_file = ::CreateFileW(pwsz, FILE_GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h_file != INVALID_HANDLE_VALUE)
    {
        if (length == 0)
            length = ::GetFileSize(h_file, NULL);
        DWORD high = (DWORD)(length >> 32);
        DWORD low = (DWORD)length;
        HANDLE h_map = ::CreateFileMapping(h_file, NULL, PAGE_READONLY, high, low, NULL);
        if (h_map != NULL)
        {
            ptr_ = (char*)::MapViewOfFile(h_map, FILE_MAP_READ, 0, 0, length);
            ::CloseHandle(h_map);
        }
         ::CloseHandle(h_file);
    }
    delete[] pwsz;
#else
    int fd = ::open(psz, O_RDONLY);
    if (fd != -1)
    {
        if (length == 0)
        {
            struct stat st;
            if (::fstat(fd, &st) == 0)
                length = st.st_size;
        }
        ptr_ = (char*)::mmap(NULL, length, PROT_READ, MAP_PRIVATE, fd, 0);
        if (ptr_ == (char*)-1LL)
            ptr_ = nullptr;
        ::close(fd);
    }
#endif
    size_ = length;
}

file_map::~file_map()
{
#ifdef _WIN32
    if (ptr_ != NULL)
        ::UnmapViewOfFile(ptr_);
#else
    if (ptr_ != nullptr)
        ::munmap(ptr_, size_);
#endif
}

int file_map::read_chunk(int64_t offset, size_t size, content_sink sink)
{
    if (ptr_ != nullptr && offset < (int64_t)size_)
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