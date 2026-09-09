#include "internal.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace chmd::detail {
namespace {

constexpr bool ascii_space(char c) noexcept { return c == ' ' || c == '\t'; }

bool escapable(char c) noexcept {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

bool ascii_control_or_space(char c) noexcept {
    const auto value = static_cast<unsigned char>(c);
    return value <= 0x20U || value == 0x7FU;
}

std::string unescape_markdown(std::string_view input) {
    if (input.find_first_of("\\&") == std::string_view::npos) return std::string(input);
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size() && escapable(input[i + 1])) ++i;
        out.push_back(input[i]);
    }
    return unescape_entities(out);
}

std::string normalized_label(std::string_view input) {
    return normalize_reference(input);
}

struct LinkTail {
    bool valid = false;
    std::size_t end = 0;
    std::string destination;
    std::string title;
};

std::size_t skip_link_space(std::string_view input, std::size_t pos, bool& newline) {
    while (pos < input.size() && ascii_space(input[pos])) ++pos;
    if (pos < input.size() && input[pos] == '\n' && !newline) {
        newline = true;
        ++pos;
        while (pos < input.size() && ascii_space(input[pos])) ++pos;
    }
    return pos;
}

LinkTail parse_inline_link(std::string_view input, std::size_t open_paren) {
    LinkTail result;
    auto pos = open_paren + 1;
    bool newline = false;
    pos = skip_link_space(input, pos, newline);
    std::string_view raw_destination;
    if (pos < input.size() && input[pos] == '<') {
        ++pos;
        const auto begin = pos;
        bool escaped = false;
        while (pos < input.size() && input[pos] != '\n') {
            const char c = input[pos];
            if (!escaped && c == '>') break;
            if (!escaped && c == '<') return result;
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            ++pos;
        }
        if (pos == input.size() || input[pos] != '>') return result;
        raw_destination = input.substr(begin, pos - begin);
        ++pos;
    } else {
        const auto begin = pos;
        int depth = 0;
        bool escaped = false;
        while (pos < input.size()) {
            const char c = input[pos];
            // Backslashes escape punctuation, never destination separators.
            if (ascii_space(c) || c == '\n') break;
            if (ascii_control_or_space(c)) return result;
            if (!escaped && c == '(') {
                if (++depth > 32) return result;
            } else if (!escaped && c == ')') {
                if (depth == 0) break;
                --depth;
            }
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            ++pos;
        }
        if (depth != 0) return result;
        raw_destination = input.substr(begin, pos - begin);
    }

    const auto after_destination = pos;
    bool title_space = false;
    while (pos < input.size() && ascii_space(input[pos])) { ++pos; title_space = true; }
    if (pos < input.size() && input[pos] == '\n' && !newline) {
        ++pos;
        newline = true;
        title_space = true;
        while (pos < input.size() && ascii_space(input[pos])) ++pos;
    }

    std::string_view raw_title;
    if (title_space && pos < input.size() && (input[pos] == '\'' || input[pos] == '"' || input[pos] == '(')) {
        const char opener = input[pos++];
        const char closer = opener == '(' ? ')' : opener;
        const auto begin = pos;
        bool escaped = false;
        bool title_newline = false;
        while (pos < input.size()) {
            const char c = input[pos];
            if (!escaped && c == closer) break;
            if (!escaped && opener == '(' && c == '(') return {};
            if (c == '\n') {
                if (title_newline && pos > 0 && input[pos - 1] == '\n') return {};
                title_newline = true;
            }
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            ++pos;
        }
        if (pos == input.size() || input[pos] != closer) return {};
        raw_title = input.substr(begin, pos - begin);
        ++pos;
        bool trailing_newline = false;
        pos = skip_link_space(input, pos, trailing_newline);
    } else {
        pos = after_destination;
        bool trailing_newline = false;
        pos = skip_link_space(input, pos, trailing_newline);
    }
    if (pos >= input.size() || input[pos] != ')') return {};
    result.valid = true;
    result.end = pos + 1;
    result.destination = unescape_markdown(raw_destination);
    result.title = unescape_markdown(raw_title);
    return result;
}

struct HtmlScanCache {
    std::size_t from = 0;
    std::size_t end = 0;
    bool initialized = false;
    std::size_t find(std::string_view input, std::string_view token, std::size_t pos) {
        if (!initialized || pos < from || end < pos) {
            from = pos;
            end = input.find(token, pos);
            initialized = true;
        }
        return end;
    }
};

std::size_t scan_html(std::string_view input, std::size_t begin, std::array<HtmlScanCache, 6>& cache) {
    if (begin >= input.size() || input[begin] != '<') return begin;
    if (input.substr(begin).starts_with("<!-->")) return begin + 5;
    if (input.substr(begin).starts_with("<!--->")) return begin + 6;
    if (input.substr(begin).starts_with("<!--")) {
        const auto end = cache[0].find(input, "-->", begin + 4);
        if (end != std::string_view::npos && input.substr(begin + 4, end - begin - 4).find("<!--") == std::string_view::npos)
            return end + 3;
        return begin;
    }
    if (input.substr(begin).starts_with("<?")) {
        const auto end = cache[1].find(input, "?>", begin + 2);
        return end == std::string_view::npos ? begin : end + 2;
    }
    if (input.substr(begin).starts_with("<![CDATA[")) {
        const auto end = cache[2].find(input, "]]>", begin + 9);
        return end == std::string_view::npos ? begin : end + 3;
    }
    if (begin + 2 < input.size() && input[begin + 1] == '!' &&
        ascii_alpha(static_cast<unsigned char>(input[begin + 2]))) {
        const auto end = cache[3].find(input, ">", begin + 3);
        return end == std::string_view::npos ? begin : end + 1;
    }

    const auto html_space = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    const auto name_start = [](char c) {
        return ascii_alpha(static_cast<unsigned char>(c)) || c == '_' || c == ':';
    };
    const auto name_char = [](char c) {
        return ascii_alnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || c == '-';
    };

    auto pos = begin + 1;
    const bool closing = pos < input.size() && input[pos] == '/';
    if (closing) ++pos;
    if (pos >= input.size() || !ascii_alpha(static_cast<unsigned char>(input[pos]))) return begin;
    while (pos < input.size() && (ascii_alnum(static_cast<unsigned char>(input[pos])) || input[pos] == '-')) ++pos;
    if (closing) {
        while (pos < input.size() && html_space(input[pos])) ++pos;
        return pos < input.size() && input[pos] == '>' ? pos + 1 : begin;
    }

    for (;;) {
        if (pos >= input.size()) return begin;
        if (input[pos] == '>') return pos + 1;
        if (input[pos] == '/' && pos + 1 < input.size() && input[pos + 1] == '>') return pos + 2;
        if (!html_space(input[pos])) return begin;
        while (pos < input.size() && html_space(input[pos])) ++pos;
        if (pos >= input.size()) return begin;
        if (input[pos] == '>' || (input[pos] == '/' && pos + 1 < input.size() && input[pos + 1] == '>')) continue;
        if (!name_start(input[pos])) return begin;
        while (pos < input.size() && name_char(input[pos])) ++pos;
        const auto after_name = pos;
        while (pos < input.size() && html_space(input[pos])) ++pos;
        if (pos >= input.size() || input[pos] != '=') {
            pos = after_name;
            continue;
        }
        ++pos;
        while (pos < input.size() && html_space(input[pos])) ++pos;
        if (pos >= input.size()) return begin;
        if (input[pos] == '\'' || input[pos] == '"') {
            const char quote = input[pos++];
            const auto close = cache[quote == '\'' ? 4 : 5].find(input, std::string_view(&quote, 1), pos);
            if (close == std::string_view::npos) return begin;
            pos = close + 1;
        } else {
            const auto value = pos;
            while (pos < input.size() && !html_space(input[pos]) && input[pos] != '"' &&
                   input[pos] != '\'' && input[pos] != '=' && input[pos] != '<' &&
                   input[pos] != '>' && input[pos] != '`') ++pos;
            if (pos == value) return begin;
        }
    }
}

struct AutoLink {
    bool valid = false;
    bool email = false;
    std::string_view label;
};

AutoLink parse_autolink(std::string_view input, std::size_t begin) {
    AutoLink result;
    auto close = begin + 1;
    while (close < input.size() && input[close] != '>') {
        if (input[close] == '<' || ascii_control_or_space(input[close])) return result;
        ++close;
    }
    if (close == input.size()) return result;
    const auto body = input.substr(begin + 1, close - begin - 1);
    if (body.empty()) return result;
    const auto colon = body.find(':');
    if (colon >= 2 && colon <= 32 && ascii_alpha(static_cast<unsigned char>(body[0]))) {
        bool scheme = true;
        for (std::size_t i = 1; i < colon; ++i) {
            const char c = body[i];
            if (!(ascii_alnum(static_cast<unsigned char>(c)) || c == '+' || c == '.' || c == '-')) scheme = false;
        }
        bool valid = scheme;
        for (std::size_t i = colon + 1; i < body.size(); ++i) {
            if (ascii_control_or_space(body[i]) || body[i] == '<' || body[i] == '>') valid = false;
        }
        if (valid) return {true, false, body};
    }

    const auto at = body.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= body.size() || body.find('@', at + 1) != std::string_view::npos)
        return result;
    const auto local = body.substr(0, at);
    for (const char c : local) {
        if (!(ascii_alnum(static_cast<unsigned char>(c)) || c == '.' || c == '!' || c == '#' ||
              c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '/' ||
              c == '=' || c == '?' || c == '^' || c == '_' || c == '`' || c == '{' || c == '|' ||
              c == '}' || c == '~' || c == '-')) return result;
    }
    const auto domain = body.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' || domain.front() == '-' || domain.back() == '-') return result;
    bool previous_dot = false;
    std::size_t label_begin = 0;
    for (std::size_t i = 0; i < domain.size(); ++i) {
        const char c = domain[i];
        if (c == '.') {
            if (previous_dot || domain[i - 1] == '-' || i - label_begin > 63) return result;
            label_begin = i + 1;
            previous_dot = true;
        } else {
            if (!(ascii_alnum(static_cast<unsigned char>(c)) || c == '-') ||
                (i == label_begin && c == '-')) return result;
            previous_dot = false;
        }
    }
    if (domain.size() - label_begin > 63) return result;
    return {true, true, body};
}

class InlineParser {
public:
    InlineParser(Builder& builder, const ReferenceMap& references, const ParseOptions& options)
        : builder_(builder), references_(references), options_(options) {}

    void run(NodeId parent, std::string input) {
        parent_ = parent;
        input_ = std::move(input);
        delimiters_.clear();
        first_delimiter_ = last_delimiter_ = -1;
        backticks_.clear();
        html_cache_ = {};
        next_angle_ = 0;
        last_bracket_ = -1;
        inactive_link_before_ = -1;
        needs_coalesce_ = false;
        emphasis_openers_ = {};
        strike_openers_ = {};
        links_possible_ = !references_.empty() || input_.find('(') != std::string::npos;
        const auto first_inline = static_cast<NodeId>(builder_.nodes().size());
        first_inline_ = first_inline;
        block_depth_ = 0;
        for (auto ancestor = parent_; builder_.get(ancestor).parent != npos; ancestor = builder_.get(ancestor).parent)
            ++block_depth_;
        track_depth_ = options_.max_nesting != 0 && options_.max_nesting <= block_depth_ + input_.size() + 1;
        heights_.clear();
        builder_.get(parent_).literal.clear();
        scan_backticks();
        for (pos_ = 0; pos_ < input_.size() && builder_.ok();) {
            const char c = input_[pos_];
            if (c == '\\') parse_backslash();
            else if (c == '`') parse_code_span();
            else if (c == '&') parse_entity();
            else if (c == '<') parse_less_than();
            else if (c == '\n') parse_newline();
            else if (c == '*' || c == '_') parse_emphasis_run();
            else if (c == '~' && options_.extensions.strikethrough) parse_strikethrough_run();
            else if (c == '[') parse_open_bracket(false);
            else if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '[') parse_open_bracket(true);
            else if (c == ']') parse_close_bracket();
            else parse_text_run();
        }
        process_emphasis(-1, static_cast<int>(delimiters_.size()));
        trim_final_whitespace();
        if (builder_.ok()) {
            if (needs_coalesce_ || std::any_of(delimiters_.begin(), delimiters_.end(), [&](const Delimiter& delimiter) {
                return builder_.get(delimiter.node).parent != npos;
            })) coalesce_text(first_inline);
            builder_.compact(first_inline, parent_);
        }
    }

private:
    struct Delimiter {
        char character = 0;
        NodeId node = npos;
        std::size_t source_pos = 0;
        std::uint32_t length = 0;
        bool can_open = false;
        bool can_close = false;
        bool image = false;
        bool removed = false;
        int previous_bracket = -1;
        int previous = -1;
        int next = -1;
    };

    void push_delimiter(Delimiter delimiter) {
        const auto index = static_cast<int>(delimiters_.size());
        delimiter.previous = last_delimiter_;
        if (last_delimiter_ >= 0) delimiters_[static_cast<std::size_t>(last_delimiter_)].next = index;
        else first_delimiter_ = index;
        delimiters_.push_back(delimiter);
        last_delimiter_ = index;
    }

    void remove_delimiter(int index) {
        auto& delimiter = delimiters_[static_cast<std::size_t>(index)];
        if (delimiter.removed) return;
        if (delimiter.previous >= 0) delimiters_[static_cast<std::size_t>(delimiter.previous)].next = delimiter.next;
        else first_delimiter_ = delimiter.next;
        if (delimiter.next >= 0) delimiters_[static_cast<std::size_t>(delimiter.next)].previous = delimiter.previous;
        else last_delimiter_ = delimiter.previous;
        delimiter.removed = true;
    }

    void coalesce_text(NodeId begin) {
        for (auto id = begin; id < builder_.nodes().size(); ++id) {
            auto& node = builder_.get(id);
            if (node.parent == npos || node.type != NodeType::text) continue;
            node.marker = 0;
            while (node.next != npos && builder_.get(node.next).type == NodeType::text) {
                const auto next = node.next;
                node.literal += builder_.get(next).literal;
                node.source.end = std::max(node.source.end, builder_.get(next).source.end);
                builder_.unlink(next);
            }
        }
    }

    bool check_span(NodeId wrapper, NodeId first = npos, NodeId last = npos) {
        if (!track_depth_) return builder_.ok();
        std::uint32_t height = 1;
        for (auto child = first; child != npos; child = builder_.get(child).next) {
            const auto index = child - first_inline_;
            const auto child_height = index < heights_.size() ? heights_[index] : 1;
            height = std::max(height, child_height + 1);
            if (child == last) break;
        }
        const auto index = wrapper - first_inline_;
        if (index >= heights_.size()) heights_.resize(static_cast<std::size_t>(index) + 1, 1);
        heights_[index] = height;
        return builder_.check_nesting(block_depth_ + height, builder_.get(wrapper).source.begin);
    }

    std::uint32_t source_offset(std::size_t local) const {
        const auto base = builder_.get(parent_).source.begin;
        return static_cast<std::uint32_t>(std::min<std::size_t>(
            static_cast<std::size_t>(base) + local, std::numeric_limits<std::uint32_t>::max()));
    }

    NodeId append_node(NodeType type, std::string_view literal, std::size_t begin, std::size_t end,
                       bool merge = true) {
        auto& parent = builder_.get(parent_);
        if (merge && type == NodeType::text && parent.last_child != npos) {
            auto& previous = builder_.get(parent.last_child);
            if (previous.type == NodeType::text && previous.marker == 0) {
                previous.literal.append(literal);
                previous.source.end = source_offset(end);
                return parent.last_child;
            }
        }
        const auto id = builder_.append(parent_, type, {source_offset(begin), source_offset(end)});
        if (id != npos) builder_.get(id).literal.assign(literal);
        return id;
    }

    void scan_backticks() {
        for (std::size_t i = 0; i < input_.size();) {
            if (input_[i] != '`') { ++i; continue; }
            const auto begin = i;
            while (i < input_.size() && input_[i] == '`') ++i;
            backticks_.emplace_back(i - begin, begin);
        }
        std::sort(backticks_.begin(), backticks_.end());
    }

    void parse_backslash() {
        if (pos_ + 1 < input_.size() && escapable(input_[pos_ + 1])) {
            append_node(NodeType::text, std::string_view(input_).substr(pos_ + 1, 1), pos_, pos_ + 2);
            pos_ += 2;
        } else if (pos_ + 1 < input_.size() && input_[pos_ + 1] == '\n') {
            append_node(NodeType::line_break, {}, pos_, pos_ + 2, false);
            pos_ += 2;
            while (pos_ < input_.size() && ascii_space(input_[pos_])) ++pos_;
        } else {
            append_node(NodeType::text, "\\", pos_, pos_ + 1);
            ++pos_;
        }
    }

    void parse_code_span() {
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] == '`') ++pos_;
        const auto run = pos_ - begin;
        const auto match = std::upper_bound(backticks_.begin(), backticks_.end(), std::pair(run, begin));
        if (match == backticks_.end() || match->first != run) {
            append_node(NodeType::text, std::string_view(input_).substr(begin, run), begin, pos_, false);
            needs_coalesce_ = true;
            return;
        }
        const auto close = match->second;
        std::string content(std::string_view(input_).substr(pos_, close - pos_));
        std::replace(content.begin(), content.end(), '\n', ' ');
        const bool all_space = std::all_of(content.begin(), content.end(), [](char c) { return c == ' '; });
        if (!all_space && content.size() >= 2 && content.front() == ' ' && content.back() == ' ') {
            content.erase(content.begin());
            content.pop_back();
        }
        const auto id = append_node(NodeType::code, {}, begin, close + run, false);
        if (id != npos) builder_.get(id).literal = std::move(content);
        pos_ = close + run;
    }

    void parse_entity() {
        const auto relative = std::string_view(input_).substr(pos_ + 1, 33).find(';');
        if (relative != std::string_view::npos) {
            const auto semicolon = pos_ + 1 + relative;
            const auto raw = std::string_view(input_).substr(pos_, semicolon - pos_ + 1);
            auto decoded = unescape_entities(raw);
            if (decoded != raw) {
                append_node(NodeType::text, decoded, pos_, semicolon + 1);
                pos_ = semicolon + 1;
                return;
            }
        }
        append_node(NodeType::text, "&", pos_, pos_ + 1);
        ++pos_;
    }

    void parse_less_than() {
        if (next_angle_ < pos_) next_angle_ = input_.find('>', pos_ + 1);
        if (next_angle_ == std::string::npos) {
            // No later '>' means these adjacent markers cannot open markup.
            // Append the run once instead of re-entering the parser per byte.
            const auto begin = pos_;
            do { ++pos_; } while (pos_ < input_.size() && input_[pos_] == '<');
            append_node(NodeType::text, std::string_view(input_).substr(begin, pos_ - begin), begin, pos_);
            return;
        }
        const auto link = parse_autolink(input_, pos_);
        if (link.valid) {
            const auto end = pos_ + link.label.size() + 2;
            const auto id = builder_.append(parent_, NodeType::link, {source_offset(pos_), source_offset(end)});
            if (id != npos) {
                auto& node = builder_.get(id);
                node.literal = link.email ? "mailto:" + std::string(link.label) : std::string(link.label);
                const auto text = builder_.append(id, NodeType::text, {source_offset(pos_ + 1), source_offset(end - 1)});
                if (text != npos) builder_.get(text).literal.assign(link.label);
                if (!check_span(id, text, text)) return;
            }
            pos_ = end;
            return;
        }
        const auto end = scan_html(input_, pos_, html_cache_);
        if (end != pos_) {
            append_node(NodeType::html_inline, std::string_view(input_).substr(pos_, end - pos_), pos_, end, false);
            pos_ = end;
            return;
        }
        append_node(NodeType::text, "<", pos_, pos_ + 1);
        ++pos_;
    }

    void trim_trailing_spaces(std::size_t& count) {
        count = 0;
        auto id = builder_.get(parent_).last_child;
        if (id == npos || builder_.get(id).type != NodeType::text) return;
        auto& text = builder_.get(id).literal;
        while (!text.empty() && text.back() == ' ') { text.pop_back(); ++count; }
        if (text.empty() && builder_.get(id).marker == 0) builder_.unlink(id);
    }

    void trim_final_whitespace() {
        const auto id = builder_.get(parent_).last_child;
        if (id == npos || builder_.get(id).type != NodeType::text) return;
        auto& text = builder_.get(id).literal;
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
        if (text.empty() && builder_.get(id).marker == 0) builder_.unlink(id);
    }

    void parse_newline() {
        const auto begin = pos_;
        std::size_t spaces = 0;
        trim_trailing_spaces(spaces);
        append_node(spaces >= 2 ? NodeType::line_break : NodeType::soft_break, {}, begin, begin + 1, false);
        ++pos_;
        while (pos_ < input_.size() && ascii_space(input_[pos_])) ++pos_;
    }

    void parse_emphasis_run() {
        const auto begin = pos_;
        const char marker = input_[pos_];
        while (pos_ < input_.size() && input_[pos_] == marker) ++pos_;
        const auto length = pos_ - begin;
        const auto before = decode_utf8_before(input_, begin);
        const auto after = decode_utf8_at(input_, pos_);
        const bool before_ws = begin == 0 || is_unicode_whitespace(before);
        const bool after_ws = pos_ == input_.size() || is_unicode_whitespace(after);
        const bool before_punct = begin != 0 && is_unicode_punctuation(before);
        const bool after_punct = pos_ != input_.size() && is_unicode_punctuation(after);
        const bool left = !after_ws && (!after_punct || before_ws || before_punct);
        const bool right = !before_ws && (!before_punct || after_ws || after_punct);
        bool can_open = left;
        bool can_close = right;
        if (marker == '_') {
            can_open = left && (!right || before_punct);
            can_close = right && (!left || after_punct);
        }
        auto& seen_opener = emphasis_openers_[marker == '*' ? 0 : 1];
        if (!can_open && (!can_close || !seen_opener)) {
            append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, pos_);
            return;
        }
        seen_opener = seen_opener || can_open;
        const auto id = append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, pos_, false);
        if (id != npos) {
            builder_.get(id).marker = marker;
            push_delimiter({marker, id, begin, static_cast<std::uint32_t>(length), can_open, can_close});
        }
    }

    void parse_strikethrough_run() {
        const auto begin = pos_;
        while (pos_ < input_.size() && input_[pos_] == '~') ++pos_;
        const auto length = pos_ - begin;
        if (length > 2) {
            append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, pos_);
            return;
        }

        const auto before = decode_utf8_before(input_, begin);
        const auto after = decode_utf8_at(input_, pos_);
        const bool before_ws = begin == 0 || is_unicode_whitespace(before);
        const bool after_ws = pos_ == input_.size() || is_unicode_whitespace(after);
        const bool before_punct = begin != 0 && is_unicode_punctuation(before);
        const bool after_punct = pos_ != input_.size() && is_unicode_punctuation(after);
        const bool can_open = !after_ws && (!after_punct || before_ws || before_punct);
        const bool can_close = !before_ws && (!before_punct || after_ws || after_punct);
        auto& seen_opener = strike_openers_[length - 1];
        if (!can_open && (!can_close || !seen_opener)) {
            append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, pos_);
            return;
        }
        seen_opener = seen_opener || can_open;
        const auto id = append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, pos_, false);
        if (id == npos) return;
        builder_.get(id).marker = '~';
        push_delimiter({'~', id, begin, static_cast<std::uint32_t>(length), can_open, can_close});
    }

    void parse_open_bracket(bool image) {
        const auto begin = pos_;
        const auto length = image ? 2U : 1U;
        if (!links_possible_) {
            append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, begin + length);
            pos_ += length;
            return;
        }
        const auto id = append_node(NodeType::text, std::string_view(input_).substr(begin, length), begin, begin + length, false);
        if (id != npos) {
            builder_.get(id).marker = '[';
            push_delimiter({'[', id, begin, 1, true, false, image, false, last_bracket_});
            last_bracket_ = static_cast<int>(delimiters_.size()) - 1;
        }
        pos_ += length;
    }

    void parse_close_bracket() {
        const auto close_pos = pos_;
        ++pos_;
        const int opener_index = last_bracket_;
        if (opener_index < 0) {
            append_node(NodeType::text, "]", close_pos, pos_);
            return;
        }
        auto& opener = delimiters_[static_cast<std::size_t>(opener_index)];
        last_bracket_ = opener.previous_bracket;
        if (!opener.image && opener_index <= inactive_link_before_) {
            remove_delimiter(opener_index);
            append_node(NodeType::text, "]", close_pos, pos_);
            return;
        }

        LinkTail tail;
        if (pos_ < input_.size() && input_[pos_] == '(') tail = parse_inline_link(input_, pos_);
        if (!tail.valid && !references_.empty()) {
            std::string_view label;
            std::size_t reference_end = pos_;
            if (pos_ < input_.size() && input_[pos_] == '[') {
                auto end = pos_ + 1;
                bool escaped = false;
                while (end < input_.size() && end - pos_ <= 1001) {
                    if (!escaped && input_[end] == ']') break;
                    escaped = !escaped && input_[end] == '\\';
                    if (input_[end] != '\\') escaped = false;
                    ++end;
                }
                if (end < input_.size() && input_[end] == ']') {
                    label = std::string_view(input_).substr(pos_ + 1, end - pos_ - 1);
                    reference_end = end + 1;
                    if (label.empty()) label = std::string_view(input_).substr(opener.source_pos + (opener.image ? 2 : 1), close_pos - opener.source_pos - (opener.image ? 2 : 1));
                }
            } else {
                label = std::string_view(input_).substr(opener.source_pos + (opener.image ? 2 : 1), close_pos - opener.source_pos - (opener.image ? 2 : 1));
            }
            if (label.size() <= 999) {
                const auto found = references_.find(normalized_label(label));
                if (found != references_.end()) {
                    tail.valid = true;
                    tail.end = reference_end;
                    tail.destination = found->second.destination;
                    tail.title = found->second.title;
                }
            }
        }
        if (!tail.valid) {
            remove_delimiter(opener_index);
            append_node(NodeType::text, "]", close_pos, pos_);
            return;
        }

        process_emphasis(opener_index, static_cast<int>(delimiters_.size()));
        const auto opener_node = opener.node;
        const auto first = builder_.get(opener_node).next;
        const auto last = builder_.get(parent_).last_child;
        const auto wrapper = builder_.insert_after(opener_node, opener.image ? NodeType::image : NodeType::link);
        if (wrapper == npos) return;
        auto& node = builder_.get(wrapper);
        node.literal = std::move(tail.destination);
        node.title = std::move(tail.title);
        node.source = {source_offset(opener.source_pos), source_offset(tail.end)};
        if (!check_span(wrapper, first, last == opener_node ? npos : last)) return;
        if (first != npos && last != opener_node) builder_.move_range(first, last, wrapper);
        builder_.unlink(opener_node);

        for (auto i = opener_index; i >= 0;) {
            const auto next = delimiters_[static_cast<std::size_t>(i)].next;
            remove_delimiter(i);
            i = next;
        }
        if (!opener.image) {
            inactive_link_before_ = opener_index - 1;
        }
        pos_ = tail.end;
    }

    static bool odd_match(const Delimiter& opener, const Delimiter& closer) noexcept {
        if (!(closer.can_open || opener.can_close)) return false;
        return (opener.length + closer.length) % 3 == 0 &&
               (opener.length % 3 != 0 || closer.length % 3 != 0);
    }

    void process_emphasis(int bottom, int top) {
        std::array<std::array<std::array<int, 2>, 3>, 2> opener_bottom{};
        std::array<int, 2> strike_bottom{bottom, bottom};
        for (auto& by_mod : opener_bottom) for (auto& by_open : by_mod) by_open.fill(bottom);
        int current = bottom < 0 ? first_delimiter_ : delimiters_[static_cast<std::size_t>(bottom)].next;
        while (current >= 0 && current < top && builder_.ok()) {
            auto& closer = delimiters_[static_cast<std::size_t>(current)];
            if (closer.removed || (closer.character != '*' && closer.character != '_' && closer.character != '~') ||
                !closer.can_close || closer.length == 0) {
                current = closer.next;
                continue;
            }
            int opener_index = closer.previous;
            if (closer.character == '~') {
                auto& lower_bound = strike_bottom[static_cast<std::size_t>(closer.length - 1)];
                for (; opener_index > lower_bound; opener_index = delimiters_[static_cast<std::size_t>(opener_index)].previous) {
                    const auto& candidate = delimiters_[static_cast<std::size_t>(opener_index)];
                    if (!candidate.removed && candidate.character == '~' && candidate.can_open &&
                        candidate.length == closer.length) break;
                }
                if (opener_index <= lower_bound) {
                    lower_bound = current - 1;
                    if (!closer.can_open) remove_delimiter(current);
                    current = closer.next;
                    continue;
                }
            } else {
                const auto type = static_cast<std::size_t>(closer.character == '*' ? 0 : 1);
                const auto mod = static_cast<std::size_t>(closer.length % 3);
                const auto can_open = static_cast<std::size_t>(closer.can_open ? 1 : 0);
                for (; opener_index > opener_bottom[type][mod][can_open]; opener_index = delimiters_[static_cast<std::size_t>(opener_index)].previous) {
                    const auto& candidate = delimiters_[static_cast<std::size_t>(opener_index)];
                    if (!candidate.removed && candidate.character == closer.character && candidate.can_open &&
                        candidate.length > 0 && !odd_match(candidate, closer)) break;
                }
                if (opener_index <= opener_bottom[type][mod][can_open]) {
                    opener_bottom[type][mod][can_open] = current - 1;
                    if (!closer.can_open) remove_delimiter(current);
                    current = closer.next;
                    continue;
                }
            }

            if (opener_index <= bottom) {
                if (!closer.can_open) remove_delimiter(current);
                current = closer.next;
                continue;
            }

            auto& opener = delimiters_[static_cast<std::size_t>(opener_index)];
            const std::uint32_t use = closer.character == '~' ? closer.length :
                (opener.length >= 2 && closer.length >= 2 ? 2 : 1);
            auto& opener_text = builder_.get(opener.node).literal;
            auto& closer_text = builder_.get(closer.node).literal;
            opener_text.erase(opener_text.size() - static_cast<std::size_t>(use));
            closer_text.resize(closer_text.size() - static_cast<std::size_t>(use));
            // insert_after() can grow the arena and invalidate Node references.
            // Preserve the only state needed below before adding the wrapper.
            const bool opener_empty = opener_text.empty();
            const bool closer_empty = closer_text.empty();
            opener.length -= use;
            closer.length -= use;

            const auto first = builder_.get(opener.node).next;
            const auto last = builder_.get(closer.node).previous;
            const auto wrapper_type = closer.character == '~' ? NodeType::strikethrough :
                (use == 2 ? NodeType::strong : NodeType::emphasis);
            const auto wrapper = builder_.insert_after(opener.node, wrapper_type);
            if (wrapper == npos) return;
            builder_.get(wrapper).source = {builder_.get(opener.node).source.end - static_cast<std::uint32_t>(use),
                                            builder_.get(closer.node).source.begin + static_cast<std::uint32_t>(use)};
            builder_.get(opener.node).source.end -= static_cast<std::uint32_t>(use);
            builder_.get(closer.node).source.begin += static_cast<std::uint32_t>(use);
            if (!check_span(wrapper, first == closer.node ? npos : first, last)) return;
            if (first != closer.node && first != npos && last != opener.node) builder_.move_range(first, last, wrapper);
            while (opener.next != current) remove_delimiter(opener.next);
            if (opener_empty) {
                builder_.unlink(opener.node);
                remove_delimiter(opener_index);
            }
            if (closer_empty) {
                builder_.unlink(closer.node);
                remove_delimiter(current);
                current = closer.next;
            }
        }
    }

    void parse_text_run() {
        const auto begin = pos_;
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == '\\' || c == '`' || c == '&' || c == '<' || c == '\n' || c == '*' ||
                c == '_' || (c == '~' && options_.extensions.strikethrough) || c == '[' || c == ']' ||
                (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '[')) break;
            ++pos_;
        }
        append_node(NodeType::text, std::string_view(input_).substr(begin, pos_ - begin), begin, pos_);
    }

    Builder& builder_;
    NodeId parent_ = npos;
    std::string input_;
    const ReferenceMap& references_;
    const ParseOptions& options_;
    std::size_t pos_ = 0;
    std::size_t next_angle_ = 0;
    std::array<HtmlScanCache, 6> html_cache_{};
    bool needs_coalesce_ = false;
    bool links_possible_ = false;
    std::array<bool, 2> emphasis_openers_{};
    std::array<bool, 2> strike_openers_{};
    bool track_depth_ = false;
    NodeId first_inline_ = 0;
    std::size_t block_depth_ = 0;
    std::vector<std::uint32_t> heights_;
    int first_delimiter_ = -1;
    int last_delimiter_ = -1;
    int last_bracket_ = -1;
    int inactive_link_before_ = -1;
    std::vector<Delimiter> delimiters_;
    std::vector<std::pair<std::size_t, std::size_t>> backticks_;
};

} // namespace

void parse_inlines(Builder& builder, const ReferenceMap& references,
                   const ParseOptions& options) {
    const auto initial_size = builder.nodes().size();
    // Estimate from actual inline-bearing blocks. Empty lines and raw/code
    // blocks reserve no inline arena, and ordinary multiline prose stays tight.
    std::size_t estimate = initial_size;
    for (const auto& node : builder.nodes()) {
        if (node.type != NodeType::paragraph && node.type != NodeType::heading && node.type != NodeType::table_cell) continue;
        ++estimate;
        const std::string_view text(node.literal);
        const bool links_possible = !references.empty() || text.find('(') != std::string_view::npos;
        std::array<bool, 3> possible_opener{};
        for (auto pos = text.find_first_of("\n*_~[`"); pos != std::string_view::npos;) {
            const char marker = text[pos];
            ++pos;
            if (marker == '*' || marker == '_' || marker == '~' || marker == '`')
                while (pos < text.size() && text[pos] == marker) ++pos;
            bool count = marker != '[' || links_possible;
            if (marker == '*' || marker == '_' || marker == '~') {
                auto& opener = possible_opener[marker == '*' ? 0 : (marker == '_' ? 1 : 2)];
                opener = opener || (pos < text.size() && !ascii_space(text[pos]) && text[pos] != '\n');
                count = opener && (marker != '~' || options.extensions.strikethrough);
            }
            if (count) estimate += marker == '`' || (node.type == NodeType::table_cell && marker != '\n' && marker != '[') ? 1 : 2;
            pos = text.find_first_of("\n*_~[`", pos);
        }
    }
    estimate = std::min(estimate, static_cast<std::size_t>(npos - 1));
    if (options.max_nodes != 0) estimate = std::min(estimate, options.max_nodes);
    builder.nodes().reserve(estimate);
    InlineParser parser(builder, references, options);
    for (NodeId id = 1; id < initial_size && builder.ok(); ++id) {
        const auto type = builder.get(id).type;
        if ((type == NodeType::paragraph || type == NodeType::heading || type == NodeType::table_cell) &&
            builder.get(id).parent != npos) {
            auto literal = std::move(builder.get(id).literal);
            parser.run(id, std::move(literal));
        }
    }
}

} // namespace chmd::detail
