#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "error.h"

namespace pulpfs {

struct PutObjectRequest {
    std::string bucket;
    std::string key;
    std::vector<uint8_t> body;
    std::string content_type = "application/octet-stream";
};

struct PutObjectResult {
    std::string etag;
    uint64_t size = 0;
};

struct GetObjectRequest {
    std::string bucket;
    std::string key;
    std::optional<uint64_t> offset;
    std::optional<uint64_t> length;
};

struct GetObjectResult {
    std::vector<uint8_t> body;
    std::string etag;
    uint64_t content_length = 0;
};

struct DeleteObjectRequest {
    std::string bucket;
    std::string key;
};

struct ObjectInfo {
    std::string key;
    uint64_t size = 0;
    int64_t last_modified_ms = 0;
};

struct ListObjectsRequest {
    std::string bucket;
    std::string prefix;
    std::string continuation_token;
};

struct ListObjectsResult {
    std::vector<ObjectInfo> objects;
    std::string next_continuation_token;
};

class ObjectStore {
  public:
    virtual ~ObjectStore() = default;

    [[nodiscard]] virtual Result<PutObjectResult> PutObject(PutObjectRequest request) = 0;
    [[nodiscard]] virtual Result<GetObjectResult> GetObject(GetObjectRequest request) = 0;
    [[nodiscard]] virtual Result<void> DeleteObject(DeleteObjectRequest request) = 0;
    [[nodiscard]] virtual Result<ListObjectsResult> ListObjects(ListObjectsRequest request) = 0;
};

}  // namespace pulpfs
