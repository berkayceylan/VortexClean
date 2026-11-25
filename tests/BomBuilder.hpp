// Test-only writer for Apple's BOMStore format — effectively a miniature mkbom.
//
// It reproduces the layout of real receipts (verified against the files in
// /private/var/db/receipts): a 512-byte header area, data blocks, the variable
// table, then the block table with its free list; leaf pages chained through
// `forward`, and branch pages holding `count` keys plus `count + 1` children.
// The returned BomLayout exposes where every structure landed so that tests can
// corrupt individual fields precisely.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "BomParser.hpp"

namespace vt {

struct BomLayout {
    struct Record {
        std::uint32_t id = 0;
        std::uint32_t info1Block = 0;
        std::uint32_t info2Block = 0;
        std::uint32_t fileBlock = 0;
        std::string path;
    };

    std::size_t indexOffset = 0;  ///< file offset of the block table
    std::size_t indexLength = 0;
    std::size_t varsOffset = 0;
    std::size_t varsLength = 0;
    std::vector<std::size_t> blockAddress;  ///< file offset of each block, by block index
    std::vector<std::size_t> blockLength;
    std::uint32_t treeBlock = 0;  ///< the "Paths" tree header
    std::uint32_t rootPage = 0;
    std::vector<std::uint32_t> leafPages;    ///< in chain order
    std::vector<std::uint32_t> branchPages;  ///< bottom level first, root last
    std::vector<Record> records;             ///< in the order they were written

    [[nodiscard]] const Record* find(std::string_view path) const {
        for (const Record& record : records) {
            if (record.path == path) return &record;
        }
        return nullptr;
    }
    /// File offset of the block-table slot (address, length) of a block.
    [[nodiscard]] std::size_t pointerSlot(std::uint32_t block) const { return indexOffset + 4 + block * 8u; }
};

class BomBuilder {
public:
    BomBuilder& file(std::string path, std::uint32_t size = 0, std::uint16_t mode = 0100644) {
        return add(std::move(path), vortex::BomEntryType::File, size, mode, {});
    }
    BomBuilder& dir(std::string path, std::uint16_t mode = 040755) {
        return add(std::move(path), vortex::BomEntryType::Directory, 0, mode, {});
    }
    BomBuilder& symlink(std::string path, std::string target) {
        return add(std::move(path), vortex::BomEntryType::Symlink, 0, 0120755, std::move(target));
    }

    BomBuilder& leafCapacity(std::size_t records) {
        leafCapacity_ = std::max<std::size_t>(1, records);
        return *this;
    }
    BomBuilder& branchFanout(std::size_t children) {
        branchFanout_ = std::max<std::size_t>(2, children);
        return *this;
    }
    /// 4096 mirrors real files; 0 makes every page exactly as large as its content.
    BomBuilder& pageSize(std::size_t bytes) {
        pageSize_ = bytes;
        return *this;
    }
    /// Writes records children-first, to prove that parsing does not depend on order.
    BomBuilder& reverseRecordOrder(bool reversed) {
        reversed_ = reversed;
        return *this;
    }

    /// Every path the image will contain (including the root and implied parents), sorted.
    [[nodiscard]] std::vector<std::string> expectedPaths() const {
        std::vector<std::string> paths;
        for (const auto& [path, entry] : complete()) paths.push_back(path);
        return paths;
    }

    [[nodiscard]] std::vector<std::uint8_t> build(BomLayout* layoutOut = nullptr) const {
        const std::map<std::string, Entry> all = complete();
        std::map<std::string, std::uint32_t> idOf;
        std::uint32_t nextId = 1;
        for (const auto& [path, entry] : all) idOf[path] = nextId++;

        BomLayout layout;
        std::vector<Bytes> blocks(1);  // block 0 is the null block
        const auto addBlock = [&blocks](Bytes bytes) {
            blocks.push_back(std::move(bytes));
            return static_cast<std::uint32_t>(blocks.size() - 1);
        };

        // ---- BomInfo ------------------------------------------------------
        Bytes info;
        u32(info, 1);
        u32(info, static_cast<std::uint32_t>(all.size() + 1));
        u32(info, 0);
        const std::uint32_t bomInfoBlock = addBlock(std::move(info));

        // ---- Path records -------------------------------------------------
        for (const auto& [path, entry] : all) {
            Bytes info2;
            u8(info2, static_cast<std::uint8_t>(entry.type));
            u8(info2, 1);
            u16(info2, entry.type == vortex::BomEntryType::Directory ? 0x000F : 0x0003);
            u16(info2, entry.mode);
            u32(info2, 0);   // uid
            u32(info2, 80);  // gid (admin)
            u32(info2, 0x6543'2100);  // mtime
            u32(info2, entry.size);
            u8(info2, 1);
            u32(info2, entry.type == vortex::BomEntryType::File ? 0xC0FF'EE00u + idOf[path] : 0);
            if (entry.linkTarget.empty()) {
                u32(info2, 0);
            } else {
                u32(info2, static_cast<std::uint32_t>(entry.linkTarget.size() + 1));
                text(info2, entry.linkTarget);
                u8(info2, 0);
            }
            BomLayout::Record record;
            record.path = path;
            record.id = idOf[path];
            record.info2Block = addBlock(std::move(info2));

            Bytes info1;
            u32(info1, record.id);
            u32(info1, record.info2Block);
            record.info1Block = addBlock(std::move(info1));

            Bytes file;
            const std::size_t slash = path.rfind('/');
            const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
            u32(file, parent.empty() ? 0 : idOf[parent]);
            text(file, slash == std::string::npos ? path : path.substr(slash + 1));
            u8(file, 0);
            record.fileBlock = addBlock(std::move(file));
            layout.records.push_back(std::move(record));
        }
        // Apple orders the tree by (parent id, name); lsbom depends on it.
        const auto key = [&idOf](const BomLayout::Record& record) {
            const std::size_t slash = record.path.rfind('/');
            return slash == std::string::npos
                       ? std::pair<std::uint32_t, std::string>(0, record.path)
                       : std::pair<std::uint32_t, std::string>(idOf.at(record.path.substr(0, slash)),
                                                               record.path.substr(slash + 1));
        };
        std::stable_sort(layout.records.begin(), layout.records.end(),
                         [&key](const BomLayout::Record& a, const BomLayout::Record& b) { return key(a) < key(b); });
        if (reversed_) std::reverse(layout.records.begin(), layout.records.end());

        // ---- Leaf pages ---------------------------------------------------
        struct Node {
            std::uint32_t block;
            std::uint32_t lastKey;  // file block of the last record below this node
        };
        std::vector<Node> level;
        const std::size_t recordCount = layout.records.size();
        const std::size_t leafCount = std::max<std::size_t>(1, (recordCount + leafCapacity_ - 1) / leafCapacity_);
        const auto firstLeaf = static_cast<std::uint32_t>(blocks.size());
        for (std::size_t leaf = 0; leaf < leafCount; ++leaf) {
            const std::size_t begin = leaf * leafCapacity_;
            const std::size_t end = std::min(recordCount, begin + leafCapacity_);
            Bytes page;
            u16(page, 1);
            u16(page, static_cast<std::uint16_t>(end - begin));
            u32(page, leaf + 1 < leafCount ? firstLeaf + static_cast<std::uint32_t>(leaf) + 1 : 0);
            u32(page, leaf > 0 ? firstLeaf + static_cast<std::uint32_t>(leaf) - 1 : 0);
            std::uint32_t lastKey = 0;
            for (std::size_t i = begin; i < end; ++i) {
                u32(page, layout.records[i].info1Block);
                u32(page, layout.records[i].fileBlock);
                lastKey = layout.records[i].fileBlock;
            }
            pad(page);
            const std::uint32_t block = addBlock(std::move(page));
            layout.leafPages.push_back(block);
            level.push_back(Node{block, lastKey});
        }

        // ---- Branch pages: `count` keys and `count + 1` children ------------
        while (level.size() > 1) {
            std::vector<Node> parents;
            for (std::size_t begin = 0; begin < level.size(); begin += branchFanout_) {
                const std::size_t end = std::min(level.size(), begin + branchFanout_);
                Bytes page;
                u16(page, 0);
                u16(page, static_cast<std::uint16_t>(end - begin - 1));
                u32(page, 0);
                u32(page, 0);
                for (std::size_t i = begin; i + 1 < end; ++i) {
                    u32(page, level[i].block);
                    u32(page, level[i].lastKey);
                }
                u32(page, level[end - 1].block);  // the trailing child
                u32(page, 0);
                pad(page);
                const std::uint32_t block = addBlock(std::move(page));
                layout.branchPages.push_back(block);
                parents.push_back(Node{block, level[end - 1].lastKey});
            }
            level = std::move(parents);
        }
        layout.rootPage = level.front().block;

        const auto treeHeader = [this](std::uint32_t child, std::uint32_t count) {
            Bytes tree;
            text(tree, "tree");
            u32(tree, 1);
            u32(tree, child);
            u32(tree, pageSize_ == 0 ? 4096u : static_cast<std::uint32_t>(pageSize_));
            u32(tree, count);
            u8(tree, 0);
            return tree;
        };
        const auto emptyLeaf = [this]() {
            Bytes page;
            u16(page, 1);
            u16(page, 0);
            u32(page, 0);
            u32(page, 0);
            pad(page);
            return page;
        };
        layout.treeBlock = addBlock(treeHeader(layout.rootPage, static_cast<std::uint32_t>(recordCount)));

        const std::uint32_t hlIndex = addBlock(treeHeader(addBlock(emptyLeaf()), 0));
        const std::uint32_t vTree = addBlock(treeHeader(addBlock(emptyLeaf()), 0));
        Bytes vIndexBytes;
        u32(vIndexBytes, 1);
        u32(vIndexBytes, vTree);
        u32(vIndexBytes, 0);
        u8(vIndexBytes, 0);
        const std::uint32_t vIndex = addBlock(std::move(vIndexBytes));
        const std::uint32_t size64 = addBlock(treeHeader(addBlock(emptyLeaf()), 0));

        // ---- File image ---------------------------------------------------
        Bytes image(512, 0);  // header + padding, as written by Apple's tools
        layout.blockAddress.assign(blocks.size(), 0);
        layout.blockLength.assign(blocks.size(), 0);
        for (std::size_t i = 1; i < blocks.size(); ++i) {
            layout.blockAddress[i] = image.size();
            layout.blockLength[i] = blocks[i].size();
            image.insert(image.end(), blocks[i].begin(), blocks[i].end());
        }

        Bytes vars;
        const std::vector<std::pair<std::string, std::uint32_t>> variables{
            {"BomInfo", bomInfoBlock}, {"Paths", layout.treeBlock}, {"HLIndex", hlIndex},
            {"VIndex", vIndex},        {"Size64", size64},
        };
        u32(vars, static_cast<std::uint32_t>(variables.size()));
        for (const auto& [name, block] : variables) {
            u32(vars, block);
            u8(vars, static_cast<std::uint8_t>(name.size()));
            text(vars, name);
        }
        layout.varsOffset = image.size();
        layout.varsLength = vars.size();
        image.insert(image.end(), vars.begin(), vars.end());

        Bytes index;
        u32(index, static_cast<std::uint32_t>(blocks.size()));
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            u32(index, static_cast<std::uint32_t>(layout.blockAddress[i]));
            u32(index, static_cast<std::uint32_t>(layout.blockLength[i]));
        }
        u32(index, 0);  // free list: no entries ...
        for (int i = 0; i < 4; ++i) u32(index, 0);  // ... plus the two spare slots real files carry
        layout.indexOffset = image.size();
        layout.indexLength = index.size();
        image.insert(image.end(), index.begin(), index.end());

        Bytes header;
        text(header, "BOMStore");
        u32(header, 1);
        u32(header, static_cast<std::uint32_t>(blocks.size() - 1));
        u32(header, static_cast<std::uint32_t>(layout.indexOffset));
        u32(header, static_cast<std::uint32_t>(layout.indexLength));
        u32(header, static_cast<std::uint32_t>(layout.varsOffset));
        u32(header, static_cast<std::uint32_t>(layout.varsLength));
        std::copy(header.begin(), header.end(), image.begin());

        if (layoutOut != nullptr) *layoutOut = std::move(layout);
        return image;
    }

    // ---- Patching helpers for corruption tests -----------------------------
    static void patchU32(std::vector<std::uint8_t>& image, std::size_t offset, std::uint32_t value) {
        image.at(offset) = static_cast<std::uint8_t>(value >> 24);
        image.at(offset + 1) = static_cast<std::uint8_t>(value >> 16);
        image.at(offset + 2) = static_cast<std::uint8_t>(value >> 8);
        image.at(offset + 3) = static_cast<std::uint8_t>(value);
    }
    static void patchU16(std::vector<std::uint8_t>& image, std::size_t offset, std::uint16_t value) {
        image.at(offset) = static_cast<std::uint8_t>(value >> 8);
        image.at(offset + 1) = static_cast<std::uint8_t>(value);
    }

private:
    using Bytes = std::vector<std::uint8_t>;

    struct Entry {
        vortex::BomEntryType type = vortex::BomEntryType::Directory;
        std::uint32_t size = 0;
        std::uint16_t mode = 040755;
        std::string linkTarget;
    };

    BomBuilder& add(std::string path, vortex::BomEntryType type, std::uint32_t size, std::uint16_t mode,
                    std::string linkTarget) {
        entries_.emplace_back(std::move(path), Entry{type, size, mode, std::move(linkTarget)});
        return *this;
    }

    /// The requested entries plus the root and every implied parent directory.
    [[nodiscard]] std::map<std::string, Entry> complete() const {
        std::map<std::string, Entry> all;
        all.emplace(".", Entry{});
        for (const auto& [path, entry] : entries_) {
            for (std::size_t slash = path.find('/', 2); slash != std::string::npos;
                 slash = path.find('/', slash + 1)) {
                all.emplace(path.substr(0, slash), Entry{});
            }
            all[path] = entry;
        }
        return all;
    }

    static void u8(Bytes& out, std::uint8_t value) { out.push_back(value); }
    static void u16(Bytes& out, std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }
    static void u32(Bytes& out, std::uint32_t value) {
        out.push_back(static_cast<std::uint8_t>(value >> 24));
        out.push_back(static_cast<std::uint8_t>(value >> 16));
        out.push_back(static_cast<std::uint8_t>(value >> 8));
        out.push_back(static_cast<std::uint8_t>(value));
    }
    static void text(Bytes& out, std::string_view value) { out.insert(out.end(), value.begin(), value.end()); }
    void pad(Bytes& page) const {
        if (page.size() < pageSize_) page.resize(pageSize_, 0);
    }

    std::vector<std::pair<std::string, Entry>> entries_;
    std::size_t leafCapacity_ = 64;
    std::size_t branchFanout_ = 8;
    std::size_t pageSize_ = 4096;
    bool reversed_ = false;
};

}  // namespace vt
