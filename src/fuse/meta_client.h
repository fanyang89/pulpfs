#pragma once

#include <memory>
#include <string>
#include <vector>

#include <grpc++/grpc++.h>

#include "error.h"
#include "pulpfs.grpc.pb.h"

namespace pulpfs {

class MetaClient {
  public:
    explicit MetaClient(std::string address);

    [[nodiscard]] Result<InodeAttrs> Mkdir(
        uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
    );
    [[nodiscard]] Result<InodeAttrs> CreateFile(
        uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
    );
    [[nodiscard]] Result<InodeAttrs> Lookup(uint64_t parent_inode_id, const std::string& name);
    [[nodiscard]] Result<InodeAttrs> GetAttr(uint64_t inode_id);
    [[nodiscard]] Result<InodeAttrs> SetAttr(const SetAttrRequest& request);
    [[nodiscard]] Result<std::vector<DirectoryEntry>> ReadDir(uint64_t inode_id);
    [[nodiscard]] Result<void> Unlink(uint64_t parent_inode_id, const std::string& name);
    [[nodiscard]] Result<void> Rmdir(uint64_t parent_inode_id, const std::string& name);
    [[nodiscard]] Result<void> Rename(
        uint64_t old_parent_inode_id, const std::string& old_name, uint64_t new_parent_inode_id,
        const std::string& new_name
    );
    [[nodiscard]] Result<FileLayout> GetLayout(uint64_t inode_id);
    [[nodiscard]] Result<std::vector<std::string>> ListLiveObjects();
    [[nodiscard]] Result<InodeAttrs> CommitWrite(
        uint64_t inode_id, uint64_t base_version, const std::vector<FileExtent>& extents
    );
    [[nodiscard]] Result<AppendWriteReply> AppendWrite(
        uint64_t inode_id, uint64_t base_version, const std::vector<FileExtent>& extents
    );
    [[nodiscard]] Result<InodeAttrs> Truncate(uint64_t inode_id, uint64_t size);

  private:
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<Meta::Stub> stub_;
};

}  // namespace pulpfs
