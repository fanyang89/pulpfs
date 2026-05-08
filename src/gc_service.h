#pragma once

#include <cstdint>
#include <string>

namespace pulpfs {

struct GcServiceConfig {
    std::string meta_address;
    std::string bucket;
    std::string object_prefix = "pulpfs";
    std::string s3_endpoint;
    std::string s3_region = "us-east-1";
    bool s3_use_https = true;
    bool s3_verify_ssl = true;
    uint64_t grace_seconds = 86400;
    bool dry_run = false;
};

class GcService {
  public:
    explicit GcService(GcServiceConfig config);

    int RunOnce();

  private:
    GcServiceConfig config_;
};

}  // namespace pulpfs
