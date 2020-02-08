#ifndef _chunked_decoder_h_
#define _chunked_decoder_h_

#include <functional>

namespace http
{

class chunked_decoder
{
    enum _state
    {
        STATE_CHUNK_SIZE,
        STATE_CHUNK_EXT,
        STATE_CHUNK_DATA,
        STATE_CHUNK_CRLF,
        STATE_TRAILERS_LINE_HEAD,
        STATE_TRAILERS_LINE_MIDDLE
    };

public:
    int decode(const char* data, size_t size, std::function<bool(const char* data, size_t size)> sink);

private:
    size_t bytes_in_chunk_ = 0; // number of bytes left in current chunk
    int hex_count_ = 0;
    _state state_ = STATE_CHUNK_SIZE;
};

} // namespace http

#endif // _chunked_decoder_h_