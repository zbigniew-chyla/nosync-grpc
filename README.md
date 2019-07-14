
## nosync::grpc - `nosync::request_handler<>` wrappers for gRPC client stubs

This is header-only implementation of class that can produce `nosync::request_handler<>`
objects wrapping gRPC async client stubs for calling gRPC service methods.

For example, given the following service:

    service PingService {
        rpc Ping(PingRequest) returns (PingResponse) {}
    }

... and `nosync::grpc::gateway` object named `gateway`, one can create
`nosync::request_handler<PingRequest, PingResponse>` object with the following call:

    auto ping_req_handler = gateway->make_request_handler(&PingService::Stub::AsyncPing)

... and then make gRPC calls using plain `nosync::request_handler<>` API.


Copyright (C) Zbigniew Chyla 2019
