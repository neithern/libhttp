#ifndef _http_worker_h_
#define _http_worker_h_

#include <functional>

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_work_s uv_work_t;

namespace http
{

class worker
{
public:
    worker(uv_loop_t* loop = nullptr);

    bool queue(std::function<void()> work, std::function<void(int)> after_work = nullptr);

private:
    static void worker_cb(uv_work_t* req);
    static void after_worker_cb(uv_work_t* req, int status);

private:
    uv_loop_t* loop_;
};

}

#endif // _file_map_h_