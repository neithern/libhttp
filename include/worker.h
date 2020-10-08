#ifndef _http_worker_h_
#define _http_worker_h_

#include <functional>

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_work_s uv_work_t;

namespace http
{

bool queue_work(std::function<intptr_t()>&& work, std::function<void(intptr_t)>&& done = nullptr, uv_loop_t* loop = nullptr);

}

#endif // _file_map_h_