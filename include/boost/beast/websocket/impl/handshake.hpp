//
// Copyright (c) 2016-2019 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// Official repository: https://github.com/boostorg/beast
//

#ifndef BOOST_BEAST_WEBSOCKET_IMPL_HANDSHAKE_HPP
#define BOOST_BEAST_WEBSOCKET_IMPL_HANDSHAKE_HPP

#include <boost/beast/websocket/impl/stream_impl.hpp>
#include <boost/beast/websocket/detail/type_traits.hpp>
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/core/async_op_base.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/asio/coroutine.hpp>
#include <boost/assert.hpp>
#include <boost/throw_exception.hpp>
#include <memory>

namespace boost {
namespace beast {
namespace websocket {

//------------------------------------------------------------------------------

// send the upgrade request and process the response
//
template<class NextLayer, bool deflateSupported>
template<class Handler>
class stream<NextLayer, deflateSupported>::handshake_op
    : public beast::stable_async_op_base<Handler,
        beast::executor_type<stream>>
    , public net::coroutine
{
    struct data
    {
        data() = default; // for msvc

        // VFALCO This really should be two separate
        //        composed operations, to save on memory
        http::request<http::empty_body> req;
        http::response_parser<
            typename response_type::body_type> p;
        flat_buffer fb;
        bool overflow;
    };

    boost::weak_ptr<impl_type> wp_;
    detail::sec_ws_key_type key_;
    response_type* res_p_;
    data& d_;

public:
    template<class Handler_, class Decorator>
    handshake_op(
        Handler_&& h,
        boost::shared_ptr<impl_type> const& sp,
        response_type* res_p,
        string_view host, string_view target,
        Decorator const& decorator)
        : stable_async_op_base<Handler,
            beast::executor_type<stream>>(
                std::forward<Handler_>(h),
                    sp->stream.get_executor())
        , wp_(sp)
        , res_p_(res_p)
        , d_(beast::allocate_stable<data>(*this))
    {
        d_.req = sp->build_request(
            key_, host, target, decorator);
        sp->reset(); // VFALCO I don't like this
        (*this)({}, 0, false);
    }

    void
    operator()(
        error_code ec = {},
        std::size_t bytes_used = 0,
        bool cont = true)
    {
        boost::ignore_unused(bytes_used);
        auto sp = wp_.lock();
        if(! sp)
            return this->invoke(cont,
                net::error::operation_aborted);
        auto& impl = *sp;
        BOOST_ASIO_CORO_REENTER(*this)
        {
            impl.change_status(status::handshake);
            impl.update_timer(this->get_executor());

            // write HTTP request
            impl.do_pmd_config(d_.req);
            BOOST_ASIO_CORO_YIELD
            http::async_write(impl.stream,
                d_.req, std::move(*this));
            if(impl.check_stop_now(ec))
                goto upcall;

            // read HTTP response
            BOOST_ASIO_CORO_YIELD
            http::async_read(impl.stream,
                impl.rd_buf, d_.p,
                    std::move(*this));
            if(ec == http::error::buffer_overflow)
            {
                // If the response overflows the internal
                // read buffer, switch to a dynamically
                // allocated flat buffer.

                d_.fb.commit(net::buffer_copy(
                    d_.fb.prepare(impl.rd_buf.size()),
                    impl.rd_buf.data()));
                impl.rd_buf.clear();

                BOOST_ASIO_CORO_YIELD
                http::async_read(impl.stream,
                    d_.fb, d_.p, std::move(*this));

                if(! ec)
                {
                    // Copy any leftovers back into the read
                    // buffer, since this represents websocket
                    // frame data.

                    if(d_.fb.size() <= impl.rd_buf.capacity())
                    {
                        impl.rd_buf.commit(net::buffer_copy(
                            impl.rd_buf.prepare(d_.fb.size()),
                            d_.fb.data()));
                    }
                    else
                    {
                        ec = http::error::buffer_overflow;
                    }
                }

                // Do this before the upcall
                d_.fb.clear();
            }
            if(impl.check_stop_now(ec))
                goto upcall;

            // success
            impl.reset_idle();
            impl.on_response(d_.p.get(), key_, ec);
            if(res_p_)
                swap(d_.p.get(), *res_p_);

        upcall:
            this->invoke(cont ,ec);
        }
    }
};

template<class NextLayer, bool deflateSupported>
struct stream<NextLayer, deflateSupported>::
    run_handshake_op
{
    template<class HandshakeHandler, class Decorator>
    void operator()(
        HandshakeHandler&& h,
        boost::shared_ptr<impl_type> const& sp,
        response_type* r,
        string_view host, string_view target,
        Decorator const& d)
    {
        // If you get an error on the following line it means
        // that your handler does not meet the documented type
        // requirements for the handler.

        static_assert(
            beast::detail::is_invocable<HandshakeHandler,
                void(error_code)>::value,
            "HandshakeHandler type requirements not met");

        handshake_op<
            typename std::decay<HandshakeHandler>::type>(
                std::forward<HandshakeHandler>(h),
                sp,
                r,
                host, target,
                d);
    }
};

//------------------------------------------------------------------------------

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator>
void
stream<NextLayer, deflateSupported>::
do_handshake(
    response_type* res_p,
    string_view host,
    string_view target,
    RequestDecorator const& decorator,
    error_code& ec)
{
    auto& impl = *impl_;
    impl.change_status(status::handshake);
    impl.reset();
    detail::sec_ws_key_type key;
    {
        auto const req = impl.build_request(
            key, host, target, decorator);
        impl.do_pmd_config(req);
        http::write(impl.stream, req, ec);
    }
    if(impl.check_stop_now(ec))
        return;
    http::response_parser<
        typename response_type::body_type> p;
    http::read(next_layer(), impl.rd_buf, p, ec);
    if(ec == http::error::buffer_overflow)
    {
        // If the response overflows the internal
        // read buffer, switch to a dynamically
        // allocated flat buffer.

        flat_buffer fb;
        fb.commit(net::buffer_copy(
            fb.prepare(impl.rd_buf.size()),
            impl.rd_buf.data()));
        impl.rd_buf.clear();

        http::read(next_layer(), fb, p, ec);;

        if(! ec)
        {
            // Copy any leftovers back into the read
            // buffer, since this represents websocket
            // frame data.

            if(fb.size() <= impl.rd_buf.capacity())
            {
                impl.rd_buf.commit(net::buffer_copy(
                    impl.rd_buf.prepare(fb.size()),
                    fb.data()));
            }
            else
            {
                ec = http::error::buffer_overflow;
            }
        }
    }
    if(impl.check_stop_now(ec))
        return;

    impl.on_response(p.get(), key, ec);
    if(impl.check_stop_now(ec))
        return;

    if(res_p)
        *res_p = p.release();
}

//------------------------------------------------------------------------------

template<class NextLayer, bool deflateSupported>
template<class HandshakeHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(
    HandshakeHandler, void(error_code))
stream<NextLayer, deflateSupported>::
async_handshake(string_view host,
    string_view target,
        HandshakeHandler&& handler)
{
    static_assert(is_async_stream<next_layer_type>::value,
        "AsyncStream type requirements not met");
    return net::async_initiate<
        HandshakeHandler,
        void(error_code)>(
            run_handshake_op{},
            handler,
            impl_,
            nullptr,
            host, target,
            &default_decorate_req);
}

template<class NextLayer, bool deflateSupported>
template<class HandshakeHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(
    HandshakeHandler, void(error_code))
stream<NextLayer, deflateSupported>::
async_handshake(response_type& res,
    string_view host,
        string_view target,
            HandshakeHandler&& handler)
{
    static_assert(is_async_stream<next_layer_type>::value,
        "AsyncStream type requirements not met");
    return net::async_initiate<
        HandshakeHandler,
        void(error_code)>(
            run_handshake_op{},
            handler,
            impl_,
            &res,
            host, target,
            &default_decorate_req);
}

template<class NextLayer, bool deflateSupported>
void
stream<NextLayer, deflateSupported>::
handshake(string_view host,
    string_view target)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    error_code ec;
    handshake(
        host, target, ec);
    if(ec)
        BOOST_THROW_EXCEPTION(system_error{ec});
}

template<class NextLayer, bool deflateSupported>
void
stream<NextLayer, deflateSupported>::
handshake(response_type& res,
    string_view host,
        string_view target)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    error_code ec;
    handshake(res, host, target, ec);
    if(ec)
        BOOST_THROW_EXCEPTION(system_error{ec});
}

template<class NextLayer, bool deflateSupported>
void
stream<NextLayer, deflateSupported>::
handshake(string_view host,
    string_view target, error_code& ec)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    do_handshake(nullptr,
        host, target, &default_decorate_req, ec);
}

template<class NextLayer, bool deflateSupported>
void
stream<NextLayer, deflateSupported>::
handshake(response_type& res,
    string_view host,
        string_view target,
            error_code& ec)
{
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    do_handshake(&res,
        host, target, &default_decorate_req, ec);
}

//------------------------------------------------------------------------------

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator>
void
stream<NextLayer, deflateSupported>::
handshake_ex(string_view host,
    string_view target,
        RequestDecorator const& decorator)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    error_code ec;
    handshake_ex(host, target, decorator, ec);
    if(ec)
        BOOST_THROW_EXCEPTION(system_error{ec});
}

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator>
void
stream<NextLayer, deflateSupported>::
handshake_ex(response_type& res,
    string_view host,
        string_view target,
            RequestDecorator const& decorator)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    error_code ec;
    handshake_ex(res, host, target, decorator, ec);
    if(ec)
        BOOST_THROW_EXCEPTION(system_error{ec});
}

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator>
void
stream<NextLayer, deflateSupported>::
handshake_ex(string_view host,
    string_view target,
        RequestDecorator const& decorator,
            error_code& ec)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    do_handshake(nullptr,
        host, target, decorator, ec);
}

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator>
void
stream<NextLayer, deflateSupported>::
handshake_ex(response_type& res,
    string_view host,
        string_view target,
            RequestDecorator const& decorator,
                error_code& ec)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_sync_stream<next_layer_type>::value,
        "SyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    do_handshake(&res,
        host, target, decorator, ec);
}

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator, class HandshakeHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(
    HandshakeHandler, void(error_code))
stream<NextLayer, deflateSupported>::
async_handshake_ex(string_view host,
    string_view target,
        RequestDecorator const& decorator,
            HandshakeHandler&& handler)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_async_stream<next_layer_type>::value,
        "AsyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    return net::async_initiate<
        HandshakeHandler,
        void(error_code)>(
            run_handshake_op{},
            handler,
            impl_,
            nullptr,
            host, target,
            decorator);
}

template<class NextLayer, bool deflateSupported>
template<class RequestDecorator, class HandshakeHandler>
BOOST_ASIO_INITFN_RESULT_TYPE(
    HandshakeHandler, void(error_code))
stream<NextLayer, deflateSupported>::
async_handshake_ex(response_type& res,
    string_view host,
        string_view target,
            RequestDecorator const& decorator,
                HandshakeHandler&& handler)
{
#if ! BOOST_BEAST_ALLOW_DEPRECATED
    static_assert(sizeof(RequestDecorator) == 0,
        BOOST_BEAST_DEPRECATION_STRING);
#endif
    static_assert(is_async_stream<next_layer_type>::value,
        "AsyncStream type requirements not met");
    static_assert(detail::is_request_decorator<
            RequestDecorator>::value,
        "RequestDecorator requirements not met");
    return net::async_initiate<
        HandshakeHandler,
        void(error_code)>(
            run_handshake_op{},
            handler,
            impl_,
            &res,
            host, target,
            decorator);
}

} // websocket
} // beast
} // boost

#endif
