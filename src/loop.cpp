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
    work_async_ = nullptr;
}

loop::~loop()
{
    if (work_async_ != nullptr)
    {
        uv_handle_set_data((uv_handle_t*)work_async_, nullptr);
        uv_close((uv_handle_t*)work_async_, on_closed_and_free_cb);
    }
    if (loop_ != nullptr && loop_ != uv_default_loop())
        uv_loop_delete(loop_);
}

int loop::async(std::function<void()>&& work)
{
    std::lock_guard<std::mutex> lock(work_mutex_);
    if (work_async_ == nullptr)
    {
        uv_async_t* async = (uv_async_t*)calloc(sizeof(uv_async_t), 1);
        int r = uv_async_init(loop_, async, on_async_cb);
        if (r != 0)
        {
            free(async);
            return r;
        }
        work_async_ = async;
        uv_handle_set_data((uv_handle_t*)work_async_, this);
    }
    work_list_.push_back(std::move(work));
    return uv_async_send(work_async_);
}

struct work_req_data
{
    std::function<intptr_t()> work;
    std::function<void(intptr_t)> done;
    intptr_t result;
};

static void worker_cb(uv_work_t* req)
{
    auto p_data = (work_req_data*)uv_req_get_data((uv_req_t*)req);
    if (p_data->work)
    {
        p_data->result = p_data->work();
        p_data->work = nullptr;
    }
}

static void after_worker_cb(uv_work_t* req, int status)
{
    auto p_data = (work_req_data*)uv_req_get_data((uv_req_t*)req);
    if (p_data->done)
    {
        p_data->done(p_data->result);
        p_data->done = nullptr;
    }
    delete p_data;
    free(req);
}

bool loop::queue_work(std::function<intptr_t()>&& work, std::function<void(intptr_t)>&& done)
{
    uv_work_t* req = (uv_work_t*)calloc(sizeof(uv_work_t), 1);
    auto p_data = new work_req_data{};
    p_data->work = std::move(work);
    p_data->done = std::move(done);
    uv_req_set_data((uv_req_t*)req, p_data);

    if ((void*)uv_thread_self() == loop_thread_)
        return uv_queue_work(loop_, req, worker_cb, after_worker_cb) == 0;

    int r = async([=]() {
        uv_queue_work(loop_, req, worker_cb, after_worker_cb) == 0;
    });
    if (r != 0)
    {
        delete p_data;
        free(req);
    }
    return r == 0;
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

void loop::on_async()
{
    size_t count = 0;
    do
    {
        std::function<void()> work = nullptr;
        {
            std::lock_guard<std::mutex> lock(work_mutex_);
            count = work_list_.size();
            if (count == 0)
                break;
            work = std::move(work_list_.front());
            work_list_.pop_front();
            count--;
        }
        if (work)
            work();
    } while (count > 0);
}

void loop::on_async_cb(uv_async_t* handle)
{
    loop* p_this = (loop*)uv_handle_get_data((uv_handle_t*)handle);
    if (p_this != nullptr)
        p_this->on_async();
}

void loop::on_closed_and_free_cb(uv_handle_t* handle)
{
    free(handle);
}

} // namespace http