#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include "client.h"

void close_file(uv_loop_t* loop, int& fd)
{
    if (fd != -1)
    {
        uv_fs_t* close_req = new uv_fs_t;
        uv_fs_close(loop, close_req, fd, [](uv_fs_t* req) {
            delete req;
        });
        fd = -1;
    }
}

void https_to_http(std::string& url)
{
    if (url.compare(0, 5, "https") == 0)
        url = url.substr(0, 4) + url.substr(5);
}

int main(int argc, const char* argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s url\n", argv[0]);
        return -1;
    }

    uv_loop_t* loop = uv_default_loop();

    int redirect_count = 0;
    uv_file out_file = -1;
    uv_fs_t fs_req;
    int64_t offset = 0;
    if (argc >= 3)
        out_file = uv_fs_open(loop, &fs_req, argv[2], UV_FS_O_CREAT | UV_FS_O_RDWR | UV_FS_O_TRUNC, 0644, nullptr);

    http::request req;
    req.url = argv[1];
    https_to_http(req.url); // don't support https
    req.headers[HEADER_USER_AGENT] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/79.0.3945.130";

    http::client client(loop);
    client.fetch(req,
        [](const http::response& res) {
            printf("%d %s\n", res.status_code, res.status_msg.c_str());
            for (auto it = res.headers.cbegin(); it != res.headers.cend(); it++)
                printf("%s: %s\n", it->first.c_str(), it->second.c_str());
            return true;
        },
        [&](const char* data, size_t size, bool final) {
            if (out_file != -1 && size > 0)
            {
                uv_buf_t buf = uv_buf_init(const_cast<char*>(data), size);
                uv_fs_write(loop, &fs_req, out_file, &buf, 1, offset, nullptr);
                offset += size;
            }
            if (out_file != -1 && final)
                close_file(loop, out_file);
            printf("%zu received\n", size);
            return true;
        },
        [&](std::string& url) {
            https_to_http(url); // don't support https
            printf("redirect: %s\n", url.c_str());
            return redirect_count++ < 5;
        },
        [&](int code) {
            if (out_file != -1)
                close_file(loop, out_file);
            printf("%d error\n", code);
        }
    );

    return uv_run(loop, UV_RUN_DEFAULT);
}
