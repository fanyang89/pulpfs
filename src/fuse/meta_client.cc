#include "fuse/meta_client.h"

#include <string_view>
#include <utility>

namespace pulpfs {
namespace {

Error FromGrpcStatus(const grpc::Status& status) {
    const std::string_view message = status.error_message();
    if (message.starts_with("NotDirectory:")) {
        return MakeError(NotDirectory{}, status.error_message());
    }
    if (message.starts_with("IsDirectory:")) {
        return MakeError(IsDirectory{}, status.error_message());
    }
    if (message.starts_with("DirectoryNotEmpty:")) {
        return MakeError(DirectoryNotEmpty{}, status.error_message());
    }

    switch (status.error_code()) {
        case grpc::StatusCode::INVALID_ARGUMENT:
            return MakeError(InvalidArgument{}, status.error_message());
        case grpc::StatusCode::NOT_FOUND:
            return MakeError(NotFound{}, status.error_message());
        case grpc::StatusCode::ALREADY_EXISTS:
            return MakeError(AlreadyExists{}, status.error_message());
        case grpc::StatusCode::FAILED_PRECONDITION:
            return MakeError(InvalidArgument{}, status.error_message());
        case grpc::StatusCode::UNAVAILABLE:
            return MakeError(Unavailable{}, status.error_message());
        case grpc::StatusCode::DEADLINE_EXCEEDED:
            return MakeError(Timeout{.operation = "metadata rpc"}, status.error_message());
        default:
            return MakeError(Internal{}, status.error_message());
    }
}

}  // namespace

MetaClient::MetaClient(std::string address) {
    channel_ = grpc::CreateChannel(std::move(address), grpc::InsecureChannelCredentials());
    stub_ = Meta::NewStub(channel_);
}

Result<InodeAttrs> MetaClient::Mkdir(
    uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
) {
    MkdirRequest request;
    request.set_parent_inode_id(parent_inode_id);
    request.set_name(name);
    request.set_mode(mode);
    request.set_uid(uid);
    request.set_gid(gid);

    MkdirReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Mkdir(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<InodeAttrs> MetaClient::CreateFile(
    uint64_t parent_inode_id, const std::string& name, uint32_t mode, uint32_t uid, uint32_t gid
) {
    CreateFileRequest request;
    request.set_parent_inode_id(parent_inode_id);
    request.set_name(name);
    request.set_mode(mode);
    request.set_uid(uid);
    request.set_gid(gid);

    CreateFileReply reply;
    grpc::ClientContext context;
    const auto status = stub_->CreateFile(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<InodeAttrs> MetaClient::Lookup(uint64_t parent_inode_id, const std::string& name) {
    LookupRequest request;
    request.set_parent_inode_id(parent_inode_id);
    request.set_name(name);

    LookupReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Lookup(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<InodeAttrs> MetaClient::GetAttr(uint64_t inode_id) {
    GetAttrRequest request;
    request.set_inode_id(inode_id);

    GetAttrReply reply;
    grpc::ClientContext context;
    const auto status = stub_->GetAttr(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<InodeAttrs> MetaClient::SetAttr(const SetAttrRequest& request) {
    SetAttrReply reply;
    grpc::ClientContext context;
    const auto status = stub_->SetAttr(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<std::vector<DirectoryEntry>> MetaClient::ReadDir(uint64_t inode_id) {
    ReadDirRequest request;
    request.set_inode_id(inode_id);

    ReadDirReply reply;
    grpc::ClientContext context;
    const auto status = stub_->ReadDir(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }

    std::vector<DirectoryEntry> entries;
    entries.reserve(reply.entries_size());
    for (const auto& entry : reply.entries()) {
        entries.push_back(entry);
    }
    return entries;
}

Result<void> MetaClient::Unlink(uint64_t parent_inode_id, const std::string& name) {
    UnlinkRequest request;
    request.set_parent_inode_id(parent_inode_id);
    request.set_name(name);

    UnlinkReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Unlink(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return {};
}

Result<void> MetaClient::Rmdir(uint64_t parent_inode_id, const std::string& name) {
    RmdirRequest request;
    request.set_parent_inode_id(parent_inode_id);
    request.set_name(name);

    RmdirReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Rmdir(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return {};
}

Result<void> MetaClient::Rename(
    uint64_t old_parent_inode_id, const std::string& old_name, uint64_t new_parent_inode_id,
    const std::string& new_name
) {
    RenameRequest request;
    request.set_old_parent_inode_id(old_parent_inode_id);
    request.set_old_name(old_name);
    request.set_new_parent_inode_id(new_parent_inode_id);
    request.set_new_name(new_name);

    RenameReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Rename(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return {};
}

Result<FileLayout> MetaClient::GetLayout(uint64_t inode_id) {
    GetLayoutRequest request;
    request.set_inode_id(inode_id);

    GetLayoutReply reply;
    grpc::ClientContext context;
    const auto status = stub_->GetLayout(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.layout();
}

Result<std::vector<std::string>> MetaClient::ListLiveObjects() {
    ListLiveObjectsRequest request;
    ListLiveObjectsReply reply;
    grpc::ClientContext context;
    const auto status = stub_->ListLiveObjects(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }

    std::vector<std::string> object_keys;
    object_keys.reserve(reply.object_keys_size());
    for (const auto& object_key : reply.object_keys()) {
        object_keys.push_back(object_key);
    }
    return object_keys;
}

Result<InodeAttrs> MetaClient::CommitWrite(
    uint64_t inode_id, uint64_t base_version, const std::vector<FileExtent>& extents
) {
    CommitWriteRequest request;
    request.set_inode_id(inode_id);
    request.set_base_version(base_version);
    for (const auto& extent : extents) {
        *request.add_extents() = extent;
    }

    CommitWriteReply reply;
    grpc::ClientContext context;
    const auto status = stub_->CommitWrite(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

Result<AppendWriteReply> MetaClient::AppendWrite(
    uint64_t inode_id, uint64_t base_version, const std::vector<FileExtent>& extents
) {
    AppendWriteRequest request;
    request.set_inode_id(inode_id);
    request.set_base_version(base_version);
    for (const auto& extent : extents) {
        *request.add_extents() = extent;
    }

    AppendWriteReply reply;
    grpc::ClientContext context;
    const auto status = stub_->AppendWrite(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply;
}

Result<InodeAttrs> MetaClient::Truncate(uint64_t inode_id, uint64_t size) {
    TruncateRequest request;
    request.set_inode_id(inode_id);
    request.set_size(size);

    TruncateReply reply;
    grpc::ClientContext context;
    const auto status = stub_->Truncate(&context, request, &reply);
    if (!status.ok()) {
        return std::unexpected(FromGrpcStatus(status));
    }
    return reply.attrs();
}

}  // namespace pulpfs
