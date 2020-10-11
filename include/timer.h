#ifndef _http_timer_h_
#define _http_timer_h_

#include <functional>

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_timer_s uv_timer_t;

namespace http
{

class timer
{
public:
    timer(std::function<void()>&& callback, uv_loop_t* loop = nullptr);
    ~timer();

    inline bool is_started() const { return started_; }

    bool start(uint64_t timeout, uint64_t repeat = 0);
    bool stop();

private:
    static void timer_cb(uv_timer_t* handle);

private:
    std::function<void()> callback_;
    uv_timer_t* timer_;
    bool started_;
};

}

#endif // _file_map_h_