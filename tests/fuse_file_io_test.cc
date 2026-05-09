#include "fuse/file_io.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace pulpfs {
namespace {

class FakeObjectStore final : public ObjectStore {
  public:
    void PutBytes(std::string bucket, std::string key, std::string body) {
        objects_[{std::move(bucket), std::move(key)}] = std::move(body);
    }

    Result<PutObjectResult> PutObject(PutObjectRequest request) override {
        objects_[{request.bucket, request.key}] = std::string(request.body.begin(), request.body.end());
        return PutObjectResult{.etag = "fake", .size = request.body.size()};
    }

    Result<GetObjectResult> GetObject(GetObjectRequest request) override {
        auto object = objects_.find({request.bucket, request.key});
        if (object == objects_.end()) {
            return std::unexpected(MakeError(NotFound{.path = request.key}, "object not found"));
        }

        const uint64_t offset = request.offset.value_or(0);
        const uint64_t length = request.length.value_or(object->second.size() - offset);
        if (offset > object->second.size()) {
            return GetObjectResult{};
        }

        const uint64_t end = std::min<uint64_t>(offset + length, object->second.size());
        GetObjectResult result;
        result.body.assign(object->second.begin() + offset, object->second.begin() + end);
        result.content_length = result.body.size();
        return result;
    }

    Result<void> DeleteObject(DeleteObjectRequest request) override {
        objects_.erase({request.bucket, request.key});
        return {};
    }

    Result<ListObjectsResult> ListObjects(ListObjectsRequest request) override {
        ListObjectsResult result;
        for (const auto& [object_key, body] : objects_) {
            const auto& [bucket, key] = object_key;
            if (bucket != request.bucket || key.rfind(request.prefix, 0) != 0) {
                continue;
            }
            result.objects.push_back(ObjectInfo{.key = key, .size = body.size()});
        }
        return result;
    }

  private:
    std::map<std::pair<std::string, std::string>, std::string> objects_;
};

FileExtent MakeExtent(uint64_t offset, uint64_t length, std::string key) {
    FileExtent extent;
    extent.set_offset(offset);
    extent.set_length(length);
    extent.set_object_key(std::move(key));
    extent.set_object_size(length);
    extent.set_object_offset(0);
    return extent;
}

FuseFileHandle MakeHandle(uint64_t size) {
    FuseFileHandle handle;
    handle.layout.mutable_attrs()->set_size(size);
    return handle;
}

std::string ToString(const std::vector<uint8_t>& bytes) {
    return {bytes.begin(), bytes.end()};
}

TEST(FuseFileIoTest, ObjectKeysUsePrefixWriterAndSequence) {
    ObjectKeyGenerator generator("writer");

    EXPECT_EQ(generator.MakeObjectKey("pulpfs", 7, 9), "pulpfs/chunks/7/writer/1/9");
    EXPECT_EQ(generator.MakeObjectKey("pulpfs", 7, 10), "pulpfs/chunks/7/writer/2/10");
    EXPECT_EQ(generator.MakeObjectKey("", 8, 0), "chunks/8/writer/3/0");
}

TEST(FuseFileIoTest, ReadsCommittedExtents) {
    FakeObjectStore store;
    store.PutBytes("bucket", "base", "hello");
    auto handle = MakeHandle(5);
    *handle.layout.add_extents() = MakeExtent(0, 5, "base");

    auto result = ReadFromHandle(store, "bucket", handle, 5, 0);
    ASSERT_TRUE(result.has_value()) << ToString(result.error());
    EXPECT_EQ(ToString(*result), "hello");
}

TEST(FuseFileIoTest, OverlaysPendingPositionedWrites) {
    FakeObjectStore store;
    store.PutBytes("bucket", "base", "aaaaaa");
    store.PutBytes("bucket", "pending", "BB");
    auto handle = MakeHandle(6);
    *handle.layout.add_extents() = MakeExtent(0, 6, "base");
    handle.pending_extents.push_back(MakeExtent(2, 2, "pending"));

    auto result = ReadFromHandle(store, "bucket", handle, 6, 0);
    ASSERT_TRUE(result.has_value()) << ToString(result.error());
    EXPECT_EQ(ToString(*result), "aaBBaa");
}

TEST(FuseFileIoTest, OverlaysPendingAppendWrites) {
    FakeObjectStore store;
    store.PutBytes("bucket", "base", "base");
    store.PutBytes("bucket", "tail", "-tail");
    auto handle = MakeHandle(4);
    *handle.layout.add_extents() = MakeExtent(0, 4, "base");
    handle.pending_append_extents.push_back(MakeExtent(0, 5, "tail"));
    handle.pending_append_length = 5;

    auto result = ReadFromHandle(store, "bucket", handle, 9, 0);
    ASSERT_TRUE(result.has_value()) << ToString(result.error());
    EXPECT_EQ(ToString(*result), "base-tail");
}

TEST(FuseFileIoTest, ReturnsZerosForSparseHoles) {
    FakeObjectStore store;
    auto handle = MakeHandle(4);

    auto result = ReadFromHandle(store, "bucket", handle, 4, 0);
    ASSERT_TRUE(result.has_value()) << ToString(result.error());
    EXPECT_EQ(*result, std::vector<uint8_t>({0, 0, 0, 0}));
}

TEST(FuseFileIoTest, RejectsNegativeReadOffset) {
    FakeObjectStore store;
    auto handle = MakeHandle(4);

    auto result = ReadFromHandle(store, "bucket", handle, 4, -1);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<InvalidArgument>(result.error().kind));
}

}  // namespace
}  // namespace pulpfs
