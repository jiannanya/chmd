#include "internal.hpp"

#include <iomanip>
#include <sstream>

namespace chmd {
namespace {

void append_escaped(std::string& out, std::string_view text, bool attribute = false) {
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out.push_back(c); break;
        }
    }
    (void)attribute;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    static constexpr char hex[] = "0123456789abcdef";
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
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

std::string percent_encode_url(std::string_view url) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(url.size());
    for (const char raw : url) {
        const auto c = static_cast<unsigned char>(raw);
        const bool safe = std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~' ||
            c == ':' || c == '/' || c == '?' || c == '#' || c == '@' ||
            c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
            c == '+' || c == ',' || c == ';' || c == '=' || c == '%';
        if (safe && c < 0x80U) out.push_back(static_cast<char>(c));
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4U]);
            out.push_back(hex[c & 0x0FU]);
        }
    }
    return out;
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
    HtmlRenderer(const Document& document, HtmlOptions options)
        : document_(document), options_(options) {}

    std::string run() {
        const auto& root = document_.node(document_.root());
        for (auto child = root.first_child; child != npos; child = document_.node(child).next) render_block(child);
        return std::move(out_);
    }

private:
    void render_children_inline(const Node& node) {
        for (auto child = node.first_child; child != npos; child = document_.node(child).next) render_inline(child);
    }

    bool tight_paragraph(const Node& node) const {
        if (node.parent == npos) return false;
        const auto& parent = document_.node(node.parent);
        if (parent.type != NodeType::item || parent.parent == npos) return false;
        return document_.node(parent.parent).tight;
    }

    void render_checkbox(const Node& item) {
        out_ += "<input";
        if (item.checked) out_ += " checked=\"\"";
        out_ += " disabled=\"\" type=\"checkbox\"> ";
    }

    void render_table_row(NodeId id, bool header) {
        const auto& row = document_.node(id);
        out_ += "<tr>\n";
        for (auto cell_id = row.first_child; cell_id != npos; cell_id = document_.node(cell_id).next) {
            const auto& cell = document_.node(cell_id);
            out_ += header ? "<th" : "<td";
            if (cell.alignment != TableAlignment::none) {
                out_ += " align=\"";
                out_ += alignment_name(cell.alignment);
                out_ += "\"";
            }
            out_ += ">";
            render_children_inline(cell);
            out_ += header ? "</th>\n" : "</td>\n";
        }
        out_ += "</tr>\n";
    }

    void render_table(const Node& table) {
        out_ += "<table>\n";
        const auto head = table.first_child;
        if (head != npos) {
            out_ += "<thead>\n";
            for (auto row = document_.node(head).first_child; row != npos; row = document_.node(row).next)
                render_table_row(row, true);
            out_ += "</thead>\n";
            const auto body = document_.node(head).next;
            if (body != npos && document_.node(body).first_child != npos) {
                out_ += "<tbody>\n";
                for (auto row = document_.node(body).first_child; row != npos; row = document_.node(row).next)
                    render_table_row(row, false);
                out_ += "</tbody>\n";
            }
        }
        out_ += "</table>\n";
    }

    void render_item(const Node& item) {
        out_ += "<li>";
        auto child = item.first_child;
        if (child == npos) {
            out_ += "</li>\n";
            return;
        }
        bool first = true;
        while (child != npos) {
            const auto& block = document_.node(child);
            const bool direct = block.type == NodeType::paragraph && tight_paragraph(block);
            const bool needs_newline = (first && !direct) ||
                (!first && !out_.empty() && out_.back() != '\n');
            if (needs_newline) out_.push_back('\n');
            render_block(child);
            first = false;
            child = block.next;
        }
        out_ += "</li>\n";
    }

    void render_block(NodeId id) {
        const auto& node = document_.node(id);
        switch (node.type) {
        case NodeType::block_quote:
            out_ += "<blockquote>\n";
            for (auto child = node.first_child; child != npos; child = document_.node(child).next) render_block(child);
            out_ += "</blockquote>\n";
            break;
        case NodeType::list:
            if (node.list_kind == ListKind::ordered) {
                out_ += "<ol";
                if (node.number != 1) out_ += " start=\"" + std::to_string(node.number) + "\"";
                out_ += ">\n";
            } else {
                out_ += "<ul>\n";
            }
            for (auto child = node.first_child; child != npos; child = document_.node(child).next) render_item(document_.node(child));
            out_ += node.list_kind == ListKind::ordered ? "</ol>\n" : "</ul>\n";
            break;
        case NodeType::item:
            render_item(node);
            break;
        case NodeType::thematic_break:
            out_ += options_.xhtml ? "<hr />\n" : "<hr>\n";
            break;
        case NodeType::heading:
            out_ += "<h" + std::to_string(node.number) + ">";
            render_children_inline(node);
            out_ += "</h" + std::to_string(node.number) + ">\n";
            break;
        case NodeType::code_block:
            out_ += "<pre><code";
            if (!node.title.empty()) {
                const auto end = node.title.find_first_of(" \t\n");
                out_ += " class=\"language-";
                append_escaped(out_, node.title.substr(0, end), true);
                out_ += "\"";
            }
            out_ += ">";
            append_escaped(out_, node.literal);
            out_ += "</code></pre>\n";
            break;
        case NodeType::html_block:
            if (options_.escape_raw_html) append_escaped(out_, node.literal);
            else out_ += node.literal;
            break;
        case NodeType::paragraph:
            if (!tight_paragraph(node)) out_ += "<p>";
            if (node.parent != npos) {
                const auto& parent = document_.node(node.parent);
                if (parent.type == NodeType::item && parent.task && parent.first_child == id)
                    render_checkbox(parent);
            }
            render_children_inline(node);
            if (!tight_paragraph(node)) out_ += "</p>\n";
            break;
        case NodeType::table:
            render_table(node);
            break;
        case NodeType::document:
            for (auto child = node.first_child; child != npos; child = document_.node(child).next) render_block(child);
            break;
        default:
            render_inline(id);
            break;
        }
    }

    void append_alt(const Node& node) {
        for (auto child = node.first_child; child != npos; child = document_.node(child).next) {
            const auto& value = document_.node(child);
            if (value.type == NodeType::text || value.type == NodeType::code) append_escaped(out_, value.literal, true);
            else if (value.type == NodeType::soft_break || value.type == NodeType::line_break) out_.push_back('\n');
            else if (value.first_child != npos) append_alt(value);
        }
    }

    void render_inline(NodeId id) {
        const auto& node = document_.node(id);
        switch (node.type) {
        case NodeType::text: append_escaped(out_, node.literal); break;
        case NodeType::soft_break: out_ += options_.soft_break_as_space ? " " : "\n"; break;
        case NodeType::line_break: out_ += options_.xhtml ? "<br />\n" : "<br>\n"; break;
        case NodeType::code:
            out_ += "<code>";
            append_escaped(out_, node.literal);
            out_ += "</code>";
            break;
        case NodeType::html_inline:
            if (options_.escape_raw_html) append_escaped(out_, node.literal);
            else out_ += node.literal;
            break;
        case NodeType::emphasis:
            out_ += "<em>"; render_children_inline(node); out_ += "</em>"; break;
        case NodeType::strong:
            out_ += "<strong>"; render_children_inline(node); out_ += "</strong>"; break;
        case NodeType::strikethrough:
            out_ += "<del>"; render_children_inline(node); out_ += "</del>"; break;
        case NodeType::link:
            out_ += "<a href=\"";
            append_escaped(out_, percent_encode_url(node.literal), true);
            out_ += "\"";
            if (!node.title.empty()) {
                out_ += " title=\"";
                append_escaped(out_, node.title, true);
                out_ += "\"";
            }
            out_ += ">"; render_children_inline(node); out_ += "</a>";
            break;
        case NodeType::image:
            out_ += "<img src=\"";
            append_escaped(out_, percent_encode_url(node.literal), true);
            out_ += "\" alt=\"";
            append_alt(node);
            out_ += "\"";
            if (!node.title.empty()) {
                out_ += " title=\"";
                append_escaped(out_, node.title, true);
                out_ += "\"";
            }
            out_ += options_.xhtml ? " />" : ">";
            break;
        default: render_children_inline(node); break;
        }
    }

    const Document& document_;
    HtmlOptions options_;
    std::string out_;
};

void render_ast_node(const Document& document, NodeId id, std::string& out, int depth, bool pretty) {
    const auto& node = document.node(id);
    const auto indent = [&](int extra = 0) {
        if (pretty) out.append(static_cast<std::size_t>(depth + extra) * 2, ' ');
    };
    if (pretty) out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += "{";
    if (pretty) out += "\n";
    indent(1); out += "\"type\":"; if (pretty) out += " "; append_json_string(out, node_type_name(node.type));
    out += ","; if (pretty) out += "\n";
    indent(1); out += "\"source\": [" + std::to_string(node.source.begin) + ", " + std::to_string(node.source.end) + "]";
    if (!node.literal.empty()) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"literal\":"; if (pretty) out += " "; append_json_string(out, node.literal); }
    if (!node.title.empty()) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"title\":"; if (pretty) out += " "; append_json_string(out, node.title); }
    if (node.type == NodeType::heading) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"level\": " + std::to_string(node.number); }
    if (node.type == NodeType::table) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"columns\": " + std::to_string(node.number); }
    if (node.type == NodeType::table_cell) { out += ","; if (pretty) out += "\n"; indent(1); out += "\"alignment\": "; append_json_string(out, alignment_name(node.alignment)); }
    if (node.type == NodeType::item && node.task) {
        out += ","; if (pretty) out += "\n"; indent(1); out += "\"task\": true,";
        if (pretty) out += "\n"; indent(1); out += "\"checked\": "; out += node.checked ? "true" : "false";
    }
    if (node.type == NodeType::list) {
        out += ","; if (pretty) out += "\n"; indent(1);
        out += "\"list_kind\": \"" + std::string(node.list_kind == ListKind::ordered ? "ordered" : "bullet") + "\",";
        if (pretty) out += "\n"; indent(1); out += "\"start\": " + std::to_string(node.number) + ",";
        if (pretty) out += "\n"; indent(1); out += "\"tight\": " + std::string(node.tight ? "true" : "false");
    }
    if (node.first_child != npos) {
        out += ","; if (pretty) out += "\n"; indent(1); out += "\"children\": ["; if (pretty) out += "\n";
        for (auto child = node.first_child; child != npos; child = document.node(child).next) {
            render_ast_node(document, child, out, depth + 2, pretty);
            if (document.node(child).next != npos) out += ",";
            if (pretty) out += "\n";
        }
        indent(1); out += "]";
    }
    if (pretty) { out += "\n"; indent(); }
    out += "}";
}

void append_event_attributes(std::string& out, const Node& node) {
    if (node.type == NodeType::heading) out += " level=" + std::to_string(node.number);
    if (node.type == NodeType::table) out += " columns=" + std::to_string(node.number);
    if (node.type == NodeType::table_cell) { out += " alignment="; out += alignment_name(node.alignment); }
    if (node.type == NodeType::item && node.task) out += node.checked ? " task=checked" : " task=unchecked";
    if (node.type == NodeType::list) {
        out += node.list_kind == ListKind::ordered ? " kind=ordered" : " kind=bullet";
        out += " start=" + std::to_string(node.number);
        out += node.tight ? " tight=true" : " tight=false";
    }
    if (node.type == NodeType::link || node.type == NodeType::image) {
        out += " destination="; append_json_string(out, node.literal);
        if (!node.title.empty()) { out += " title="; append_json_string(out, node.title); }
    }
    if (node.type == NodeType::code_block && !node.title.empty()) { out += " info="; append_json_string(out, node.title); }
}

void render_event_node(const Document& document, NodeId id, std::string& out) {
    const auto& node = document.node(id);
    if (detail::is_textual(node.type)) {
        out += "text "; out += node_type_name(node.type); out += " "; append_json_string(out, node.literal); out += "\n";
        return;
    }
    out += "enter "; out += node_type_name(node.type); append_event_attributes(out, node); out += "\n";
    for (auto child = node.first_child; child != npos; child = document.node(child).next) render_event_node(document, child, out);
    out += "leave "; out += node_type_name(node.type); out += "\n";
}

void walk_node(const Document& document, NodeId id, EventHandler& handler) {
    const auto& node = document.node(id);
    if (detail::is_textual(node.type)) {
        handler.text(node);
        return;
    }
    handler.enter(node);
    for (auto child = node.first_child; child != npos; child = document.node(child).next) walk_node(document, child, handler);
    handler.leave(node);
}

} // namespace

std::string render_html(const Document& document, HtmlOptions options) {
    return HtmlRenderer(document, options).run();
}

std::string render_ast(const Document& document, bool pretty) {
    std::string out;
    out.reserve(document.size() * 80);
    render_ast_node(document, document.root(), out, 0, pretty);
    if (pretty) out.push_back('\n');
    return out;
}

std::string render_events(const Document& document) {
    std::string out;
    out.reserve(document.size() * 32);
    render_event_node(document, document.root(), out);
    return out;
}

void walk_events(const Document& document, EventHandler& handler) {
    walk_node(document, document.root(), handler);
}

} // namespace chmd
