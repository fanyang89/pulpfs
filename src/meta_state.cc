#include "meta_state.h"

#include <algorithm>
#include <chrono>

namespace pulpfs {
namespace {

uint64_t ExtentEnd(const FileExtent& extent) {
    return extent.offset() + extent.length();
}

bool Overlaps(const FileExtent& lhs, const FileExtent& rhs) {
    return lhs.offset() < ExtentEnd(rhs) && rhs.offset() < ExtentEnd(lhs);
}

FileExtent SliceExtent(const FileExtent& extent, uint64_t offset, uint64_t length) {
    FileExtent sliced = extent;
    const uint64_t delta = offset - extent.offset();
    sliced.set_offset(offset);
    sliced.set_length(length);
    sliced.set_object_offset(extent.object_offset() + delta);
    return sliced;
}

void MergeExtent(
    google::protobuf::RepeatedPtrField<FileExtent>* stored_extents, const FileExtent& extent
) {
    std::vector<FileExtent> merged;
    merged.reserve(stored_extents->size() + 1);
    for (const auto& existing : *stored_extents) {
        if (!Overlaps(existing, extent)) {
            merged.push_back(existing);
            continue;
        }

        if (existing.offset() < extent.offset()) {
            merged.push_back(
                SliceExtent(existing, existing.offset(), extent.offset() - existing.offset())
            );
        }
        if (ExtentEnd(extent) < ExtentEnd(existing)) {
            merged.push_back(
                SliceExtent(existing, ExtentEnd(extent), ExtentEnd(existing) - ExtentEnd(extent))
            );
        }
    }
    merged.push_back(extent);
    std::sort(merged.begin(), merged.end(), [](const FileExtent& lhs, const FileExtent& rhs) {
        return lhs.offset() < rhs.offset();
    });

    stored_extents->Clear();
    for (const auto& merged_extent : merged) {
        *stored_extents->Add() = merged_extent;
    }
}

}  // namespace

MetaState::MetaState(uint64_t default_chunk_size) : default_chunk_size_(default_chunk_size) {
    const int64_t now_ns = NowNs();

    InodeAttrs root;
    root.set_inode_id(kRootInodeId);
    root.set_type(INODE_TYPE_DIRECTORY);
    root.set_mode(0755);
    root.set_uid(0);
    root.set_gid(0);
    root.set_size(0);
    root.set_nlink(2);
    root.set_version(1);
    root.set_atime_ns(now_ns);
    root.set_mtime_ns(now_ns);
    root.set_ctime_ns(now_ns);
    root.set_chunk_size(0);

    inodes_.emplace(kRootInodeId, std::move(root));
}

Result<InodeAttrs> MetaState::Mkdir(
    uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
) {
    return CreateNode(parent_inode_id, name, INODE_TYPE_DIRECTORY, mode, uid, gid);
}

Result<InodeAttrs> MetaState::CreateFile(
    uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
) {
    return CreateNode(parent_inode_id, name, INODE_TYPE_FILE, mode, uid, gid);
}

Result<InodeAttrs> MetaState::Lookup(uint64_t parent_inode_id, const std::string& name) const {
    if (auto result = ValidateParent(parent_inode_id, name); !result) {
        return std::unexpected(result.error());
    }

    auto dentry = dentries_.find({parent_inode_id, name});
    if (dentry == dentries_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = PathFor(parent_inode_id, name)}, "directory entry not found")
        );
    }

    return inodes_.at(dentry->second);
}

Result<InodeAttrs> MetaState::GetAttr(uint64_t inode_id) const {
    auto inode = inodes_.find(inode_id);
    if (inode == inodes_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "inode not found")
        );
    }
    return inode->second;
}

Result<InodeAttrs> MetaState::SetAttr(const SetAttrCommand& command) {
    auto inode = inodes_.find(command.inode_id());
    if (inode == inodes_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(command.inode_id())}, "inode not found")
        );
    }

    if (command.set_mode()) {
        inode->second.set_mode(command.mode());
    }
    if (command.set_uid()) {
        inode->second.set_uid(command.uid());
    }
    if (command.set_gid()) {
        inode->second.set_gid(command.gid());
    }
    if (command.set_atime()) {
        inode->second.set_atime_ns(command.atime_ns());
    }
    if (command.set_mtime()) {
        inode->second.set_mtime_ns(command.mtime_ns());
    }

    inode->second.set_ctime_ns(NowNs());
    inode->second.set_version(inode->second.version() + 1);
    if (auto layout = layouts_.find(command.inode_id()); layout != layouts_.end()) {
        *layout->second.mutable_attrs() = inode->second;
    }
    return inode->second;
}

Result<std::vector<DirectoryEntry>> MetaState::ReadDir(uint64_t inode_id) const {
    auto inode = inodes_.find(inode_id);
    if (inode == inodes_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "inode not found")
        );
    }
    if (inode->second.type() != INODE_TYPE_DIRECTORY) {
        return std::unexpected(
            MakeError(NotDirectory{.path = std::to_string(inode_id)}, "inode is not a directory")
        );
    }

    std::vector<DirectoryEntry> entries;
    for (const auto& [key, child_inode_id] : dentries_) {
        if (key.first != inode_id) {
            continue;
        }

        DirectoryEntry entry;
        entry.set_name(key.second);
        *entry.mutable_attrs() = inodes_.at(child_inode_id);
        entries.push_back(std::move(entry));
    }
    return entries;
}

Result<InodeAttrs> MetaState::Unlink(uint64_t parent_inode_id, const std::string& name) {
    if (auto result = ValidateParent(parent_inode_id, name); !result) {
        return std::unexpected(result.error());
    }

    const auto dentry_key = DentryKey{parent_inode_id, name};
    auto dentry = dentries_.find(dentry_key);
    if (dentry == dentries_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = PathFor(parent_inode_id, name)}, "directory entry not found")
        );
    }

    const uint64_t inode_id = dentry->second;
    const auto& inode = inodes_.at(inode_id);
    if (inode.type() == INODE_TYPE_DIRECTORY) {
        return std::unexpected(MakeError(
            IsDirectory{.path = PathFor(parent_inode_id, name)}, "cannot unlink directory"
        ));
    }

    RemoveNode(parent_inode_id, name, inode_id);
    return inodes_.at(parent_inode_id);
}

Result<InodeAttrs> MetaState::Rmdir(uint64_t parent_inode_id, const std::string& name) {
    if (auto result = ValidateParent(parent_inode_id, name); !result) {
        return std::unexpected(result.error());
    }

    const auto dentry_key = DentryKey{parent_inode_id, name};
    auto dentry = dentries_.find(dentry_key);
    if (dentry == dentries_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = PathFor(parent_inode_id, name)}, "directory entry not found")
        );
    }

    const uint64_t inode_id = dentry->second;
    const auto& inode = inodes_.at(inode_id);
    if (inode.type() != INODE_TYPE_DIRECTORY) {
        return std::unexpected(MakeError(
            NotDirectory{.path = PathFor(parent_inode_id, name)}, "cannot remove non-directory"
        ));
    }
    if (!IsDirectoryEmpty(inode_id)) {
        return std::unexpected(MakeError(
            DirectoryNotEmpty{.path = PathFor(parent_inode_id, name)}, "directory is not empty"
        ));
    }

    RemoveNode(parent_inode_id, name, inode_id);
    return inodes_.at(parent_inode_id);
}

Result<InodeAttrs> MetaState::Rename(
    uint64_t old_parent_inode_id, const std::string& old_name, uint64_t new_parent_inode_id,
    const std::string& new_name
) {
    if (auto result = ValidateParent(old_parent_inode_id, old_name); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = ValidateParent(new_parent_inode_id, new_name); !result) {
        return std::unexpected(result.error());
    }

    const auto old_key = DentryKey{old_parent_inode_id, old_name};
    auto old_dentry = dentries_.find(old_key);
    if (old_dentry == dentries_.end()) {
        return std::unexpected(MakeError(
            NotFound{.path = PathFor(old_parent_inode_id, old_name)}, "source entry not found"
        ));
    }

    const uint64_t moved_inode_id = old_dentry->second;
    const auto& moved_inode = inodes_.at(moved_inode_id);
    if (moved_inode.type() == INODE_TYPE_DIRECTORY &&
        IsAncestor(moved_inode_id, new_parent_inode_id)) {
        return std::unexpected(
            MakeError(InvalidArgument{}, "cannot move directory into itself or its descendant")
        );
    }

    const auto new_key = DentryKey{new_parent_inode_id, new_name};
    auto existing_target = dentries_.find(new_key);
    if (existing_target != dentries_.end() && existing_target->second == moved_inode_id) {
        return moved_inode;
    }

    if (existing_target != dentries_.end()) {
        const uint64_t target_inode_id = existing_target->second;
        const auto& target_inode = inodes_.at(target_inode_id);
        if (moved_inode.type() == INODE_TYPE_DIRECTORY &&
            target_inode.type() != INODE_TYPE_DIRECTORY) {
            return std::unexpected(MakeError(
                NotDirectory{.path = PathFor(new_parent_inode_id, new_name)},
                "cannot replace non-directory with directory"
            ));
        }
        if (moved_inode.type() != INODE_TYPE_DIRECTORY &&
            target_inode.type() == INODE_TYPE_DIRECTORY) {
            return std::unexpected(MakeError(
                IsDirectory{.path = PathFor(new_parent_inode_id, new_name)},
                "cannot replace directory with non-directory"
            ));
        }
        if (target_inode.type() == INODE_TYPE_DIRECTORY && !IsDirectoryEmpty(target_inode_id)) {
            return std::unexpected(MakeError(
                DirectoryNotEmpty{.path = PathFor(new_parent_inode_id, new_name)},
                "target directory is not empty"
            ));
        }

        RemoveNode(new_parent_inode_id, new_name, target_inode_id);
    }

    dentries_.erase(old_key);
    dentries_.emplace(new_key, moved_inode_id);

    const int64_t now_ns = NowNs();
    TouchInode(old_parent_inode_id, now_ns);
    if (old_parent_inode_id != new_parent_inode_id) {
        TouchInode(new_parent_inode_id, now_ns);
        if (moved_inode.type() == INODE_TYPE_DIRECTORY) {
            auto& old_parent = inodes_.at(old_parent_inode_id);
            auto& new_parent = inodes_.at(new_parent_inode_id);
            old_parent.set_nlink(old_parent.nlink() - 1);
            new_parent.set_nlink(new_parent.nlink() + 1);
        }
    }

    auto& moved = inodes_.at(moved_inode_id);
    moved.set_ctime_ns(now_ns);
    moved.set_version(moved.version() + 1);
    if (auto layout = layouts_.find(moved_inode_id); layout != layouts_.end()) {
        *layout->second.mutable_attrs() = moved;
    }
    return moved;
}

Result<FileLayout> MetaState::GetLayout(uint64_t inode_id) const {
    if (auto result = ValidateRegularFile(inode_id); !result) {
        return std::unexpected(result.error());
    }

    auto layout = layouts_.find(inode_id);
    if (layout == layouts_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "file layout not found")
        );
    }

    FileLayout result = layout->second;
    *result.mutable_attrs() = inodes_.at(inode_id);
    return result;
}

std::vector<std::string> MetaState::ListLiveObjects() const {
    std::vector<std::string> object_keys;
    for (const auto& [_, layout] : layouts_) {
        for (const auto& extent : layout.extents()) {
            if (!extent.object_key().empty()) {
                object_keys.push_back(extent.object_key());
            }
        }
    }
    std::sort(object_keys.begin(), object_keys.end());
    object_keys.erase(std::unique(object_keys.begin(), object_keys.end()), object_keys.end());
    return object_keys;
}

Result<InodeAttrs> MetaState::CommitWrite(
    uint64_t inode_id, uint64_t base_version,
    const google::protobuf::RepeatedPtrField<FileExtent>& extents
) {
    (void)base_version;

    if (auto result = ValidateRegularFile(inode_id); !result) {
        return std::unexpected(result.error());
    }

    auto layout = layouts_.find(inode_id);
    if (layout == layouts_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "file layout not found")
        );
    }

    auto* stored_extents = layout->second.mutable_extents();
    for (const auto& extent : extents) {
        if (extent.length() == 0 || extent.object_key().empty()) {
            return std::unexpected(MakeError(InvalidArgument{}, "invalid committed extent"));
        }
        MergeExtent(stored_extents, extent);
    }

    auto& inode = inodes_.at(inode_id);
    const int64_t now_ns = NowNs();
    uint64_t size = inode.size();
    for (const auto& extent : extents) {
        size = std::max(size, ExtentEnd(extent));
    }
    inode.set_size(size);
    inode.set_version(inode.version() + 1);
    inode.set_mtime_ns(now_ns);
    inode.set_ctime_ns(now_ns);
    *layout->second.mutable_attrs() = inode;
    return inode;
}

Result<AppendWriteStateResult> MetaState::AppendWrite(
    uint64_t inode_id, uint64_t base_version,
    const google::protobuf::RepeatedPtrField<FileExtent>& extents
) {
    (void)base_version;

    if (auto result = ValidateRegularFile(inode_id); !result) {
        return std::unexpected(result.error());
    }

    auto layout = layouts_.find(inode_id);
    if (layout == layouts_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "file layout not found")
        );
    }

    auto& inode = inodes_.at(inode_id);
    const uint64_t append_start = inode.size();
    uint64_t append_length = 0;
    std::vector<FileExtent> committed_extents;
    committed_extents.reserve(extents.size());

    auto* stored_extents = layout->second.mutable_extents();
    for (const auto& extent : extents) {
        if (extent.length() == 0 || extent.object_key().empty()) {
            return std::unexpected(MakeError(InvalidArgument{}, "invalid appended extent"));
        }

        append_length = std::max(append_length, ExtentEnd(extent));
        FileExtent committed = extent;
        committed.set_offset(append_start + extent.offset());
        MergeExtent(stored_extents, committed);
        committed_extents.push_back(std::move(committed));
    }

    const int64_t now_ns = NowNs();
    inode.set_size(append_start + append_length);
    inode.set_version(inode.version() + 1);
    inode.set_mtime_ns(now_ns);
    inode.set_ctime_ns(now_ns);
    *layout->second.mutable_attrs() = inode;

    return AppendWriteStateResult{
        .attrs = inode,
        .append_start = append_start,
        .committed_extents = std::move(committed_extents),
    };
}

Result<InodeAttrs> MetaState::Truncate(uint64_t inode_id, uint64_t size) {
    if (auto result = ValidateRegularFile(inode_id); !result) {
        return std::unexpected(result.error());
    }

    auto layout = layouts_.find(inode_id);
    if (layout == layouts_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "file layout not found")
        );
    }

    std::vector<FileExtent> kept;
    for (const auto& extent : layout->second.extents()) {
        if (extent.offset() >= size) {
            continue;
        }
        if (ExtentEnd(extent) > size) {
            kept.push_back(SliceExtent(extent, extent.offset(), size - extent.offset()));
        } else {
            kept.push_back(extent);
        }
    }

    auto* stored_extents = layout->second.mutable_extents();
    stored_extents->Clear();
    for (const auto& extent : kept) {
        *stored_extents->Add() = extent;
    }

    auto& inode = inodes_.at(inode_id);
    const int64_t now_ns = NowNs();
    inode.set_size(size);
    inode.set_version(inode.version() + 1);
    inode.set_mtime_ns(now_ns);
    inode.set_ctime_ns(now_ns);
    *layout->second.mutable_attrs() = inode;
    return inode;
}

std::string MetaState::SerializeSnapshot() const {
    MetaSnapshot snapshot;
    snapshot.set_next_inode_id(next_inode_id_);
    snapshot.set_default_chunk_size(default_chunk_size_);

    for (const auto& [_, inode] : inodes_) {
        *snapshot.add_inodes() = inode;
    }
    for (const auto& [key, child_inode_id] : dentries_) {
        auto* dentry = snapshot.add_dentries();
        dentry->set_parent_inode_id(key.first);
        dentry->set_name(key.second);
        dentry->set_child_inode_id(child_inode_id);
    }
    for (const auto& [inode_id, layout] : layouts_) {
        auto* snapshot_layout = snapshot.add_layouts();
        snapshot_layout->set_inode_id(inode_id);
        *snapshot_layout->mutable_layout() = layout;
    }

    return snapshot.SerializeAsString();
}

Result<void> MetaState::RestoreSnapshot(const std::string& data) {
    MetaSnapshot snapshot;
    if (!snapshot.ParseFromString(data)) {
        return std::unexpected(MakeError(InvalidArgument{}, "failed to parse metadata snapshot"));
    }

    std::map<uint64_t, InodeAttrs> inodes;
    std::map<DentryKey, uint64_t> dentries;
    std::map<uint64_t, FileLayout> layouts;

    for (const auto& inode : snapshot.inodes()) {
        inodes.emplace(inode.inode_id(), inode);
    }
    if (!inodes.contains(kRootInodeId)) {
        return std::unexpected(MakeError(InvalidArgument{}, "metadata snapshot has no root inode"));
    }

    for (const auto& dentry : snapshot.dentries()) {
        if (!inodes.contains(dentry.parent_inode_id()) ||
            !inodes.contains(dentry.child_inode_id())) {
            return std::unexpected(
                MakeError(InvalidArgument{}, "metadata snapshot has dangling dentry")
            );
        }
        dentries.emplace(
            DentryKey{dentry.parent_inode_id(), dentry.name()}, dentry.child_inode_id()
        );
    }

    for (const auto& snapshot_layout : snapshot.layouts()) {
        if (!inodes.contains(snapshot_layout.inode_id())) {
            return std::unexpected(
                MakeError(InvalidArgument{}, "metadata snapshot has dangling layout")
            );
        }
        layouts.emplace(snapshot_layout.inode_id(), snapshot_layout.layout());
    }

    next_inode_id_ = snapshot.next_inode_id();
    default_chunk_size_ = snapshot.default_chunk_size();
    inodes_ = std::move(inodes);
    dentries_ = std::move(dentries);
    layouts_ = std::move(layouts);
    return {};
}

Result<void> MetaState::ValidateName(const std::string& name) const {
    if (name.empty()) {
        return std::unexpected(MakeError(InvalidArgument{}, "entry name is empty"));
    }
    if (name == "." || name == "..") {
        return std::unexpected(MakeError(InvalidArgument{}, "reserved directory entry name"));
    }
    if (name.find('/') != std::string::npos || name.find('\0') != std::string::npos) {
        return std::unexpected(
            MakeError(InvalidArgument{}, "entry name contains an invalid character")
        );
    }
    return {};
}

Result<void> MetaState::ValidateParent(uint64_t parent_inode_id, const std::string& name) const {
    if (auto result = ValidateName(name); !result) {
        return result;
    }

    auto parent = inodes_.find(parent_inode_id);
    if (parent == inodes_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(parent_inode_id)}, "parent inode not found")
        );
    }
    if (parent->second.type() != INODE_TYPE_DIRECTORY) {
        return std::unexpected(MakeError(
            NotDirectory{.path = std::to_string(parent_inode_id)}, "parent inode is not a directory"
        ));
    }
    return {};
}

Result<InodeAttrs> MetaState::CreateNode(
    uint64_t parent_inode_id, const std::string& name, InodeType type, uint32_t mode, uint32_t uid,
    uint32_t gid
) {
    if (auto result = ValidateParent(parent_inode_id, name); !result) {
        return std::unexpected(result.error());
    }

    const auto dentry_key = DentryKey{parent_inode_id, name};
    if (dentries_.contains(dentry_key)) {
        return std::unexpected(MakeError(
            AlreadyExists{.path = PathFor(parent_inode_id, name)}, "directory entry already exists"
        ));
    }

    const uint64_t inode_id = next_inode_id_++;
    const int64_t now_ns = NowNs();

    InodeAttrs inode;
    inode.set_inode_id(inode_id);
    inode.set_type(type);
    inode.set_mode(mode);
    inode.set_uid(uid);
    inode.set_gid(gid);
    inode.set_size(0);
    inode.set_nlink(type == INODE_TYPE_DIRECTORY ? 2 : 1);
    inode.set_version(1);
    inode.set_atime_ns(now_ns);
    inode.set_mtime_ns(now_ns);
    inode.set_ctime_ns(now_ns);
    inode.set_chunk_size(type == INODE_TYPE_FILE ? default_chunk_size_ : 0);

    auto& parent = inodes_.at(parent_inode_id);
    parent.set_mtime_ns(now_ns);
    parent.set_ctime_ns(now_ns);
    parent.set_version(parent.version() + 1);
    if (type == INODE_TYPE_DIRECTORY) {
        parent.set_nlink(parent.nlink() + 1);
    }

    dentries_.emplace(dentry_key, inode_id);
    inodes_.emplace(inode_id, inode);
    if (type == INODE_TYPE_FILE) {
        FileLayout layout;
        *layout.mutable_attrs() = inode;
        layout.set_chunk_size(default_chunk_size_);
        layouts_.emplace(inode_id, std::move(layout));
    }
    return inode;
}

Result<void> MetaState::ValidateRegularFile(uint64_t inode_id) const {
    auto inode = inodes_.find(inode_id);
    if (inode == inodes_.end()) {
        return std::unexpected(
            MakeError(NotFound{.path = std::to_string(inode_id)}, "inode not found")
        );
    }
    if (inode->second.type() != INODE_TYPE_FILE) {
        return std::unexpected(MakeError(InvalidArgument{}, "inode is not a regular file"));
    }
    return {};
}

bool MetaState::IsDirectoryEmpty(uint64_t inode_id) const {
    for (const auto& [key, _] : dentries_) {
        if (key.first == inode_id) {
            return false;
        }
    }
    return true;
}

bool MetaState::IsAncestor(uint64_t ancestor_inode_id, uint64_t inode_id) const {
    uint64_t current = inode_id;
    while (current != kRootInodeId) {
        if (current == ancestor_inode_id) {
            return true;
        }
        const uint64_t parent = ParentOf(current);
        if (parent == current) {
            return false;
        }
        current = parent;
    }
    return ancestor_inode_id == kRootInodeId;
}

uint64_t MetaState::ParentOf(uint64_t inode_id) const {
    if (inode_id == kRootInodeId) {
        return kRootInodeId;
    }
    for (const auto& [key, child_inode_id] : dentries_) {
        if (child_inode_id == inode_id) {
            return key.first;
        }
    }
    return inode_id;
}

void MetaState::TouchInode(uint64_t inode_id, int64_t now_ns) {
    auto& inode = inodes_.at(inode_id);
    inode.set_mtime_ns(now_ns);
    inode.set_ctime_ns(now_ns);
    inode.set_version(inode.version() + 1);
    if (auto layout = layouts_.find(inode_id); layout != layouts_.end()) {
        *layout->second.mutable_attrs() = inode;
    }
}

void MetaState::RemoveNode(uint64_t parent_inode_id, const std::string& name, uint64_t inode_id) {
    const auto type = inodes_.at(inode_id).type();
    dentries_.erase(DentryKey{parent_inode_id, name});
    layouts_.erase(inode_id);
    inodes_.erase(inode_id);

    const int64_t now_ns = NowNs();
    TouchInode(parent_inode_id, now_ns);
    if (type == INODE_TYPE_DIRECTORY) {
        auto& parent = inodes_.at(parent_inode_id);
        parent.set_nlink(parent.nlink() - 1);
    }
}

int64_t MetaState::NowNs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string MetaState::PathFor(uint64_t parent_inode_id, const std::string& name) {
    return std::to_string(parent_inode_id) + "/" + name;
}

}  // namespace pulpfs
