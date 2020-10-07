#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "worker.h"

namespace http
{

struct work_req : public uv_work_t
{
    std::function<int()> work;
    std::function<void(int)> done;
    int result;
};

worker::worker(uv_loop_t* loop)
{
    loop_ = loop != nullptr ? loop : uv_default_loop();
}

bool worker::queue(std::function<int()>&& work, std::function<void(int)>&& done)
{
    work_req* req = new work_req{};
    req->work = std::move(work);
    req->done = std::move(done);
    uv_handle_set_data((uv_handle_t*)req, req);
    return uv_queue_work(loop_, req, worker_cb, after_worker_cb) == 0;
}

void worker::worker_cb(uv_work_t* req)
{
    work_req* p_req = (work_req*)req;
    if (p_req->work)
        p_req->result = p_req->work();
    p_req->work = nullptr;
}

void worker::after_worker_cb(uv_work_t* req, int status)
{
    work_req* p_req = (work_req*)req;
    if (p_req->done)
        p_req->done(p_req->result);
    p_req->done = nullptr;
    delete p_req;
}

} // namespace http