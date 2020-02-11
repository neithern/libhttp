#ifndef _reference_count_h_
#define _reference_count_h_

namespace http
{

#define define_reference_count(T) \
    public: \
        inline T* aquire() { ref_count_++; return this; } \
        inline void release() { if (--ref_count_ == 0) delete this; } \
    private: \
        int ref_count_ = 1;

} // namespace http

#endif // _reference_count_h_