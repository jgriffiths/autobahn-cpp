///////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) Tavendo GmbH
//
// Boost Software License - Version 1.0 - August 17th, 2003
//
// Permission is hereby granted, free of charge, to any person or organization
// obtaining a copy of the software and accompanying documentation covered by
// this license (the "Software") to use, reproduce, display, distribute,
// execute, and transmit the Software, and to prepare derivative works of the
// Software, and to permit third-parties to whom the Software is furnished to
// do so, all subject to the following:
//
// The copyright notices in the Software and this entire statement, including
// the above license grant, this restriction and the following disclaimer,
// must be included in all copies of the Software, in whole or in part, and
// all derivative works of the Software, unless such copies or derivative
// works are solely in the form of machine-executable object code generated by
// a source language processor.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
// SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
// FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.
//
///////////////////////////////////////////////////////////////////////////////

#include "exceptions.hpp"
#include "wamp_message.hpp"
#include "wamp_transport_handler.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/placeholders.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <system_error>

namespace autobahn {

template <class Socket>
wamp_rawsocket_transport<Socket>::wamp_rawsocket_transport(
            boost::asio::io_service& io_service,
            const endpoint_type& remote_endpoint,
            bool debug_enabled)
    : wamp_transport()
    , m_socket(io_service)
    , m_remote_endpoint(remote_endpoint)
    , m_connect()
    , m_disconnect()
    , m_handshake_buffer()
    , m_message_length(0)
    , m_message_unpacker()
    , m_debug_enabled(debug_enabled)
{
    memset(m_handshake_buffer, 0, sizeof(m_handshake_buffer));
}

template <class Socket>
boost::future<void> wamp_rawsocket_transport<Socket>::connect()
{
    m_connect =  boost::promise<void>();  // reset the promise

    if (m_socket.is_open()) {
        m_connect.set_exception(boost::copy_exception(network_error("network transport already connected")));
        return m_connect.get_future();
    }

    std::weak_ptr<wamp_rawsocket_transport<Socket>> weak_self = this->shared_from_this();
    auto connect_handler = [=](const boost::system::error_code& error_code) {
        auto shared_self = weak_self.lock();
        if (!shared_self) {
            return;
        }

        if (error_code) {
            m_socket.close();  // async_connect will leave it open
            m_connect.set_exception(boost::copy_exception(
                            std::system_error(error_code.value(), std::system_category(), "connect")));
            return;
        }

        // FIXME: There should be a wamp_rawsocket_properties object or something
        //        similar that can be passed in to help deal with overiding these
        //        handshake parameters.
        //
        // Send the initial handshake packet informing the server which
        // serialization format we wish to use, and our maximum message size.
        m_handshake_buffer[0] = 0x7F; // magic byte
        m_handshake_buffer[1] = 0xF2; // we are ready to receive messages up to 2**24 octets and encoded using MsgPack
        m_handshake_buffer[2] = 0x00; // reserved
        m_handshake_buffer[3] = 0x00; // reserved

        boost::asio::write(
                m_socket,
                boost::asio::buffer(m_handshake_buffer, sizeof(m_handshake_buffer)));

        auto handshake_reply = [=](
                const boost::system::error_code& error,
                std::size_t bytes_transferred) {
            auto shared_self = weak_self.lock();
            if (shared_self) {
                handshake_reply_handler(error, bytes_transferred);
            }
        };

        try {
            // Read the 4-byte handshake reply from the server
            boost::asio::async_read(
                    m_socket,
                    boost::asio::buffer(m_handshake_buffer, sizeof(m_handshake_buffer)),
                    handshake_reply);
        } catch (const std::exception& e) {
            m_connect.set_exception(boost::copy_exception(e));
        }
    };

    m_socket.async_connect(m_remote_endpoint, connect_handler);

    return m_connect.get_future();
}

template <class Socket>
boost::future<void> wamp_rawsocket_transport<Socket>::disconnect()
{
    if (!m_socket.is_open()) {
        throw network_error("network transport already disconnected");
    }

    m_socket.close();

    m_disconnect.set_value();
    return m_disconnect.get_future();
}

template <class Socket>
bool wamp_rawsocket_transport<Socket>::is_connected() const
{
    return m_socket.is_open();
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::send_message(wamp_message&& message)
{
    auto buffer = std::make_shared<msgpack::sbuffer>();
    msgpack::packer<msgpack::sbuffer> packer(*buffer);
    packer.pack(message.fields());

    // Write the length prefix as the message header.
    uint32_t length = htonl(buffer->size());
    boost::asio::write(m_socket, boost::asio::buffer(&length, sizeof(length)));

    // Write actual serialized message.
    boost::asio::write(m_socket, boost::asio::buffer(buffer->data(), buffer->size()));

    if (m_debug_enabled) {
        std::cerr << "TX message (" << buffer->size() << " octets) ..." << std::endl;
        std::cerr << "TX message: " << message << std::endl;
    }
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::set_pause_handler(pause_handler&& handler)
{
    m_pause_handler = std::move(handler);
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::set_resume_handler(resume_handler&& handler)
{
    m_resume_handler = std::move(handler);
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::pause()
{
    if (m_pause_handler) {
        m_pause_handler();
    }
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::resume()
{
    if (m_resume_handler) {
        m_resume_handler();
    }
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::attach(
        const std::shared_ptr<wamp_transport_handler>& handler)
{
    if (m_handler) {
        throw std::logic_error("handler already attached");
    }

    m_handler = handler;

    m_handler->on_attach(this->shared_from_this());
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::detach()
{
    if (!m_handler) {
        throw std::logic_error("no handler attached");
    }

    m_handler->on_detach(true, "wamp.error.goodbye");
    m_handler.reset();
}

template <class Socket>
bool wamp_rawsocket_transport<Socket>::has_handler() const
{
    return m_handler != nullptr;
}

template <class Socket>
Socket& wamp_rawsocket_transport<Socket>::socket()
{
    return m_socket;
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::handshake_reply_handler(
        const boost::system::error_code& error_code,
        std::size_t /* bytes_transferred */)
{
    if (error_code) {
        if (m_debug_enabled) {
            std::cerr << "rawsocket handshake error: " << error_code << std::endl;
        }

        m_connect.set_exception(boost::copy_exception(
                std::system_error(error_code.value(), std::system_category(), "async_read")));
        return;
    }

    if (m_debug_enabled) {
        std::cerr << "RawSocket handshake reply received" << std::endl;
    }

    if (m_handshake_buffer[0] != 0x7F) {
        m_connect.set_exception(boost::copy_exception(protocol_error("invalid handshake frame")));
        return;
    }

    // Indicates that the handshake reply is an error.
    if ((m_handshake_buffer[1] & 0x0F) == 0x00) {
        uint32_t error = m_handshake_buffer[1] & 0xF0;
        if (m_debug_enabled) {
            std::cerr << "rawsocket handshake error: " << std::hex << error << std::endl;
        }

        std::stringstream error_string;
        if (error == 0x00) {
            error_string << "illegal error code (" << error << ")";
        } else if (error == 0x10) {
            error_string << "serializer unsupported";
        } else if (error == 0x20) {
            error_string << "maximum message length unacceptable";
        } else if (error == 0x30) {
            error_string << "use of reserved bits (unsupported feature)";
        } else if (error == 0x40) {
            error_string << "maximum connection count reached";
        } else {
            error_string << "unknown/reserved error code (" << error << ")";
        }

        m_connect.set_exception(boost::copy_exception(protocol_error(error_string.str())));
        return;
    }

    uint32_t serializer_type = (m_handshake_buffer[1] & 0x0F);
    if (serializer_type == 0x01) {
        m_connect.set_exception(boost::copy_exception(protocol_error("json currently not supported")));
    } else if (serializer_type == 0x02) {
        if (m_debug_enabled) {
            std::cerr << "connect successful: valid handshake" << std::endl;
        }
        m_connect.set_value();
        receive_message();
    } else {
        std::stringstream error_string;
        error_string << "rawsocket handshake error: invalid serializer type (" << serializer_type << ")";
        m_connect.set_exception(boost::copy_exception(protocol_error(error_string.str())));
    }
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::receive_message()
{
    if (m_debug_enabled) {
        std::cerr << "RX preparing to receive message .." << std::endl;
    }

    boost::asio::async_read(
        m_socket,
        boost::asio::buffer(&m_message_length, sizeof(m_message_length)),
        bind(&wamp_rawsocket_transport<Socket>::receive_message_header,
            this->shared_from_this(),
            boost::asio::placeholders::error,
            boost::asio::placeholders::bytes_transferred));
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::receive_message_header(
        const boost::system::error_code& error_code,
        std::size_t /* bytes transferred */)
{
    if (!error_code) {
        m_message_length = ntohl(m_message_length);

        if (m_debug_enabled) {
            std::cerr << "RX message (" << m_message_length << " octets) ..." << std::endl;
        }

        m_message_unpacker.reserve_buffer(m_message_length);

        boost::asio::async_read(
            m_socket,
            boost::asio::buffer(m_message_unpacker.buffer(), m_message_length),
            bind(&wamp_rawsocket_transport<Socket>::receive_message_body,
                this->shared_from_this(),
                boost::asio::placeholders::error,
                boost::asio::placeholders::bytes_transferred));
        return;
    }
}

template <class Socket>
void wamp_rawsocket_transport<Socket>::receive_message_body(
        const boost::system::error_code& error_code,
        std::size_t /* bytes transferred */)
{
    if (error_code) {
        if (m_debug_enabled && error_code != boost::asio::error::operation_aborted) {
            std::cerr << "Receive error: " << error_code << std::endl;
        }
        return;
    }

    if (m_debug_enabled) {
        std::cerr << "RX message received." << std::endl;
    }

    if (m_handler) {
        m_message_unpacker.buffer_consumed(m_message_length);
        msgpack::unpacked result;

        while (m_message_unpacker.next(&result)) {
            wamp_message::message_fields fields;
            result.get().convert(fields);

            wamp_message message(std::move(fields), std::move(*(result.zone())));
            if (m_debug_enabled) {
                std::cerr << "RX message: " << message << std::endl;
            }

            m_handler->on_message(std::move(message));
        }
    } else {
        std::cerr << "RX message ignored: no handler attached" << std::endl;
    }

    receive_message();
}

} // namespace autobahn
