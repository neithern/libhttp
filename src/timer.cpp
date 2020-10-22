#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "timer.h"

namespace http
{

timer::timer(std::function<void()>&& callback, uv_loop_t* loop)
{
    callback_ = std::move(callback);
    started_ = false;
    timer_ = (uv_timer_t*)calloc(sizeof(uv_timer_t), 1);
    uv_handle_set_data((uv_handle_t*)timer_, this);

    if (loop == nullptr)
        loop = uv_default_loop();
    uv_timer_init(loop, timer_);
}

timer::~timer()
{
    if (started_)
        uv_timer_stop(timer_);

    if (timer_ != nullptr)
    {
        uv_handle_set_data((uv_handle_t*)timer_, nullptr);
        uv_close((uv_handle_t*)timer_, [](uv_handle_t* handle) {
            free(handle);
        });
    }
}

bool timer::start(uint64_t timeout, uint64_t repeat)
{
    if (!started_)
        started_ = uv_timer_start(timer_, timer_cb, timeout, repeat) == 0;
    return started_;
}

bool timer::stop()
{
    if (started_ && uv_timer_stop(timer_) == 0)
        started_ = false;
    return !started_;
}

void timer::timer_cb(uv_timer_t* handle)
{
    timer* p_this = (timer*)uv_handle_get_data((uv_handle_t*)handle);
    if (p_this->callback_)
        p_this->callback_();
}

} // namespace http