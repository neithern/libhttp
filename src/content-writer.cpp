#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
#include <uv.h>
#include "buffer-pool.h"
#include "common.h"
#include "content-writer.h"
#include "parser.h"

namespace http
{

content_writer::write_req::write_req(const char* data, size_t size, content_done done)
{
    buf.base = const_cast<char*>(data);
    buf.len = static_cast<decltype(buf.len)>(size);
    done_ = done;
}

content_writer::write_req::~write_req()
{
    if (done_)
        done_();
}

content_writer::content_writer(uv_loop_t* loop)
{
    loop_ = loop;

    content_written_ = 0;
    content_to_write_ = 0;

    content_sink_ = [this](const char* data, size_t size, content_done done)
    {
        auto req = std::make_shared<write_req>(data, size, done);
        if (!headers_written_ || writing_req_)
        {
            // push to list tail, will be written in on_written_cb()
            req_list.push_back(req);
            return;
        }

        if (!req_list.empty())
        {
            // push to list tail, pop the front
            req_list.push_back(req);
            req = req_list.front();
            req_list.pop_front();
        }

        int r = (int)req->buf.len;
        if (r > 0)
            r = write_content(req);
        if (r < 0)
        {
            // to stop write
            printf("%p:%p stop: %s\n", this, socket_, uv_err_name(r));
            if (writing_req_)
                on_written_cb(writing_req_.get(), r);
            else
                on_write_end(r);
        }
    };
}

content_writer::~content_writer()
{
    if (writing_req_)
        uv_req_set_data((uv_req_t*)writing_req_.get(), nullptr);

    req_list.clear();

    uv_stream_t* tcp = socket_;
    socket_ = nullptr;
    if (tcp != nullptr)
    {
        uv_handle_set_data((uv_handle_t*)tcp, nullptr);
        uv_close((uv_handle_t*)tcp, parser::on_closed_and_delete_cb);
    }
}

int content_writer::start_write(std::shared_ptr<write_req> headers, content_provider provider)
{
    content_provider_ = provider;

    assert(!writing_req_);
    writing_req_ = headers;
    uv_req_set_data((uv_req_t*)writing_req_.get(), this);

    int r = uv_write(writing_req_.get(), socket_, &writing_req_->buf, 1, on_written_cb);
    if (r < 0)
    {
        writing_req_.reset();
        return r;
    }

    headers_written_ = true;
    prepare_next();
    return 0;
}

void content_writer::prepare_next()
{
    if (content_provider_ && !is_write_done())
        content_provider_(content_written_, content_to_write_, content_sink_);
    else
        set_write_done();
}

int content_writer::write_content(std::shared_ptr<write_req> req)
{
    int64_t max_write = content_to_write_ - content_written_;
    if (req->buf.len > max_write)
        req->buf.len = static_cast<decltype(req->buf.len)>(max_write);

    content_written_ += req->buf.len;

    assert(!writing_req_);
    writing_req_ = req;
    uv_req_set_data((uv_req_t*)writing_req_.get(), this);

    int r = uv_write(writing_req_.get(), socket_, &writing_req_->buf, 1, on_written_cb);
    if (r < 0)
        writing_req_.reset();
    return r;
}

int content_writer::write_next()
{
    if (is_write_done())
        return 0;

    if (req_list.empty())
    {
        prepare_next();
        return 0;
    }

    auto req = req_list.front();
    req_list.pop_front();

    int r = (int)req->buf.len;
    if (r >= 0)
        r = write_content(req);

    if (r >= 0 && req_list.empty())
        prepare_next();
    return r;
}

void content_writer::on_written_cb(uv_write_t* req, int status)
{
    content_writer* p_this = (content_writer*)uv_req_get_data((uv_req_t*)req);
    if (p_this == nullptr)
        return;

    p_this->writing_req_.reset();

    int r = status;
    if (status >= 0)
    {
        r = p_this->write_next();
        if (r == UV_E_USER_CANCELLED)
            p_this->set_write_done();
        else if (r < 0)
            printf("%p:%p write socket: %s\n", p_this, p_this->socket_, uv_err_name(r));
    }
    else
        printf("%p:%p on_written_cb: %s\n", p_this, p_this->socket_, uv_err_name(r));

    if (r == UV_EOF)
        p_this->set_write_done();

    bool done = p_this->is_write_done();
    if (r < 0 || done)
        p_this->on_write_end(done ? 0 : r);
}

} // namespace http