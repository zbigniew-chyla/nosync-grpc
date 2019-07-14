#ifndef NOSYNC__GRPC__GATEWAY_H
#define NOSYNC__GRPC__GATEWAY_H

#include <functional>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <nosync/event-loop.h>
#include <nosync/interface-type.h>
#include <nosync/request-handler.h>
#include <string>


namespace nosync::grpc
{

class gateway : public interface_type
{
public:
    gateway(
        event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
        std::shared_ptr<::grpc::ChannelInterface> channel);

    template<typename ClientStub, typename Req, typename Res>
    std::shared_ptr<request_handler<Req, Res>> make_request_handler(
        std::unique_ptr<::grpc::ClientAsyncResponseReader<Res>> (ClientStub::*method)(::grpc::ClientContext *, const Req &, ::grpc::CompletionQueue *));

private:
    event_loop &evloop;
    std::shared_ptr<::grpc::ChannelInterface> channel;
    std::shared_ptr<std::shared_ptr<::grpc::CompletionQueue>> cqueue_ptr;
    std::shared_ptr<void> collector_handle;
};


std::shared_ptr<gateway> make_gateway(
    event_loop &evloop, std::function<void(std::function<void()>)> res_handlers_executor,
    std::shared_ptr<::grpc::ChannelInterface> channel);

}

#include <nosync/grpc/gateway-impl.h>

#endif
