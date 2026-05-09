#include "error.h"

#include <string>
#include <type_traits>

#include <absl/base/optimization.h>

namespace pulpfs {
namespace {

std::string KindName(const ErrorKind& kind) {
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
            ABSL_UNREACHABLE();
        },
        kind
    );
}

grpc::StatusCode ToGrpcStatusCode(const ErrorKind& kind) {
    return std::visit(
        []<typename T>(const T&) -> grpc::StatusCode {
            if constexpr (std::is_same_v<T, InvalidArgument>) {
                return grpc::StatusCode::INVALID_ARGUMENT;
            } else if constexpr (std::is_same_v<T, NotFound>) {
                return grpc::StatusCode::NOT_FOUND;
            } else if constexpr (std::is_same_v<T, AlreadyExists>) {
                return grpc::StatusCode::ALREADY_EXISTS;
            } else if constexpr (std::is_same_v<T, NotDirectory>) {
                return grpc::StatusCode::FAILED_PRECONDITION;
            } else if constexpr (std::is_same_v<T, IsDirectory>) {
                return grpc::StatusCode::FAILED_PRECONDITION;
            } else if constexpr (std::is_same_v<T, DirectoryNotEmpty>) {
                return grpc::StatusCode::FAILED_PRECONDITION;
            } else if constexpr (std::is_same_v<T, NotLeader>) {
                return grpc::StatusCode::FAILED_PRECONDITION;
            } else if constexpr (std::is_same_v<T, Unavailable>) {
                return grpc::StatusCode::UNAVAILABLE;
            } else if constexpr (std::is_same_v<T, Timeout>) {
                return grpc::StatusCode::DEADLINE_EXCEEDED;
            } else if constexpr (std::is_same_v<T, Internal>) {
                return grpc::StatusCode::INTERNAL;
            } else if constexpr (std::is_same_v<T, RaftFailure>) {
                return grpc::StatusCode::UNAVAILABLE;
            }
            ABSL_UNREACHABLE();
        },
        kind
    );
}

}  // namespace

Error MakeError(ErrorKind kind, std::string message) {
    return Error{std::move(kind), std::move(message)};
}

Error FromRaftError(const raftpp::RaftError& error) {
    if (error.Is(raftpp::RpcErrorCode::Timeout)) {
        return MakeError(Timeout{.operation = "raft proposal"}, error.ToString());
    }
    if (error.Is(raftpp::RaftErrorCode::LostLeadership)) {
        return MakeError(NotLeader{}, error.ToString());
    }
    if (error.Is(raftpp::RaftErrorCode::ShuttingDown)) {
        return MakeError(Unavailable{}, error.ToString());
    }
    return MakeError(RaftFailure{}, error.ToString());
}

std::string ToString(const Error& error) {
    if (error.message.empty()) {
        return KindName(error.kind);
    }
    return fmt::format("{}: {}", KindName(error.kind), error.message);
}

grpc::Status ToGrpcStatus(const Error& error) {
    return {ToGrpcStatusCode(error.kind), ToString(error)};
}

}  // namespace pulpfs
