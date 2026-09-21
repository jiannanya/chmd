#include "internal.hpp"

#include <algorithm>
#include <array>

namespace chmd {
namespace {

// Every renderer emits small non-negative integers far more often than any
// other token. Building them in a stack buffer avoids the temporary string
// (and its allocation) that std::to_string plus operator+ would create.
void append_uint(std::string& out, std::uint32_t value) {
    char buffer[10];
    auto* const end = buffer + sizeof(buffer);
    auto* pos = end;
    do {
        *--pos = static_cast<char>('0' + static_cast<char>(value % 10U));
        value /= 10U;
    } while (value != 0);
    out.append(pos, static_cast<std::size_t>(end - pos));
}

constexpr std::array<bool, 256> escape_table() {
    std::array<bool, 256> table{};
    table[static_cast<unsigned char>('&')] = true;
    table[static_cast<unsigned char>('<')] = true;
    table[static_cast<unsigned char>('>')] = true;
    table[static_cast<unsigned char>('"')] = true;
    return table;
}

void append_escaped(std::string& out, std::string_view text, bool attribute = false) {
    // A 256-entry lookup is a single load per byte; find_first_of() over a
    // four-character set costs up to four comparisons per byte.
    static constexpr auto specials = escape_table();
    for (std::size_t begin = 0; begin < text.size();) {
        auto i = begin;
        while (i < text.size() && !specials[static_cast<unsigned char>(text[i])]) ++i;
        out.append(text.substr(begin, i - begin));
        if (i == text.size()) break;
        switch (text[i]) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: break;
        }
        begin = i + 1;
    }
    (void)attribute;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0; i < text.size();) {
        const auto begin = i;
        while (i < text.size()) {
            const auto byte = static_cast<unsigned char>(text[i]);
            if (byte < 0x20U || byte >= 0x80U || byte == '"' || byte == '\\') break;
            ++i;
        }
        out.append(text.substr(begin, i - begin));
        if (i == text.size()) break;
        const auto c = static_cast<unsigned char>(text[i++]);
        if (c >= 0x80U) {
            const std::size_t width = c >= 0xC2U && c <= 0xDFU ? 2 :
                (c >= 0xE0U && c <= 0xEFU ? 3 : (c >= 0xF0U && c <= 0xF4U ? 4 : 1));
            const auto sequence = text.substr(i - 1, width);
            std::size_t bad = 0;
            if (sequence.size() == width && detail::valid_utf8(sequence, bad)) {
                out.append(sequence);
                i += width - 1;
            } else out += "\\ufffd";
            continue;
        }
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20U) {
                out += "\\u00";
                out.push_back(hex[c >> 4U]);
                out.push_back(hex[c & 0x0FU]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

void append_url(std::string& out, std::string_view url) {
    static constexpr char hex[] = "0123456789ABCDEF";
    static constexpr auto safe = [] {
        std::array<bool, 256> table{};
        constexpr std::string_view punctuation = "-._~:/?#@!$'()*+,;=%";
        for (unsigned c = 0; c < 128; ++c)
            table[c] = detail::ascii_alnum(static_cast<unsigned char>(c)) ||
                punctuation.find(static_cast<char>(c)) != std::string_view::npos;
        return table;
    }();
    for (std::size_t pos = 0; pos < url.size();) {
        const auto begin = pos;
        while (pos < url.size() && safe[static_cast<unsigned char>(url[pos])]) ++pos;
        out.append(url.substr(begin, pos - begin));
        if (pos == url.size()) break;
        const auto c = static_cast<unsigned char>(url[pos++]);
        if (c == '&') out += "&amp;";
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4U]);
            out.push_back(hex[c & 0x0FU]);
        }
    }
}

std::string_view alignment_name(TableAlignment alignment) noexcept {
    switch (alignment) {
    case TableAlignment::left: return "left";
    case TableAlignment::center: return "center";
    case TableAlignment::right: return "right";
    case TableAlignment::none: return "none";
    }
    return "none";
}

class HtmlRenderer {
public:
    HtmlRenderer(const Document& document, HtmlOptions options, std::string& out)
        : document_(document), options_(options), out_(out) {}

    void run() {
        out_.clear();
        out_.reserve(std::min(document_.source().size(), document_.size() * 64));
        detail::walk(document_, [&](NodeId id, std::size_t) { return enter(id); },
                     [&](NodeId id, std::size_t) { leave(id); });
    }

private:
    bool tight_paragraph(const Node& node) const {
        if (node.parent == npos) return false;
        const auto& parent = document_.node(node.parent);
        return parent.type == NodeType::item && parent.parent != npos && document_.node(parent.parent).tight;
    }
    bool header_cell(const Node& node) const {
        const auto row = document_.node(node.parent).parent;
        return row != npos && document_.node(row).type == NodeType::table_head;
    }
    void render_checkbox(const Node& item) {
        out_ += "<input";
        if (item.checked) out_ += " checked=\"\"";
        out_ += " disabled=\"\" type=\"checkbox\"> ";
    }
    void append_alt(NodeId id) {
        detail::walk(document_, [&](NodeId child, std::size_t) {
            const auto& value = document_.node(child);
            if (value.type == NodeType::text || value.type == NodeType::code) append_escaped(out_, value.literal, true);
            else if (value.type == NodeType::soft_break || value.type == NodeType::line_break) out_.push_back('\n');
            return true;
        }, [](NodeId, std::size_t) {}, id);
    }
    bool enter(NodeId id) {
        const auto& node = document_.node(id);
        if (node.parent != npos && document_.node(node.parent).type == NodeType::item) {
            const bool direct = node.type == NodeType::paragraph && tight_paragraph(node);
            if ((node.previous == npos && !direct) ||
                (node.previous != npos && !out_.empty() && out_.back() != '\n')) out_.push_back('\n');
        }
        switch (node.type) {
        case NodeType::block_quote: out_ += "<blockquote>\n"; break;
        case NodeType::list:
            if (node.list_kind == ListKind::ordered) {
                out_ += "<ol";
                if (node.number != 1) { out_ += " start=\""; append_uint(out_, node.number); out_ += "\""; }
                out_ += ">\n";
            } else out_ += "<ul>\n";
            break;
        case NodeType::item: out_ += "<li>"; break;
        case NodeType::thematic_break: out_ += options_.xhtml ? "<hr />\n" : "<hr>\n"; break;
        case NodeType::heading: out_ += "<h"; append_uint(out_, node.number); out_ += ">"; break;
        case NodeType::code_block:
            out_ += "<pre><code";
            if (!node.title.empty()) {
                out_ += " class=\"language-";
                append_escaped(out_, std::string_view(node.title).substr(0, node.title.find_first_of(" \t\n")), true);
                out_ += "\"";
            }
            out_ += ">";
            append_escaped(out_, node.literal);
            out_ += "</code></pre>\n";
            break;
        case NodeType::html_block:
        case NodeType::html_inline:
            if (options_.escape_raw_html) append_escaped(out_, node.literal);
            else out_ += node.literal;
            break;
        case NodeType::paragraph:
            if (!tight_paragraph(node)) out_ += "<p>";
            if (node.parent != npos) {
                const auto& parent = document_.node(node.parent);
                if (parent.type == NodeType::item && parent.task && parent.first_child == id) render_checkbox(parent);
            }
            break;
        case NodeType::table: out_ += "<table>\n"; break;
        case NodeType::table_head: out_ += "<thead>\n"; break;
        case NodeType::table_body:
            if (node.first_child != npos) out_ += "<tbody>\n";
            break;
        case NodeType::table_row: out_ += "<tr>\n"; break;
        case NodeType::table_cell:
            out_ += header_cell(node) ? "<th" : "<td";
            if (node.alignment != TableAlignment::none) {
                out_ += " align=\""; out_ += alignment_name(node.alignment); out_ += "\"";
            }
            out_ += ">";
            break;
        case NodeType::text: append_escaped(out_, node.literal); break;
        case NodeType::soft_break: out_ += options_.soft_break_as_space ? " " : "\n"; break;
        case NodeType::line_break: out_ += options_.xhtml ? "<br />\n" : "<br>\n"; break;
        case NodeType::code:
            out_ += "<code>"; append_escaped(out_, node.literal); out_ += "</code>"; break;
        case NodeType::emphasis: out_ += "<em>"; break;
        case NodeType::strong: out_ += "<strong>"; break;
        case NodeType::strikethrough: out_ += "<del>"; break;
        case NodeType::link:
        case NodeType::image:
            out_ += node.type == NodeType::link ? "<a href=\"" : "<img src=\"";
            append_url(out_, node.literal);
            out_ += "\"";
            if (node.type == NodeType::image) { out_ += " alt=\""; append_alt(id); out_ += "\""; }
            if (!node.title.empty()) { out_ += " title=\""; append_escaped(out_, node.title, true); out_ += "\""; }
            out_ += node.type == NodeType::image && options_.xhtml ? " />" : ">";
            return node.type == NodeType::link;
        default: break;
        }
        return true;
    }
    void leave(NodeId id) {
        const auto& node = document_.node(id);
        switch (node.type) {
        case NodeType::block_quote: out_ += "</blockquote>\n"; break;
        case NodeType::list: out_ += node.list_kind == ListKind::ordered ? "</ol>\n" : "</ul>\n"; break;
        case NodeType::item: out_ += "</li>\n"; break;
        case NodeType::heading: out_ += "</h"; append_uint(out_, node.number); out_ += ">\n"; break;
        case NodeType::paragraph: if (!tight_paragraph(node)) out_ += "</p>\n"; break;
        case NodeType::table: out_ += "</table>\n"; break;
        case NodeType::table_head: out_ += "</thead>\n"; break;
        case NodeType::table_body: if (node.first_child != npos) out_ += "</tbody>\n"; break;
        case NodeType::table_row: out_ += "</tr>\n"; break;
        case NodeType::table_cell: out_ += header_cell(node) ? "</th>\n" : "</td>\n"; break;
        case NodeType::emphasis: out_ += "</em>"; break;
        case NodeType::strong: out_ += "</strong>"; break;
        case NodeType::strikethrough: out_ += "</del>"; break;
        case NodeType::link: out_ += "</a>"; break;
        default: break;
        }
    }
    const Document& document_;
    HtmlOptions options_;
    std::string& out_;
};

void render_ast_enter(const Document& document, NodeId id, std::string& out, std::size_t depth, bool pretty) {
    const auto& node = document.node(id);
    const auto indent = [&](std::size_t extra = 0) {
        if (pretty) out.append(static_cast<std::size_t>(depth + extra) * 2, ' ');
    };
    if (pretty) out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += "{";
    if (pretty) out += "\n";
    indent(1); out += "\"type\":"; if (pretty) out += " "; append_json_string(out, node_type_name(node.type));
    out += ","; if (pretty) out += "\n";
    indent(1); out += "\"source\": [";
    append_uint(out, node.source.begin);
    out += ", ";
    append_uint(out, node.source.end);
    out += "]";
    if (!node.literal.empty()) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"literal\":"; if (pretty) out += " "; append_json_string(out, node.literal); }
    if (!node.title.empty()) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"title\":"; if (pretty) out += " "; append_json_string(out, node.title); }
    if (node.type == NodeType::heading) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"level\": "; append_uint(out, node.number); }
    if (node.type == NodeType::table) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"columns\": "; append_uint(out, node.number); }
    if (node.type == NodeType::table_cell) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"alignment\": "; append_json_string(out, alignment_name(node.alignment)); }
    if (node.type == NodeType::item && node.task) {
        out += ","; if (pretty) out += "\n"; indent(1); out += "\"task\": true,";
        if (pretty) out += "\n"; indent(1); out += "\"checked\": "; out += node.checked ? "true" : "false";
    }
    if (node.type == NodeType::list) {
        out += ","; if (pretty) out += "\n"; indent(1);
        out += node.list_kind == ListKind::ordered ? "\"list_kind\": \"ordered\"," : "\"list_kind\": \"bullet\",";
        if (pretty) out += "\n"; indent(1); out += "\"start\": "; append_uint(out, node.number); out += ",";
        if (pretty) out += "\n"; indent(1); out += node.tight ? "\"tight\": true" : "\"tight\": false";
    }
    if (node.first_child != npos) {
        out += ","; if (pretty) out += "\n"; indent(1); out += "\"children\": ["; if (pretty) out += "\n";
    }
}

void render_ast_leave(const Document& document, NodeId id, std::string& out, std::size_t depth, bool pretty) {
    const auto& node = document.node(id);
    if (node.first_child != npos) {
        if (pretty) out.append((depth + 1) * 2, ' ');
        out += "]";
    }
    if (pretty) { out += "\n"; out.append(depth * 2, ' '); }
    out += "}";
    if (node.next != npos) out += ",";
    if (pretty && node.parent != npos) out += "\n";
}

void append_event_attributes(std::string& out, const Node& node) {
    if (node.type == NodeType::heading) { out += " level="; append_uint(out, node.number); }
    if (node.type == NodeType::table) { out += " columns="; append_uint(out, node.number); }
    if (node.type == NodeType::table_cell) { out += " alignment="; out += alignment_name(node.alignment); }
    if (node.type == NodeType::item && node.task) out += node.checked ? " task=checked" : " task=unchecked";
    if (node.type == NodeType::list) {
        out += node.list_kind == ListKind::ordered ? " kind=ordered" : " kind=bullet";
        out += " start="; append_uint(out, node.number);
        out += node.tight ? " tight=true" : " tight=false";
    }
    if (node.type == NodeType::link || node.type == NodeType::image) {
        out += " destination="; append_json_string(out, node.literal);
        if (!node.title.empty()) { out += " title="; append_json_string(out, node.title); }
    }
    if (node.type == NodeType::code_block && !node.title.empty()) { out += " info="; append_json_string(out, node.title); }
}

} // namespace

void render_html_to(const Document& document, std::string& out, HtmlOptions options) {
    HtmlRenderer(document, options, out).run();
}
std::string render_html(const Document& document, HtmlOptions options) {
    std::string out;
    render_html_to(document, out, options);
    return out;
}
void render_ast_to(const Document& document, std::string& out, bool pretty) {
    out.clear();
    out.reserve(document.size() * 80);
    detail::walk(document, [&](NodeId id, std::size_t depth) {
        render_ast_enter(document, id, out, depth * 2, pretty);
        return true;
    }, [&](NodeId id, std::size_t depth) {
        render_ast_leave(document, id, out, depth * 2, pretty);
    });
    if (pretty) out.push_back('\n');
}
std::string render_ast(const Document& document, bool pretty) {
    std::string out;
    render_ast_to(document, out, pretty);
    return out;
}
void render_events_to(const Document& document, std::string& out) {
    out.clear();
    out.reserve(document.size() * 32);
    detail::walk(document, [&](NodeId id, std::size_t) {
        const auto& node = document.node(id);
        if (detail::is_textual(node.type)) {
            out += "text "; out += node_type_name(node.type); out += " ";
            append_json_string(out, node.literal); out += "\n";
            return false;
        }
        out += "enter "; out += node_type_name(node.type); append_event_attributes(out, node); out += "\n";
        return true;
    }, [&](NodeId id, std::size_t) {
        const auto& node = document.node(id);
        if (!detail::is_textual(node.type)) {
            out += "leave "; out += node_type_name(node.type); out += "\n";
        }
    });
}
std::string render_events(const Document& document) {
    std::string out;
    render_events_to(document, out);
    return out;
}
void walk_events(const Document& document, EventHandler& handler) {
    detail::walk(document, [&](NodeId id, std::size_t) {
        const auto& node = document.node(id);
        if (detail::is_textual(node.type)) { handler.text(node); return false; }
        handler.enter(node);
        return true;
    }, [&](NodeId id, std::size_t) {
        const auto& node = document.node(id);
        if (!detail::is_textual(node.type)) handler.leave(node);
    });
}
} // namespace chmd
