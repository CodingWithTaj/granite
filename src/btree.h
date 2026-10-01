// A B+ tree mapping 64-bit keys to byte strings, stored in fixed-size pages.
//
// Leaf pages hold the rows, sorted by key, and link to the next leaf so a
// table can be scanned in order. Internal pages hold separator keys and
// child page numbers. The root page never moves: when it splits, its
// contents move to a new page and the root becomes the parent.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "storage.h"

namespace granite {

using Bytes = std::vector<uint8_t>;

/// Largest value a single row may hold, so any leaf can always fit at least three rows.
constexpr size_t MAX_VALUE = 1000;

class BTree {
public:
    BTree(Pager& pager, uint32_t root) : p_(pager), root_(root) {}
    /// Write an empty leaf to `pgno`, making it the root of a new tree.
    static void create(Pager& pager, uint32_t pgno);

    bool find(int64_t key, Bytes& out);
    /// Insert, or replace if the key exists.
    void upsert(int64_t key, const Bytes& value);
    bool remove(int64_t key);
    /// Every page belonging to this tree (for dropping a table).
    std::vector<uint32_t> pages();

    /// An in-order position in the tree.
    class Cursor {
    public:
        bool valid() const { return valid_; }
        int64_t key() const { return keys_[i_]; }
        /// Leaf pages this cursor has read.
        int pagesLoaded() const { return loads_; }
        const Bytes& value() const { return vals_[i_]; }
        void next();
    private:
        friend class BTree;
        bool load(uint32_t pgno);
        void settle();  // skip past the end of a leaf to the next non-empty one
        Pager* p_ = nullptr;
        std::vector<int64_t> keys_;
        std::vector<Bytes> vals_;
        uint32_t nextLeaf_ = 0;
        size_t i_ = 0;
        int loads_ = 0;
        bool valid_ = false;
    };
    Cursor first();
    /// The first entry with key >= `key`.
    Cursor seek(int64_t key);
    int pagesRead() const { return pagesRead_; }

    /// Structure as JSON, for the visualizer.
    std::string json(int maxNodes = 160);
    /// Check every structural invariant. Returns an empty string if the tree is sound.
    std::string check();
    int depth();

private:
    struct Split { int64_t key; uint32_t right; };
    std::optional<Split> insert(uint32_t pgno, int64_t key, const Bytes& value);
    bool read(uint32_t pgno, Page& page);
    std::string checkNode(uint32_t pgno, int depth, int& leafDepth, std::optional<int64_t> lo,
                          std::optional<int64_t> hi, std::vector<uint32_t>& leaves, int& count);
    void jsonNode(uint32_t pgno, std::string& out, int& budget);
    Pager& p_;
    uint32_t root_;
    int pagesRead_ = 0;
};

}  // namespace granite
