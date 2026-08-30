#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

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
    const auto destination_start = pos;
    std::string raw_destination;
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
        raw_destination.assign(input.substr(begin, pos - begin));
        ++pos;
    } else {
        const auto begin = pos;
        int depth = 0;
        bool escaped = false;
        while (pos < input.size()) {
            const char c = input[pos];
            if (!escaped && (ascii_space(c) || c == '\n')) break;
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
        raw_destination.assign(input.substr(begin, pos - begin));
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

    std::string raw_title;
    if (title_space && pos < input.size() && (input[pos] == '\'' || input[pos] == '"' || input[pos] == '(')) {
        const char opener = input[pos++];
        const char closer = opener == '(' ? ')' : opener;
        const auto begin = pos;
        bool escaped = false;
        bool title_newline = false;
        while (pos < input.size()) {
            const char c = input[pos];
            if (!escaped && c == closer) break;
            if (c == '\n') {
                if (title_newline && pos > 0 && input[pos - 1] == '\n') return {};
                title_newline = true;
            }
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            ++pos;
        }
        if (pos == input.size() || input[pos] != closer) return {};
        raw_title.assign(input.substr(begin, pos - begin));
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
    (void)destination_start;
    return result;
}

std::size_t scan_html(std::string_view input, std::size_t begin) {
    if (begin >= input.size() || input[begin] != '<') return begin;
    if (input.substr(begin).starts_with("<!-->")) return begin + 5;
    if (input.substr(begin).starts_with("<!--->")) return begin + 6;
    if (input.substr(begin).starts_with("<!--")) {
        const auto end = input.find("-->", begin + 4);
        if (end != std::string_view::npos && input.substr(begin + 4, end - begin - 4).find("<!--") == std::string_view::npos)
            return end + 3;
        return begin;
    }
    if (input.substr(begin).starts_with("<?")) {
        const auto end = input.find("?>", begin + 2);
        return end == std::string_view::npos ? begin : end + 2;
    }
    if (input.substr(begin).starts_with("<![CDATA[")) {
        const auto end = input.find("]]>", begin + 9);
        return end == std::string_view::npos ? begin : end + 3;
    }
    if (begin + 2 < input.size() && input[begin + 1] == '!' &&
        std::isalpha(static_cast<unsigned char>(input[begin + 2]))) {
        const auto end = input.find('>', begin + 3);
        return end == std::string_view::npos ? begin : end + 1;
    }

    const auto html_space = [](char c) { return c == ' ' || c == '\t' || c == '\n'; };
    const auto name_start = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == ':';
    };
    const auto name_char = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':' || c == '-';
    };

    auto pos = begin + 1;
    const bool closing = pos < input.size() && input[pos] == '/';
    if (closing) ++pos;
    if (pos >= input.size() || !std::isalpha(static_cast<unsigned char>(input[pos]))) return begin;
    while (pos < input.size() && (std::isalnum(static_cast<unsigned char>(input[pos])) || input[pos] == '-')) ++pos;
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
            const auto close = input.find(quote, pos);
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
    const auto close = input.find('>', begin + 1);
    if (close == std::string_view::npos) return result;
    const auto body = input.substr(begin + 1, close - begin - 1);
    if (body.empty()) return result;
    const auto colon = body.find(':');
    if (colon >= 2 && colon <= 32 && std::isalpha(static_cast<unsigned char>(body[0]))) {
        bool scheme = true;
        for (std::size_t i = 1; i < colon; ++i) {
            const char c = body[i];
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '.' || c == '-')) scheme = false;
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
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '!' || c == '#' ||
              c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '/' ||
              c == '=' || c == '?' || c == '^' || c == '_' || c == '`' || c == '{' || c == '|' ||
              c == '}' || c == '~' || c == '-')) return result;
    }
    const auto domain = body.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' || domain.front() == '-' || domain.back() == '-') return result;
    bool previous_dot = false;
    for (const char c : domain) {
        if (c == '.') {
            if (previous_dot) return result;
            previous_dot = true;
        } else {
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) return result;
            previous_dot = false;
        }
    }
    return {true, true, body};
}

class InlineParser {
public:
    InlineParser(Builder& builder, NodeId parent,
                 std::string input, const ReferenceMap& references)
        : builder_(builder), parent_(parent), input_(std::move(input)),
          references_(references) {}

    void run() {
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
            else if (c == '[') parse_open_bracket(false);
            else if (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '[') parse_open_bracket(true);
            else if (c == ']') parse_close_bracket();
            else parse_text_run();
        }
        process_emphasis(-1, static_cast<int>(delimiters_.size()));
        trim_final_whitespace();
    }

private:
    struct Delimiter {
        char character = 0;
        NodeId node = npos;
        std::size_t source_pos = 0;
        int length = 0;
        bool can_open = false;
        bool can_close = false;
        bool active = true;
        bool image = false;
        bool removed = false;
    };

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
            backticks_[i - begin].push_back(begin);
        }
    }

    void parse_backslash() {
        if (pos_ + 1 < input_.size() && escapable(input_[pos_ + 1])) {
            append_node(NodeType::text, input_.substr(pos_ + 1, 1), pos_, pos_ + 2);
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
        const auto found = backticks_.find(run);
        if (found == backticks_.end()) {
            append_node(NodeType::text, input_.substr(begin, run), begin, pos_, false);
            return;
        }
        const auto& positions = found->second;
        const auto close = std::upper_bound(positions.begin(), positions.end(), begin);
        if (close == positions.end()) {
            append_node(NodeType::text, input_.substr(begin, run), begin, pos_, false);
            return;
        }
        std::string content(input_.substr(pos_, *close - pos_));
        std::replace(content.begin(), content.end(), '\n', ' ');
        const bool all_space = std::all_of(content.begin(), content.end(), [](char c) { return c == ' '; });
        if (!all_space && content.size() >= 2 && content.front() == ' ' && content.back() == ' ') {
            content.erase(content.begin());
            content.pop_back();
        }
        append_node(NodeType::code, content, begin, *close + run, false);
        pos_ = *close + run;
    }

    void parse_entity() {
        const auto semicolon = input_.find(';', pos_ + 1);
        if (semicolon != std::string::npos && semicolon - pos_ <= 33) {
            const auto raw = input_.substr(pos_, semicolon - pos_ + 1);
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
        const auto link = parse_autolink(input_, pos_);
        if (link.valid) {
            const auto end = pos_ + link.label.size() + 2;
            const auto id = builder_.append(parent_, NodeType::link, {source_offset(pos_), source_offset(end)});
            if (id != npos) {
                auto& node = builder_.get(id);
                node.literal = link.email ? "mailto:" + std::string(link.label) : std::string(link.label);
                const auto text = builder_.append(id, NodeType::text, {source_offset(pos_ + 1), source_offset(end - 1)});
                if (text != npos) builder_.get(text).literal.assign(link.label);
            }
            pos_ = end;
            return;
        }
        const auto end = scan_html(input_, pos_);
        if (end != pos_) {
            append_node(NodeType::html_inline, input_.substr(pos_, end - pos_), pos_, end, false);
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
        const auto id = append_node(NodeType::text, input_.substr(begin, length), begin, pos_, false);
        if (id != npos) {
            builder_.get(id).marker = marker;
            delimiters_.push_back({marker, id, begin, static_cast<int>(length), can_open, can_close});
        }
    }

    void parse_open_bracket(bool image) {
        const auto begin = pos_;
        const auto length = image ? 2U : 1U;
        const auto id = append_node(NodeType::text, input_.substr(begin, length), begin, begin + length, false);
        if (id != npos) {
            builder_.get(id).marker = '[';
            delimiters_.push_back({'[', id, begin, 1, true, false, true, image});
        }
        pos_ += length;
    }

    int find_opener() const {
        for (auto i = static_cast<int>(delimiters_.size()) - 1; i >= 0; --i) {
            const auto& delimiter = delimiters_[static_cast<std::size_t>(i)];
            if (!delimiter.removed && delimiter.character == '[') return i;
        }
        return -1;
    }

    void parse_close_bracket() {
        const auto close_pos = pos_;
        const auto closer_node = append_node(NodeType::text, "]", pos_, pos_ + 1, false);
        ++pos_;
        const int opener_index = find_opener();
        if (opener_index < 0 || closer_node == npos) return;
        auto& opener = delimiters_[static_cast<std::size_t>(opener_index)];
        if (!opener.active) {
            opener.removed = true;
            return;
        }

        LinkTail tail;
        if (pos_ < input_.size() && input_[pos_] == '(') tail = parse_inline_link(input_, pos_);
        if (!tail.valid) {
            std::string label;
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
                    label.assign(input_.substr(pos_ + 1, end - pos_ - 1));
                    reference_end = end + 1;
                    if (label.empty()) label.assign(input_.substr(opener.source_pos + (opener.image ? 2 : 1), close_pos - opener.source_pos - (opener.image ? 2 : 1)));
                }
            } else {
                label.assign(input_.substr(opener.source_pos + (opener.image ? 2 : 1), close_pos - opener.source_pos - (opener.image ? 2 : 1)));
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
            opener.removed = true;
            return;
        }

        process_emphasis(opener_index, static_cast<int>(delimiters_.size()));
        const auto opener_node = opener.node;
        const auto first = builder_.get(opener_node).next;
        const auto last = builder_.get(closer_node).previous;
        const auto wrapper = builder_.insert_after(opener_node, opener.image ? NodeType::image : NodeType::link);
        if (wrapper == npos) return;
        auto& node = builder_.get(wrapper);
        node.literal = std::move(tail.destination);
        node.title = std::move(tail.title);
        node.source = {source_offset(opener.source_pos), source_offset(tail.end)};
        if (first != closer_node && first != npos && last != opener_node) builder_.move_range(first, last, wrapper);
        builder_.unlink(opener_node);
        builder_.unlink(closer_node);

        for (std::size_t i = static_cast<std::size_t>(opener_index); i < delimiters_.size(); ++i)
            delimiters_[i].removed = true;
        if (!opener.image) {
            for (int i = opener_index - 1; i >= 0; --i) {
                auto& delimiter = delimiters_[static_cast<std::size_t>(i)];
                if (!delimiter.removed && delimiter.character == '[' && !delimiter.image) delimiter.active = false;
            }
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
        for (auto& by_mod : opener_bottom) for (auto& by_open : by_mod) by_open.fill(bottom);
        int current = bottom + 1;
        while (current < top) {
            auto& closer = delimiters_[static_cast<std::size_t>(current)];
            if (closer.removed || (closer.character != '*' && closer.character != '_') || !closer.can_close || closer.length == 0) {
                ++current;
                continue;
            }
            const auto type = static_cast<std::size_t>(closer.character == '*' ? 0 : 1);
            const auto mod = static_cast<std::size_t>(closer.length % 3);
            const auto can_open = static_cast<std::size_t>(closer.can_open ? 1 : 0);
            int opener_index = current - 1;
            for (; opener_index > opener_bottom[type][mod][can_open]; --opener_index) {
                const auto& candidate = delimiters_[static_cast<std::size_t>(opener_index)];
                if (!candidate.removed && candidate.character == closer.character && candidate.can_open &&
                    candidate.length > 0 && !odd_match(candidate, closer)) break;
            }
            if (opener_index <= opener_bottom[type][mod][can_open]) {
                opener_bottom[type][mod][can_open] = current - 1;
                if (!closer.can_open) closer.removed = true;
                ++current;
                continue;
            }

            auto& opener = delimiters_[static_cast<std::size_t>(opener_index)];
            const int use = opener.length >= 2 && closer.length >= 2 ? 2 : 1;
            auto& opener_text = builder_.get(opener.node).literal;
            auto& closer_text = builder_.get(closer.node).literal;
            opener_text.erase(opener_text.size() - static_cast<std::size_t>(use));
            closer_text.erase(0, static_cast<std::size_t>(use));
            // insert_after() can grow the arena and invalidate Node references.
            // Preserve the only state needed below before adding the wrapper.
            const bool opener_empty = opener_text.empty();
            const bool closer_empty = closer_text.empty();
            opener.length -= use;
            closer.length -= use;

            const auto first = builder_.get(opener.node).next;
            const auto last = builder_.get(closer.node).previous;
            const auto wrapper = builder_.insert_after(opener.node, use == 2 ? NodeType::strong : NodeType::emphasis);
            if (wrapper == npos) return;
            builder_.get(wrapper).source = {builder_.get(opener.node).source.end - static_cast<std::uint32_t>(use),
                                            builder_.get(closer.node).source.begin + static_cast<std::uint32_t>(use)};
            if (first != closer.node && first != npos && last != opener.node) builder_.move_range(first, last, wrapper);
            for (int i = opener_index + 1; i < current; ++i) delimiters_[static_cast<std::size_t>(i)].removed = true;
            if (opener_empty) {
                builder_.unlink(opener.node);
                opener.removed = true;
            }
            if (closer_empty) {
                builder_.unlink(closer.node);
                closer.removed = true;
                ++current;
            }
        }
    }

    void parse_text_run() {
        const auto begin = pos_;
        while (pos_ < input_.size()) {
            const char c = input_[pos_];
            if (c == '\\' || c == '`' || c == '&' || c == '<' || c == '\n' || c == '*' ||
                c == '_' || c == '[' || c == ']' || (c == '!' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '[')) break;
            ++pos_;
        }
        append_node(NodeType::text, input_.substr(begin, pos_ - begin), begin, pos_);
    }

    Builder& builder_;
    NodeId parent_;
    std::string input_;
    const ReferenceMap& references_;
    std::size_t pos_ = 0;
    std::vector<Delimiter> delimiters_;
    std::unordered_map<std::size_t, std::vector<std::size_t>> backticks_;
};

} // namespace

void parse_inlines(Builder& builder, const ReferenceMap& references) {
    const auto initial_size = builder.nodes().size();
    for (NodeId id = 1; id < initial_size && builder.ok(); ++id) {
        const auto type = builder.get(id).type;
        if ((type == NodeType::paragraph || type == NodeType::heading) && builder.get(id).parent != npos) {
            auto literal = std::move(builder.get(id).literal);
            InlineParser parser(builder, id, std::move(literal), references);
            parser.run();
        }
    }
}

} // namespace chmd::detail
