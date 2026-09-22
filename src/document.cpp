#include "internal.hpp"

#include <algorithm>
#include <new>

namespace chmd {

Document::Document() {
    nodes_.emplace_back();
    nodes_.back().type = NodeType::document;
}

void Document::shrink_to_fit() {
    source_.shrink_to_fit();
    for (auto& node : nodes_) {
        node.literal.shrink_to_fit();
        node.title.shrink_to_fit();
    }
    nodes_.shrink_to_fit();
}

const char* version() noexcept { return "1.2.0"; }

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
    case table: return "table";
    case table_head: return "table_head";
    case table_body: return "table_body";
    case table_row: return "table_row";
    case table_cell: return "table_cell";
    case text: return "text";
    case soft_break: return "soft_break";
    case line_break: return "line_break";
    case code: return "code";
    case html_inline: return "html_inline";
    case emphasis: return "emphasis";
    case strong: return "strong";
    case strikethrough: return "strikethrough";
    case link: return "link";
    case image: return "image";
    }
    return "unknown";
}

namespace detail {

NodeId Builder::make(NodeType type, SourceRange source) {
    if (!ok()) return npos;
    if (options_.max_nodes != 0 && nodes_.size() >= options_.max_nodes) {
        error_ = {ErrorCode::node_limit, source.begin, "node limit exceeded"};
        return npos;
    }
    if (nodes_.size() >= static_cast<std::size_t>(npos)) {
        error_ = {ErrorCode::node_limit, source.begin, "node index range exhausted"};
        return npos;
    }
    if (nodes_.size() == nodes_.capacity()) {
        auto capacity = std::max<std::size_t>(8, nodes_.capacity() +
            std::min(nodes_.capacity(), static_cast<std::size_t>(npos) - nodes_.capacity()));
        if (reserve_hint_ > capacity) capacity = reserve_hint_;
        if (options_.max_nodes != 0) capacity = std::min(capacity, options_.max_nodes);
        if (capacity < nodes_.size() + 1) capacity = nodes_.size() + 1;
        nodes_.reserve(capacity);
    }
    // emplace_back() constructs the node directly in the arena; push_back()
    // would build a temporary Node (with two strings) and then move it.
    nodes_.emplace_back();
    auto& node = nodes_.back();
    const auto id = static_cast<NodeId>(nodes_.size() - 1);
    node.type = type;
    node.source = source;
    return id;
}

void Builder::compact(NodeId begin, NodeId parent) {
    auto first_hole = begin;
    while (first_hole < nodes_.size() && nodes_[first_hole].parent != npos) ++first_hole;
    if (first_hole == nodes_.size()) return;
    remap_.resize(nodes_.size() - begin);
    auto next = begin;
    for (auto id = begin; id < nodes_.size(); ++id)
        remap_[id - begin] = nodes_[id].parent == npos ? npos : next++;
    const auto map = [&](NodeId id) {
        return id == npos || id < begin ? id : remap_[id - begin];
    };
    nodes_[parent].first_child = map(nodes_[parent].first_child);
    nodes_[parent].last_child = map(nodes_[parent].last_child);
    for (auto id = begin; id < nodes_.size(); ++id) {
        const auto destination = remap_[id - begin];
        if (destination == npos) continue;
        auto& node = nodes_[id];
        node.parent = map(node.parent);
        node.first_child = map(node.first_child);
        node.last_child = map(node.last_child);
        node.previous = map(node.previous);
        node.next = map(node.next);
        if (destination != id) nodes_[destination] = std::move(node);
    }
    nodes_.resize(next);
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
