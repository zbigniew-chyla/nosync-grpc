#ifndef NOSYNC__GRPC__GATEWAY_H
#define NOSYNC__GRPC__GATEWAY_H

#include <functional>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <nosync/event-loop.h>
#include <nosync/interface-type.h>
#include <nosync/request-handler.h>
#include <system_error>


namespace nosync::grpc
{

class gateway : public interface_type
{
public:
    template<typename ClientStub, typename Req, typename Res>
    std::shared_ptr<request_handler<Req, Res>> make_call_request_handler(
        std::unique_ptr<::grpc::ClientAsyncResponseReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *)) const;

    template<typename ClientStub, typename Req, typename Res>
    std::shared_ptr<request_handler<Req, std::shared_ptr<request_handler<std::nullptr_t, Res>>>> make_open_read_stream_request_handler(
        std::unique_ptr<::grpc::ClientAsyncReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *, void*)) const;

protected:
    gateway(
        event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
        std::shared_ptr<::grpc::ChannelInterface> channel);

private:
    event_loop &evloop;
    std::shared_ptr<::grpc::ChannelInterface> channel;
    std::shared_ptr<std::shared_ptr<::grpc::CompletionQueue>> cqueue_ptr;
    std::shared_ptr<void> collector_handle;
};


std::shared_ptr<gateway> make_gateway(
    event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
    std::shared_ptr<::grpc::ChannelInterface> channel);


template<typename ClientStub, typename Req, typename Res>
std::function<std::shared_ptr<request_handler<std::nullptr_t, Res>>(Req &&)> make_read_stream_factory_function(
    event_loop &evloop, const std::shared_ptr<gateway> &gateway,
    std::unique_ptr<::grpc::ClientAsyncReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *, void*));


bool is_end_of_stream(const std::error_code &ec);

}

#include <nosync/grpc/gateway-impl.h>

#endif
