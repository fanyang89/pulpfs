#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include <raftpp/raftor/raftor_config.h>

namespace grpc {
class Server;
}  // namespace grpc

namespace raftpp::raftor {
class Raftor;
}  // namespace raftpp::raftor

namespace pulpfs {

struct MetaServerConfig {
    std::string grpc_address;
    raftpp::raftor::RaftorConfig raftor;
};

class MetaServer {
  public:
    explicit MetaServer(MetaServerConfig config);
    ~MetaServer();

    MetaServer(const MetaServer&) = delete;
    MetaServer& operator=(const MetaServer&) = delete;

    int Run();
    void Stop();

  private:
    MetaServerConfig config_;
    std::unique_ptr<raftpp::raftor::Raftor> raftor_;
    std::jthread raftor_thread_;
    std::unique_ptr<grpc::Server> grpc_server_;
    std::atomic_bool stopping_{false};
};

}  // namespace pulpfs
