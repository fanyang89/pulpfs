#include <gflags/gflags.h>
#include <raftpp/logging.h>
#include <spdlog/cfg/env.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "fuse/fuse_server.h"
#include "gc_service.h"
#include "logging.h"
#include "meta_server.h"

DEFINE_string(service, "meta", "the service name");
DEFINE_string(listen_address, "0.0.0.0:3000", "RPC server listen address");
DEFINE_uint64(node_id, 1, "Raft node id");
DEFINE_string(raft_listen_address, "127.0.0.1:9001", "Raft transport listen address");
DEFINE_string(raft_data_dir, "./pulpfs-meta-data", "Raft data directory");
DEFINE_string(meta_address, "127.0.0.1:3000", "Metadata service address for clients");
DEFINE_string(mountpoint, "", "FUSE mount point");
DEFINE_bool(fuse_foreground, true, "Run FUSE in the foreground");
DEFINE_string(s3_endpoint, "", "S3-compatible endpoint for FUSE data IO");
DEFINE_string(s3_region, "us-east-1", "S3 region");
DEFINE_string(s3_bucket, "", "S3 bucket for file chunks");
DEFINE_string(s3_prefix, "pulpfs", "S3 object key prefix");
DEFINE_bool(s3_use_https, true, "Use HTTPS for S3 endpoint");
DEFINE_bool(s3_verify_ssl, true, "Verify S3 TLS certificates");
DEFINE_uint64(gc_grace_seconds, 86400, "Object GC grace period in seconds");
DEFINE_bool(gc_dry_run, false, "Only log orphan objects without deleting them");

using namespace pulpfs;

int main(int argc, char** argv) {
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    spdlog::set_default_logger(spdlog::stdout_color_mt("pulpfs"));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] [%@] %v");
    spdlog::cfg::load_env_levels();

    if (FLAGS_service == "meta") {
        MetaServerConfig config;
        config.grpc_address = FLAGS_listen_address;
        config.raftor.node_id = FLAGS_node_id;
        config.raftor.listen_addr = FLAGS_raft_listen_address;
        config.raftor.transport_kind = raftpp::raftor::TransportKind::Noop;
        config.raftor.data_dir = FLAGS_raft_data_dir;

        MetaServer server(config);
        return server.Run();
    }

    if (FLAGS_service == "fuse") {
        if (FLAGS_mountpoint.empty()) {
            PULPFS_LOG_ERROR("--mountpoint is required for --service=fuse");
            return -1;
        }
        if (FLAGS_s3_endpoint.empty()) {
            PULPFS_LOG_ERROR("--s3_endpoint is required for --service=fuse");
            return -1;
        }
        if (FLAGS_s3_bucket.empty()) {
            PULPFS_LOG_ERROR("--s3_bucket is required for --service=fuse");
            return -1;
        }

        FuseServerConfig config;
        config.mountpoint = FLAGS_mountpoint;
        config.meta_address = FLAGS_meta_address;
        config.bucket = FLAGS_s3_bucket;
        config.object_prefix = FLAGS_s3_prefix;
        config.s3_endpoint = FLAGS_s3_endpoint;
        config.s3_region = FLAGS_s3_region;
        config.s3_use_https = FLAGS_s3_use_https;
        config.s3_verify_ssl = FLAGS_s3_verify_ssl;
        config.foreground = FLAGS_fuse_foreground;

        FuseServer server(config);
        return server.Run();
    }

    if (FLAGS_service == "gc") {
        GcServiceConfig config;
        config.meta_address = FLAGS_meta_address;
        config.bucket = FLAGS_s3_bucket;
        config.object_prefix = FLAGS_s3_prefix;
        config.s3_endpoint = FLAGS_s3_endpoint;
        config.s3_region = FLAGS_s3_region;
        config.s3_use_https = FLAGS_s3_use_https;
        config.s3_verify_ssl = FLAGS_s3_verify_ssl;
        config.grace_seconds = FLAGS_gc_grace_seconds;
        config.dry_run = FLAGS_gc_dry_run;

        GcService service(config);
        return service.RunOnce();
    }

    PULPFS_LOG_ERROR("unknown service: {}", FLAGS_service);
    return -1;
}
