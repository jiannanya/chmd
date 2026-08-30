#include "internal.hpp"

#include <new>

namespace chmd {

Document::Document() {
    nodes_.reserve(64);
    nodes_.emplace_back();
    nodes_.back().type = NodeType::document;
}

const char* version() noexcept { return "1.0.0"; }

std::string_view node_type_name(NodeType type) noexcept {
    using enum NodeType;
    switch (type) {
    case document: return "document";
    case block_quote: return "block_quote";
    case list: return "list";
    case item: return "item";
    case thematic_break: return "thematic_break";
    case heading: return "heading";
    case code_block: return "code_block";
    case html_block: return "html_block";
    case paragraph: return "paragraph";
    case text: return "text";
    case soft_break: return "soft_break";
    case line_break: return "line_break";
    case code: return "code";
    case html_inline: return "html_inline";
    case emphasis: return "emphasis";
    case strong: return "strong";
    case link: return "link";
    case image: return "image";
    }
    return "unknown";
}

namespace detail {

NodeId Builder::make(NodeType type, SourceRange source) {
    if (options_.max_nodes != 0 && nodes_.size() >= options_.max_nodes) {
        error_ = {ErrorCode::node_limit, source.begin, "node limit exceeded"};
        return npos;
    }
    if (nodes_.size() >= static_cast<std::size_t>(npos)) {
        error_ = {ErrorCode::node_limit, source.begin, "node index range exhausted"};
        return npos;
    }
    nodes_.push_back(Node{});
    auto id = static_cast<NodeId>(nodes_.size() - 1);
    nodes_[id].type = type;
    nodes_[id].source = source;
    return id;
}

NodeId Builder::append(NodeId parent, NodeType type, SourceRange source) {
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

NodeId Builder::insert_after(NodeId sibling, NodeType type) {
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

void Builder::unlink(NodeId id) {
    auto& n = nodes_[id];
    if (n.parent == npos) return;
    auto& p = nodes_[n.parent];
    if (n.previous != npos) nodes_[n.previous].next = n.next;
    else p.first_child = n.next;
    if (n.next != npos) nodes_[n.next].previous = n.previous;
    else p.last_child = n.previous;
    n.parent = npos;
    n.previous = npos;
    n.next = npos;
}

void Builder::set_children(NodeId parent, NodeId first, NodeId last) {
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

void Builder::move_range(NodeId first, NodeId last, NodeId new_parent) {
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

void Builder::remove_if_empty(NodeId id) {
    if (nodes_[id].literal.empty() && nodes_[id].first_child == npos) unlink(id);
}

bool is_leaf(NodeType type) noexcept {
    using enum NodeType;
    return type == thematic_break || type == code_block || type == html_block ||
           type == text || type == soft_break || type == line_break || type == code ||
           type == html_inline;
}

bool is_textual(NodeType type) noexcept {
    using enum NodeType;
    return type == text || type == soft_break || type == line_break || type == code ||
           type == html_inline || type == code_block || type == html_block;
}

} // namespace detail
} // namespace chmd
