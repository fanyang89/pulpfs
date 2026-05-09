#pragma once

#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "error.h"
#include "object/object_store.h"
#include "pulpfs.pb.h"

namespace pulpfs {

struct FuseFileHandle {
    uint64_t inode_id = 0;
    bool writable = false;
    bool append = false;
    FileLayout layout;
    std::vector<FileExtent> pending_extents;
    std::vector<FileExtent> pending_append_extents;
    uint64_t pending_append_length = 0;
};

class ObjectKeyGenerator {
  public:
    ObjectKeyGenerator();
    explicit ObjectKeyGenerator(std::string writer_id);

    [[nodiscard]] std::string MakeObjectKey(
        std::string_view object_prefix, uint64_t inode_id, uint64_t offset
    );

    [[nodiscard]] const std::string& writer_id() const { return writer_id_; }

  private:
    std::string writer_id_;
    std::atomic<uint64_t> next_sequence_{1};
};

[[nodiscard]] uint64_t ExtentEnd(const FileExtent& extent);

[[nodiscard]] Result<std::vector<uint8_t>> ReadFromHandle(
    ObjectStore& object_store, const std::string& bucket, const FuseFileHandle& handle,
    size_t size, off_t off
);

}  // namespace pulpfs
