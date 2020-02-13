#include <stdlib.h>
#include <assert.h>
#include "chunked-decoder.h"
#include "utils.h"

namespace http
{

int chunked_decoder::decode(const char* data, size_t size, std::function<bool(const char* data, size_t size)> sink)
{
    size_t src = 0;
    int ret = 0;

    while (1)
    {
        switch (state_)
        {
        case STATE_CHUNK_SIZE:
            for (;; ++src)
            {
                int v;
                if (src == size)
                    goto Exit;
                if ((v = decode_hex(data[src])) == -1)
                {
                    if (hex_count_ == 0)
                    {
                        ret = -1;
                        goto Exit;
                    }
                    break;
                }
                if (hex_count_ == sizeof(size_t) * 2)
                    return -1;
                bytes_in_chunk_ = bytes_in_chunk_ * 16 + v;
                ++hex_count_;
            }
            hex_count_ = 0;
            state_ = STATE_CHUNK_EXT;
        /* fallthru */
        case STATE_CHUNK_EXT:
            /* RFC 7230 A.2 "Line folding in chunk extensions is disallowed" */
            for (;; ++src)
            {
                if (src == size)
                    goto Exit;
                if (data[src] == '\n')
                    break;
            }
            ++src;
            if (bytes_in_chunk_ == 0)
            {
                sink(data + src, 0); // done
                goto Complete;
            }
            state_ = STATE_CHUNK_DATA;
        /* fallthru */
        case STATE_CHUNK_DATA:
        {
            size_t avail = size - src;
            if (avail < bytes_in_chunk_)
            {
                if (avail > 0 && !sink(data + src, avail))
                    return -1;
                src += avail;
                bytes_in_chunk_ -= avail;
                goto Exit;
            }
            if (!sink(data + src, bytes_in_chunk_))
                return -1;
            src += bytes_in_chunk_;
            bytes_in_chunk_ = 0;
            state_ = STATE_CHUNK_CRLF;
        }
        /* fallthru */
        case STATE_CHUNK_CRLF:
            for (;; ++src)
            {
                if (src == size)
                    goto Exit;
                if (data[src] != '\r')
                    break;
            }
            if (data[src] != '\n')
            {
                ret = -1;
                goto Exit;
            }
            ++src;
            state_ = STATE_CHUNK_SIZE;
            break;
        case STATE_TRAILERS_LINE_HEAD:
            for (;; ++src)
            {
                if (src == size)
                    goto Exit;
                if (data[src] != '\r')
                    break;
            }
            if (data[src++] == '\n')
                goto Complete;
            state_ = STATE_TRAILERS_LINE_MIDDLE;
        /* fallthru */
        case STATE_TRAILERS_LINE_MIDDLE:
            for (;; ++src)
            {
                if (src == size)
                    goto Exit;
                if (data[src] == '\n')
                    break;
            }
            ++src;
            state_ = STATE_TRAILERS_LINE_HEAD;
            break;
        default:
            assert(!"decoder is corrupt");
        }
    }

Complete:
    ret = (int)(size - src);
Exit:
    return ret;
}

} // namespace http