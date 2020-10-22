#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "loop.h"

namespace http
{

loop::loop(bool use_default)
{
    loop_ = use_default ? uv_default_loop() : uv_loop_new();
    loop_thread_ = (void*)uv_thread_self();
}

loop::~loop()
{
    if (loop_ != nullptr && loop_ != uv_default_loop())
        uv_loop_delete(loop_);
}

int loop::async(std::function<void()>&& work)
{
    auto data = new async_data{};
    data->work = std::move(work);

    uv_async_t* async = (uv_async_t*)calloc(sizeof(uv_async_t), 1);
    uv_async_init(loop_, async, on_async_cb);
    uv_handle_set_data((uv_handle_t*)async, data);
    int r = uv_async_send(async);
    if (r == 0)
        return 0;

    delete data;
    uv_close((uv_handle_t*)async, on_closed_and_free_cb);
    return r;
}

int loop::run_loop(bool once, bool nowait)
{
    loop_thread_ = (void*)uv_thread_self();
    uv_run_mode mode = UV_RUN_DEFAULT;
    if (once)
        mode = nowait ? UV_RUN_NOWAIT : UV_RUN_ONCE;
    return uv_run(loop_, mode);
}

void loop::stop_loop()
{
    uv_stop(loop_);
}

void loop::on_async_cb(uv_async_t* handle)
{
    async_data* data = (async_data*)uv_handle_get_data((uv_handle_t*)handle);
    if (data->work)
    {
        data->work();
        data->work = nullptr;
    }
    delete data;
    uv_close((uv_handle_t*)handle, on_closed_and_free_cb);
}

void loop::on_closed_and_free_cb(uv_handle_t* handle)
{
    free(handle);
}

} // namespace http