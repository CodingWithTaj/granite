#include "btree.h"

#include <algorithm>
#include <cstring>

namespace granite {

// Page layouts (little-endian):
//   leaf:     [type=1][count:2][next leaf:4][pad:1] then cells: [key:8][length:2][value]
//   internal: [type=2][count:2][rightmost unused:4][pad:1] then [child0:4] ([key:8][child:4])*
// Pages are decoded into vectors, changed, and re-encoded. That keeps the
// code simple; a production engine would edit slotted pages in place.

namespace {
constexpr uint8_t LEAF = 1, INTERNAL = 2;
constexpr size_t HDR = 8;

struct Leaf {
    std::vector<int64_t> keys;
    std::vector<Bytes> vals;
    uint32_t next = 0;
    size_t bytes() const {
        size_t n = HDR;
        for (auto& v : vals) n += 10 + v.size();
        return n;
    }
};
struct Internal {
    std::vector<int64_t> keys;   // keys[i] separates kids[i] (< key) from kids[i+1] (>= key)
    std::vector<uint32_t> kids;  // always keys.size() + 1 children
    size_t bytes() const { return HDR + 4 + keys.size() * 12; }
};

void w16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void w32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
void w64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = uint8_t(v >> (8 * i)); }
uint16_t r16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t r32(const uint8_t* p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= uint32_t(p[i]) << (8 * i); return v; }
int64_t r64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v |= uint64_t(p[i]) << (8 * i); return int64_t(v); }

bool decodeLeaf(const Page& pg, Leaf& out) {
    if (pg[0] != LEAF) return false;
    size_t n = r16(&pg[1]), off = HDR;
    out.next = r32(&pg[3]);
    out.keys.clear();
    out.vals.clear();
    for (size_t i = 0; i < n; i++) {
        if (off + 10 > PAGE_BYTES) return false;
        int64_t k = r64(&pg[off]);
        size_t len = r16(&pg[off + 8]);
        off += 10;
        if (off + len > PAGE_BYTES) return false;
        out.keys.push_back(k);
        out.vals.emplace_back(pg.begin() + off, pg.begin() + off + len);
        off += len;
    }
    return true;
}

void encodeLeaf(const Leaf& l, Page& pg) {
    pg.fill(0);
    pg[0] = LEAF;
    w16(&pg[1], uint16_t(l.keys.size()));
    w32(&pg[3], l.next);
    size_t off = HDR;
    for (size_t i = 0; i < l.keys.size(); i++) {
        w64(&pg[off], uint64_t(l.keys[i]));
        w16(&pg[off + 8], uint16_t(l.vals[i].size()));
        std::memcpy(&pg[off + 10], l.vals[i].data(), l.vals[i].size());
        off += 10 + l.vals[i].size();
    }
}

bool decodeInternal(const Page& pg, Internal& out) {
    if (pg[0] != INTERNAL) return false;
    size_t n = r16(&pg[1]);
    if (HDR + 4 + n * 12 > PAGE_BYTES) return false;
    out.keys.resize(n);
    out.kids.resize(n + 1);
    out.kids[0] = r32(&pg[HDR]);
    for (size_t i = 0; i < n; i++) {
        out.keys[i] = r64(&pg[HDR + 4 + i * 12]);
        out.kids[i + 1] = r32(&pg[HDR + 4 + i * 12 + 8]);
    }
    return true;
}

void encodeInternal(const Internal& in, Page& pg) {
    pg.fill(0);
    pg[0] = INTERNAL;
    w16(&pg[1], uint16_t(in.keys.size()));
    w32(&pg[HDR], in.kids[0]);
    for (size_t i = 0; i < in.keys.size(); i++) {
        w64(&pg[HDR + 4 + i * 12], uint64_t(in.keys[i]));
        w32(&pg[HDR + 4 + i * 12 + 8], in.kids[i + 1]);
    }
}

/// Which child of an internal node may contain `key`.
size_t childFor(const Internal& in, int64_t key) {
    return size_t(std::upper_bound(in.keys.begin(), in.keys.end(), key) - in.keys.begin());
}
}  // namespace

void BTree::create(Pager& pager, uint32_t pgno) {
    Page pg;
    encodeLeaf(Leaf{}, pg);
    pager.put(pgno, pg);
}

bool BTree::read(uint32_t pgno, Page& page) {
    pagesRead_++;
    return p_.get(pgno, page);
}

bool BTree::find(int64_t key, Bytes& out) {
    Page pg;
    uint32_t pgno = root_;
    for (int guard = 0; guard < 64; guard++) {
        if (!read(pgno, pg)) return false;
        if (pg[0] == INTERNAL) {
            Internal in;
            if (!decodeInternal(pg, in)) return false;
            pgno = in.kids[childFor(in, key)];
            continue;
        }
        Leaf l;
        if (!decodeLeaf(pg, l)) return false;
        auto it = std::lower_bound(l.keys.begin(), l.keys.end(), key);
        if (it == l.keys.end() || *it != key) return false;
        out = l.vals[size_t(it - l.keys.begin())];
        return true;
    }
    return false;
}

std::optional<BTree::Split> BTree::insert(uint32_t pgno, int64_t key, const Bytes& value) {
    Page pg;
    if (!read(pgno, pg)) return std::nullopt;
    if (pg[0] == LEAF) {
        Leaf l;
        decodeLeaf(pg, l);
        auto it = std::lower_bound(l.keys.begin(), l.keys.end(), key);
        size_t pos = size_t(it - l.keys.begin());
        if (it != l.keys.end() && *it == key) l.vals[pos] = value;
        else {
            l.keys.insert(l.keys.begin() + long(pos), key);
            l.vals.insert(l.vals.begin() + long(pos), value);
        }
        if (l.bytes() <= PAGE_BYTES) {
            encodeLeaf(l, pg);
            p_.put(pgno, pg);
            return std::nullopt;
        }
        // Split the leaf roughly in half by bytes; the right half moves to a new page.
        size_t half = l.bytes() / 2, acc = HDR, cut = 0;
        while (cut < l.keys.size() - 1 && acc + 10 + l.vals[cut].size() <= half) acc += 10 + l.vals[cut++].size();
        cut = std::clamp<size_t>(cut, 1, l.keys.size() - 1);
        Leaf right;
        right.keys.assign(l.keys.begin() + long(cut), l.keys.end());
        right.vals.assign(l.vals.begin() + long(cut), l.vals.end());
        l.keys.resize(cut);
        l.vals.resize(cut);
        uint32_t rightPg = p_.allocate();
        right.next = l.next;
        l.next = rightPg;
        encodeLeaf(l, pg);
        p_.put(pgno, pg);
        Page rp;
        encodeLeaf(right, rp);
        p_.put(rightPg, rp);
        return Split{right.keys[0], rightPg};
    }
    Internal in;
    if (!decodeInternal(pg, in)) return std::nullopt;
    size_t idx = childFor(in, key);
    auto split = insert(in.kids[idx], key, value);
    if (!split) return std::nullopt;
    in.keys.insert(in.keys.begin() + long(idx), split->key);
    in.kids.insert(in.kids.begin() + long(idx) + 1, split->right);
    if (in.bytes() <= PAGE_BYTES) {
        encodeInternal(in, pg);
        p_.put(pgno, pg);
        return std::nullopt;
    }
    // Split the internal node: the middle key moves up to the parent.
    size_t mid = in.keys.size() / 2;
    Internal right;
    int64_t up = in.keys[mid];
    right.keys.assign(in.keys.begin() + long(mid) + 1, in.keys.end());
    right.kids.assign(in.kids.begin() + long(mid) + 1, in.kids.end());
    in.keys.resize(mid);
    in.kids.resize(mid + 1);
    uint32_t rightPg = p_.allocate();
    encodeInternal(in, pg);
    p_.put(pgno, pg);
    Page rp;
    encodeInternal(right, rp);
    p_.put(rightPg, rp);
    return Split{up, rightPg};
}

void BTree::upsert(int64_t key, const Bytes& value) {
    auto split = insert(root_, key, value);
    if (!split) return;
    // The root split. Keep the root's page number fixed (the catalog points
    // at it): move its contents to a new page and make the root their parent.
    Page old;
    read(root_, old);
    uint32_t left = p_.allocate();
    p_.put(left, old);
    Internal root;
    root.keys = {split->key};
    root.kids = {left, split->right};
    Page pg;
    encodeInternal(root, pg);
    p_.put(root_, pg);
}

bool BTree::remove(int64_t key) {
    Page pg;
    uint32_t pgno = root_;
    for (int guard = 0; guard < 64; guard++) {
        if (!read(pgno, pg)) return false;
        if (pg[0] == INTERNAL) {
            Internal in;
            decodeInternal(pg, in);
            pgno = in.kids[childFor(in, key)];
            continue;
        }
        Leaf l;
        decodeLeaf(pg, l);
        auto it = std::lower_bound(l.keys.begin(), l.keys.end(), key);
        if (it == l.keys.end() || *it != key) return false;
        size_t pos = size_t(it - l.keys.begin());
        l.keys.erase(it);
        l.vals.erase(l.vals.begin() + long(pos));
        // Leaves are allowed to become underfull: like many real engines,
        // space is reclaimed lazily rather than rebalancing on every delete.
        encodeLeaf(l, pg);
        p_.put(pgno, pg);
        return true;
    }
    return false;
}

std::vector<uint32_t> BTree::pages() {
    std::vector<uint32_t> out, stack{root_};
    Page pg;
    while (!stack.empty()) {
        uint32_t pgno = stack.back();
        stack.pop_back();
        out.push_back(pgno);
        if (!read(pgno, pg) || pg[0] != INTERNAL) continue;
        Internal in;
        decodeInternal(pg, in);
        for (uint32_t k : in.kids) stack.push_back(k);
    }
    return out;
}

// ------------------------------------------------------------------ cursors

bool BTree::Cursor::load(uint32_t pgno) {
    Page pg;
    Leaf l;
    loads_++;
    if (!p_->get(pgno, pg) || !decodeLeaf(pg, l)) { valid_ = false; return false; }
    keys_ = std::move(l.keys);
    vals_ = std::move(l.vals);
    nextLeaf_ = l.next;
    i_ = 0;
    valid_ = true;
    return true;
}

void BTree::Cursor::settle() {
    for (int guard = 0; valid_ && i_ >= keys_.size(); guard++) {
        if (nextLeaf_ == 0 || guard > 1000000) { valid_ = false; return; }
        load(nextLeaf_);
    }
}

void BTree::Cursor::next() {
    if (!valid_) return;
    i_++;
    settle();
}

BTree::Cursor BTree::first() { return seek(INT64_MIN); }

BTree::Cursor BTree::seek(int64_t key) {
    Cursor c;
    c.p_ = &p_;
    Page pg;
    uint32_t pgno = root_;
    for (int guard = 0; guard < 64; guard++) {
        if (!read(pgno, pg)) return c;
        if (pg[0] != INTERNAL) break;
        Internal in;
        decodeInternal(pg, in);
        pgno = in.kids[childFor(in, key)];
    }
    if (!c.load(pgno)) return c;
    c.i_ = size_t(std::lower_bound(c.keys_.begin(), c.keys_.end(), key) - c.keys_.begin());
    c.settle();
    return c;
}

// ------------------------------------------------------------------ checking and drawing

std::string BTree::checkNode(uint32_t pgno, int depth, int& leafDepth, std::optional<int64_t> lo,
                             std::optional<int64_t> hi, std::vector<uint32_t>& leaves, int& count) {
    Page pg;
    if (++count > 100000) return "too many pages: is there a cycle?";
    if (!p_.get(pgno, pg)) return "page " + std::to_string(pgno) + " is past the end of the file";
    auto inRange = [&](int64_t k) { return (!lo || k >= *lo) && (!hi || k < *hi); };
    if (pg[0] == LEAF) {
        Leaf l;
        if (!decodeLeaf(pg, l)) return "leaf page " + std::to_string(pgno) + " is corrupt";
        if (leafDepth < 0) leafDepth = depth;
        if (depth != leafDepth) return "leaves at different depths (unbalanced tree)";
        for (size_t i = 0; i < l.keys.size(); i++) {
            if (i && l.keys[i] <= l.keys[i - 1]) return "keys out of order in leaf " + std::to_string(pgno);
            if (!inRange(l.keys[i])) return "key " + std::to_string(l.keys[i]) + " is in the wrong leaf";
        }
        leaves.push_back(pgno);
        return "";
    }
    Internal in;
    if (!decodeInternal(pg, in)) return "page " + std::to_string(pgno) + " is neither a leaf nor an internal page";
    for (size_t i = 0; i < in.keys.size(); i++) {
        if (i && in.keys[i] <= in.keys[i - 1]) return "separator keys out of order in page " + std::to_string(pgno);
        if (!inRange(in.keys[i])) return "separator out of range in page " + std::to_string(pgno);
    }
    for (size_t i = 0; i < in.kids.size(); i++) {
        auto clo = i == 0 ? lo : std::optional<int64_t>(in.keys[i - 1]);
        auto chi = i == in.keys.size() ? hi : std::optional<int64_t>(in.keys[i]);
        std::string err = checkNode(in.kids[i], depth + 1, leafDepth, clo, chi, leaves, count);
        if (!err.empty()) return err;
    }
    return "";
}

std::string BTree::check() {
    int leafDepth = -1, count = 0;
    std::vector<uint32_t> leaves;
    std::string err = checkNode(root_, 0, leafDepth, std::nullopt, std::nullopt, leaves, count);
    if (!err.empty()) return err;
    // the sibling links must visit the leaves in exactly tree order
    for (size_t i = 0; i < leaves.size(); i++) {
        Page pg;
        Leaf l;
        p_.get(leaves[i], pg);
        decodeLeaf(pg, l);
        uint32_t expect = i + 1 < leaves.size() ? leaves[i + 1] : 0;
        if (l.next != expect) return "broken sibling link from leaf " + std::to_string(leaves[i]);
    }
    return "";
}

int BTree::depth() {
    Page pg;
    int d = 1;
    uint32_t pgno = root_;
    while (p_.get(pgno, pg) && pg[0] == INTERNAL && d < 64) {
        Internal in;
        decodeInternal(pg, in);
        pgno = in.kids[0];
        d++;
    }
    return d;
}

void BTree::jsonNode(uint32_t pgno, std::string& out, int& budget) {
    Page pg;
    if (budget-- <= 0 || !p_.get(pgno, pg)) { out += "null"; return; }
    if (pg[0] == LEAF) {
        Leaf l;
        decodeLeaf(pg, l);
        out += "{\"page\":" + std::to_string(pgno) + ",\"leaf\":true,\"count\":" + std::to_string(l.keys.size()) +
               ",\"fill\":" + std::to_string(int(100.0 * double(l.bytes()) / PAGE_BYTES)) +
               (l.keys.empty() ? std::string("") : ",\"first\":" + std::to_string(l.keys.front()) + ",\"last\":" + std::to_string(l.keys.back())) + "}";
        return;
    }
    Internal in;
    decodeInternal(pg, in);
    out += "{\"page\":" + std::to_string(pgno) + ",\"leaf\":false,\"keys\":[";
    for (size_t i = 0; i < in.keys.size(); i++) out += (i ? "," : "") + std::to_string(in.keys[i]);
    out += "],\"fill\":" + std::to_string(int(100.0 * double(in.bytes()) / PAGE_BYTES)) + ",\"kids\":[";
    for (size_t i = 0; i < in.kids.size(); i++) {
        if (i) out += ",";
        jsonNode(in.kids[i], out, budget);
    }
    out += "]}";
}

std::string BTree::json(int maxNodes) {
    std::string out;
    int budget = maxNodes;
    jsonNode(root_, out, budget);
    return out;
}

}  // namespace granite
