#include "meta_service.h"

#include <array>
#include <string>
#include <type_traits>
#include <utility>

#include <nonstd/expected.hpp>
#include <nonstd/span.hpp>

#include "raftpp/core/capnp_util.h"
#include "raftpp/core/error.h"

namespace pulpfs {
namespace {

std::string ErrorKindName(const ErrorKind& kind) {
    return std::visit(
        []<typename T>(const T&) -> std::string {
            if constexpr (std::is_same_v<T, InvalidArgument>) {
                return "InvalidArgument";
            } else if constexpr (std::is_same_v<T, NotFound>) {
                return "NotFound";
            } else if constexpr (std::is_same_v<T, AlreadyExists>) {
                return "AlreadyExists";
            } else if constexpr (std::is_same_v<T, NotDirectory>) {
                return "NotDirectory";
            } else if constexpr (std::is_same_v<T, IsDirectory>) {
                return "IsDirectory";
            } else if constexpr (std::is_same_v<T, DirectoryNotEmpty>) {
                return "DirectoryNotEmpty";
            } else if constexpr (std::is_same_v<T, NotLeader>) {
                return "NotLeader";
            } else if constexpr (std::is_same_v<T, Unavailable>) {
                return "Unavailable";
            } else if constexpr (std::is_same_v<T, Timeout>) {
                return "Timeout";
            } else if constexpr (std::is_same_v<T, Internal>) {
                return "Internal";
            } else if constexpr (std::is_same_v<T, RaftFailure>) {
                return "RaftFailure";
            }
            return "Internal";
        },
        kind
    );
}

std::string ErrorPath(const ErrorKind& kind) {
    return std::visit(
        []<typename T>(const T& value) -> std::string {
            if constexpr (requires { value.path; }) {
                return value.path;
            }
            return {};
        },
        kind
    );
}

Error ErrorFromResponse(const ErrorResponse& response) {
    const std::string& kind = response.kind();
    if (kind == "InvalidArgument") {
        return MakeError(InvalidArgument{}, response.message());
    }
    if (kind == "NotFound") {
        return MakeError(NotFound{.path = response.path()}, response.message());
    }
    if (kind == "AlreadyExists") {
        return MakeError(AlreadyExists{.path = response.path()}, response.message());
    }
    if (kind == "NotDirectory") {
        return MakeError(NotDirectory{.path = response.path()}, response.message());
    }
    if (kind == "IsDirectory") {
        return MakeError(IsDirectory{.path = response.path()}, response.message());
    }
    if (kind == "DirectoryNotEmpty") {
        return MakeError(DirectoryNotEmpty{.path = response.path()}, response.message());
    }
    return MakeError(Internal{}, response.message());
}

RaftCommandResponse MakeErrorResponse(const Error& error) {
    RaftCommandResponse response;
    auto* error_response = response.mutable_error();
    error_response->set_kind(ErrorKindName(error.kind));
    error_response->set_message(error.message);
    error_response->set_path(ErrorPath(error.kind));
    return response;
}

raftpp::RaftError SnapshotError(const Error& error) {
    return raftpp::RaftError(raftpp::StorageErrorOther{ToString(error)});
}

}  // namespace

grpc::Status MetaService::Mkdir(
    grpc::ServerContext* context, const MkdirRequest* request, MkdirReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* mkdir = command.mutable_mkdir();
    mkdir->set_parent_inode_id(request->parent_inode_id());
    mkdir->set_name(request->name());
    mkdir->set_mode(request->mode());
    mkdir->set_uid(request->uid());
    mkdir->set_gid(request->gid());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::CreateFile(
    grpc::ServerContext* context, const CreateFileRequest* request, CreateFileReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* create_file = command.mutable_create_file();
    create_file->set_parent_inode_id(request->parent_inode_id());
    create_file->set_name(request->name());
    create_file->set_mode(request->mode());
    create_file->set_uid(request->uid());
    create_file->set_gid(request->gid());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::Lookup(
    grpc::ServerContext* context, const LookupRequest* request, LookupReply* reply
) {
    (void)context;

    if (auto result = LinearizableRead("lookup"); !result) {
        return ToGrpcStatus(result.error());
    }

    std::shared_lock lock(mutex_);
    auto result = state_.Lookup(request->parent_inode_id(), request->name());
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::GetAttr(
    grpc::ServerContext* context, const GetAttrRequest* request, GetAttrReply* reply
) {
    (void)context;

    if (auto result = LinearizableRead("getattr"); !result) {
        return ToGrpcStatus(result.error());
    }

    std::shared_lock lock(mutex_);
    auto result = state_.GetAttr(request->inode_id());
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::SetAttr(
    grpc::ServerContext* context, const SetAttrRequest* request, SetAttrReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* set_attr = command.mutable_set_attr();
    set_attr->set_inode_id(request->inode_id());
    set_attr->set_set_mode(request->set_mode());
    set_attr->set_mode(request->mode());
    set_attr->set_set_uid(request->set_uid());
    set_attr->set_uid(request->uid());
    set_attr->set_set_gid(request->set_gid());
    set_attr->set_gid(request->gid());
    set_attr->set_set_atime(request->set_atime());
    set_attr->set_atime_ns(request->atime_ns());
    set_attr->set_set_mtime(request->set_mtime());
    set_attr->set_mtime_ns(request->mtime_ns());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::ReadDir(
    grpc::ServerContext* context, const ReadDirRequest* request, ReadDirReply* reply
) {
    (void)context;

    if (auto result = LinearizableRead("readdir"); !result) {
        return ToGrpcStatus(result.error());
    }

    std::shared_lock lock(mutex_);
    auto result = state_.ReadDir(request->inode_id());
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    for (const auto& entry : *result) {
        *reply->add_entries() = entry;
    }
    return grpc::Status::OK;
}

grpc::Status MetaService::Unlink(
    grpc::ServerContext* context, const UnlinkRequest* request, UnlinkReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* unlink = command.mutable_unlink();
    unlink->set_parent_inode_id(request->parent_inode_id());
    unlink->set_name(request->name());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::Rmdir(
    grpc::ServerContext* context, const RmdirRequest* request, RmdirReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* rmdir = command.mutable_rmdir();
    rmdir->set_parent_inode_id(request->parent_inode_id());
    rmdir->set_name(request->name());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::Rename(
    grpc::ServerContext* context, const RenameRequest* request, RenameReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* rename = command.mutable_rename();
    rename->set_old_parent_inode_id(request->old_parent_inode_id());
    rename->set_old_name(request->old_name());
    rename->set_new_parent_inode_id(request->new_parent_inode_id());
    rename->set_new_name(request->new_name());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::GetLayout(
    grpc::ServerContext* context, const GetLayoutRequest* request, GetLayoutReply* reply
) {
    (void)context;

    if (auto result = LinearizableRead("getlayout"); !result) {
        return ToGrpcStatus(result.error());
    }

    std::shared_lock lock(mutex_);
    auto result = state_.GetLayout(request->inode_id());
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_layout() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::ListLiveObjects(
    grpc::ServerContext* context, const ListLiveObjectsRequest* request, ListLiveObjectsReply* reply
) {
    (void)context;
    (void)request;

    if (auto result = LinearizableRead("list-live-objects"); !result) {
        return ToGrpcStatus(result.error());
    }

    std::shared_lock lock(mutex_);
    for (const auto& object_key : state_.ListLiveObjects()) {
        reply->add_object_keys(object_key);
    }
    return grpc::Status::OK;
}

grpc::Status MetaService::CommitWrite(
    grpc::ServerContext* context, const CommitWriteRequest* request, CommitWriteReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* commit_write = command.mutable_commit_write();
    commit_write->set_inode_id(request->inode_id());
    commit_write->set_base_version(request->base_version());
    for (const auto& extent : request->extents()) {
        *commit_write->add_extents() = extent;
    }

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::AppendWrite(
    grpc::ServerContext* context, const AppendWriteRequest* request, AppendWriteReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* append_write = command.mutable_append_write();
    append_write->set_inode_id(request->inode_id());
    append_write->set_base_version(request->base_version());
    for (const auto& extent : request->extents()) {
        *append_write->add_extents() = extent;
    }

    auto result = ProposeAppendWriteCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply = *result;
    return grpc::Status::OK;
}

grpc::Status MetaService::Truncate(
    grpc::ServerContext* context, const TruncateRequest* request, TruncateReply* reply
) {
    (void)context;

    RaftCommand command;
    auto* truncate = command.mutable_truncate();
    truncate->set_inode_id(request->inode_id());
    truncate->set_size(request->size());

    auto result = ProposeInodeCommand(command);
    if (!result) {
        return ToGrpcStatus(result.error());
    }
    *reply->mutable_attrs() = *result;
    return grpc::Status::OK;
}

Result<void> MetaService::LinearizableRead(std::string context) {
    if (raftor_ == nullptr) {
        return std::unexpected(MakeError(Internal{}, "raftor is not initialized"));
    }

    auto result = raftor_->ReadIndexSync(std::move(context));
    if (!result) {
        return std::unexpected(FromRaftError(result.error()));
    }
    return {};
}

Result<InodeAttrs> MetaService::ProposeInodeCommand(const RaftCommand& command) {
    if (raftor_ == nullptr) {
        return std::unexpected(MakeError(Internal{}, "raftor is not initialized"));
    }

    auto result = raftor_->ProposeSync(command.SerializeAsString());
    if (!result) {
        return std::unexpected(FromRaftError(result.error()));
    }

    RaftCommandResponse response;
    if (!response.ParseFromString(*result)) {
        return std::unexpected(MakeError(Internal{}, "failed to parse raft command response"));
    }
    if (response.has_error()) {
        return std::unexpected(ErrorFromResponse(response.error()));
    }
    if (!response.has_attrs()) {
        return std::unexpected(MakeError(Internal{}, "raft command returned no inode attrs"));
    }
    return response.attrs();
}

Result<AppendWriteReply> MetaService::ProposeAppendWriteCommand(const RaftCommand& command) {
    if (raftor_ == nullptr) {
        return std::unexpected(MakeError(Internal{}, "raftor is not initialized"));
    }

    auto result = raftor_->ProposeSync(command.SerializeAsString());
    if (!result) {
        return std::unexpected(FromRaftError(result.error()));
    }

    RaftCommandResponse response;
    if (!response.ParseFromString(*result)) {
        return std::unexpected(MakeError(Internal{}, "failed to parse raft command response"));
    }
    if (response.has_error()) {
        return std::unexpected(ErrorFromResponse(response.error()));
    }
    if (!response.has_append_write()) {
        return std::unexpected(
            MakeError(Internal{}, "raft command returned no append write result")
        );
    }
    return response.append_write();
}

raftpp::Result<raftpp::raftor::ApplyResult> MetaService::Apply(const raftpp::Entry& entry) {
    auto entry_reader = raftpp::capnp_util::reader<raftpp::msg::Entry>(entry);
    auto data = entry_reader.getData();
    if (data.size() == 0) {
        return raftpp::raftor::ApplyResult{};
    }

    std::string command_data(reinterpret_cast<const char*>(data.begin()), data.size());
    RaftCommand command;
    if (!command.ParseFromString(command_data)) {
        return nonstd::make_unexpected(raftpp::RaftError(raftpp::RaftErrorCode::ProposalDropped));
    }

    Result<InodeAttrs> result =
        std::unexpected(MakeError(InvalidArgument{}, "unknown raft command"));
    Result<AppendWriteStateResult> append_result =
        std::unexpected(MakeError(InvalidArgument{}, "not an append command"));
    bool is_append_command = false;
    {
        std::unique_lock lock(mutex_);
        switch (command.command_case()) {
            case RaftCommand::kMkdir:
                result = state_.Mkdir(
                    command.mkdir().parent_inode_id(), command.mkdir().name(),
                    command.mkdir().mode(), command.mkdir().uid(), command.mkdir().gid()
                );
                break;
            case RaftCommand::kCreateFile:
                result = state_.CreateFile(
                    command.create_file().parent_inode_id(), command.create_file().name(),
                    command.create_file().mode(), command.create_file().uid(),
                    command.create_file().gid()
                );
                break;
            case RaftCommand::kSetAttr:
                result = state_.SetAttr(command.set_attr());
                break;
            case RaftCommand::kCommitWrite:
                result = state_.CommitWrite(
                    command.commit_write().inode_id(), command.commit_write().base_version(),
                    command.commit_write().extents()
                );
                break;
            case RaftCommand::kAppendWrite:
                is_append_command = true;
                append_result = state_.AppendWrite(
                    command.append_write().inode_id(), command.append_write().base_version(),
                    command.append_write().extents()
                );
                break;
            case RaftCommand::kTruncate:
                result = state_.Truncate(command.truncate().inode_id(), command.truncate().size());
                break;
            case RaftCommand::kUnlink:
                result = state_.Unlink(command.unlink().parent_inode_id(), command.unlink().name());
                break;
            case RaftCommand::kRmdir:
                result = state_.Rmdir(command.rmdir().parent_inode_id(), command.rmdir().name());
                break;
            case RaftCommand::kRename:
                result = state_.Rename(
                    command.rename().old_parent_inode_id(), command.rename().old_name(),
                    command.rename().new_parent_inode_id(), command.rename().new_name()
                );
                break;
            case RaftCommand::COMMAND_NOT_SET:
                break;
        }
    }

    RaftCommandResponse response;
    if (is_append_command && append_result) {
        auto* append_write = response.mutable_append_write();
        *append_write->mutable_attrs() = append_result->attrs;
        append_write->set_append_start(append_result->append_start);
        for (const auto& extent : append_result->committed_extents) {
            *append_write->add_committed_extents() = extent;
        }
    } else if (is_append_command) {
        response = MakeErrorResponse(append_result.error());
    } else if (result) {
        *response.mutable_attrs() = *result;
    } else {
        response = MakeErrorResponse(result.error());
    }

    raftpp::raftor::ApplyResult apply_result;
    apply_result.response = response.SerializeAsString();
    return apply_result;
}

raftpp::Result<raftpp::SnapshotMetadata> MetaService::TakeSnapshot(
    uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state,
    raftpp::raftor::SnapshotWriter& writer
) {
    std::string snapshot;
    {
        std::shared_lock lock(mutex_);
        snapshot = state_.SerializeSnapshot();
    }

    auto write_result = writer.Write(
        nonstd::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(snapshot.data()), snapshot.size()
        )
    );
    if (!write_result) {
        return nonstd::make_unexpected(write_result.error());
    }

    auto metadata = raftpp::capnp_util::make<raftpp::msg::SnapshotMetadata>();
    auto builder = raftpp::capnp_util::builder<raftpp::msg::SnapshotMetadata>(metadata);
    builder.setIndex(applied_index);
    builder.setTerm(applied_term);
    builder.setConfState(raftpp::capnp_util::reader<raftpp::msg::ConfState>(conf_state));
    return metadata;
}

raftpp::Result<void> MetaService::RestoreSnapshot(
    const raftpp::SnapshotMetadata& metadata, raftpp::raftor::SnapshotReader& reader
) {
    (void)metadata;

    std::string snapshot;
    std::array<uint8_t, 4096> buffer{};
    while (true) {
        auto result = reader.Read(buffer);
        if (!result) {
            return nonstd::make_unexpected(result.error());
        }
        if (*result == 0) {
            break;
        }
        snapshot.append(reinterpret_cast<const char*>(buffer.data()), *result);
    }

    std::unique_lock lock(mutex_);
    auto restore_result = state_.RestoreSnapshot(snapshot);
    if (!restore_result) {
        return nonstd::make_unexpected(SnapshotError(restore_result.error()));
    }
    return {};
}

}  // namespace pulpfs
