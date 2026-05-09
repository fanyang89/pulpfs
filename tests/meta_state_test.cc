#include "meta_state.h"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

namespace pulpfs {
namespace {

FileExtent MakeExtent(uint64_t offset, uint64_t length, std::string key) {
    FileExtent extent;
    extent.set_offset(offset);
    extent.set_length(length);
    extent.set_object_key(std::move(key));
    extent.set_object_size(length);
    extent.set_object_offset(0);
    return extent;
}

google::protobuf::RepeatedPtrField<FileExtent> Extents(std::initializer_list<FileExtent> extents) {
    google::protobuf::RepeatedPtrField<FileExtent> out;
    for (const auto& extent : extents) {
        *out.Add() = extent;
    }
    return out;
}

template <typename ErrorType, typename T>
void ExpectErrorKind(const Result<T>& result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<ErrorType>(result.error().kind)) << ToString(result.error());
}

std::vector<std::string> EntryNames(const std::vector<DirectoryEntry>& entries) {
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (const auto& entry : entries) {
        names.push_back(entry.name());
    }
    std::sort(names.begin(), names.end());
    return names;
}

TEST(MetaStateTest, InitializesRootDirectory) {
    MetaState state;

    auto root = state.GetAttr(MetaState::kRootInodeId);
    ASSERT_TRUE(root.has_value()) << ToString(root.error());
    EXPECT_EQ(root->inode_id(), MetaState::kRootInodeId);
    EXPECT_EQ(root->type(), INODE_TYPE_DIRECTORY);
    EXPECT_EQ(root->mode(), 0755U);
    EXPECT_EQ(root->nlink(), 2U);
    EXPECT_EQ(root->version(), 1U);
    EXPECT_EQ(root->chunk_size(), 0U);
}

TEST(MetaStateTest, CreatesAndListsDirectoryEntries) {
    MetaState state;

    auto dir = state.Mkdir(MetaState::kRootInodeId, "dir", 0755, 1000, 1000);
    ASSERT_TRUE(dir.has_value()) << ToString(dir.error());
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 1000, 1000);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    auto lookup = state.Lookup(MetaState::kRootInodeId, "file");
    ASSERT_TRUE(lookup.has_value()) << ToString(lookup.error());
    EXPECT_EQ(lookup->inode_id(), file->inode_id());
    EXPECT_EQ(lookup->type(), INODE_TYPE_FILE);
    EXPECT_EQ(lookup->chunk_size(), MetaState::kDefaultChunkSize);

    auto entries = state.ReadDir(MetaState::kRootInodeId);
    ASSERT_TRUE(entries.has_value()) << ToString(entries.error());
    EXPECT_EQ(EntryNames(*entries), std::vector<std::string>({"dir", "file"}));
}

TEST(MetaStateTest, RejectsInvalidAndDuplicateNames) {
    MetaState state;

    ExpectErrorKind<InvalidArgument>(state.CreateFile(MetaState::kRootInodeId, "", 0644, 0, 0));
    ExpectErrorKind<InvalidArgument>(state.CreateFile(MetaState::kRootInodeId, ".", 0644, 0, 0));
    ExpectErrorKind<InvalidArgument>(state.CreateFile(MetaState::kRootInodeId, "..", 0644, 0, 0));
    ExpectErrorKind<InvalidArgument>(
        state.CreateFile(MetaState::kRootInodeId, "bad/name", 0644, 0, 0)
    );

    auto first = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(first.has_value()) << ToString(first.error());
    ExpectErrorKind<AlreadyExists>(state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0));
}

TEST(MetaStateTest, EnforcesUnlinkAndRmdirRules) {
    MetaState state;

    auto dir = state.Mkdir(MetaState::kRootInodeId, "dir", 0755, 0, 0);
    ASSERT_TRUE(dir.has_value()) << ToString(dir.error());
    auto file = state.CreateFile(dir->inode_id(), "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    ExpectErrorKind<IsDirectory>(state.Unlink(MetaState::kRootInodeId, "dir"));
    ExpectErrorKind<DirectoryNotEmpty>(state.Rmdir(MetaState::kRootInodeId, "dir"));

    auto unlink = state.Unlink(dir->inode_id(), "file");
    ASSERT_TRUE(unlink.has_value()) << ToString(unlink.error());
    ExpectErrorKind<NotFound>(state.Lookup(dir->inode_id(), "file"));

    auto rmdir = state.Rmdir(MetaState::kRootInodeId, "dir");
    ASSERT_TRUE(rmdir.has_value()) << ToString(rmdir.error());
    ExpectErrorKind<NotFound>(state.Lookup(MetaState::kRootInodeId, "dir"));
}

TEST(MetaStateTest, RejectsRenameDirectoryIntoDescendant) {
    MetaState state;

    auto parent = state.Mkdir(MetaState::kRootInodeId, "parent", 0755, 0, 0);
    ASSERT_TRUE(parent.has_value()) << ToString(parent.error());
    auto child = state.Mkdir(parent->inode_id(), "child", 0755, 0, 0);
    ASSERT_TRUE(child.has_value()) << ToString(child.error());

    ExpectErrorKind<InvalidArgument>(
        state.Rename(MetaState::kRootInodeId, "parent", child->inode_id(), "moved")
    );
}

TEST(MetaStateTest, RenameOverwritesFileTargetAndRemovesTargetLayout) {
    MetaState state;
    auto source = state.CreateFile(MetaState::kRootInodeId, "source", 0644, 0, 0);
    ASSERT_TRUE(source.has_value()) << ToString(source.error());
    auto target = state.CreateFile(MetaState::kRootInodeId, "target", 0644, 0, 0);
    ASSERT_TRUE(target.has_value()) << ToString(target.error());

    ASSERT_TRUE(
        state.CommitWrite(source->inode_id(), source->version(), Extents({MakeExtent(0, 3, "src")}))
            .has_value()
    );
    ASSERT_TRUE(
        state.CommitWrite(target->inode_id(), target->version(), Extents({MakeExtent(0, 3, "dst")}))
            .has_value()
    );

    auto renamed = state.Rename(MetaState::kRootInodeId, "source", MetaState::kRootInodeId, "target");
    ASSERT_TRUE(renamed.has_value()) << ToString(renamed.error());
    EXPECT_EQ(renamed->inode_id(), source->inode_id());
    ExpectErrorKind<NotFound>(state.Lookup(MetaState::kRootInodeId, "source"));

    auto lookup = state.Lookup(MetaState::kRootInodeId, "target");
    ASSERT_TRUE(lookup.has_value()) << ToString(lookup.error());
    EXPECT_EQ(lookup->inode_id(), source->inode_id());
    EXPECT_EQ(state.ListLiveObjects(), std::vector<std::string>({"src"}));
}

TEST(MetaStateTest, RenameRejectsDirectoryOverFileAndFileOverDirectory) {
    MetaState state;
    auto dir = state.Mkdir(MetaState::kRootInodeId, "dir", 0755, 0, 0);
    ASSERT_TRUE(dir.has_value()) << ToString(dir.error());
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    ExpectErrorKind<NotDirectory>(
        state.Rename(MetaState::kRootInodeId, "dir", MetaState::kRootInodeId, "file")
    );
    ExpectErrorKind<IsDirectory>(
        state.Rename(MetaState::kRootInodeId, "file", MetaState::kRootInodeId, "dir")
    );
}

TEST(MetaStateTest, RmdirRejectsFile) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    ExpectErrorKind<NotDirectory>(state.Rmdir(MetaState::kRootInodeId, "file"));
}

TEST(MetaStateTest, CommitWriteMergesOverlappingExtents) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    auto first =
        state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 10, "a")}));
    ASSERT_TRUE(first.has_value()) << ToString(first.error());
    auto second =
        state.CommitWrite(file->inode_id(), first->version(), Extents({MakeExtent(4, 4, "b")}));
    ASSERT_TRUE(second.has_value()) << ToString(second.error());

    auto layout = state.GetLayout(file->inode_id());
    ASSERT_TRUE(layout.has_value()) << ToString(layout.error());
    ASSERT_EQ(layout->extents_size(), 3);
    EXPECT_EQ(layout->extents(0).offset(), 0U);
    EXPECT_EQ(layout->extents(0).length(), 4U);
    EXPECT_EQ(layout->extents(0).object_key(), "a");
    EXPECT_EQ(layout->extents(1).offset(), 4U);
    EXPECT_EQ(layout->extents(1).length(), 4U);
    EXPECT_EQ(layout->extents(1).object_key(), "b");
    EXPECT_EQ(layout->extents(2).offset(), 8U);
    EXPECT_EQ(layout->extents(2).length(), 2U);
    EXPECT_EQ(layout->extents(2).object_key(), "a");
    EXPECT_EQ(layout->extents(2).object_offset(), 8U);
    EXPECT_EQ(layout->attrs().size(), 10U);
}

TEST(MetaStateTest, AppendWriteRemapsRelativeExtents) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());
    auto base =
        state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 4, "base")}));
    ASSERT_TRUE(base.has_value()) << ToString(base.error());

    auto append =
        state.AppendWrite(file->inode_id(), base->version(), Extents({MakeExtent(0, 3, "tail")}));
    ASSERT_TRUE(append.has_value()) << ToString(append.error());
    EXPECT_EQ(append->append_start, 4U);
    ASSERT_EQ(append->committed_extents.size(), 1U);
    EXPECT_EQ(append->committed_extents[0].offset(), 4U);
    EXPECT_EQ(append->attrs.size(), 7U);

    auto layout = state.GetLayout(file->inode_id());
    ASSERT_TRUE(layout.has_value()) << ToString(layout.error());
    ASSERT_EQ(layout->extents_size(), 2);
    EXPECT_EQ(layout->extents(1).offset(), 4U);
    EXPECT_EQ(layout->extents(1).object_key(), "tail");
}

TEST(MetaStateTest, TruncateSlicesExtentsAndUpdatesSize) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());
    auto write =
        state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 10, "data")}));
    ASSERT_TRUE(write.has_value()) << ToString(write.error());

    auto truncated = state.Truncate(file->inode_id(), 6);
    ASSERT_TRUE(truncated.has_value()) << ToString(truncated.error());
    EXPECT_EQ(truncated->size(), 6U);

    auto layout = state.GetLayout(file->inode_id());
    ASSERT_TRUE(layout.has_value()) << ToString(layout.error());
    ASSERT_EQ(layout->extents_size(), 1);
    EXPECT_EQ(layout->extents(0).offset(), 0U);
    EXPECT_EQ(layout->extents(0).length(), 6U);
    EXPECT_EQ(layout->attrs().size(), 6U);
}

TEST(MetaStateTest, LiveObjectsExcludeOverwrittenTruncatedAndUnlinkedExtents) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    auto first = state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 8, "old")}));
    ASSERT_TRUE(first.has_value()) << ToString(first.error());
    auto overwrite = state.CommitWrite(file->inode_id(), first->version(), Extents({MakeExtent(0, 8, "new")}));
    ASSERT_TRUE(overwrite.has_value()) << ToString(overwrite.error());
    EXPECT_EQ(state.ListLiveObjects(), std::vector<std::string>({"new"}));

    auto truncate = state.Truncate(file->inode_id(), 0);
    ASSERT_TRUE(truncate.has_value()) << ToString(truncate.error());
    EXPECT_TRUE(state.ListLiveObjects().empty());

    auto rewrite = state.CommitWrite(file->inode_id(), truncate->version(), Extents({MakeExtent(0, 4, "live")}));
    ASSERT_TRUE(rewrite.has_value()) << ToString(rewrite.error());
    EXPECT_EQ(state.ListLiveObjects(), std::vector<std::string>({"live"}));

    auto unlink = state.Unlink(MetaState::kRootInodeId, "file");
    ASSERT_TRUE(unlink.has_value()) << ToString(unlink.error());
    EXPECT_TRUE(state.ListLiveObjects().empty());
}

TEST(MetaStateTest, RejectsInvalidCommittedAndAppendedExtents) {
    MetaState state;
    auto file = state.CreateFile(MetaState::kRootInodeId, "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());

    ExpectErrorKind<InvalidArgument>(
        state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 0, "empty")}))
    );
    ExpectErrorKind<InvalidArgument>(
        state.CommitWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 1, "")}))
    );
    ExpectErrorKind<InvalidArgument>(
        state.AppendWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 0, "empty")}))
    );
    ExpectErrorKind<InvalidArgument>(
        state.AppendWrite(file->inode_id(), file->version(), Extents({MakeExtent(0, 1, "")}))
    );
}

TEST(MetaStateTest, SnapshotRoundTripPreservesLiveLayout) {
    MetaState state;
    auto dir = state.Mkdir(MetaState::kRootInodeId, "dir", 0755, 0, 0);
    ASSERT_TRUE(dir.has_value()) << ToString(dir.error());
    auto file = state.CreateFile(dir->inode_id(), "file", 0644, 0, 0);
    ASSERT_TRUE(file.has_value()) << ToString(file.error());
    auto write = state.CommitWrite(
        file->inode_id(), file->version(), Extents({MakeExtent(0, 5, "live-key")})
    );
    ASSERT_TRUE(write.has_value()) << ToString(write.error());

    MetaState restored;
    auto restore = restored.RestoreSnapshot(state.SerializeSnapshot());
    ASSERT_TRUE(restore.has_value()) << ToString(restore.error());

    auto lookup = restored.Lookup(dir->inode_id(), "file");
    ASSERT_TRUE(lookup.has_value()) << ToString(lookup.error());
    auto layout = restored.GetLayout(file->inode_id());
    ASSERT_TRUE(layout.has_value()) << ToString(layout.error());
    ASSERT_EQ(layout->extents_size(), 1);
    EXPECT_EQ(layout->extents(0).object_key(), "live-key");

    auto live_objects = restored.ListLiveObjects();
    EXPECT_EQ(live_objects, std::vector<std::string>({"live-key"}));
}

TEST(MetaStateTest, RestoreSnapshotRejectsDanglingDentry) {
    MetaState state;
    MetaSnapshot snapshot;
    ASSERT_TRUE(snapshot.ParseFromString(state.SerializeSnapshot()));
    auto* dentry = snapshot.add_dentries();
    dentry->set_parent_inode_id(MetaState::kRootInodeId);
    dentry->set_name("dangling");
    dentry->set_child_inode_id(42);

    MetaState restored;
    ExpectErrorKind<InvalidArgument>(restored.RestoreSnapshot(snapshot.SerializeAsString()));
}

TEST(MetaStateTest, RestoreSnapshotRejectsDanglingLayout) {
    MetaState state;
    MetaSnapshot snapshot;
    ASSERT_TRUE(snapshot.ParseFromString(state.SerializeSnapshot()));
    auto* layout = snapshot.add_layouts();
    layout->set_inode_id(42);

    MetaState restored;
    ExpectErrorKind<InvalidArgument>(restored.RestoreSnapshot(snapshot.SerializeAsString()));
}

}  // namespace
}  // namespace pulpfs
