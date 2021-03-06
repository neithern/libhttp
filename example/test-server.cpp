#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "server.h"

int main(int argc, const char* argv[])
{
    int port = 8000;
    std::string path = "";

    if (argc > 1)
        port = strtol(argv[1], nullptr, 10);
    if (argc > 2)
        path = argv[2];

    http::server server;

#ifdef _TEST_POST_
    http::router router =
    {
        [](const http::request2& req) {
            printf("request start: %s %s\n", req.method.c_str(), req.url.c_str());
            return true;
        },
        [](const char* data, size_t size) {
            printf("request data: %zu\n", size);
            return true;
        },
        [](const http::request2& req, http::response2& res) {
            printf("request end: %s %s\n", req.method.c_str(), req.url.c_str());
        }
    };
    server.serve("/push/.*", router);
#endif

#ifdef _TEST_HELLO_
    server.serve("/hello", [&](const http::request2& req, http::response2& res) {
        auto pstr = std::make_shared<std::string>("Hello World!");
        res.content_length = pstr->size();
        res.provider = [pstr](int64_t offset, int64_t length, http::content_sink sink) {
            sink(pstr->c_str(), pstr->size(), [pstr]() {});
        };
    });
#endif

    server.serve(".*", [&](const http::request2& req, http::response2& res) {
        printf("request: %s %s\n", req.method.c_str(), req.url.c_str());

        std::string path = req.url;
        if (path.size() > 0 && path.back() == '/')
            path += "index.html";
        if (path.size() > 0 && path[0] == '/')
            path = path.substr(1);

        server.serve_file(path, req, res);
    });

    bool ret = server.listen("0.0.0.0", port);
    printf("Server listen on port %d%s\n", port, ret ? "." : " failed!");
 
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN); // for linux
#endif
    return ret ? server.run_loop() : -1;
}