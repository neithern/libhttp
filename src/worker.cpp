#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <mutex>
#include "worker.h"

namespace http
{

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

static std::mutex s_work_mutex;

bool queue_work(std::function<intptr_t()>&& work, std::function<void(intptr_t)>&& done, uv_loop_t* loop)
{
    if (loop == nullptr)
        loop = uv_default_loop();

    uv_work_t* req = (uv_work_t*)calloc(sizeof(uv_work_t), 1);
    auto p_data = new work_req_data{};
    p_data->work = std::move(work);
    p_data->done = std::move(done);
    uv_req_set_data((uv_req_t*)req, p_data);

    std::lock_guard<std::mutex> lock(s_work_mutex);
    return uv_queue_work(loop, req, worker_cb, after_worker_cb) == 0;
}

} // namespace http