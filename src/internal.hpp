#pragma once

#include <chmd/chmd.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace chmd {

struct detail_access {
    static std::string& source(Document& d) noexcept { return d.source_; }
    static std::vector<Node>& nodes(Document& d) noexcept { return d.nodes_; }
};

namespace detail {

constexpr bool ascii_alpha(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool ascii_alnum(unsigned char c) noexcept {
    return ascii_alpha(c) || (c >= '0' && c <= '9');
}

struct Reference {
    std::string destination;
    std::string title;
    std::uint32_t source_offset = 0;
};

// Reference definitions are looked up once per bracketed span, inserted once
// per definition, and never erased. An open-addressing table over a single
// vector keeps every lookup within one allocation instead of chasing hash
// nodes, and removes the per-definition allocation a node map would make.
class ReferenceMap {
public:
    bool empty() const noexcept { return size_ == 0; }

    const Reference* find(std::string_view key) const noexcept {
        if (slots_.empty()) return nullptr;
        const auto hash = hash_key(key);
        const auto mask = slots_.size() - 1;
        for (auto index = hash & mask;; index = (index + 1) & mask) {
            const auto& slot = slots_[index];
            if (slot.hash == 0) return nullptr;
            if (slot.hash == hash && slot.key == key) return &slot.value;
        }
    }

    // Follows std::unordered_map::try_emplace: a duplicate key leaves the
    // arguments untouched, which callers rely on for precedence updates.
    std::pair<Reference*, bool> try_emplace(std::string&& key, Reference&& value) {
        if (slots_.empty() || (size_ + 1) * 4 > slots_.size() * 3) grow();
        const auto hash = hash_key(key);
        const auto mask = slots_.size() - 1;
        auto index = hash & mask;
        while (slots_[index].hash != 0) {
            if (slots_[index].hash == hash && slots_[index].key == key)
                return {&slots_[index].value, false};
            index = (index + 1) & mask;
        }
        slots_[index].hash = hash;
        slots_[index].key = std::move(key);
        slots_[index].value = std::move(value);
        ++size_;
        return {&slots_[index].value, true};
    }

private:
    struct Slot {
        std::uint64_t hash = 0; // zero marks an empty slot
        std::string key;
        Reference value;
    };

    static std::uint64_t hash_key(std::string_view key) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const char c : key) hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
        return hash == 0 ? 1 : hash;
    }

    void grow() {
        std::vector<Slot> grown(std::max<std::size_t>(16, slots_.size() * 2));
        const auto mask = grown.size() - 1;
        for (auto& slot : slots_) {
            if (slot.hash == 0) continue;
            auto index = slot.hash & mask;
            while (grown[index].hash != 0) index = (index + 1) & mask;
            grown[index].hash = slot.hash;
            grown[index].key = std::move(slot.key);
            grown[index].value = std::move(slot.value);
        }
        slots_.swap(grown);
    }

    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

class Builder {
public:
    Builder(Document& document, const ParseOptions& options, ParseError& error)
        : nodes_(detail_access::nodes(document)), options_(options), error_(error) {}

    inline NodeId append(NodeId parent, NodeType type, SourceRange source = {});
    inline NodeId insert_after(NodeId sibling, NodeType type);
    inline void unlink(NodeId id);
    inline void set_children(NodeId parent, NodeId first, NodeId last);
    inline void move_range(NodeId first, NodeId last, NodeId new_parent);
    inline void remove_if_empty(NodeId id);
    void compact(NodeId begin, NodeId parent);
    inline bool check_nesting(std::size_t depth, std::size_t offset);

    Node& get(NodeId id) noexcept { return nodes_[id]; }
    const Node& get(NodeId id) const noexcept { return nodes_[id]; }
    std::vector<Node>& nodes() noexcept { return nodes_; }
    bool ok() const noexcept { return !error_; }
    // Number of nodes detached so far. compact() only has work to do when
    // this changes, which lets callers skip it entirely for hole-free ranges.
    std::size_t unlinks() const noexcept { return unlinks_; }
    // Extrapolated final node count from the parser's progress so far. Used
    // only when the arena has to grow, so an inaccurate hint costs nothing,
    // but a good one avoids moving every node through a dozen reallocations.
    void set_reserve_hint(std::size_t hint) noexcept { reserve_hint_ = hint; }

private:
    NodeId make(NodeType type, SourceRange source);

    std::vector<Node>& nodes_;
    const ParseOptions& options_;
    ParseError& error_;
    std::vector<NodeId> remap_;
    std::size_t unlinks_ = 0;
    std::size_t reserve_hint_ = 0;
};

// Definitions kept out-of-class (but inline) so the declarations above stay
// readable; both parser.cpp and inline.cpp call append() once per AST node,
// so letting it inline into its callers removes a call/ABI boundary from the
// single hottest path in the library. make()'s rare capacity-growth branch
// and the larger compact() stay out-of-line in document.cpp so this header
// doesn't pull cold code into every translation unit that includes it.

inline NodeId Builder::append(NodeId parent, NodeType type, SourceRange source) {
    const auto id = make(type, source);
    if (id == npos) return id;
    auto& p = nodes_[parent];
    auto& n = nodes_[id];
    n.parent = parent;
    n.previous = p.last_child;
    if (p.last_child != npos) nodes_[p.last_child].next = id;
    else p.first_child = id;
    p.last_child = id;
    return id;
}

inline NodeId Builder::insert_after(NodeId sibling, NodeType type) {
    const auto id = make(type, nodes_[sibling].source);
    if (id == npos) return id;
    auto& s = nodes_[sibling];
    auto& n = nodes_[id];
    n.parent = s.parent;
    n.previous = sibling;
    n.next = s.next;
    if (s.next != npos) nodes_[s.next].previous = id;
    else nodes_[s.parent].last_child = id;
    s.next = id;
    return id;
}

inline void Builder::unlink(NodeId id) {
    auto& n = nodes_[id];
    if (n.parent == npos) return;
    ++unlinks_;
    auto& p = nodes_[n.parent];
    if (n.previous != npos) nodes_[n.previous].next = n.next;
    else p.first_child = n.next;
    if (n.next != npos) nodes_[n.next].previous = n.previous;
    else p.last_child = n.previous;
    n.parent = npos;
    n.previous = npos;
    n.next = npos;
}

inline void Builder::set_children(NodeId parent, NodeId first, NodeId last) {
    auto& p = nodes_[parent];
    p.first_child = first;
    p.last_child = last;
    if (first == npos) return;
    nodes_[first].previous = npos;
    nodes_[last].next = npos;
    for (auto id = first; id != npos; id = nodes_[id].next) {
        nodes_[id].parent = parent;
        if (id == last) break;
    }
}

inline void Builder::move_range(NodeId first, NodeId last, NodeId new_parent) {
    const auto old_parent = nodes_[first].parent;
    const auto before = nodes_[first].previous;
    const auto after = nodes_[last].next;
    if (before != npos) nodes_[before].next = after;
    else nodes_[old_parent].first_child = after;
    if (after != npos) nodes_[after].previous = before;
    else nodes_[old_parent].last_child = before;
    nodes_[first].previous = npos;
    nodes_[last].next = npos;
    set_children(new_parent, first, last);
}

inline void Builder::remove_if_empty(NodeId id) {
    if (nodes_[id].literal.empty() && nodes_[id].first_child == npos) unlink(id);
}

inline bool Builder::check_nesting(std::size_t depth, std::size_t offset) {
    if (ok() && options_.max_nesting != 0 && depth >= options_.max_nesting)
        error_ = {ErrorCode::nesting_limit, offset, "nesting limit exceeded"};
    return ok();
}

bool valid_utf8(std::string_view input, std::size_t& bad_offset) noexcept;
void normalize_input(std::string_view input, std::string& output);
void normalize_reference_into(std::string_view label, std::string& out);
std::string normalize_reference(std::string_view label);
struct LinkLabel {
    std::string_view text;
    std::size_t end = 0;
};
LinkLabel scan_link_label(std::string_view input, std::size_t begin);
// Append exactly one entity at the start of input; return bytes consumed.
// The buffer form writes up to eight bytes without touching a std::string.
std::size_t append_entity(std::string_view input, std::string& output);
std::size_t append_entity(std::string_view input, char* out, std::size_t& written);
std::string unescape_markdown(std::string_view input);

constexpr bool ascii_punctuation(std::uint32_t cp) noexcept {
    return (cp >= 0x21U && cp <= 0x2FU) || (cp >= 0x3AU && cp <= 0x40U) ||
           (cp >= 0x5BU && cp <= 0x60U) || (cp >= 0x7BU && cp <= 0x7EU);
}

std::uint32_t in_unicode_whitespace_ranges(std::uint32_t cp) noexcept;
std::uint32_t in_unicode_punctuation_ranges(std::uint32_t cp) noexcept;

// The delimiter scanner classifies the byte on each side of every run. Almost
// all of those bytes are ASCII, so the table-free cases stay inline and only
// genuinely non-ASCII input reaches the range search.
inline bool is_unicode_whitespace(std::uint32_t cp) noexcept {
    if (cp < 0x80U) return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r';
    return in_unicode_whitespace_ranges(cp) != 0;
}

inline bool is_unicode_punctuation(std::uint32_t cp) noexcept {
    if (cp < 0x80U) return ascii_punctuation(cp);
    return in_unicode_punctuation_ranges(cp) != 0;
}
// The UTF-8 decoders sit in the inline scanner's innermost loops. Keeping the
// bodies here lets every translation unit inline them instead of paying a call
// per delimiter run or link label.
inline std::uint32_t decode_utf8_at(std::string_view text, std::size_t offset,
                                    std::size_t* width = nullptr) noexcept {
    if (offset >= text.size()) { if (width) *width = 0; return 0; }
    const auto c = static_cast<unsigned char>(text[offset]);
    std::size_t count = 1;
    std::uint32_t value = c;
    if ((c & 0xE0U) == 0xC0U) { count = 2; value = c & 0x1FU; }
    else if ((c & 0xF0U) == 0xE0U) { count = 3; value = c & 0x0FU; }
    else if ((c & 0xF8U) == 0xF0U) { count = 4; value = c & 0x07U; }
    if (offset + count > text.size()) { if (width) *width = 1; return c; }
    for (std::size_t i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0U) != 0x80U) { if (width) *width = 1; return c; }
        value = (value << 6U) | (next & 0x3FU);
    }
    if (width) *width = count;
    return value;
}

inline std::uint32_t decode_utf8_before(std::string_view text, std::size_t offset) noexcept {
    if (offset == 0 || offset > text.size()) return 0;
    auto begin = offset - 1;
    std::size_t continuation = 0;
    while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U && continuation < 3) {
        --begin;
        ++continuation;
    }
    return decode_utf8_at(text, begin);
}

void append_utf8(std::string& out, std::uint32_t cp);

void parse_inlines(Builder& builder, const ReferenceMap& references,
                   const ParseOptions& options);

bool is_leaf(NodeType type) noexcept;
bool is_textual(NodeType type) noexcept;

// Walk the indexed tree without allocating a stack or consuming call-stack
// space. Returning false from enter skips a node's children.
template <class Enter, class Leave>
void walk(const Document& document, Enter&& enter, Leave&& leave, NodeId root = 0) {
    if (document.size() == 0) return;
    auto id = root;
    std::size_t depth = 0;
    for (;;) {
        const auto& node = document.node(id);
        if (enter(id, depth) && node.first_child != npos) {
            id = node.first_child;
            ++depth;
            continue;
        }
        for (;;) {
            leave(id, depth);
            const auto& finished = document.node(id);
            if (id == root) return;
            if (finished.next != npos) { id = finished.next; break; }
            id = finished.parent;
            --depth;
        }
    }
}

} // namespace detail
} // namespace chmd
