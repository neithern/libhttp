#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "worker.h"

namespace http
{

struct work_req : public uv_work_t
{
    std::function<void()> work;
    std::function<void(int)> after_work;
};

worker::worker(uv_loop_t* loop)
{
    loop_ = loop != nullptr ? loop : uv_default_loop();
}

bool worker::queue(std::function<void()> work, std::function<void(int)> after_work)
{
    work_req* req = new work_req{};
    req->work = std::move(work);
    req->after_work = std::move(after_work);
    uv_handle_set_data((uv_handle_t*)req, req);
    return uv_queue_work(loop_, req, worker_cb, after_worker_cb) == 0;
}

void worker::worker_cb(uv_work_t* req)
{
    work_req* p_req = (work_req*)req;
    if (p_req->work)
        p_req->work();
}

void worker::after_worker_cb(uv_work_t* req, int status)
{
    work_req* p_req = (work_req*)req;
    if (p_req->after_work)
        p_req->after_work(status);
    delete p_req;
}

} // namespace http