/**************************************************************************
   Copyright (c) 2021 sewenew

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 *************************************************************************/

#include "async_connection.h"
#include <hiredis/async.h>
#include "errors.h"

namespace {

using namespace sw::redis;

void set_options_callback(redisAsyncContext *ctx, void *r, void *) {
    assert(ctx != nullptr);

    auto *context = static_cast<AsyncContext *>(ctx->data);
    assert(context != nullptr);

    auto &connection = context->connection;
    assert(connection);

    redisReply *reply = static_cast<redisReply *>(r);
    if (reply == nullptr) {
        // Connection has bee closed.
        // TODO: not sure if we should set this to be State::BROKEN
        return;
    }

    try {
        if (reply::is_error(*reply)) {
            throw_error(*reply);
        }

        reply::parse<void>(*reply);
    } catch (const Error &e) {
        // TODO: disconnect and connect_callback might throw
        connection->disconnect(std::make_exception_ptr(e));

        return;
    }

    connection->connect_callback();
}

}

namespace sw {

namespace redis {

FormattedCommand::FormattedCommand(char *data, int len) : _data(data), _size(len) {
    if (data == nullptr || len < 0) {
        throw Error("failed to format command");
    }
}

FormattedCommand::~FormattedCommand() noexcept {
    if (_data != nullptr) {
        redisFreeCommand(_data);
    }
}

void FormattedCommand::_move(FormattedCommand &&that) noexcept {
    _data = that._data;
    _size = that._size;
    that._data = nullptr;
    that._size = 0;
}

AsyncConnection::AsyncConnection(const ConnectionOptions &opts, EventLoop *loop) :
    _opts(opts),
    _loop(loop),
    _state(State::BROKEN) {
    assert(_loop != nullptr);
}

AsyncConnection::~AsyncConnection() {
    auto events = _get_events();

    // TODO: assert(events.empty());

    auto err = std::make_exception_ptr(Error("connection is closing"));
    for (auto &event : events) {
        event->set_exception(err);
    }
}

void AsyncConnection::event_callback() {
    switch (_state) {
    case State::BROKEN:
        _connect();
        break;

    case State::READY:
        _send();
        break;

    default:
        break;
    }
}

void AsyncConnection::connect_callback(std::exception_ptr err) {
    if (err) {
        // Failed to connect to Redis, fail all pending events.
        _fail_events(err);

        return;
    }

    // Connect OK.
    try {
        switch (_state) {
        case State::CONNECTING:
            _connecting_callback();
            break;

        case State::AUTHING:
            _authing_callback();
            break;

        default:
            assert(_state == State::SELECTING_DB);

            _set_ready();
        }
    } catch (const Error &e) {
        disconnect(std::make_exception_ptr(e));
    }
}

void AsyncConnection::disconnect(std::exception_ptr err) {
    if (_ctx == nullptr) {
        return;
    }

    _disable_disconnect_callback();

    redisAsyncDisconnect(_ctx);

    _fail_events(err);
}

void AsyncConnection::disconnect_callback(std::exception_ptr err) {
    _fail_events(err);
}

void AsyncConnection::_disable_disconnect_callback() {
    assert(_ctx != nullptr);

    auto *ctx = static_cast<AsyncContext *>(_ctx->data);

    assert(ctx != nullptr);

    ctx->run_disconnect_callback = false;
}

void AsyncConnection::_send() {
    auto events = _get_events();
    auto &ctx = _context();
    for (auto idx = 0U; idx != events.size(); ++idx) {
        auto &event = events[idx];
        try {
            event->handle(ctx);

            // CommandEvent::_reply_callback will release the memory.
            event.release();
        } catch (...) {
            // Failed to send command, fail subsequent events.
            auto err = std::current_exception();
            for (; idx != events.size(); ++idx) {
                auto &event = events[idx];
                event->set_exception(err);
            }

            disconnect(err);

            break;
        }
    }
}

std::vector<AsyncEventUPtr> AsyncConnection::_get_events() {
    std::vector<AsyncEventUPtr> events;
    {
        std::lock_guard<std::mutex> lock(_mtx);

        events.swap(_events);
    }

    return events;
}

void AsyncConnection::_fail_events(std::exception_ptr err) {
    _ctx = nullptr;

    if (!err) {
        err = std::make_exception_ptr(Error("connection is closing"));
    }

    auto events = _get_events();
    for (auto &event : events) {
        assert(event);

        event->set_exception(err);
    }

    _state = State::BROKEN;
}

void AsyncConnection::_connecting_callback() {
    if (_need_auth()) {
        _auth();
    } else if (_need_select_db()) {
        _select_db();
    } else {
        _set_ready();
    }
}

void AsyncConnection::_authing_callback() {
    if (_need_select_db()) {
        _select_db();
    } else {
        _set_ready();
    }
}

void AsyncConnection::_auth() {
    assert(!broken());

    if (_opts.user == "default") {
        if (redisAsyncCommand(_ctx, set_options_callback, nullptr, "AUTH %b",
                    _opts.password.data(), _opts.password.size()) != REDIS_OK) {
            throw Error("failed to send auth command");
        }
    } else {
        // Redis 6.0 or latter
        if (redisAsyncCommand(_ctx, set_options_callback, nullptr, "AUTH %b %b",
                    _opts.user.data(), _opts.user.size(),
                    _opts.password.data(), _opts.password.size()) != REDIS_OK) {
            throw Error("failed to send auth command");
        }
    }

    _state = State::AUTHING;
}

void AsyncConnection::_select_db() {
    assert(!broken());

    if (redisAsyncCommand(_ctx, set_options_callback, nullptr, "SELECT %d",
            _opts.db) != REDIS_OK) {
        throw Error("failed to send select command");
    }

    _state = State::SELECTING_DB;
}

void AsyncConnection::_set_ready() {
    _state = State::READY;

    // Send pending commands.
    _send();
}

void AsyncConnection::_connect() {
    try {
        auto ctx = _connect(_opts);

        assert(ctx && ctx->err == REDIS_OK);

        _loop->watch(*ctx);

        _ctx = ctx.release();

        _state = State::CONNECTING;
    } catch (const Error &err) {
        _fail_events(std::make_exception_ptr(err));
    }
}

bool AsyncConnection::_need_auth() const {
    return !_opts.password.empty() || _opts.user != "default";
}

bool AsyncConnection::_need_select_db() const {
    return _opts.db != 0;
}

void AsyncConnection::_clean_async_context(void *data) {
    auto *ctx = static_cast<AsyncContext *>(data);

    assert(ctx != nullptr);

    delete ctx;
}

AsyncConnection::AsyncContextUPtr AsyncConnection::_connect(const ConnectionOptions &opts) {
    redisAsyncContext *context = nullptr;
    switch (opts.type) {
    case ConnectionType::TCP:
        context = redisAsyncConnect(opts.host.c_str(), _opts.port);
        break;

    case ConnectionType::UNIX:
        context = redisAsyncConnectUnix(opts.path.c_str());
        break;

    default:
        // Never goes here.
        throw Error("Unknown connection type");
    }

    if (context == nullptr) {
        throw Error("Failed to allocate memory for connection.");
    }

    auto ctx = AsyncContextUPtr(context);
    if (ctx->err != REDIS_OK) {
        throw_error(ctx->c, "failed to connect to server");
    }

    ctx->data = new AsyncContext(shared_from_this());
    ctx->dataCleanup = _clean_async_context;

    return ctx;
}

}

}
