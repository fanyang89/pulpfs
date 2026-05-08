#include "gc_service.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "fuse/meta_client.h"
#include "logging.h"
#include "object/object_store.h"

#ifdef PULPFS_WITH_AWS_S3
#include "object/s3_store.h"
#endif

namespace pulpfs {
namespace {

std::string ChunkPrefix(const std::string& object_prefix) {
    if (object_prefix.empty()) {
        return "chunks/";
    }
    return object_prefix + "/chunks/";
}

int64_t NowMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::unique_ptr<ObjectStore> CreateObjectStore(const GcServiceConfig& config) {
#ifdef PULPFS_WITH_AWS_S3
    S3StoreConfig s3_config;
    s3_config.endpoint = config.s3_endpoint;
    s3_config.region = config.s3_region;
    s3_config.use_https = config.s3_use_https;
    s3_config.verify_ssl = config.s3_verify_ssl;
    s3_config.path_style = true;
    return std::make_unique<S3Store>(std::move(s3_config));
#else
    (void)config;
    return nullptr;
#endif
}

}  // namespace

GcService::GcService(GcServiceConfig config) : config_(std::move(config)) {}

int GcService::RunOnce() {
    if (config_.bucket.empty()) {
        PULPFS_LOG_ERROR("--s3_bucket is required for --service=gc");
        return -1;
    }
    if (config_.s3_endpoint.empty()) {
        PULPFS_LOG_ERROR("--s3_endpoint is required for --service=gc");
        return -1;
    }

    auto object_store = CreateObjectStore(config_);
    if (object_store == nullptr) {
        PULPFS_LOG_ERROR("gc service requires PULPFS_WITH_AWS_S3=ON");
        return -1;
    }

    MetaClient meta_client(config_.meta_address);
    auto live_result = meta_client.ListLiveObjects();
    if (!live_result) {
        PULPFS_LOG_ERROR("failed to list live objects: {}", ToString(live_result.error()));
        return -1;
    }

    std::unordered_set<std::string> live_objects(live_result->begin(), live_result->end());
    const int64_t cutoff_ms = NowMs() - static_cast<int64_t>(config_.grace_seconds * 1000);
    const std::string prefix = ChunkPrefix(config_.object_prefix);

    uint64_t scanned = 0;
    uint64_t live = 0;
    uint64_t skipped_young = 0;
    uint64_t deleted = 0;
    uint64_t delete_failed = 0;
    std::string continuation_token;

    PULPFS_LOG_INFO(
        "starting orphan object gc: bucket={}, prefix={}, grace_seconds={}, dry_run={}",
        config_.bucket, prefix, config_.grace_seconds, config_.dry_run
    );

    while (true) {
        ListObjectsRequest request;
        request.bucket = config_.bucket;
        request.prefix = prefix;
        request.continuation_token = continuation_token;

        auto list_result = object_store->ListObjects(std::move(request));
        if (!list_result) {
            PULPFS_LOG_ERROR("failed to list objects: {}", ToString(list_result.error()));
            return -1;
        }

        for (const auto& object : list_result->objects) {
            ++scanned;
            if (live_objects.contains(object.key)) {
                ++live;
                continue;
            }
            if (object.last_modified_ms > cutoff_ms) {
                ++skipped_young;
                continue;
            }

            if (config_.dry_run) {
                PULPFS_LOG_INFO(
                    "would delete orphan object: key={}, size={}", object.key, object.size
                );
                ++deleted;
                continue;
            }

            DeleteObjectRequest delete_request;
            delete_request.bucket = config_.bucket;
            delete_request.key = object.key;
            auto delete_result = object_store->DeleteObject(std::move(delete_request));
            if (!delete_result) {
                ++delete_failed;
                PULPFS_LOG_WARN(
                    "failed to delete orphan object: key={}, error={}", object.key,
                    ToString(delete_result.error())
                );
                continue;
            }
            ++deleted;
            PULPFS_LOG_INFO("deleted orphan object: key={}, size={}", object.key, object.size);
        }

        continuation_token = list_result->next_continuation_token;
        if (continuation_token.empty()) {
            break;
        }
    }

    PULPFS_LOG_INFO(
        "orphan object gc finished: scanned={}, live={}, skipped_young={}, deleted={}, "
        "delete_failed={}",
        scanned, live, skipped_young, deleted, delete_failed
    );
    return delete_failed == 0 ? 0 : -1;
}

}  // namespace pulpfs
