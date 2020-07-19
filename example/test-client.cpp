#include <stdio.h>
#include <stdlib.h>
#include "client.h"

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

    int redirect_count = 0;
    int64_t offset = 0;
    FILE* out_file = nullptr;
    if (argc >= 3)
        out_file = fopen(argv[2], "wb+");

    http::request req;
    req.url = argv[1];
    https_to_http(req.url); // don't support https
    req.headers[http::HEADER_USER_AGENT] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/79.0.3945.130";

    http::client client;
    client.fetch(req,
        [](const http::response& res) {
            printf("%d %s\n", res.status_code, res.status_msg.c_str());
            for (auto& p : res.headers)
                printf("%s: %s\n", p.first.c_str(), p.second.c_str());
            return true;
        },
        [&](const char* data, size_t size, bool final) {
            if (out_file != nullptr && size > 0)
            {
                size = fwrite(data, 1, size, out_file);
                offset += size;
            }
            if (out_file != nullptr && final)
            {
                fclose(out_file);
                out_file = nullptr;
            }
            printf("%zu received\n", size);
            return true;
        },
        [&](std::string& url) {
            https_to_http(url); // don't support https
            printf("redirect: %s\n", url.c_str());
            return redirect_count++ < 5;
        },
        [&](int code) {
            if (out_file != nullptr)
            {
                fclose(out_file);
                out_file = nullptr;
            }
            printf("%d error\n", code);
        }
    );

    http::request req2;
    req2.url = "http://example.com/";
    client.fetch(req2,
        [](const std::string& body, int error) {
            printf("%s\n", body.c_str());
        }
    );

    return client.run_loop();
}
