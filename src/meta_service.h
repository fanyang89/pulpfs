#pragma once

#include <memory>
#include <shared_mutex>
#include <string>

#include <raftpp/raftor/raftor.h>

#include "error.h"
#include "meta_state.h"
#include "pulpfs.grpc.pb.h"

namespace grpc {
class Server;
}  // namespace grpc

namespace raftpp::raftor {
class Raftor;
}  // namespace raftpp::raftor

namespace pulpfs {

class MetaService final : public Meta::Service, public raftpp::raftor::StateMachine {
  public:
    void SetRaftor(raftpp::raftor::Raftor* raftor) { raftor_ = raftor; }

    grpc::Status Mkdir(
        grpc::ServerContext* context, const MkdirRequest* request, MkdirReply* reply
    ) override;
    grpc::Status CreateFile(
        grpc::ServerContext* context, const CreateFileRequest* request, CreateFileReply* reply
    ) override;
    grpc::Status Lookup(
        grpc::ServerContext* context, const LookupRequest* request, LookupReply* reply
    ) override;
    grpc::Status GetAttr(
        grpc::ServerContext* context, const GetAttrRequest* request, GetAttrReply* reply
    ) override;
    grpc::Status SetAttr(
        grpc::ServerContext* context, const SetAttrRequest* request, SetAttrReply* reply
    ) override;
    grpc::Status ReadDir(
        grpc::ServerContext* context, const ReadDirRequest* request, ReadDirReply* reply
    ) override;
    grpc::Status Unlink(
        grpc::ServerContext* context, const UnlinkRequest* request, UnlinkReply* reply
    ) override;
    grpc::Status Rmdir(
        grpc::ServerContext* context, const RmdirRequest* request, RmdirReply* reply
    ) override;
    grpc::Status Rename(
        grpc::ServerContext* context, const RenameRequest* request, RenameReply* reply
    ) override;
    grpc::Status GetLayout(
        grpc::ServerContext* context, const GetLayoutRequest* request, GetLayoutReply* reply
    ) override;
    grpc::Status ListLiveObjects(
        grpc::ServerContext* context, const ListLiveObjectsRequest* request,
        ListLiveObjectsReply* reply
    ) override;
    grpc::Status CommitWrite(
        grpc::ServerContext* context, const CommitWriteRequest* request, CommitWriteReply* reply
    ) override;
    grpc::Status AppendWrite(
        grpc::ServerContext* context, const AppendWriteRequest* request, AppendWriteReply* reply
    ) override;
    grpc::Status Truncate(
        grpc::ServerContext* context, const TruncateRequest* request, TruncateReply* reply
    ) override;

    [[nodiscard]] raftpp::Result<raftpp::raftor::ApplyResult> Apply(
        const raftpp::Entry& entry
    ) override;

    [[nodiscard]] raftpp::Result<raftpp::SnapshotMetadata> TakeSnapshot(
        uint64_t applied_index, uint64_t applied_term, const raftpp::ConfState& conf_state,
        raftpp::raftor::SnapshotWriter& writer
    ) override;

    [[nodiscard]] raftpp::Result<void> RestoreSnapshot(
        const raftpp::SnapshotMetadata& metadata, raftpp::raftor::SnapshotReader& reader
    ) override;

  private:
    Result<void> LinearizableRead(std::string context);
    Result<InodeAttrs> ProposeInodeCommand(const RaftCommand& command);
    Result<AppendWriteReply> ProposeAppendWriteCommand(const RaftCommand& command);

    raftpp::raftor::Raftor* raftor_ = nullptr;
    mutable std::shared_mutex mutex_;
    MetaState state_;
};

}  // namespace pulpfs
