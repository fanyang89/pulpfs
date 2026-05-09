#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "fuse/file_io.h"
#include "fuse/meta_client.h"
#include "object/object_store.h"

namespace pulpfs {

struct FuseServerConfig {
    std::string mountpoint;
    std::string meta_address;
    std::string bucket;
    std::string object_prefix = "pulpfs";
    std::string s3_endpoint;
    std::string s3_region = "us-east-1";
    bool s3_use_https = true;
    bool s3_verify_ssl = true;
    bool foreground = true;
};

class FuseServer {
  public:
    explicit FuseServer(FuseServerConfig config);

    int Run();

    [[nodiscard]] MetaClient& meta_client() { return meta_client_; }

    [[nodiscard]] ObjectStore* object_store() { return object_store_.get(); }

    [[nodiscard]] const FuseServerConfig& config() const { return config_; }

    [[nodiscard]] std::string MakeObjectKey(uint64_t inode_id, uint64_t offset);

    [[nodiscard]] uint64_t ParentOf(uint64_t inode_id);
    void RememberParent(uint64_t inode_id, uint64_t parent_inode_id);
    uint64_t AllocateFileHandle(FuseFileHandle handle);
    [[nodiscard]] std::shared_ptr<FuseFileHandle> GetFileHandle(uint64_t handle_id);
    void ReleaseFileHandle(uint64_t handle_id);

  private:
    FuseServerConfig config_;
    MetaClient meta_client_;
    std::unique_ptr<ObjectStore> object_store_;
    ObjectKeyGenerator object_key_generator_;
    std::mutex parent_mutex_;
    std::unordered_map<uint64_t, uint64_t> parents_;
    std::mutex handle_mutex_;
    uint64_t next_handle_id_ = 1;
    std::unordered_map<uint64_t, std::shared_ptr<FuseFileHandle>> handles_;
};

}  // namespace pulpfs
