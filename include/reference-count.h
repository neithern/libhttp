#ifndef _reference_count_h_
#define _reference_count_h_

#include <assert.h>

namespace http
{

#define define_reference_count(T) \
    public: \
        inline T* aquire() \
        { \
            ref_count_++; \
            return this; \
        } \
        inline void release() \
        { \
            assert(ref_count_ > 0); \
            if (--ref_count_ == 0) \
                delete this; \
        } \
    private: \
        int ref_count_ = 1;

} // namespace http

#endif // _reference_count_h_