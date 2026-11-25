// Tests for the disk footprint engine: exact logical sizes, hard-link
// de-duplication by (st_dev, st_ino), sparse files, and immunity to symlink loops.
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "DiskAnalyzer.hpp"
#include "TestSupport.hpp"

using namespace vortex;

VT_TEST(disk_sums_logical_sizes_exactly) {
    const vt::TempDir temp("vortex-disk");
    vt::writeFileOfSize(temp.sub("tree/empty.bin"), 0);
    vt::writeFileOfSize(temp.sub("tree/one.bin"), 1);
    vt::writeFileOfSize(temp.sub("tree/page.bin"), 4096);
    vt::writeFileOfSize(temp.sub("tree/nested/deep/blob.bin"), 100000);
    vt::writeFileOfSize(temp.sub("tree/nested/odd.bin"), 12345);
    vt::makeDirs(temp.sub("tree/empty-dir"));

    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("tree"));
    VT_CHECK_EQ(usage.logicalBytes, std::uint64_t{0 + 1 + 4096 + 100000 + 12345});
    VT_CHECK_EQ(usage.fileCount, std::uint64_t{5});
    VT_CHECK_EQ(usage.directoryCount, std::uint64_t{4});  // tree, nested, deep, empty-dir
    VT_CHECK_EQ(usage.symlinkCount, std::uint64_t{0});
    VT_CHECK_EQ(usage.hardLinkDuplicates, std::uint64_t{0});
    VT_CHECK_EQ(usage.errorCount, std::uint64_t{0});
    VT_CHECK(!usage.permissionDenied);
    VT_CHECK(!usage.cancelled);

    // Allocation is reported in whole 512-byte units and covers at least the data written.
    VT_CHECK_EQ(usage.allocatedBytes % 512, std::uint64_t{0});
    VT_CHECK(usage.allocatedBytes >= 100000);
}

VT_TEST(disk_measures_a_single_file) {
    const vt::TempDir temp("vortex-disk");
    vt::writeFileOfSize(temp.sub("solo.bin"), 7777);
    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("solo.bin"));
    VT_CHECK_EQ(usage.logicalBytes, std::uint64_t{7777});
    VT_CHECK_EQ(usage.fileCount, std::uint64_t{1});
    VT_CHECK_EQ(usage.directoryCount, std::uint64_t{0});
}

VT_TEST(disk_counts_hard_linked_inodes_once) {
    const vt::TempDir temp("vortex-disk");
    const std::string original = temp.sub("tree/original.bin");
    vt::writeFileOfSize(original, 50000);
    vt::makeDirs(temp.sub("tree/links"));
    VT_REQUIRE(::link(original.c_str(), temp.sub("tree/links/alias-1.bin").c_str()) == 0);
    VT_REQUIRE(::link(original.c_str(), temp.sub("tree/links/alias-2.bin").c_str()) == 0);
    VT_REQUIRE(::link(original.c_str(), temp.sub("tree/alias-3.bin").c_str()) == 0);
    vt::writeFileOfSize(temp.sub("tree/unrelated.bin"), 300);

    struct stat st {};
    VT_REQUIRE(::stat(original.c_str(), &st) == 0);
    VT_CHECK_EQ(st.st_nlink, static_cast<nlink_t>(4));

    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("tree"));
    // Four names, one inode: the 50 000 bytes are counted exactly once.
    VT_CHECK_EQ(usage.logicalBytes, std::uint64_t{50000 + 300});
    VT_CHECK_EQ(usage.fileCount, std::uint64_t{2});
    VT_CHECK_EQ(usage.hardLinkDuplicates, std::uint64_t{3});

    // The same inode measured through a single name still counts in full.
    const DiskUsage single = DiskAnalyzer::measure(temp.sub("tree/links/alias-1.bin"));
    VT_CHECK_EQ(single.logicalBytes, std::uint64_t{50000});

    // Allocation must not be multiplied by the number of names either.
    const DiskUsage alone = DiskAnalyzer::measure(original);
    const DiskUsage linksOnly = DiskAnalyzer::measure(temp.sub("tree/links"));
    VT_CHECK(linksOnly.allocatedBytes < 2 * alone.allocatedBytes + 4096);
}

VT_TEST(disk_is_immune_to_symlink_loops) {
    const vt::TempDir temp("vortex-disk");
    vt::writeFileOfSize(temp.sub("tree/real/data.bin"), 2000);
    vt::writeFileOfSize(temp.sub("outside/secret.bin"), 999999);

    // a <-> b: two links that point at each other.
    VT_REQUIRE(::symlink("b", temp.sub("tree/a").c_str()) == 0);
    VT_REQUIRE(::symlink("a", temp.sub("tree/b").c_str()) == 0);
    // A link to itself, to its own directory, to the parent, and all the way up to "/".
    VT_REQUIRE(::symlink("self", temp.sub("tree/self").c_str()) == 0);
    VT_REQUIRE(::symlink(".", temp.sub("tree/real/here").c_str()) == 0);
    VT_REQUIRE(::symlink("..", temp.sub("tree/real/up").c_str()) == 0);
    VT_REQUIRE(::symlink("/", temp.sub("tree/root").c_str()) == 0);
    // A link that leaves the tree and a dangling one.
    VT_REQUIRE(::symlink(temp.sub("outside").c_str(), temp.sub("tree/escape").c_str()) == 0);
    VT_REQUIRE(::symlink("/nonexistent/target", temp.sub("tree/dangling").c_str()) == 0);

    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("tree"));
    // Terminating at all is the first assertion. Links are recorded, never followed.
    VT_CHECK_EQ(usage.symlinkCount, std::uint64_t{8});
    VT_CHECK_EQ(usage.fileCount, std::uint64_t{1});
    VT_CHECK_EQ(usage.logicalBytes, std::uint64_t{2000});
    VT_CHECK_EQ(usage.directoryCount, std::uint64_t{2});
    VT_CHECK_EQ(usage.errorCount, std::uint64_t{0});

    // A symlink given as the root is measured as a link, not as its target.
    const DiskUsage link = DiskAnalyzer::measure(temp.sub("tree/escape"));
    VT_CHECK_EQ(link.symlinkCount, std::uint64_t{1});
    VT_CHECK_EQ(link.fileCount, std::uint64_t{0});
    VT_CHECK_EQ(link.logicalBytes, std::uint64_t{0});
}

VT_TEST(disk_reports_sparse_files_by_allocation) {
    const vt::TempDir temp("vortex-disk");
    const std::string sparse = temp.sub("sparse.img");
    const int fd = ::open(sparse.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    VT_REQUIRE(fd >= 0);
    constexpr off_t kLogical = off_t{64} * 1024 * 1024;
    VT_REQUIRE(::ftruncate(fd, kLogical) == 0);
    VT_REQUIRE(::pwrite(fd, "tail", 4, kLogical - 4) == 4);
    ::close(fd);

    const DiskUsage usage = DiskAnalyzer::measure(sparse);
    VT_CHECK_EQ(usage.logicalBytes, static_cast<std::uint64_t>(kLogical));
    // APFS stores the hole for free: the allocation is a tiny fraction of the logical size.
    VT_CHECK(usage.allocatedBytes < static_cast<std::uint64_t>(kLogical) / 8);
    vt::note("64 MiB sparse file: logical " + std::to_string(usage.logicalBytes) + " bytes, allocated " +
             std::to_string(usage.allocatedBytes) + " bytes");
}

VT_TEST(disk_reports_missing_and_unreadable_paths) {
    const vt::TempDir temp("vortex-disk");
    const DiskUsage missing = DiskAnalyzer::measure(temp.sub("does-not-exist"));
    VT_CHECK_EQ(missing.logicalBytes, std::uint64_t{0});
    VT_CHECK(missing.errorCount >= 1);
    VT_CHECK(!missing.permissionDenied);

    const DiskUsage empty = DiskAnalyzer::measure("");
    VT_CHECK(empty.errorCount >= 1);

    if (::geteuid() == 0) {
        vt::note("running as root: the permission-denied case cannot be provoked");
        return;
    }
    vt::writeFileOfSize(temp.sub("tree/visible.bin"), 100);
    vt::writeFileOfSize(temp.sub("tree/locked/hidden.bin"), 5000);
    VT_REQUIRE(::chmod(temp.sub("tree/locked").c_str(), 0000) == 0);

    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("tree"));
    VT_CHECK(usage.permissionDenied);
    VT_CHECK(usage.errorCount >= 1);
    VT_CHECK_EQ(usage.firstDeniedPath, temp.sub("tree/locked"));
    VT_CHECK_EQ(usage.logicalBytes, std::uint64_t{100});  // only what could be read
    ::chmod(temp.sub("tree/locked").c_str(), 0700);
}

VT_TEST(disk_stops_when_cancelled) {
    const vt::TempDir temp("vortex-disk");
    for (int i = 0; i < 200; ++i) vt::writeFileOfSize(temp.sub("tree/f" + std::to_string(i)), 10);

    const CancellationToken token;
    token.cancel();
    const DiskUsage usage = DiskAnalyzer::measure(temp.sub("tree"), token);
    VT_CHECK(usage.cancelled);
    VT_CHECK_EQ(usage.fileCount, std::uint64_t{0});

    DiskUsage total;
    total += DiskAnalyzer::measure(temp.sub("tree"));
    total += DiskAnalyzer::measure(temp.sub("tree"));
    VT_CHECK_EQ(total.logicalBytes, std::uint64_t{2 * 200 * 10});
    VT_CHECK_EQ(total.fileCount, std::uint64_t{400});
}
