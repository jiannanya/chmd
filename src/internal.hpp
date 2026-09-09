#pragma once

#include <chmd/chmd.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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
};

using ReferenceMap = std::unordered_map<std::string, Reference>;

class Builder {
public:
    Builder(Document& document, const ParseOptions& options, ParseError& error)
        : nodes_(detail_access::nodes(document)), options_(options), error_(error) {}

    NodeId append(NodeId parent, NodeType type, SourceRange source = {});
    NodeId insert_after(NodeId sibling, NodeType type);
    void unlink(NodeId id);
    void set_children(NodeId parent, NodeId first, NodeId last);
    void move_range(NodeId first, NodeId last, NodeId new_parent);
    void remove_if_empty(NodeId id);
    void compact(NodeId begin, NodeId parent);
    bool check_nesting(std::size_t depth, std::size_t offset);

    Node& get(NodeId id) noexcept { return nodes_[id]; }
    const Node& get(NodeId id) const noexcept { return nodes_[id]; }
    std::vector<Node>& nodes() noexcept { return nodes_; }
    bool ok() const noexcept { return !error_; }

private:
    NodeId make(NodeType type, SourceRange source);

    std::vector<Node>& nodes_;
    const ParseOptions& options_;
    ParseError& error_;
    std::vector<NodeId> remap_;
};

bool valid_utf8(std::string_view input, std::size_t& bad_offset) noexcept;
void normalize_input(std::string_view input, std::string& output);
std::string normalize_reference(std::string_view label);
std::string unescape_entities(std::string_view input);
std::string clean_url(std::string_view input);
std::string clean_title(std::string_view input);
bool is_unicode_whitespace(std::uint32_t cp) noexcept;
bool is_unicode_punctuation(std::uint32_t cp) noexcept;
std::uint32_t decode_utf8_before(std::string_view text, std::size_t offset) noexcept;
std::uint32_t decode_utf8_at(std::string_view text, std::size_t offset, std::size_t* width = nullptr) noexcept;
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
