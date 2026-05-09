#include "fuse/file_io.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace pulpfs {
namespace {

std::string MakeWriterId() {
    std::random_device random;
    const uint64_t random_hi = (static_cast<uint64_t>(random()) << 32) ^ random();
    const uint64_t random_lo = (static_cast<uint64_t>(random()) << 32) ^ random();
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        )
            .count()
    );

    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << random_hi << std::setw(16)
        << (random_lo ^ now);
    return out.str();
}

template <typename Extents>
Result<void> ReadExtentsIntoBuffer(
    ObjectStore& object_store, const std::string& bucket, const Extents& extents,
    uint64_t read_begin, uint64_t read_end, std::vector<uint8_t>* output
) {
    for (const auto& extent : extents) {
        const uint64_t extent_begin = extent.offset();
        const uint64_t extent_end = ExtentEnd(extent);
        const uint64_t overlap_begin = std::max(read_begin, extent_begin);
        const uint64_t overlap_end = std::min(read_end, extent_end);
        if (overlap_begin >= overlap_end) {
            continue;
        }

        GetObjectRequest request;
        request.bucket = bucket;
        request.key = extent.object_key();
        request.offset = extent.object_offset() + (overlap_begin - extent_begin);
        request.length = overlap_end - overlap_begin;

        auto object_result = object_store.GetObject(std::move(request));
        if (!object_result) {
            return std::unexpected(object_result.error());
        }

        const size_t output_offset = overlap_begin - read_begin;
        const size_t copy_size =
            std::min<size_t>(object_result->body.size(), overlap_end - overlap_begin);
        std::copy_n(object_result->body.begin(), copy_size, output->begin() + output_offset);
    }

    return {};
}

}  // namespace

ObjectKeyGenerator::ObjectKeyGenerator() : ObjectKeyGenerator(MakeWriterId()) {}

ObjectKeyGenerator::ObjectKeyGenerator(std::string writer_id) : writer_id_(std::move(writer_id)) {}

std::string ObjectKeyGenerator::MakeObjectKey(
    std::string_view object_prefix, uint64_t inode_id, uint64_t offset
) {
    std::ostringstream key;
    if (!object_prefix.empty()) {
        key << object_prefix << "/";
    }
    key << "chunks/" << inode_id << "/" << writer_id_ << "/"
        << next_sequence_.fetch_add(1, std::memory_order_relaxed) << "/" << offset;
    return key.str();
}

uint64_t ExtentEnd(const FileExtent& extent) {
    return extent.offset() + extent.length();
}

Result<std::vector<uint8_t>> ReadFromHandle(
    ObjectStore& object_store, const std::string& bucket, const FuseFileHandle& handle,
    size_t size, off_t off
) {
    std::vector<uint8_t> output(size, 0);
    if (size == 0) {
        return output;
    }
    if (off < 0) {
        return std::unexpected(MakeError(InvalidArgument{}, "negative read offset"));
    }

    uint64_t visible_size = handle.layout.attrs().size();
    for (const auto& extent : handle.pending_extents) {
        visible_size = std::max(visible_size, ExtentEnd(extent));
    }

    std::vector<FileExtent> pending_append_extents;
    pending_append_extents.reserve(handle.pending_append_extents.size());
    const uint64_t append_start = handle.layout.attrs().size();
    for (const auto& extent : handle.pending_append_extents) {
        FileExtent visible_extent = extent;
        visible_extent.set_offset(append_start + extent.offset());
        visible_size = std::max(visible_size, ExtentEnd(visible_extent));
        pending_append_extents.push_back(std::move(visible_extent));
    }

    if (static_cast<uint64_t>(off) >= visible_size) {
        output.clear();
        return output;
    }

    const uint64_t read_begin = static_cast<uint64_t>(off);
    const uint64_t read_end = std::min<uint64_t>(read_begin + size, visible_size);
    output.resize(read_end - read_begin);

    if (auto result = ReadExtentsIntoBuffer(
            object_store, bucket, handle.layout.extents(), read_begin, read_end, &output
        ); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = ReadExtentsIntoBuffer(
            object_store, bucket, handle.pending_extents, read_begin, read_end, &output
        ); !result) {
        return std::unexpected(result.error());
    }
    if (auto result = ReadExtentsIntoBuffer(
            object_store, bucket, pending_append_extents, read_begin, read_end, &output
        ); !result) {
        return std::unexpected(result.error());
    }

    return output;
}

}  // namespace pulpfs
