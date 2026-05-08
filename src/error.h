#pragma once

#include <expected>
#include <string>
#include <variant>

#include <grpc++/grpc++.h>
#include <raftpp/core/error.h>

namespace pulpfs {

struct InvalidArgument {};

struct NotFound {
    std::string path;
};

struct AlreadyExists {
    std::string path;
};

struct NotDirectory {
    std::string path;
};

struct IsDirectory {
    std::string path;
};

struct DirectoryNotEmpty {
    std::string path;
};

struct NotLeader {
    uint64_t leader_id = 0;
};

struct Unavailable {};

struct Timeout {
    std::string operation;
};

struct Internal {};

struct RaftFailure {};

using ErrorKind = std::variant<
    InvalidArgument, NotFound, AlreadyExists, NotDirectory, IsDirectory, DirectoryNotEmpty,
    NotLeader, Unavailable, Timeout, Internal, RaftFailure>;

struct Error {
    ErrorKind kind;
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

Error MakeError(ErrorKind kind, std::string message);
Error FromRaftError(const raftpp::RaftError& error);
std::string ToString(const Error& error);
grpc::Status ToGrpcStatus(const Error& error);

}  // namespace pulpfs
