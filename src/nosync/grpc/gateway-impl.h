#ifndef NOSYNC__GRPC__GATEWAY_IMPL_H
#define NOSYNC__GRPC__GATEWAY_IMPL_H

#include <functional>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <nosync/const-response-request-handler.h>
#include <nosync/destroy-notifier.h>
#include <nosync/event-loop.h>
#include <nosync/func-request-handler.h>
#include <nosync/interface-type.h>
#include <nosync/lazy-init-request-handler.h>
#include <nosync/memory-utils.h>
#include <nosync/noconcurrent-request-handler.h>
#include <nosync/request-handler.h>
#include <nosync/result-handler-utils.h>
#include <nosync/time-utils.h>
#include <nosync/transforming-request-handler.h>
#include <system_error>
#include <thread>
#include <utility>


namespace nosync::grpc
{

namespace gateway_impl
{

constexpr auto end_of_stream_errc = std::errc::no_message_available;

constexpr auto cqueue_closed_errc = std::errc::operation_canceled;

constexpr auto gateway_destroyed_errc = std::errc::operation_canceled;


template<typename Res>
struct async_call_context
{
    ::grpc::ClientContext grpc_context;
    ::grpc::Status grpc_status;
    Res response;
};


template<typename Res>
struct async_reader_context
{
    ::grpc::ClientContext grpc_context;
    std::unique_ptr<::grpc::ClientAsyncReader<Res>> reader;
};


inline raw_error_result make_error_result_from_grpc_status(const ::grpc::Status &status)
{
    using std::errc;

    errc errc_code;
    switch (status.error_code()) {
    case ::grpc::CANCELLED:
        errc_code = errc::operation_canceled;
        break;
    case ::grpc::INVALID_ARGUMENT:
    case ::grpc::FAILED_PRECONDITION:
        errc_code = errc::invalid_argument;
        break;
    case ::grpc::DEADLINE_EXCEEDED:
        errc_code = errc::timed_out;
        break;
    case ::grpc::UNAUTHENTICATED:
    case ::grpc::PERMISSION_DENIED:
        errc_code = errc::permission_denied;
        break;
    case ::grpc::UNIMPLEMENTED:
        errc_code = errc::function_not_supported;
        break;
    case ::grpc::NOT_FOUND:
    case ::grpc::ALREADY_EXISTS:
    case ::grpc::OUT_OF_RANGE:
    case ::grpc::RESOURCE_EXHAUSTED:
    case ::grpc::ABORTED:
    case ::grpc::INTERNAL:
    case ::grpc::UNAVAILABLE:
    case ::grpc::DATA_LOSS:
    default:
        errc_code = errc::io_error;
        break;
    }

    return raw_error_result(errc_code);
}


inline void *make_gateway_cqueue_tag(std::function<void(bool)> &&tag_handler_func)
{
    auto handler_ptr = std::make_unique<std::function<void(bool)>>(std::move(tag_handler_func));
    return reinterpret_cast<void *>(handler_ptr.release());
}


template<typename ReadRes, typename HandlerRes>
void finish_stream_reading(const std::shared_ptr<async_reader_context<ReadRes>> &ctx, result_handler<HandlerRes> &&res_handler)
{
    using std::move;

    auto shared_status = std::make_shared<::grpc::Status>();
    auto status_ptr = shared_status.get();
    ctx->reader->Finish(
        status_ptr,
        make_gateway_cqueue_tag(
            [ctx, shared_status = move(shared_status), res_handler = move(res_handler)](bool) {
                ctx->reader.reset();
                res_handler(
                    shared_status->ok()
                        ? raw_error_result(end_of_stream_errc)
                        : gateway_impl::make_error_result_from_grpc_status(*shared_status));
            }));
}


template<typename Res>
std::shared_ptr<request_handler<std::nullptr_t, Res>> make_read_stream_request_handler(
    event_loop &evloop, std::shared_ptr<std::shared_ptr<::grpc::CompletionQueue>> cqueue_ptr, std::shared_ptr<async_reader_context<Res>> ctx)
{
    using std::move;

    auto raw_req_handler = make_func_request_handler<std::nullptr_t, Res>(
        [&evloop, cqueue_ptr = move(cqueue_ptr), ctx = move(ctx)](auto, auto, auto &&res_handler) {
            if (!*cqueue_ptr) {
                invoke_result_handler_later(evloop, move(res_handler), raw_error_result(cqueue_closed_errc));
                return;
            } else if (!ctx->reader) {
                invoke_result_handler_later(evloop, move(res_handler), raw_error_result(end_of_stream_errc));
                return;
            }

            auto shared_res = std::make_shared<Res>();
            ctx->reader->Read(
                shared_res.get(),
                make_gateway_cqueue_tag(
                    [ctx, shared_res, res_handler = move(res_handler)](bool ok) mutable {
                        if (ok) {
                            res_handler(make_ok_result(move(*shared_res)));
                        } else {
                            finish_stream_reading(ctx, move(res_handler));
                        }
                    }));
        });

    return make_noconcurrent_request_handler(evloop, move(raw_req_handler));
}

}


inline gateway::gateway(
    event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
    std::shared_ptr<::grpc::ChannelInterface> channel)
    : evloop(evloop), channel(std::move(channel)),
    cqueue_ptr(std::make_shared<std::shared_ptr<::grpc::CompletionQueue>>(std::make_shared<::grpc::CompletionQueue>())),
    collector_handle()
{
    using std::move;

    auto collector_thread = std::thread(
        [res_handlers_executor = move(res_handlers_executor), cqueue = *cqueue_ptr]() {
            while (true) {
                void *received_tag = nullptr;
                bool ok = false;
                if (!cqueue->Next(&received_tag, &ok)) {
                    break;
                }

                if (received_tag != nullptr) {
                    auto handler = std::unique_ptr<std::function<void(bool)>>(reinterpret_cast<std::function<void(bool)> *>(received_tag));
                    res_handlers_executor(
                        [handler = std::move(*handler), ok]() {
                            handler(ok);
                        });
                }
            }
        });

    collector_handle = make_destroy_notifier(
        [cqueue_ptr = cqueue_ptr, collector_thread = std::make_shared<std::thread>(move(collector_thread))]() {
            auto cqueue = move(*cqueue_ptr);
            (*cqueue_ptr).reset();
            cqueue->Shutdown();
            collector_thread->join();
        });
}


template<typename ClientStub, typename Req, typename Res>
std::shared_ptr<request_handler<Req, Res>> gateway::make_call_request_handler(
    std::unique_ptr<::grpc::ClientAsyncResponseReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *)) const
{
    using namespace gateway_impl;
    using std::move;

    auto stub = std::make_shared<ClientStub>(channel);

    return make_func_request_handler<Req, Res>(
        [&evloop = evloop, cqueue_ptr = cqueue_ptr, stub = move(stub), method](Req &&req, eclock::duration timeout, result_handler<Res> &&res_handler) {
            if (!*cqueue_ptr) {
                invoke_result_handler_later(evloop, move(res_handler), raw_error_result(cqueue_closed_errc));
                return;
            }

            auto ctx = std::make_shared<async_call_context<Res>>();
            ctx->grpc_context.set_deadline(time_point_sat_add(std::chrono::system_clock::now(), timeout));

            auto rpc = ((*stub).*method)(&ctx->grpc_context, move(req), (*cqueue_ptr).get());
            rpc->Finish(
                &ctx->response, &ctx->grpc_status,
                make_gateway_cqueue_tag(
                    [ctx, res_handler = move(res_handler)](bool) {
                        res_handler(
                            ctx->grpc_status.ok()
                                ? make_ok_result(move(ctx->response))
                                : gateway_impl::make_error_result_from_grpc_status(ctx->grpc_status));
                    }));
        });
}


template<typename ClientStub, typename Req, typename Res>
std::shared_ptr<request_handler<Req, std::shared_ptr<request_handler<std::nullptr_t, Res>>>> gateway::make_open_read_stream_request_handler(
    std::unique_ptr<::grpc::ClientAsyncReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *, void*)) const
{
    using namespace gateway_impl;
    using std::move;

    auto stub = std::make_shared<ClientStub>(channel);

    return make_func_request_handler<Req, std::shared_ptr<request_handler<std::nullptr_t, Res>>>(
        [&evloop = evloop, cqueue_ptr = cqueue_ptr, stub = move(stub), method](auto &&req, auto timeout, auto &&res_handler) {
            if (!*cqueue_ptr) {
                invoke_result_handler_later(evloop, move(res_handler), raw_error_result(cqueue_closed_errc));
                return;
            }

            auto ctx = std::make_shared<async_reader_context<Res>>();
            ctx->grpc_context.set_deadline(time_point_sat_add(std::chrono::system_clock::now(), timeout));

            ctx->reader = ((*stub).*method)(
                &ctx->grpc_context, move(req), cqueue_ptr->get(),
                make_gateway_cqueue_tag(
                    [&evloop, cqueue_ptr, ctx, res_handler = move(res_handler)](bool ok) mutable {
                        if (ok) {
                            res_handler(
                                make_ok_result(
                                    make_read_stream_request_handler(
                                        evloop, move(cqueue_ptr), move(ctx))));
                        } else if (!*cqueue_ptr) {
                            res_handler(raw_error_result(cqueue_closed_errc));
                        } else {
                            finish_stream_reading(ctx, move(res_handler));
                        }
                    }));
        });
}


inline std::shared_ptr<gateway> make_gateway(
    event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
    std::shared_ptr<::grpc::ChannelInterface> channel)
{
    class gateway_impl : public gateway
    {
    public:
        gateway_impl(
            event_loop &evloop, std::function<void(std::function<void()>)> &&res_handlers_executor,
            std::shared_ptr<::grpc::ChannelInterface> &&channel)
            : gateway(evloop, std::move(res_handlers_executor), std::move(channel))
        {
        }
    };

    return std::make_shared<gateway_impl>(evloop, std::move(res_handlers_executor), std::move(channel));
}


template<typename ClientStub, typename Req, typename Res>
std::function<std::shared_ptr<request_handler<std::nullptr_t, Res>>(Req &&)> make_read_stream_factory_function(
    event_loop &evloop, const std::shared_ptr<gateway> &gateway,
    std::unique_ptr<::grpc::ClientAsyncReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *, void*))
{
    using namespace gateway_impl;
    using std::move;

    return [gateway_wptr = weak_from_shared(gateway), &evloop = evloop, method](Req &&request) {
        auto gateway = gateway_wptr.lock();
        if (!gateway) {
            return make_const_response_request_handler<std::nullptr_t, Res>(
                evloop, raw_error_result(gateway_destroyed_errc));
        }

        return make_lazy_init_request_handler(
            evloop,
            make_transforming_request_handler<std::nullptr_t, Req>(
                evloop,
                gateway->make_open_read_stream_request_handler(method),
                [request = move(request)](std::nullptr_t) mutable {
                    return make_ok_result(move(request));
                }));
    };
}


inline bool is_end_of_stream(const std::error_code &ec)
{
    using namespace gateway_impl;
    return ec == std::make_error_code(end_of_stream_errc);
}

}

#endif
