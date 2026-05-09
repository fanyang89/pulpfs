#include "meta_server.h"

#include <memory>
#include <string>
#include <utility>

#include <grpc++/grpc++.h>
#include <raftpp/raftor/raftor.h>

#include "error.h"
#include "logging.h"
#include "meta_service.h"
#include "pulpfs.grpc.pb.h"

namespace pulpfs {

MetaServer::MetaServer(MetaServerConfig config) : config_(std::move(config)) {}

MetaServer::~MetaServer() {
    Stop();
}

int MetaServer::Run() {
    PULPFS_LOG_INFO(
        "starting meta server: grpc_addr={}, raft_addr={}, node_id={}, data_dir={}",
        config_.grpc_address, config_.raftor.listen_addr, config_.raftor.node_id,
        config_.raftor.data_dir.string()
    );

    auto meta_service = std::make_unique<MetaService>();
    auto* meta_service_ptr = meta_service.get();

    auto raftor_result = raftpp::raftor::Raftor::Create(config_.raftor, std::move(meta_service));
    if (!raftor_result) {
        PULPFS_LOG_ERROR("failed to create raftor: {}", raftor_result.error().ToString());
        return -1;
    }

    raftor_ = std::move(*raftor_result);
    meta_service_ptr->SetRaftor(raftor_.get());

    if (auto result = raftor_->Start(); !result) {
        PULPFS_LOG_ERROR("failed to start raftor: {}", result.error().ToString());
        return -1;
    }

    raftor_thread_ = std::jthread([raftor = raftor_.get()] { raftor->Run(); });

    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.grpc_address, grpc::InsecureServerCredentials());
    builder.RegisterService(meta_service_ptr);

    grpc_server_ = builder.BuildAndStart();
    if (grpc_server_ == nullptr) {
        PULPFS_LOG_ERROR("failed to start grpc server on {}", config_.grpc_address);
        return -1;
    }

    PULPFS_LOG_INFO("meta grpc server listening on {}", config_.grpc_address);
    grpc_server_->Wait();
    Stop();
    PULPFS_LOG_INFO("meta grpc server stopped");

    return 0;
}

void MetaServer::Stop() {
    if (stopping_.exchange(true)) {
        return;
    }

    if (grpc_server_ != nullptr) {
        grpc_server_->Shutdown();
    }

    if (raftor_ != nullptr) {
        raftor_->Stop();
    }

    if (raftor_thread_.joinable()) {
        raftor_thread_.join();
    }

    grpc_server_.reset();
    raftor_.reset();
}

}  // namespace pulpfs
