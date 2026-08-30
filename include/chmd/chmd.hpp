#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace chmd {

using NodeId = std::uint32_t;
inline constexpr NodeId npos = static_cast<NodeId>(-1);

enum class NodeType : std::uint8_t {
    document,
    block_quote,
    list,
    item,
    thematic_break,
    heading,
    code_block,
    html_block,
    paragraph,
    text,
    soft_break,
    line_break,
    code,
    html_inline,
    emphasis,
    strong,
    link,
    image
};

enum class ListKind : std::uint8_t { bullet, ordered };

struct SourceRange {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
};

// Nodes live in a flat arena. Relationships are indices rather than pointers,
// so a Document can move without invalidating the tree and each node stays small.
struct Node {
    NodeType type = NodeType::text;
    NodeId parent = npos;
    NodeId first_child = npos;
    NodeId last_child = npos;
    NodeId previous = npos;
    NodeId next = npos;
    SourceRange source{};

    // Meaning depends on type: text/code/html content, fenced-code info string,
    // or a link/image destination. title is used by links and images.
    std::string literal;
    std::string title;

    std::uint32_t number = 0;       // heading level or ordered-list start
    std::uint16_t marker_offset = 0;
    std::uint16_t padding = 0;
    char marker = 0;
    ListKind list_kind = ListKind::bullet;
    bool tight = true;
    bool fenced = false;
};

struct ParseOptions {
    // Zero means unlimited. Limits allow services to bound attacker-controlled
    // documents without changing ordinary CommonMark behavior.
    std::size_t max_input_bytes = 0;
    std::size_t max_nodes = 0;
    std::size_t max_nesting = 1000;
    bool validate_utf8 = false;
};

enum class ErrorCode : std::uint8_t {
    none,
    input_too_large,
    node_limit,
    nesting_limit,
    invalid_utf8,
    out_of_memory
};

struct ParseError {
    ErrorCode code = ErrorCode::none;
    std::size_t offset = 0;
    std::string message;
    explicit operator bool() const noexcept { return code != ErrorCode::none; }
};

class Document {
public:
    Document();

    [[nodiscard]] NodeId root() const noexcept { return 0; }
    [[nodiscard]] const Node& node(NodeId id) const noexcept { return nodes_[id]; }
    [[nodiscard]] std::span<const Node> nodes() const noexcept { return nodes_; }
    [[nodiscard]] std::string_view source() const noexcept { return source_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

private:
    std::string source_;
    std::vector<Node> nodes_;

    friend class Parser;
    friend struct detail_access;
};

struct ParseResult {
    Document document;
    ParseError error;
    [[nodiscard]] explicit operator bool() const noexcept { return !error; }
};

class EventHandler {
public:
    virtual ~EventHandler() = default;
    virtual void enter(const Node& node) = 0;
    virtual void leave(const Node& node) = 0;
    virtual void text(const Node& node) = 0;
};

class Parser {
public:
    explicit Parser(ParseOptions options = {}) noexcept : options_(options) {}

    [[nodiscard]] ParseResult parse(std::string_view markdown) const;
    [[nodiscard]] ParseError parse_events(std::string_view markdown, EventHandler& handler) const;

private:
    ParseOptions options_;
};

struct HtmlOptions {
    bool escape_raw_html = false;
    bool soft_break_as_space = false;
    bool xhtml = true;
};

[[nodiscard]] std::string render_html(const Document& document, HtmlOptions options = {});
[[nodiscard]] std::string render_ast(const Document& document, bool pretty = true);
[[nodiscard]] std::string render_events(const Document& document);
void walk_events(const Document& document, EventHandler& handler);
[[nodiscard]] std::string_view node_type_name(NodeType type) noexcept;
[[nodiscard]] const char* version() noexcept;

} // namespace chmd

