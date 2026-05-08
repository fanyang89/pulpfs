#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "error.h"
#include "pulpfs.pb.h"

namespace pulpfs {

struct AppendWriteStateResult {
    InodeAttrs attrs;
    uint64_t append_start = 0;
    std::vector<FileExtent> committed_extents;
};

class MetaState {
  public:
    static constexpr uint64_t kRootInodeId = 1;
    static constexpr uint64_t kDefaultChunkSize = 64 * 1024 * 1024;

    explicit MetaState(uint64_t default_chunk_size = kDefaultChunkSize);

    [[nodiscard]] Result<InodeAttrs> Mkdir(
        uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
    );
    [[nodiscard]] Result<InodeAttrs> CreateFile(
        uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
    );
    [[nodiscard]] Result<InodeAttrs> Lookup(
        uint64_t parent_inode_id, const std::string& name
    ) const;
    [[nodiscard]] Result<InodeAttrs> GetAttr(uint64_t inode_id) const;
    [[nodiscard]] Result<InodeAttrs> SetAttr(const SetAttrCommand& command);
    [[nodiscard]] Result<std::vector<DirectoryEntry>> ReadDir(uint64_t inode_id) const;
    [[nodiscard]] Result<InodeAttrs> Unlink(uint64_t parent_inode_id, const std::string& name);
    [[nodiscard]] Result<InodeAttrs> Rmdir(uint64_t parent_inode_id, const std::string& name);
    [[nodiscard]] Result<InodeAttrs> Rename(
        uint64_t old_parent_inode_id, const std::string& old_name, uint64_t new_parent_inode_id,
        const std::string& new_name
    );
    [[nodiscard]] Result<FileLayout> GetLayout(uint64_t inode_id) const;
    [[nodiscard]] std::vector<std::string> ListLiveObjects() const;
    [[nodiscard]] Result<InodeAttrs> CommitWrite(
        uint64_t inode_id, uint64_t base_version,
        const google::protobuf::RepeatedPtrField<FileExtent>& extents
    );
    [[nodiscard]] Result<AppendWriteStateResult> AppendWrite(
        uint64_t inode_id, uint64_t base_version,
        const google::protobuf::RepeatedPtrField<FileExtent>& extents
    );
    [[nodiscard]] Result<InodeAttrs> Truncate(uint64_t inode_id, uint64_t size);

    [[nodiscard]] std::string SerializeSnapshot() const;
    [[nodiscard]] Result<void> RestoreSnapshot(const std::string& data);

  private:
    using DentryKey = std::pair<uint64_t, std::string>;

    [[nodiscard]] Result<void> ValidateName(const std::string& name) const;
    [[nodiscard]] Result<void> ValidateParent(
        uint64_t parent_inode_id, const std::string& name
    ) const;
    [[nodiscard]] Result<InodeAttrs> CreateNode(
        uint64_t parent_inode_id, const std::string& name, InodeType type, uint32_t mode,
        uint32_t uid, uint32_t gid
    );
    [[nodiscard]] Result<void> ValidateRegularFile(uint64_t inode_id) const;
    [[nodiscard]] bool IsDirectoryEmpty(uint64_t inode_id) const;
    [[nodiscard]] bool IsAncestor(uint64_t ancestor_inode_id, uint64_t inode_id) const;
    [[nodiscard]] uint64_t ParentOf(uint64_t inode_id) const;
    void TouchInode(uint64_t inode_id, int64_t now_ns);
    void RemoveNode(uint64_t parent_inode_id, const std::string& name, uint64_t inode_id);
    [[nodiscard]] static int64_t NowNs();
    [[nodiscard]] static std::string PathFor(uint64_t parent_inode_id, const std::string& name);

    uint64_t next_inode_id_ = kRootInodeId + 1;
    uint64_t default_chunk_size_ = kDefaultChunkSize;
    std::map<uint64_t, InodeAttrs> inodes_;
    std::map<DentryKey, uint64_t> dentries_;
    std::map<uint64_t, FileLayout> layouts_;
};

}  // namespace pulpfs
