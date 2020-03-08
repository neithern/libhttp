#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <uv.h>
#include "buffer-pool.h"
#include "common.h"
#include "content-writer.h"
#include "parser.h"

namespace http
{

content_writer::_content_holder::_content_holder(const char* data, size_t size, content_done d)
{
    base = const_cast<char*>(data);
    len = (unsigned int)size;
    done = std::move(d);
}

content_writer::_content_holder::~_content_holder()
{
    if (done)
        done();
}

content_writer::_write_req::_write_req()
{
    memset(this, 0, sizeof(uv_write_t));
}

void content_writer::_write_req::done()
{
    holder = nullptr;
}

content_writer::content_writer(uv_loop_t* loop, std::shared_ptr<buffer_pool> buffer_pool)
{
    loop_ = loop;
    buffer_pool_ = buffer_pool;
    content_written_ = 0;
    content_to_write_ = 0;

    content_sink_ = [this](const char* data, size_t size, content_done done)
    {
        auto holder = std::make_shared<_content_holder>(data, size, done);
        if (writing_req_ != nullptr)
        {
            // push to list tail, will be written in on_written_cb()
            _holder_list.push_back(holder);
            return;
        }

        if (!_holder_list.empty())
        {
            // push to list tail, pop the front
            _holder_list.push_back(holder);
            holder = _holder_list.front();
            _holder_list.pop_front();
        }

        int r = (int)holder->len;
        if (r > 0)
            r = write_content(holder);
        if (r < 0)
        {
            // to stop write
            printf("%p:%p stop: %s\n", this, socket_, uv_err_name(r));
            if (writing_req_ != nullptr)
                on_written_cb((uv_write_t*)writing_req_, r);
            else
                on_write_end(r);
        }
    };
}

content_writer::~content_writer()
{
    if (writing_req_ != nullptr)
        uv_req_set_data((uv_req_t*)writing_req_, nullptr);

    _holder_list.clear();

    uv_stream_t* tcp = socket_;
    socket_ = nullptr;
    if (tcp != nullptr)
    {
        uv_handle_set_data((uv_handle_t*)tcp, nullptr);
        uv_close((uv_handle_t*)tcp, parser::on_closed_and_delete_cb);
    }
}

int content_writer::start_write(std::shared_ptr<_content_holder> res_headers, content_provider provider)
{
    content_provider_ = provider;

    assert(writing_req_ == nullptr);
    writing_req_ = prepare_write_req(res_headers);
    if (writing_req_ == nullptr)
        return UV_ENOMEM;
    return uv_write(writing_req_, socket_, res_headers.get(), 1, on_written_cb);
}

content_writer::_write_req* content_writer::prepare_write_req(std::shared_ptr<_content_holder> holder)
{
    // reuse the write req if possible
    _write_req* req = write_req_back_ != nullptr ? write_req_back_ : new _write_req;
    write_req_back_ = nullptr;
    if (req != nullptr)
    {
        req->holder = std::move(holder);
        uv_req_set_data((uv_req_t*)req, this);
    }
    return req;
}

int content_writer::write_content(std::shared_ptr<_content_holder> holder)
{
    int64_t max_write = content_to_write_ - content_written_;
    if (holder->len > max_write)
        holder->len = (size_t)max_write;

    content_written_ += holder->len;

    assert(writing_req_ == nullptr);
    writing_req_ = prepare_write_req(holder);
    if (writing_req_ == nullptr)
        return UV_ENOMEM;
    return uv_write(writing_req_, socket_, holder.get(), 1, on_written_cb);
}

int content_writer::write_next()
{
    if (content_to_write_ <= content_written_) // write done
        return 0;

    if (_holder_list.empty())
    {
        // prepare next content
        if (content_provider_)
            content_provider_(content_written_, content_to_write_, content_sink_);
        else
            set_write_done();
        return 0;
    }

    auto holder = _holder_list.front();
    _holder_list.pop_front();
    if ((ssize_t)holder->len < 0)
    {
        // to stop write
        return (int)holder->len;
    }

    return write_content(holder);
}

void content_writer::on_written_cb(uv_write_t* req, int status)
{
    content_writer* p_this = (content_writer*)uv_req_get_data((uv_req_t*)req);
    _write_req* write_req = (_write_req*)req;
    write_req->done();
    if (p_this == nullptr)
    {
        delete write_req;
        return;
    }

    // swap the write req to reuse it
    p_this->write_req_back_ = write_req;
    p_this->writing_req_ = nullptr;

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