#pragma once

#include <memory>
#include <string>

#include <aws/core/Aws.h>
#include <aws/s3/S3Client.h>

#include "object/object_store.h"

namespace pulpfs {

struct S3StoreConfig {
    std::string endpoint;
    std::string region = "us-east-1";
    bool use_https = true;
    bool verify_ssl = true;
    bool path_style = true;
    unsigned max_connections = 128;
    long connect_timeout_ms = 3000;
    long request_timeout_ms = 30000;
};

class AwsSdkRuntime {
  public:
    AwsSdkRuntime();
    ~AwsSdkRuntime();

    AwsSdkRuntime(const AwsSdkRuntime&) = delete;
    AwsSdkRuntime& operator=(const AwsSdkRuntime&) = delete;
};

class S3Store final : public ObjectStore {
  public:
    explicit S3Store(S3StoreConfig config);

    [[nodiscard]] Result<PutObjectResult> PutObject(PutObjectRequest request) override;
    [[nodiscard]] Result<GetObjectResult> GetObject(GetObjectRequest request) override;
    [[nodiscard]] Result<void> DeleteObject(DeleteObjectRequest request) override;
    [[nodiscard]] Result<ListObjectsResult> ListObjects(ListObjectsRequest request) override;

  private:
    AwsSdkRuntime runtime_;
    S3StoreConfig config_;
    std::unique_ptr<Aws::S3::S3Client> client_;
};

}  // namespace pulpfs
