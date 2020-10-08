#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "worker.h"

namespace http
{

struct work_req : public uv_work_t
{
    std::function<intptr_t()> work;
    std::function<void(intptr_t)> done;
    intptr_t result;
};

static void worker_cb(uv_work_t* req)
{
    auto p_req = (work_req*)req;
    if (p_req->work)
    {
        p_req->result = p_req->work();
        p_req->work = nullptr;
    }
}

static void after_worker_cb(uv_work_t* req, int status)
{
    auto p_req = (work_req*)req;
    if (p_req->done)
    {
        p_req->done(p_req->result);
        p_req->done = nullptr;
    }
    delete p_req;
}

bool queue_work(std::function<intptr_t()>&& work, std::function<void(intptr_t)>&& done, uv_loop_t* loop)
{
    if (loop == nullptr)
        loop = uv_default_loop();

    auto req = new work_req{};
    req->work = std::move(work);
    req->done = std::move(done);
    uv_handle_set_data((uv_handle_t*)req, req);
    return uv_queue_work(loop, req, worker_cb, after_worker_cb) == 0;
}

} // namespace http