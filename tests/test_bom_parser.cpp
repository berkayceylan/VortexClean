// Tests for the native BOMStore parser: exact path extraction from synthetic
// images, structural corruption, and fuzzing. Run under ASan/UBSan these prove
// that no malformed input can make the parser read out of bounds or loop.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "BomBuilder.hpp"
#include "BomParser.hpp"
#include "FileUtil.hpp"
#include "PathUtil.hpp"
#include "ReceiptStore.hpp"
#include "StringUtil.hpp"
#include "TestSupport.hpp"

using namespace vortex;
using vt::BomBuilder;
using vt::BomLayout;

namespace {

std::vector<std::string> sortedPaths(const BomContents& contents) {
    std::vector<std::string> paths;
    paths.reserve(contents.entries.size());
    for (const BomEntry& entry : contents.entries) paths.push_back(entry.path);
    std::sort(paths.begin(), paths.end());
    return paths;
}

const BomEntry* findEntry(const BomContents& contents, std::string_view path) {
    for (const BomEntry& entry : contents.entries) {
        if (entry.path == path) return &entry;
    }
    return nullptr;
}

/// Parses a copy held in an exactly-sized heap block, so that AddressSanitizer
/// flags a read even one byte past the end of the input.
Result<BomContents> parseExact(const std::uint8_t* data, std::size_t size) {
    const std::vector<std::uint8_t> exact(data, data + size);
    return BomParser::parse(std::span<const std::uint8_t>(exact.data(), exact.size()));
}
Result<BomContents> parseExact(const std::vector<std::uint8_t>& image) {
    return parseExact(image.data(), image.size());
}

/// Small deterministic generator: fuzz runs must be reproducible.
class Random {
public:
    explicit Random(std::uint64_t seed) : state_(seed) {}
    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }
    std::size_t below(std::size_t bound) { return static_cast<std::size_t>(next() % bound); }

private:
    std::uint64_t state_;
};

BomBuilder sampleBuilder() {
    BomBuilder builder;
    builder.dir("./Applications/Rocket.app/Contents/MacOS")
        .file("./Applications/Rocket.app/Contents/MacOS/Rocket", 123456, 0100755)
        .file("./Applications/Rocket.app/Contents/Info.plist", 812)
        .symlink("./usr/local/bin/rocketctl", "/Applications/Rocket.app/Contents/MacOS/Rocket")
        .file("./Library/LaunchDaemons/com.acme.rocket.daemon.plist", 430);
    return builder;
}

/// A compact multi-level tree: three records per leaf, two children per branch.
BomBuilder compactBuilder() {
    BomBuilder builder;
    for (int i = 0; i < 10; ++i) {
        builder.file("./opt/acme/lib/module" + std::to_string(i) + ".dylib", static_cast<std::uint32_t>(1000 + i));
    }
    builder.symlink("./opt/acme/current", "lib").leafCapacity(3).branchFanout(2).pageSize(0);
    return builder;
}

}  // namespace

// ---------------------------------------------------------------------------
// Exact extraction
// ---------------------------------------------------------------------------

VT_TEST(bom_extracts_exact_paths_and_metadata) {
    const BomBuilder builder = sampleBuilder();
    const std::vector<std::uint8_t> image = builder.build();
    const auto parsed = parseExact(image);
    VT_REQUIRE(parsed.ok());
    const BomContents& contents = parsed.value();

    // Spelled out literally so the expectation does not depend on the builder.
    const std::vector<std::string> expected{
        ".",
        "./Applications",
        "./Applications/Rocket.app",
        "./Applications/Rocket.app/Contents",
        "./Applications/Rocket.app/Contents/Info.plist",
        "./Applications/Rocket.app/Contents/MacOS",
        "./Applications/Rocket.app/Contents/MacOS/Rocket",
        "./Library",
        "./Library/LaunchDaemons",
        "./Library/LaunchDaemons/com.acme.rocket.daemon.plist",
        "./usr",
        "./usr/local",
        "./usr/local/bin",
        "./usr/local/bin/rocketctl",
    };
    VT_CHECK_EQ(sortedPaths(contents), expected);
    VT_CHECK_EQ(builder.expectedPaths(), expected);
    VT_CHECK_EQ(contents.declaredPathCount, static_cast<std::uint32_t>(expected.size()));
    VT_CHECK_EQ(contents.skippedEntries, 0u);

    const BomEntry* executable = findEntry(contents, "./Applications/Rocket.app/Contents/MacOS/Rocket");
    VT_REQUIRE(executable != nullptr);
    VT_CHECK(executable->type == BomEntryType::File);
    VT_CHECK_EQ(executable->size, 123456u);
    VT_CHECK_EQ(executable->mode, static_cast<std::uint16_t>(0100755));
    VT_CHECK_EQ(executable->gid, 80u);

    const BomEntry* link = findEntry(contents, "./usr/local/bin/rocketctl");
    VT_REQUIRE(link != nullptr);
    VT_CHECK(link->type == BomEntryType::Symlink);
    VT_CHECK_EQ(link->linkTarget, std::string("/Applications/Rocket.app/Contents/MacOS/Rocket"));

    const BomEntry* directory = findEntry(contents, "./Applications/Rocket.app");
    VT_REQUIRE(directory != nullptr);
    VT_CHECK(directory->type == BomEntryType::Directory);

    const std::vector<std::string> expectedVariables{"BomInfo", "Paths", "HLIndex", "VIndex", "Size64"};
    VT_CHECK_EQ(contents.variableNames, expectedVariables);
}

VT_TEST(bom_walks_multi_level_btree) {
    BomBuilder builder;
    for (int group = 0; group < 20; ++group) {
        for (int item = 0; item < 15; ++item) {
            builder.file("./Library/Application Support/Acme/group" + std::to_string(group) + "/item" +
                             std::to_string(item) + ".dat",
                         static_cast<std::uint32_t>(group * 100 + item));
        }
    }
    // Four records per leaf and three children per branch force a deep tree
    // whose leaves can only be reached through the forward chain.
    builder.leafCapacity(4).branchFanout(3);
    BomLayout layout;
    const std::vector<std::uint8_t> image = builder.build(&layout);
    VT_CHECK(layout.leafPages.size() > 70);
    VT_CHECK(layout.branchPages.size() > 30);

    const auto parsed = parseExact(image);
    VT_REQUIRE(parsed.ok());
    VT_CHECK_EQ(sortedPaths(parsed.value()), builder.expectedPaths());
    VT_CHECK_EQ(parsed.value().entries.size(), std::size_t{20 * 15 + 20 + 4});
    VT_CHECK_EQ(parsed.value().declaredPathCount, static_cast<std::uint32_t>(parsed.value().entries.size()));
}

VT_TEST(bom_does_not_depend_on_record_order) {
    BomBuilder builder = sampleBuilder();
    builder.reverseRecordOrder(true).leafCapacity(2).branchFanout(2);
    const auto parsed = parseExact(builder.build());
    VT_REQUIRE(parsed.ok());
    VT_CHECK_EQ(sortedPaths(parsed.value()), builder.expectedPaths());
    VT_CHECK_EQ(parsed.value().skippedEntries, 0u);
}

VT_TEST(bom_parse_file_round_trip) {
    const vt::TempDir temp("vortex-bom");
    const BomBuilder builder = sampleBuilder();
    const std::string file = temp.sub("receipt.bom");
    vt::writeFile(file, builder.build());

    const auto parsed = BomParser::parseFile(file);
    VT_REQUIRE(parsed.ok());
    VT_CHECK_EQ(sortedPaths(parsed.value()), builder.expectedPaths());

    VT_CHECK(!BomParser::parseFile(temp.sub("missing.bom")).ok());
    VT_CHECK(!BomParser::parseFile(temp.path()).ok());  // a directory is not a BOM

    BomParseOptions tiny;
    tiny.maxFileBytes = 64;
    VT_CHECK(!BomParser::parseFile(file, tiny).ok());
}

VT_TEST(bom_honours_cancellation) {
    BomBuilder builder;
    for (int i = 0; i < 9000; ++i) builder.file("./data/f" + std::to_string(i));
    const std::vector<std::uint8_t> image = builder.build();
    const CancellationToken token;
    token.cancel();
    const auto parsed = BomParser::parse(image, BomParseOptions{}, token);
    VT_CHECK(!parsed.ok());
    VT_CHECK_EQ(parsed.error().code, ECANCELED);
}

// ---------------------------------------------------------------------------
// Structural corruption
// ---------------------------------------------------------------------------

VT_TEST(bom_rejects_structurally_invalid_images) {
    BomLayout layout;
    const std::vector<std::uint8_t> good = sampleBuilder().build(&layout);
    VT_REQUIRE(parseExact(good).ok());

    VT_CHECK(!parseExact(nullptr, 0).ok());
    VT_CHECK(!parseExact(good.data(), 8).ok());
    VT_CHECK(!parseExact(good.data(), 31).ok());

    {
        auto image = good;  // wrong magic
        image[0] = 'X';
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // unsupported version
        BomBuilder::patchU32(image, 8, 2);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // block table beyond the end of the file
        BomBuilder::patchU32(image, 16, static_cast<std::uint32_t>(image.size() - 2));
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // block table length overflowing 32 bits when added to its offset
        BomBuilder::patchU32(image, 20, 0xFFFFFFFFu);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // more block pointers than the table region can hold
        BomBuilder::patchU32(image, layout.indexOffset, 0x10000000u);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // variable table beyond the end of the file
        BomBuilder::patchU32(image, 24, 0xFFFFFF00u);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // a variable count far larger than the table
        BomBuilder::patchU32(image, layout.varsOffset, 5000);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // no variable is called "Paths" any more
        const char needle[] = "Paths";
        const auto found = std::search(image.begin() + static_cast<std::ptrdiff_t>(layout.varsOffset), image.end(),
                                       needle, needle + 5);
        VT_REQUIRE(found != image.end());
        *found = 'X';
        const auto parsed = parseExact(image);
        VT_CHECK(!parsed.ok());
    }
    {
        auto image = good;  // the tree header lost its magic
        image[layout.blockAddress[layout.treeBlock]] = 'x';
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // the tree root points at the null block
        BomBuilder::patchU32(image, layout.blockAddress[layout.treeBlock] + 8, 0);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // the tree root points past the block table
        BomBuilder::patchU32(image, layout.blockAddress[layout.treeBlock] + 8, 0x00FFFFFFu);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // a block whose address lies outside the file
        BomBuilder::patchU32(image, layout.pointerSlot(layout.leafPages.front()),
                             static_cast<std::uint32_t>(image.size() + 100));
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // a block whose length runs past the end of the file
        BomBuilder::patchU32(image, layout.pointerSlot(layout.leafPages.front()) + 4, 0x7FFFFFFFu);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // a leaf claiming more entries than fit in its page
        BomBuilder::patchU16(image, layout.blockAddress[layout.leafPages.front()] + 2, 0xFFFF);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // a leaf entry pointing at a non-existent path-info block
        BomBuilder::patchU32(image, layout.blockAddress[layout.leafPages.front()] + 12, 0x00FFFFF0u);
        VT_CHECK(!parseExact(image).ok());
    }
}

VT_TEST(bom_detects_page_cycles) {
    BomLayout layout;
    const BomBuilder builder = compactBuilder();
    const std::vector<std::uint8_t> good = builder.build(&layout);
    VT_REQUIRE(parseExact(good).ok());
    VT_REQUIRE(layout.leafPages.size() >= 4);
    VT_REQUIRE(layout.branchPages.size() >= 2);

    {
        auto image = good;  // a leaf whose forward link points back at itself
        const std::uint32_t leaf = layout.leafPages[1];
        BomBuilder::patchU32(image, layout.blockAddress[leaf] + 4, leaf);
        const auto parsed = parseExact(image);
        VT_CHECK(!parsed.ok());
    }
    {
        auto image = good;  // the last leaf links back to the first: a full ring
        BomBuilder::patchU32(image, layout.blockAddress[layout.leafPages.back()] + 4, layout.leafPages.front());
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // the root branch names itself as its leftmost child
        BomBuilder::patchU32(image, layout.blockAddress[layout.rootPage] + 12, layout.rootPage);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // two branch pages pointing at each other
        const std::uint32_t lower = layout.branchPages.front();
        BomBuilder::patchU32(image, layout.blockAddress[lower] + 12, layout.rootPage);
        BomBuilder::patchU32(image, layout.blockAddress[layout.rootPage] + 12, lower);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        auto image = good;  // the leaf chain runs into a branch page
        BomBuilder::patchU32(image, layout.blockAddress[layout.leafPages.front()] + 4, layout.rootPage);
        VT_CHECK(!parseExact(image).ok());
    }
}

VT_TEST(bom_survives_hostile_path_records) {
    BomLayout layout;
    const BomBuilder builder = sampleBuilder();
    const std::vector<std::uint8_t> good = builder.build(&layout);
    const std::size_t total = builder.expectedPaths().size();

    const auto fileNameOffset = [&layout](std::string_view path) {
        const BomLayout::Record* record = layout.find(path);
        return record == nullptr ? std::size_t{0} : layout.blockAddress[record->fileBlock] + 4;
    };
    const auto parentOffset = [&layout](std::string_view path) {
        const BomLayout::Record* record = layout.find(path);
        return record == nullptr ? std::size_t{0} : layout.blockAddress[record->fileBlock];
    };

    {
        // Two directories that claim each other as parent: both, and everything
        // below them, must be dropped — without looping.
        auto image = good;
        const BomLayout::Record* usr = layout.find("./usr");
        const BomLayout::Record* local = layout.find("./usr/local");
        VT_REQUIRE(usr != nullptr && local != nullptr);
        BomBuilder::patchU32(image, parentOffset("./usr"), local->id);
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 4u);  // usr, local, bin, rocketctl
        VT_CHECK_EQ(parsed.value().entries.size(), total - 4);
        for (const BomEntry& entry : parsed.value().entries) VT_CHECK(!entry.path.starts_with("./usr"));
    }
    {
        // A record that is its own parent.
        auto image = good;
        const BomLayout::Record* library = layout.find("./Library");
        VT_REQUIRE(library != nullptr);
        BomBuilder::patchU32(image, parentOffset("./Library"), library->id);
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 3u);
    }
    {
        // An orphan: the parent id does not exist.
        auto image = good;
        BomBuilder::patchU32(image, parentOffset("./usr/local/bin"), 0x00ABCDEFu);
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 2u);  // bin and rocketctl
        VT_CHECK(findEntry(parsed.value(), "./usr/local") != nullptr);
    }
    {
        // A second root (parent 0, but not ".") and everything below it: not payload.
        // Seen in a real Apple receipt (".TemporaryItems"); lsbom does not list it either.
        auto image = good;
        BomBuilder::patchU32(image, parentOffset("./usr/local/bin"), 0u);
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 2u);  // bin and rocketctl
        VT_CHECK(findEntry(parsed.value(), "./usr/local") != nullptr);
        for (const BomEntry& entry : parsed.value().entries) {
            VT_CHECK(entry.path == "." || entry.path.starts_with("./"));
        }
    }
    {
        // A ".." component must never survive: it could escape the install prefix.
        auto image = good;
        const std::size_t offset = fileNameOffset("./usr");
        image[offset] = '.';
        image[offset + 1] = '.';
        image[offset + 2] = '\0';
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 4u);
        for (const BomEntry& entry : parsed.value().entries) {
            VT_CHECK(entry.path.find("..") == std::string::npos);
        }
    }
    {
        // A name containing a path separator is rejected.
        auto image = good;
        image[fileNameOffset("./Library") + 2] = '/';
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 3u);
    }
    {
        // An empty name is rejected.
        auto image = good;
        image[fileNameOffset("./Library")] = '\0';
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK_EQ(parsed.value().skippedEntries, 3u);
    }
    {
        // Two records with the same id: the second one is dropped.
        auto image = good;
        const BomLayout::Record* first = layout.find("./Library");
        const BomLayout::Record* second = layout.find("./usr");
        VT_REQUIRE(first != nullptr && second != nullptr);
        BomBuilder::patchU32(image, layout.blockAddress[second->info1Block], first->id);
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK(parsed.value().skippedEntries >= 1u);
    }
    {
        // A symlink target length that runs past its block.
        auto image = good;
        const BomLayout::Record* link = layout.find("./usr/local/bin/rocketctl");
        VT_REQUIRE(link != nullptr);
        BomBuilder::patchU32(image, layout.blockAddress[link->info2Block] + 27, 0x0FFFFFFFu);
        VT_CHECK(!parseExact(image).ok());
    }
    {
        // A name that is not NUL-terminated inside its block: shrink the block so
        // that it ends in the middle of the name.
        auto image = good;
        const BomLayout::Record* record = layout.find("./Applications");
        VT_REQUIRE(record != nullptr);
        BomBuilder::patchU32(image, layout.pointerSlot(record->fileBlock) + 4, 4 + 5);  // "Appli"
        const auto parsed = parseExact(image);
        VT_REQUIRE(parsed.ok());
        VT_CHECK(findEntry(parsed.value(), "./Appli") != nullptr);
        VT_CHECK(findEntry(parsed.value(), "./Appli/Rocket.app") != nullptr);
    }
    {
        // Paths longer than the configured limit are dropped, not truncated.
        BomParseOptions strict;
        strict.maxPathLength = 30;
        const auto parsed = BomParser::parse(good, strict);
        VT_REQUIRE(parsed.ok());
        for (const BomEntry& entry : parsed.value().entries) VT_CHECK(entry.path.size() <= 30);
        VT_CHECK(parsed.value().skippedEntries > 0u);
    }
    {
        // The record cap is enforced.
        BomParseOptions capped;
        capped.maxEntries = 5;
        VT_CHECK(!BomParser::parse(good, capped).ok());
    }
}

// ---------------------------------------------------------------------------
// Fuzzing
// ---------------------------------------------------------------------------

VT_TEST(bom_fuzz_every_truncation) {
    const BomBuilder builder = compactBuilder();
    const std::vector<std::uint8_t> image = builder.build();
    const std::size_t expected = builder.expectedPaths().size();
    VT_REQUIRE(parseExact(image).ok());

    std::size_t accepted = 0;
    for (std::size_t length = 0; length < image.size(); ++length) {
        const auto parsed = parseExact(image.data(), length);
        if (parsed.ok()) {
            ++accepted;
            VT_CHECK(parsed.value().entries.size() <= expected);
        }
    }
    // The block table sits at the very end, so no strict prefix can be complete.
    VT_CHECK_EQ(accepted, std::size_t{0});
    vt::note(std::to_string(image.size()) + " truncated images rejected cleanly");
}

VT_TEST(bom_fuzz_every_field_with_boundary_values) {
    const BomBuilder builder = compactBuilder();
    const std::vector<std::uint8_t> image = builder.build();
    const std::size_t expected = builder.expectedPaths().size();
    const auto size = static_cast<std::uint32_t>(image.size());
    const std::vector<std::uint32_t> values{
        0u, 1u, 2u, 0x7Fu, 0xFFu, 0x100u, 0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
        size, size - 1, size + 1, size - 4, size / 2,
    };

    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (std::size_t offset = 0; offset + 4 <= image.size(); ++offset) {
        for (const std::uint32_t value : values) {
            std::vector<std::uint8_t> mutated = image;
            BomBuilder::patchU32(mutated, offset, value);
            const auto parsed = parseExact(mutated);
            if (parsed.ok()) {
                ++accepted;
                // Whatever survives must stay within the limits of the input.
                VT_CHECK(parsed.value().entries.size() <= expected);
                for (const BomEntry& entry : parsed.value().entries) {
                    VT_CHECK(entry.path.find("/../") == std::string::npos);
                    VT_CHECK(!entry.path.ends_with("/.."));
                }
            } else {
                ++rejected;
            }
        }
    }
    VT_CHECK(rejected > 0);
    vt::note(std::to_string(accepted + rejected) + " mutated images: " + std::to_string(rejected) +
             " rejected, " + std::to_string(accepted) + " parsed within bounds");
}

VT_TEST(bom_fuzz_random_mutations) {
    BomBuilder builder = sampleBuilder();
    builder.leafCapacity(3).branchFanout(2).pageSize(64);
    const std::vector<std::uint8_t> image = builder.build();
    const std::size_t expected = builder.expectedPaths().size();
    VT_REQUIRE(parseExact(image).ok());

    Random random(0x5EEDBA5EULL);
    std::size_t accepted = 0;
    constexpr int kIterations = 40000;
    for (int iteration = 0; iteration < kIterations; ++iteration) {
        std::vector<std::uint8_t> mutated = image;
        const std::size_t edits = 1 + random.below(8);
        for (std::size_t edit = 0; edit < edits; ++edit) {
            const std::size_t offset = random.below(mutated.size());
            switch (random.below(4)) {
                case 0: mutated[offset] = static_cast<std::uint8_t>(random.next()); break;
                case 1: mutated[offset] ^= static_cast<std::uint8_t>(1u << random.below(8)); break;
                case 2: mutated[offset] = 0xFF; break;
                default: mutated[offset] = 0x00; break;
            }
        }
        if (random.below(10) == 0) mutated.resize(random.below(mutated.size()) + 1);  // also truncate sometimes
        const auto parsed = parseExact(mutated);
        if (parsed.ok()) {
            ++accepted;
            VT_CHECK(parsed.value().entries.size() <= expected);
        }
    }
    vt::note(std::to_string(kIterations) + " random mutations: " + std::to_string(accepted) +
             " still parsed, the rest were rejected; no crash, no hang");
}

VT_TEST(bom_fuzz_random_garbage) {
    Random random(0xC0FFEEULL);
    for (int iteration = 0; iteration < 5000; ++iteration) {
        std::vector<std::uint8_t> garbage(32 + random.below(600));
        for (std::uint8_t& byte : garbage) byte = static_cast<std::uint8_t>(random.next());
        std::memcpy(garbage.data(), "BOMStore", 8);
        garbage[8] = garbage[9] = garbage[10] = 0;
        garbage[11] = 1;  // valid version, everything else random
        if (iteration % 2 == 0) {
            // Aim the table offsets inside the buffer so deeper code is reached.
            BomBuilder::patchU32(garbage, 16, static_cast<std::uint32_t>(random.below(garbage.size())));
            BomBuilder::patchU32(garbage, 20, static_cast<std::uint32_t>(random.below(garbage.size())));
            BomBuilder::patchU32(garbage, 24, static_cast<std::uint32_t>(random.below(garbage.size())));
            BomBuilder::patchU32(garbage, 28, static_cast<std::uint32_t>(random.below(garbage.size())));
        }
        (void)parseExact(garbage);
    }
}

// ---------------------------------------------------------------------------
// Install-prefix resolution
// ---------------------------------------------------------------------------

VT_TEST(receipt_paths_resolve_against_install_prefix) {
    const ReceiptStore store("/");
    VT_CHECK_EQ(store.resolve("", "."), std::string("/"));
    VT_CHECK_EQ(store.resolve("/", "./Applications/Foo.app"), std::string("/Applications/Foo.app"));
    VT_CHECK_EQ(store.resolve("", "./Applications/Foo.app"), std::string("/Applications/Foo.app"));
    VT_CHECK_EQ(store.resolve("Applications", "./Foo.app/Contents"), std::string("/Applications/Foo.app/Contents"));
    VT_CHECK_EQ(store.resolve("/Applications/", "."), std::string("/Applications"));
    VT_CHECK_EQ(store.resolve("usr/local/share/dotnet", "./host/fxr"), std::string("/usr/local/share/dotnet/host/fxr"));
    // The root aliases are folded onto their real location.
    VT_CHECK_EQ(store.resolve("", "./etc/acme.conf"), std::string("/private/etc/acme.conf"));
    VT_CHECK_EQ(store.resolve("var/db", "./acme"), std::string("/private/var/db/acme"));
    // Anything that tries to climb out is refused outright.
    VT_CHECK_EQ(store.resolve("", "./../etc/passwd"), std::string());
    VT_CHECK_EQ(store.resolve("../..", "./x"), std::string());
    VT_CHECK_EQ(store.resolve("Applications", "./Foo.app/../../etc"), std::string());

    const ReceiptStore scratch("/private/tmp/vortex-root");
    VT_CHECK_EQ(scratch.resolve("", "./Library/Acme"), std::string("/private/tmp/vortex-root/Library/Acme"));
    VT_CHECK_EQ(scratch.resolve("/", "."), std::string("/private/tmp/vortex-root"));
    VT_CHECK_EQ(scratch.resolve("", "./etc/acme.conf"), std::string("/private/tmp/vortex-root/etc/acme.conf"));
}

// ---------------------------------------------------------------------------
// Real receipts written by Installer.app on this machine
// ---------------------------------------------------------------------------

VT_TEST(bom_parses_every_real_receipt_on_this_machine) {
    const std::string directory = "/private/var/db/receipts";
    auto listing = io::listDirectory(directory);
    if (!listing) {
        vt::note("no receipts directory on this machine; nothing to cross-check");
        return;
    }
    std::size_t files = 0;
    std::size_t paths = 0;
    for (const io::DirEntry& entry : listing.value()) {
        if (!str::iendsWith(entry.name, ".bom")) continue;
        const std::string file = path::join(directory, entry.name);
        const auto parsed = BomParser::parseFile(file);
        if (!parsed.ok()) {
            vt::reportFailure(__FILE__, __LINE__, "cannot parse " + file + ": " + parsed.error().message);
            continue;
        }
        ++files;
        paths += parsed.value().entries.size();
        // Apple's own record count is the independent oracle: the leftmost-descent
        // plus forward-chain walk must account for every record.
        if (parsed.value().entries.size() != parsed.value().declaredPathCount ||
            parsed.value().skippedEntries != 0) {
            vt::reportFailure(__FILE__, __LINE__,
                              file + ": extracted " + std::to_string(parsed.value().entries.size()) + " of " +
                                  std::to_string(parsed.value().declaredPathCount) + " declared paths, skipped " +
                                  std::to_string(parsed.value().skippedEntries));
        }
        bool hasRoot = false;
        for (const BomEntry& bomEntry : parsed.value().entries) {
            if (bomEntry.path == ".") hasRoot = true;
        }
        VT_CHECK(hasRoot);
    }
    vt::note(std::to_string(files) + " real receipts, " + std::to_string(paths) +
             " paths: every record accounted for");
}
