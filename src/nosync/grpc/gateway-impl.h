#ifndef NOSYNC__GRPC__GATEWAY_IMPL_H
#define NOSYNC__GRPC__GATEWAY_IMPL_H

#include <functional>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <nosync/destroy-notifier.h>
#include <nosync/event-loop.h>
#include <nosync/func-request-handler.h>
#include <nosync/request-handler.h>
#include <nosync/result-handler-utils.h>
#include <string>
#include <system_error>
#include <thread>
#include <utility>


namespace nosync::grpc
{

namespace gateway_impl
{

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

                if (ok && received_tag != nullptr) {
                    auto handler = std::unique_ptr<std::function<void()>>(reinterpret_cast<std::function<void()> *>(received_tag));
                    res_handlers_executor(move(*handler));
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
std::shared_ptr<request_handler<Req, Res>> gateway::make_request_handler(
    std::unique_ptr<::grpc::ClientAsyncResponseReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *))
{
    using std::move;

    return make_func_request_handler<Req, Res>(
        [&evloop = evloop, stub = std::make_shared<ClientStub>(channel), cqueue_ptr = cqueue_ptr, method](Req &&req, eclock::duration timeout, result_handler<Res> &&res_handler) {
            if (!*cqueue_ptr) {
                invoke_result_handler_later(evloop, move(res_handler), raw_error_result(std::errc::operation_canceled));
                return;
            }

            struct call_data
            {
                ::grpc::ClientContext context;
                ::grpc::Status status;
                Res response;
            };
            auto call_data_ptr = std::make_shared<call_data>();

            auto grpc_res_handler = std::make_unique<std::function<void()>>(
                [call_data_ptr, res_handler = move(res_handler)]() {
                    const auto &status = call_data_ptr->status;
                    res_handler(
                        status.ok()
                            ? make_ok_result(move(call_data_ptr->response))
                            : gateway_impl::make_error_result_from_grpc_status(status));
                });

            auto deadline = std::chrono::system_clock::now() + timeout;
            call_data_ptr->context.set_deadline(deadline);

            auto rpc = ((*stub).*method)(&call_data_ptr->context, move(req), (*cqueue_ptr).get());
            rpc->Finish(
                &call_data_ptr->response, &call_data_ptr->status,
                reinterpret_cast<void *>(grpc_res_handler.release()));
        });
}


std::shared_ptr<gateway> make_gateway(
    event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
    std::shared_ptr<::grpc::ChannelInterface> channel)
{
    return std::make_shared<gateway>(evloop, std::move(res_handlers_executor), std::move(channel));
}

}

#endif
