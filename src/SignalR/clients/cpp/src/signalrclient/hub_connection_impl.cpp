// Copyright (c) .NET Foundation. All rights reserved.
// Licensed under the Apache License, Version 2.0. See License.txt in the project root for license information.

#include "stdafx.h"
#include "hub_connection_impl.h"
#include "signalrclient/hub_exception.h"
#include "trace_log_writer.h"
#include "make_unique.h"
#include "signalrclient/signalr_exception.h"

using namespace web;

namespace signalr
{
    // unnamed namespace makes it invisble outside this translation unit
    namespace
    {
        static std::function<void(const json::value&)> create_hub_invocation_callback(const logger& logger,
            const std::function<void(const json::value&)>& set_result,
            const std::function<void(const std::exception_ptr e)>& set_exception);
    }

    std::shared_ptr<hub_connection_impl> hub_connection_impl::create(const utility::string_t& url, trace_level trace_level,
        const std::shared_ptr<log_writer>& log_writer)
    {
        return hub_connection_impl::create(url, trace_level, log_writer,
            std::make_unique<web_request_factory>(), std::make_unique<transport_factory>());
    }

    std::shared_ptr<hub_connection_impl> hub_connection_impl::create(const utility::string_t& url, trace_level trace_level,
        const std::shared_ptr<log_writer>& log_writer, std::unique_ptr<web_request_factory> web_request_factory,
        std::unique_ptr<transport_factory> transport_factory)
    {
        auto connection = std::shared_ptr<hub_connection_impl>(new hub_connection_impl(url, trace_level,
            log_writer ? log_writer : std::make_shared<trace_log_writer>(), std::move(web_request_factory), std::move(transport_factory)));

        connection->initialize();

        return connection;
    }

    hub_connection_impl::hub_connection_impl(const utility::string_t& url, trace_level trace_level,
        const std::shared_ptr<log_writer>& log_writer, std::unique_ptr<web_request_factory> web_request_factory,
        std::unique_ptr<transport_factory> transport_factory)
        : m_connection(connection_impl::create(url, trace_level, log_writer,
        std::move(web_request_factory), std::move(transport_factory))),m_logger(log_writer, trace_level),
        m_callback_manager(json::value::parse(_XPLATSTR("{ \"error\" : \"connection went out of scope before invocation result was received\"}"))),
        m_disconnected([]() noexcept {}), m_handshakeReceived(false)
    { }

    void hub_connection_impl::initialize()
    {
        auto this_hub_connection = shared_from_this();

        // weak_ptr prevents a circular dependency leading to memory leak and other problems
        auto weak_hub_connection = std::weak_ptr<hub_connection_impl>(this_hub_connection);

        m_connection->set_message_received([weak_hub_connection](const utility::string_t& message)
        {
            auto connection = weak_hub_connection.lock();
            if (connection)
            {
                connection->process_message(message);
            }
        });

        m_connection->set_disconnected([weak_hub_connection]()
        {
            auto connection = weak_hub_connection.lock();
            if (connection)
            {
                connection->m_handshakeTask.set_exception(signalr_exception(_XPLATSTR("connection closed while handshake was in progress.")));
                connection->m_disconnected();
            }
        });
    }

    void hub_connection_impl::on(const utility::string_t& event_name, const std::function<void(const json::value &)>& handler)
    {
        if (event_name.length() == 0)
        {
            throw std::invalid_argument("event_name cannot be empty");
        }

        auto weak_connection = std::weak_ptr<hub_connection_impl>(shared_from_this());
        auto connection = weak_connection.lock();
        if (connection && connection->get_connection_state() != connection_state::disconnected)
        {
            throw signalr_exception(_XPLATSTR("can't register a handler if the connection is in a disconnected state"));
        }

        if (m_subscriptions.find(event_name) != m_subscriptions.end())
        {
            throw signalr_exception(
                _XPLATSTR("an action for this event has already been registered. event name: ") + event_name);
        }

        m_subscriptions.insert(std::pair<utility::string_t, std::function<void(const json::value &)>> {event_name, handler});
    }

    pplx::task<void> hub_connection_impl::start()
    {
        if (m_connection->get_connection_state() != connection_state::disconnected)
        {
            throw signalr_exception(
                _XPLATSTR("the connection can only be started if it is in the disconnected state"));
        }

        m_connection->set_client_config(m_signalr_client_config);
        m_handshakeTask = pplx::task_completion_event<void>();
        m_handshakeReceived = false;
        auto weak_connection = weak_from_this();
        return m_connection->start()
            .then([weak_connection](pplx::task<void> startTask)
            {
                startTask.get();
                auto connection = weak_connection.lock();
                if (!connection)
                {
                    // The connection has been destructed
                    return pplx::task_from_exception<void>(signalr_exception(_XPLATSTR("the hub connection has been deconstructed")));
                }
                return connection->m_connection->send(_XPLATSTR("{\"protocol\":\"json\",\"version\":1}\x1e"))
                    .then([weak_connection](pplx::task<void> previous_task)
                    {
                        auto connection = weak_connection.lock();
                        if (!connection)
                        {
                            // The connection has been destructed
                            return pplx::task_from_exception<void>(signalr_exception(_XPLATSTR("the hub connection has been deconstructed")));
                        }
                        previous_task.get();
                        return pplx::task<void>(connection->m_handshakeTask);
                    })
                    .then([weak_connection](pplx::task<void> previous_task)
                    {
                        try
                        {
                            previous_task.get();
                            return previous_task;
                        }
                        catch (std::exception e)
                        {
                            auto connection = weak_connection.lock();
                            if (connection)
                            {
                                return connection->m_connection->stop()
                                    .then([e]() {
                                        throw e;
                                    });
                            }
                            throw e;
                        }
                    });
            });
    }

    pplx::task<void> hub_connection_impl::stop()
    {
        m_callback_manager.clear(json::value::parse(_XPLATSTR("{ \"error\" : \"connection was stopped before invocation result was received\"}")));
        return m_connection->stop();
    }

    enum MessageType
    {
        Invocation = 1,
        StreamItem,
        Completion,
        StreamInvocation,
        CancelInvocation,
        Ping,
        Close,
    };

    void hub_connection_impl::process_message(const utility::string_t& response)
    {
        try
        {
            auto pos = response.find('\x1e');
            std::size_t lastPos = 0;
            while (pos != utility::string_t::npos)
            {
                auto message = response.substr(lastPos, pos - lastPos);
                const auto result = web::json::value::parse(message);

                if (!result.is_object())
                {
                    m_logger.log(trace_level::info, utility::string_t(_XPLATSTR("unexpected response received from the server: "))
                        .append(message));

                    return;
                }

                if (!m_handshakeReceived)
                {
                    if (result.has_field(_XPLATSTR("error")))
                    {
                        auto error = result.at(_XPLATSTR("error")).as_string();
                        m_logger.log(trace_level::errors, utility::string_t(_XPLATSTR("handshake error: "))
                            .append(error));
                        m_handshakeTask.set_exception(signalr_exception(utility::string_t(_XPLATSTR("Received an error during handshake: ")).append(error)));
                        return;
                    }
                    else
                    {
                        if (result.has_field(_XPLATSTR("type")))
                        {
                            m_handshakeTask.set_exception(signalr_exception(utility::string_t(_XPLATSTR("Received unexpected message while waiting for the handshake response."))));
                        }
                        m_handshakeReceived = true;
                        m_handshakeTask.set();
                        return;
                    }
                }

                auto messageType = result.at(_XPLATSTR("type"));
                switch (messageType.as_integer())
                {
                case MessageType::Invocation:
                {
                    auto method = result.at(_XPLATSTR("target")).as_string();
                    auto event = m_subscriptions.find(method);
                    if (event != m_subscriptions.end())
                    {
                        event->second(result.at(_XPLATSTR("arguments")));
                    }
                    break;
                }
                case MessageType::StreamInvocation:
                    // Sent to server only, should not be received by client
                    throw std::runtime_error("Received unexpected message type 'StreamInvocation'.");
                case MessageType::StreamItem:
                    // TODO
                    break;
                case MessageType::Completion:
                {
                    if (result.has_field(_XPLATSTR("error")) && result.has_field(_XPLATSTR("result")))
                    {
                        // TODO: error
                    }
                    invoke_callback(result);
                    break;
                }
                case MessageType::CancelInvocation:
                    // Sent to server only, should not be received by client
                    throw std::runtime_error("Received unexpected message type 'CancelInvocation'.");
                case MessageType::Ping:
                    // TODO
                    break;
                case MessageType::Close:
                    // TODO
                    break;
                }

                lastPos = pos + 1;
                pos = response.find('\x1e', lastPos);
            }
        }
        catch (const std::exception &e)
        {
            m_logger.log(trace_level::errors, utility::string_t(_XPLATSTR("error occured when parsing response: "))
                .append(utility::conversions::to_string_t(e.what()))
                .append(_XPLATSTR(". response: "))
                .append(response));
        }
    }

    bool hub_connection_impl::invoke_callback(const web::json::value& message)
    {
        auto id = message.at(_XPLATSTR("invocationId")).as_string();
        if (!m_callback_manager.invoke_callback(id, message, true))
        {
            m_logger.log(trace_level::info, utility::string_t(_XPLATSTR("no callback found for id: ")).append(id));
            return false;
        }

        return true;
    }

    pplx::task<json::value> hub_connection_impl::invoke(const utility::string_t& method_name, const json::value& arguments)
    {
        _ASSERTE(arguments.is_array());

        pplx::task_completion_event<json::value> tce;

        const auto callback_id = m_callback_manager.register_callback(
            create_hub_invocation_callback(m_logger, [tce](const json::value& result) { tce.set(result); },
                [tce](const std::exception_ptr e) { tce.set_exception(e); }));

        invoke_hub_method(method_name, arguments, callback_id, nullptr,
            [tce](const std::exception_ptr e){ tce.set_exception(e); });

        return pplx::create_task(tce);
    }

    pplx::task<void> hub_connection_impl::send(const utility::string_t& method_name, const json::value& arguments)
    {
        _ASSERTE(arguments.is_array());

        pplx::task_completion_event<void> tce;

        invoke_hub_method(method_name, arguments, _XPLATSTR(""),
            [tce]() { tce.set(); },
            [tce](const std::exception_ptr e){ tce.set_exception(e); });

        return pplx::create_task(tce);
    }

    void hub_connection_impl::invoke_hub_method(const utility::string_t& method_name, const json::value& arguments,
        const utility::string_t& callback_id, std::function<void()> set_completion, std::function<void(const std::exception_ptr)> set_exception)
    {
        json::value request;
        request[_XPLATSTR("type")] = json::value::value(1);
        if (!callback_id.empty())
        {
            request[_XPLATSTR("invocationId")] = json::value::string(callback_id);
        }
        request[_XPLATSTR("target")] = json::value::string(method_name);
        request[_XPLATSTR("arguments")] = arguments;

        auto this_hub_connection = shared_from_this();

        // weak_ptr prevents a circular dependency leading to memory leak and other problems
        auto weak_hub_connection = std::weak_ptr<hub_connection_impl>(this_hub_connection);

        m_connection->send(request.serialize() + _XPLATSTR('\x1e'))
            .then([set_completion, set_exception, weak_hub_connection, callback_id](pplx::task<void> send_task)
            {
                try
                {
                    send_task.get();
                    if (callback_id.empty())
                    {
                        // complete nonBlocking call
                        set_completion();
                    }
                }
                catch (const std::exception&)
                {
                    set_exception(std::current_exception());
                    auto hub_connection = weak_hub_connection.lock();
                    if (hub_connection)
                    {
                        hub_connection->m_callback_manager.remove_callback(callback_id);
                    }
                }
            });
    }

    connection_state hub_connection_impl::get_connection_state() const noexcept
    {
        return m_connection->get_connection_state();
    }

    utility::string_t hub_connection_impl::get_connection_id() const
    {
        return m_connection->get_connection_id();
    }

    void hub_connection_impl::set_client_config(const signalr_client_config& config)
    {
        m_signalr_client_config = config;
        m_connection->set_client_config(config);
    }

    void hub_connection_impl::set_disconnected(const std::function<void()>& disconnected)
    {
        m_disconnected = disconnected;
    }

    // unnamed namespace makes it invisble outside this translation unit
    namespace
    {
        static std::function<void(const json::value&)> create_hub_invocation_callback(const logger& logger,
            const std::function<void(const json::value&)>& set_result,
            const std::function<void(const std::exception_ptr)>& set_exception)
        {
            return [logger, set_result, set_exception](const json::value& message)
            {
                if (message.has_field(_XPLATSTR("result")))
                {
                    set_result(message.at(_XPLATSTR("result")));
                }
                else if (message.has_field(_XPLATSTR("error")))
                {
                    set_exception(
                        std::make_exception_ptr(
                            hub_exception(message.at(_XPLATSTR("error")).serialize())));
                }

                set_result(json::value::null());
            };
        }
    }
}
