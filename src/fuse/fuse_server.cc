#define FUSE_USE_VERSION 35

#include "fuse/fuse_server.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fuse3/fuse_lowlevel.h>

#include "logging.h"

#ifdef PULPFS_WITH_AWS_S3
#include "object/s3_store.h"
#endif

namespace pulpfs {
namespace {

constexpr double kAttrTimeout = 0.0;
constexpr double kEntryTimeout = 0.0;
constexpr uint32_t kModeMask = 07777;

FuseServer* ServerFromReq(fuse_req_t req) {
    return static_cast<FuseServer*>(fuse_req_userdata(req));
}

int ToErrno(const Error& error) {
    return std::visit(
        []<typename T>(const T&) -> int {
            if constexpr (std::is_same_v<T, InvalidArgument>) {
                return EINVAL;
            } else if constexpr (std::is_same_v<T, NotFound>) {
                return ENOENT;
            } else if constexpr (std::is_same_v<T, AlreadyExists>) {
                return EEXIST;
            } else if constexpr (std::is_same_v<T, NotDirectory>) {
                return ENOTDIR;
            } else if constexpr (std::is_same_v<T, IsDirectory>) {
                return EISDIR;
            } else if constexpr (std::is_same_v<T, DirectoryNotEmpty>) {
                return ENOTEMPTY;
            } else if constexpr (std::is_same_v<T, Timeout>) {
                return ETIMEDOUT;
            } else if constexpr (std::is_same_v<T, Unavailable> || std::is_same_v<T, NotLeader>) {
                return EAGAIN;
            }
            return EIO;
        },
        error.kind
    );
}

mode_t FileTypeMode(InodeType type) {
    switch (type) {
        case INODE_TYPE_DIRECTORY:
            return S_IFDIR;
        case INODE_TYPE_FILE:
            return S_IFREG;
        case INODE_TYPE_UNSPECIFIED:
            break;
    }
    return 0;
}

timespec TimespecFromNs(int64_t ns) {
    timespec ts{};
    ts.tv_sec = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    return ts;
}

struct stat StatFromAttrs(const InodeAttrs& attrs) {
    struct stat st{};
    st.st_ino = attrs.inode_id();
    st.st_mode = FileTypeMode(attrs.type()) | (attrs.mode() & kModeMask);
    st.st_nlink = attrs.nlink();
    st.st_uid = attrs.uid();
    st.st_gid = attrs.gid();
    st.st_size = static_cast<off_t>(attrs.size());
    st.st_blksize = 4096;
    st.st_blocks = static_cast<blkcnt_t>((attrs.size() + 511) / 512);
    st.st_atim = TimespecFromNs(attrs.atime_ns());
    st.st_mtim = TimespecFromNs(attrs.mtime_ns());
    st.st_ctim = TimespecFromNs(attrs.ctime_ns());
    return st;
}

fuse_entry_param EntryFromAttrs(const InodeAttrs& attrs) {
    fuse_entry_param entry{};
    entry.ino = attrs.inode_id();
    entry.attr = StatFromAttrs(attrs);
    entry.attr_timeout = kAttrTimeout;
    entry.entry_timeout = kEntryTimeout;
    return entry;
}

void ReplyEntry(fuse_req_t req, const InodeAttrs& attrs) {
    auto entry = EntryFromAttrs(attrs);
    fuse_reply_entry(req, &entry);
}

uint32_t RequestUid(fuse_req_t req) {
    const auto* ctx = fuse_req_ctx(req);
    return ctx == nullptr ? 0 : ctx->uid;
}

uint32_t RequestGid(fuse_req_t req) {
    const auto* ctx = fuse_req_ctx(req);
    return ctx == nullptr ? 0 : ctx->gid;
}

std::string MakeObjectKey(const FuseServer& server, uint64_t inode_id, off_t offset) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nonce = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    std::ostringstream key;
    if (!server.config().object_prefix.empty()) {
        key << server.config().object_prefix << "/";
    }
    key << "chunks/" << inode_id << "/" << nonce << "/" << offset;
    return key.str();
}

bool IsWritable(int flags) {
    const int access_mode = flags & O_ACCMODE;
    return access_mode == O_WRONLY || access_mode == O_RDWR;
}

Result<void> CommitHandle(FuseServer& server, const std::shared_ptr<FuseFileHandle>& handle) {
    if (handle == nullptr) {
        return {};
    }

    if (!handle->pending_extents.empty()) {
        auto result = server.meta_client().CommitWrite(
            handle->inode_id, handle->layout.attrs().version(), handle->pending_extents
        );
        if (!result) {
            return std::unexpected(result.error());
        }
        handle->pending_extents.clear();
        auto layout = server.meta_client().GetLayout(handle->inode_id);
        if (layout) {
            handle->layout = *layout;
        }
    }

    if (!handle->pending_append_extents.empty()) {
        auto result = server.meta_client().AppendWrite(
            handle->inode_id, handle->layout.attrs().version(), handle->pending_append_extents
        );
        if (!result) {
            return std::unexpected(result.error());
        }
        handle->pending_append_extents.clear();
        handle->pending_append_length = 0;
        auto layout = server.meta_client().GetLayout(handle->inode_id);
        if (layout) {
            handle->layout = *layout;
        }
    }
    return {};
}

Result<std::vector<uint8_t>> ReadFromLayout(
    FuseServer& server, const FileLayout& layout, size_t size, off_t off
) {
    std::vector<uint8_t> output(size, 0);
    if (size == 0 || off >= static_cast<off_t>(layout.attrs().size())) {
        return output;
    }

    const uint64_t read_begin = static_cast<uint64_t>(off);
    const uint64_t read_end = std::min<uint64_t>(read_begin + size, layout.attrs().size());
    output.resize(read_end - read_begin);

    for (const auto& extent : layout.extents()) {
        const uint64_t extent_begin = extent.offset();
        const uint64_t extent_end = extent.offset() + extent.length();
        const uint64_t overlap_begin = std::max(read_begin, extent_begin);
        const uint64_t overlap_end = std::min(read_end, extent_end);
        if (overlap_begin >= overlap_end) {
            continue;
        }

        auto* object_store = server.object_store();
        if (object_store == nullptr) {
            return std::unexpected(MakeError(Unavailable{}, "object store is not configured"));
        }

        GetObjectRequest request;
        request.bucket = server.config().bucket;
        request.key = extent.object_key();
        request.offset = extent.object_offset() + (overlap_begin - extent_begin);
        request.length = overlap_end - overlap_begin;

        auto object_result = object_store->GetObject(std::move(request));
        if (!object_result) {
            return std::unexpected(object_result.error());
        }

        const size_t output_offset = overlap_begin - read_begin;
        const size_t copy_size =
            std::min<size_t>(object_result->body.size(), overlap_end - overlap_begin);
        std::copy_n(object_result->body.begin(), copy_size, output.begin() + output_offset);
    }

    return output;
}

void PulpfsLookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
    auto* server = ServerFromReq(req);
    auto result = server->meta_client().Lookup(parent, name);
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }

    server->RememberParent(result->inode_id(), parent);
    ReplyEntry(req, *result);
}

void PulpfsGetAttr(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    (void)fi;

    auto result = ServerFromReq(req)->meta_client().GetAttr(ino);
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }

    const auto st = StatFromAttrs(*result);
    fuse_reply_attr(req, &st, kAttrTimeout);
}

void PulpfsSetAttr(
    fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, fuse_file_info* fi
) {
    (void)fi;

    constexpr int supported = FUSE_SET_ATTR_MODE | FUSE_SET_ATTR_UID | FUSE_SET_ATTR_GID |
        FUSE_SET_ATTR_SIZE | FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME;
    if ((to_set & ~supported) != 0) {
        fuse_reply_err(req, EOPNOTSUPP);
        return;
    }

    auto* server = ServerFromReq(req);
    Result<InodeAttrs> result = server->meta_client().GetAttr(ino);
    if ((to_set & FUSE_SET_ATTR_SIZE) != 0) {
        result = server->meta_client().Truncate(ino, attr->st_size);
    }
    if (result && (to_set & ~FUSE_SET_ATTR_SIZE) != 0) {
        SetAttrRequest request;
        request.set_inode_id(ino);
        if ((to_set & FUSE_SET_ATTR_MODE) != 0) {
            request.set_set_mode(true);
            request.set_mode(attr->st_mode & kModeMask);
        }
        if ((to_set & FUSE_SET_ATTR_UID) != 0) {
            request.set_set_uid(true);
            request.set_uid(attr->st_uid);
        }
        if ((to_set & FUSE_SET_ATTR_GID) != 0) {
            request.set_set_gid(true);
            request.set_gid(attr->st_gid);
        }
        if ((to_set & FUSE_SET_ATTR_ATIME) != 0) {
            request.set_set_atime(true);
            request.set_atime_ns(
                static_cast<int64_t>(attr->st_atim.tv_sec) * 1000000000 + attr->st_atim.tv_nsec
            );
        }
        if ((to_set & FUSE_SET_ATTR_MTIME) != 0) {
            request.set_set_mtime(true);
            request.set_mtime_ns(
                static_cast<int64_t>(attr->st_mtim.tv_sec) * 1000000000 + attr->st_mtim.tv_nsec
            );
        }
        result = server->meta_client().SetAttr(request);
    }
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }

    const auto st = StatFromAttrs(*result);
    fuse_reply_attr(req, &st, kAttrTimeout);
}

void AddDirEntry(
    fuse_req_t req, std::vector<char>& buffer, const std::string& name, const InodeAttrs& attrs,
    off_t next_offset
) {
    const auto st = StatFromAttrs(attrs);
    const size_t old_size = buffer.size();
    const size_t entry_size = fuse_add_direntry(req, nullptr, 0, name.c_str(), &st, next_offset);
    buffer.resize(old_size + entry_size);
    fuse_add_direntry(req, buffer.data() + old_size, entry_size, name.c_str(), &st, next_offset);
}

void PulpfsReadDir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi) {
    (void)fi;

    auto* server = ServerFromReq(req);
    auto dir_result = server->meta_client().GetAttr(ino);
    if (!dir_result) {
        fuse_reply_err(req, ToErrno(dir_result.error()));
        return;
    }
    if (dir_result->type() != INODE_TYPE_DIRECTORY) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    auto entries_result = server->meta_client().ReadDir(ino);
    if (!entries_result) {
        fuse_reply_err(req, ToErrno(entries_result.error()));
        return;
    }

    std::vector<std::pair<std::string, InodeAttrs>> entries;
    entries.emplace_back(".", *dir_result);

    auto parent_result = server->meta_client().GetAttr(server->ParentOf(ino));
    entries.emplace_back("..", parent_result ? *parent_result : *dir_result);

    for (const auto& entry : *entries_result) {
        entries.emplace_back(entry.name(), entry.attrs());
        server->RememberParent(entry.attrs().inode_id(), ino);
    }

    std::vector<char> buffer;
    for (size_t index = static_cast<size_t>(std::max<off_t>(off, 0)); index < entries.size();
         ++index) {
        AddDirEntry(req, buffer, entries[index].first, entries[index].second, index + 1);
    }

    if (static_cast<size_t>(size) < buffer.size()) {
        buffer.resize(size);
    }
    fuse_reply_buf(req, buffer.data(), buffer.size());
}

void PulpfsMkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
    auto* server = ServerFromReq(req);
    auto result = server->meta_client().Mkdir(
        parent, name, static_cast<uint32_t>(mode & kModeMask), RequestUid(req), RequestGid(req)
    );
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }

    server->RememberParent(result->inode_id(), parent);
    ReplyEntry(req, *result);
}

void PulpfsUnlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
    auto result = ServerFromReq(req)->meta_client().Unlink(parent, name);
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsRmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
    auto result = ServerFromReq(req)->meta_client().Rmdir(parent, name);
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsRename(
    fuse_req_t req, fuse_ino_t old_parent, const char* old_name, fuse_ino_t new_parent,
    const char* new_name, unsigned int flags
) {
    if (flags != 0) {
        fuse_reply_err(req, EINVAL);
        return;
    }

    auto result =
        ServerFromReq(req)->meta_client().Rename(old_parent, old_name, new_parent, new_name);
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsCreate(
    fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, fuse_file_info* fi
) {
    auto* server = ServerFromReq(req);
    auto result = server->meta_client().CreateFile(
        parent, name, static_cast<uint32_t>(mode & kModeMask), RequestUid(req), RequestGid(req)
    );
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }

    server->RememberParent(result->inode_id(), parent);
    FuseFileHandle handle;
    handle.inode_id = result->inode_id();
    handle.writable = IsWritable(fi->flags);
    handle.append = (fi->flags & O_APPEND) != 0;
    auto layout_result = server->meta_client().GetLayout(result->inode_id());
    if (layout_result) {
        handle.layout = *layout_result;
    }
    fi->fh = server->AllocateFileHandle(std::move(handle));
    auto entry = EntryFromAttrs(*result);
    fuse_reply_create(req, &entry, fi);
}

void PulpfsOpen(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    auto* server = ServerFromReq(req);
    auto result = server->meta_client().GetLayout(ino);
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }
    if (result->attrs().type() != INODE_TYPE_FILE) {
        fuse_reply_err(req, EISDIR);
        return;
    }

    if (IsWritable(fi->flags) && (fi->flags & O_TRUNC) != 0) {
        auto truncate_result = server->meta_client().Truncate(ino, 0);
        if (!truncate_result) {
            fuse_reply_err(req, ToErrno(truncate_result.error()));
            return;
        }
        result = server->meta_client().GetLayout(ino);
        if (!result) {
            fuse_reply_err(req, ToErrno(result.error()));
            return;
        }
    }

    FuseFileHandle handle;
    handle.inode_id = ino;
    handle.writable = IsWritable(fi->flags);
    handle.append = (fi->flags & O_APPEND) != 0;
    handle.layout = *result;
    fi->fh = server->AllocateFileHandle(std::move(handle));
    fuse_reply_open(req, fi);
}

void PulpfsOpenDir(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    auto result = ServerFromReq(req)->meta_client().GetAttr(ino);
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }
    if (result->type() != INODE_TYPE_DIRECTORY) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    fi->fh = ino;
    fuse_reply_open(req, fi);
}

void PulpfsRead(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, fuse_file_info* fi) {
    (void)ino;

    auto* server = ServerFromReq(req);
    auto handle = server->GetFileHandle(fi->fh);
    if (handle == nullptr) {
        fuse_reply_err(req, EBADF);
        return;
    }

    auto result = ReadFromLayout(*server, handle->layout, size, off);
    if (!result) {
        fuse_reply_err(req, ToErrno(result.error()));
        return;
    }
    fuse_reply_buf(req, reinterpret_cast<const char*>(result->data()), result->size());
}

void PulpfsWrite(
    fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, fuse_file_info* fi
) {
    auto* server = ServerFromReq(req);
    auto handle = server->GetFileHandle(fi->fh);
    if (handle == nullptr || !handle->writable) {
        fuse_reply_err(req, EBADF);
        return;
    }
    if (server->object_store() == nullptr || server->config().bucket.empty()) {
        fuse_reply_err(req, EIO);
        return;
    }

    const uint64_t write_offset =
        handle->append ? handle->pending_append_length : static_cast<uint64_t>(off);

    PutObjectRequest request;
    request.bucket = server->config().bucket;
    request.key = MakeObjectKey(*server, ino, static_cast<off_t>(write_offset));
    const std::string object_key = request.key;
    request.body.assign(
        reinterpret_cast<const uint8_t*>(buf), reinterpret_cast<const uint8_t*>(buf) + size
    );

    auto put_result = server->object_store()->PutObject(std::move(request));
    if (!put_result) {
        fuse_reply_err(req, ToErrno(put_result.error()));
        return;
    }

    FileExtent extent;
    extent.set_offset(write_offset);
    extent.set_length(size);
    extent.set_object_key(object_key);
    extent.set_etag(put_result->etag);
    extent.set_object_size(put_result->size);
    extent.set_object_offset(0);
    if (handle->append) {
        handle->pending_append_length += size;
        handle->pending_append_extents.push_back(std::move(extent));
    } else {
        handle->pending_extents.push_back(std::move(extent));
    }

    fuse_reply_write(req, size);
}

void PulpfsFlush(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    (void)ino;

    auto result = CommitHandle(*ServerFromReq(req), ServerFromReq(req)->GetFileHandle(fi->fh));
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsFsync(fuse_req_t req, fuse_ino_t ino, int datasync, fuse_file_info* fi) {
    (void)ino;
    (void)datasync;
    auto result = CommitHandle(*ServerFromReq(req), ServerFromReq(req)->GetFileHandle(fi->fh));
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsRelease(fuse_req_t req, fuse_ino_t ino, fuse_file_info* fi) {
    (void)ino;

    auto* server = ServerFromReq(req);
    auto result = CommitHandle(*server, server->GetFileHandle(fi->fh));
    server->ReleaseFileHandle(fi->fh);
    fuse_reply_err(req, result ? 0 : ToErrno(result.error()));
}

void PulpfsForget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup) {
    (void)ino;
    (void)nlookup;

    fuse_reply_none(req);
}

void PulpfsStatFs(fuse_req_t req, fuse_ino_t ino) {
    (void)ino;

    struct statvfs st{};
    st.f_bsize = 4096;
    st.f_frsize = 4096;
    st.f_blocks = 1ULL << 40;
    st.f_bfree = st.f_blocks;
    st.f_bavail = st.f_blocks;
    st.f_files = 1ULL << 32;
    st.f_ffree = st.f_files;
    st.f_favail = st.f_files;
    st.f_namemax = 255;
    fuse_reply_statfs(req, &st);
}

fuse_lowlevel_ops MakeOps() {
    fuse_lowlevel_ops ops{};
    ops.lookup = PulpfsLookup;
    ops.getattr = PulpfsGetAttr;
    ops.setattr = PulpfsSetAttr;
    ops.readdir = PulpfsReadDir;
    ops.mkdir = PulpfsMkdir;
    ops.unlink = PulpfsUnlink;
    ops.rmdir = PulpfsRmdir;
    ops.rename = PulpfsRename;
    ops.create = PulpfsCreate;
    ops.open = PulpfsOpen;
    ops.opendir = PulpfsOpenDir;
    ops.read = PulpfsRead;
    ops.write = PulpfsWrite;
    ops.flush = PulpfsFlush;
    ops.fsync = PulpfsFsync;
    ops.release = PulpfsRelease;
    ops.releasedir = PulpfsRelease;
    ops.forget = PulpfsForget;
    ops.statfs = PulpfsStatFs;
    return ops;
}

}  // namespace

FuseServer::FuseServer(FuseServerConfig config)
    : config_(std::move(config)), meta_client_(config_.meta_address) {
    parents_.emplace(1, 1);

#ifdef PULPFS_WITH_AWS_S3
    if (!config_.s3_endpoint.empty()) {
        S3StoreConfig s3_config;
        s3_config.endpoint = config_.s3_endpoint;
        s3_config.region = config_.s3_region;
        s3_config.use_https = config_.s3_use_https;
        s3_config.verify_ssl = config_.s3_verify_ssl;
        s3_config.path_style = true;
        object_store_ = std::make_unique<S3Store>(std::move(s3_config));
    }
#endif
}

int FuseServer::Run() {
    PULPFS_LOG_INFO(
        "starting fuse server: mountpoint={}, meta_address={}", config_.mountpoint,
        config_.meta_address
    );

    fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    fuse_opt_add_arg(&args, "pulpfs");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "fsname=pulpfs");
    fuse_opt_add_arg(&args, "-o");
    fuse_opt_add_arg(&args, "default_permissions");

    auto ops = MakeOps();
    fuse_session* session = fuse_session_new(&args, &ops, sizeof(ops), this);
    if (session == nullptr) {
        PULPFS_LOG_ERROR("failed to create fuse session");
        fuse_opt_free_args(&args);
        return -1;
    }

    if (fuse_session_mount(session, config_.mountpoint.c_str()) != 0) {
        PULPFS_LOG_ERROR("failed to mount fuse filesystem at {}", config_.mountpoint);
        fuse_session_destroy(session);
        fuse_opt_free_args(&args);
        return -1;
    }

    if (!config_.foreground && fuse_daemonize(0) != 0) {
        PULPFS_LOG_ERROR("failed to daemonize fuse process");
        fuse_session_unmount(session);
        fuse_session_destroy(session);
        fuse_opt_free_args(&args);
        return -1;
    }

    if (fuse_set_signal_handlers(session) != 0) {
        PULPFS_LOG_ERROR("failed to install fuse signal handlers");
        fuse_session_unmount(session);
        fuse_session_destroy(session);
        fuse_opt_free_args(&args);
        return -1;
    }

    const int result = fuse_session_loop(session);
    fuse_remove_signal_handlers(session);
    fuse_session_unmount(session);
    fuse_session_destroy(session);
    fuse_opt_free_args(&args);

    PULPFS_LOG_INFO("fuse server stopped");
    return result;
}

uint64_t FuseServer::ParentOf(uint64_t inode_id) {
    std::lock_guard lock(parent_mutex_);
    auto parent = parents_.find(inode_id);
    if (parent == parents_.end()) {
        return inode_id;
    }
    return parent->second;
}

void FuseServer::RememberParent(uint64_t inode_id, uint64_t parent_inode_id) {
    std::lock_guard lock(parent_mutex_);
    parents_[inode_id] = parent_inode_id;
}

uint64_t FuseServer::AllocateFileHandle(FuseFileHandle handle) {
    std::lock_guard lock(handle_mutex_);
    const uint64_t handle_id = next_handle_id_++;
    handles_[handle_id] = std::make_shared<FuseFileHandle>(std::move(handle));
    return handle_id;
}

std::shared_ptr<FuseFileHandle> FuseServer::GetFileHandle(uint64_t handle_id) {
    std::lock_guard lock(handle_mutex_);
    auto handle = handles_.find(handle_id);
    if (handle == handles_.end()) {
        return nullptr;
    }
    return handle->second;
}

void FuseServer::ReleaseFileHandle(uint64_t handle_id) {
    std::lock_guard lock(handle_mutex_);
    handles_.erase(handle_id);
}

}  // namespace pulpfs
