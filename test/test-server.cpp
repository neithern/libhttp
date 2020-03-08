#include <stdio.h>
#include <stdlib.h>
#include "server.h"

http::string_map mime_types =
{
    { "txt", "text/plain" },
    { "htm", "text/html" },
    { "html","text/html" },
    { "css", "text/css" },
    { "gif", "image/gif" },
    { "jpg", "image/jpg" },
    { "png", "image/png" },
    { "svg", "image/svg+xml" },
    { "flv", "video/x-flv" },
    { "mp4", "video/mp4" },
    { "js",  "application/javascript" },
    { "json","application/json" },
    { "pdf", "application/pdf" },
    { "wasm","application/wasm" },
    { "xml", "application/xml" },
};

std::string file_extension(const std::string& path)
{
    std::smatch m;
    static auto re = std::regex("\\.([a-zA-Z0-9]+)$");
    if (std::regex_search(path, m, re))
        return m[1];
    return std::string();
}

int main(int argc, const char* argv[])
{
    int port = 80;
    std::string path = "";

    if (argc > 1)
        port = std::stoi(argv[1]);
    if (argc > 2)
        path = argv[2];

    http::server server;

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

    server.serve(".*", [&](const http::request2& req, http::response2& res) {
        printf("request: %s %s\n", req.method.c_str(), req.url.c_str());

        std::string path = req.url;
        if (path.size() > 0 && path.back() == '/')
            path += "index.html";
        if (path.size() > 0 && path[0] == '/')
            path = path.substr(1);

        if (server.serve_file(path, req, res))
        {
            std::string ext = file_extension(path);
            if (auto p = mime_types.find(ext); p != mime_types.cend())
                res.headers[http::HEADER_CONTENT_TYPE] = p->second;
        }
    });

    bool ret = server.listen("0.0.0.0", port);
    printf("Server listen on port %d: %s\n", port, ret ? "true" : "false");
 
    return ret ? server.run_loop() : -1;
}
