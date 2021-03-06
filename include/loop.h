#ifndef _http_loop_h_
#define _http_loop_h_

#include <stdlib.h>
#include <functional>
#include <memory>
#include <mutex>
#include <list>

typedef struct uv_async_s uv_async_t;
typedef struct uv_handle_s uv_handle_t;
typedef struct uv_loop_s uv_loop_t;

namespace http
{

class loop
{
public:
    loop(bool use_default = true);
    ~loop();

    int async(std::function<void()>&& work);

    bool queue_work(std::function<intptr_t()>&& work, std::function<void(intptr_t)>&& done = nullptr);

    inline uv_loop_t* get_loop() const { return loop_; }

    int run_loop(bool once = false, bool nowait = false);
 
    void stop_loop();

    static void on_closed_and_free_cb(uv_handle_t* handle);

protected:
    void on_async();
    static void on_async_cb(uv_async_t* handle);

protected:
    uv_loop_t* loop_;
    void* loop_thread_;
    uv_async_t* work_async_;
    std::mutex work_mutex_;
    std::list<std::function<void()>> work_list_;
};

} // namespace http

#endif // _http_loop_h_