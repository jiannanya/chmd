#include "internal.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace chmd::detail {
namespace {

struct Line {
    std::size_t begin;
    std::size_t end;
    std::size_t next;
};

struct Indent {
    std::size_t first;
    std::size_t columns;
    bool blank;
};

struct ListMarker {
    bool valid = false;
    ListKind kind = ListKind::bullet;
    char marker = 0;
    std::uint32_t start = 0;
    std::size_t marker_offset = 0;
    std::size_t width = 0;
    std::size_t padding = 0;
    std::size_t content = 0;
    std::size_t content_column = 0;
    std::size_t residual_spaces = 0;
    bool content_blank = false;
};

constexpr bool ascii_space(char c) noexcept { return c == ' ' || c == '\t'; }

char lower_ascii(char c) noexcept {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c + ('a' - 'A'));
    return c;
}

bool iequal_prefix(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (lower_ascii(value[i]) != lower_ascii(prefix[i])) return false;
    }
    return true;
}

Indent indentation(std::string_view line, std::size_t offset, std::size_t column) {
    auto pos = offset;
    auto col = column;
    while (pos < line.size()) {
        if (line[pos] == ' ') {
            ++pos;
            ++col;
        } else if (line[pos] == '\t') {
            ++pos;
            col += 4 - (col % 4);
        } else {
            break;
        }
    }
    return {pos, col - column, pos == line.size()};
}

std::size_t consume_columns(std::string_view line, std::size_t& pos,
                            std::size_t& column, std::size_t wanted,
                            std::string* partial = nullptr) {
    const auto initial = column;
    while (pos < line.size() && column - initial < wanted) {
        if (line[pos] == ' ') {
            ++pos;
            ++column;
        } else if (line[pos] == '\t') {
            const auto width = 4 - (column % 4);
            ++pos;
            column += width;
            if (column - initial > wanted && partial != nullptr) {
                partial->append(column - initial - wanted, ' ');
            }
        } else {
            break;
        }
    }
    return column - initial;
}

bool thematic_break(std::string_view line, std::size_t pos, std::size_t column) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank) return false;
    const char marker = line[ind.first];
    if (marker != '*' && marker != '-' && marker != '_') return false;
    std::size_t count = 0;
    for (auto i = ind.first; i < line.size(); ++i) {
        if (line[i] == marker) ++count;
        else if (!ascii_space(line[i])) return false;
    }
    return count >= 3;
}

bool setext_underline(std::string_view line, std::size_t pos, std::size_t column,
                      std::uint32_t& level) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank) return false;
    const char marker = line[ind.first];
    if (marker != '=' && marker != '-') return false;
    auto i = ind.first;
    while (i < line.size() && line[i] == marker) ++i;
    while (i < line.size() && ascii_space(line[i])) ++i;
    if (i != line.size()) return false;
    level = marker == '=' ? 1U : 2U;
    return true;
}

bool atx_heading(std::string_view line, std::size_t pos, std::size_t column,
                 std::uint32_t& level, std::size_t& content_begin,
                 std::size_t& content_end) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank || line[ind.first] != '#') return false;
    auto i = ind.first;
    while (i < line.size() && line[i] == '#' && i - ind.first < 7) ++i;
    const auto count = i - ind.first;
    if (count == 0 || count > 6) return false;
    if (i < line.size() && !ascii_space(line[i])) return false;
    level = static_cast<std::uint32_t>(count);
    while (i < line.size() && ascii_space(line[i])) ++i;
    content_begin = i;
    content_end = line.size();
    while (content_end > content_begin && ascii_space(line[content_end - 1])) --content_end;
    auto hashes = content_end;
    while (hashes > content_begin && line[hashes - 1] == '#') --hashes;
    if (hashes < content_end && (hashes == content_begin || ascii_space(line[hashes - 1]))) {
        content_end = hashes == content_begin ? hashes : hashes - 1;
        while (content_end > content_begin && ascii_space(line[content_end - 1])) --content_end;
    }
    return true;
}

bool fence_open(std::string_view line, std::size_t pos, std::size_t column,
                char& marker, std::size_t& count, std::size_t& marker_offset,
                std::size_t& info_begin) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank) return false;
    marker = line[ind.first];
    if (marker != '`' && marker != '~') return false;
    auto i = ind.first;
    while (i < line.size() && line[i] == marker) ++i;
    count = i - ind.first;
    if (count < 3) return false;
    if (marker == '`' && line.substr(i).find('`') != std::string_view::npos) return false;
    marker_offset = ind.columns;
    while (i < line.size() && ascii_space(line[i])) ++i;
    info_begin = i;
    return true;
}

bool fence_close(std::string_view line, std::size_t pos, std::size_t column,
                 char marker, std::size_t count) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank || line[ind.first] != marker) return false;
    auto i = ind.first;
    while (i < line.size() && line[i] == marker) ++i;
    if (i - ind.first < count) return false;
    while (i < line.size() && ascii_space(line[i])) ++i;
    return i == line.size();
}

ListMarker parse_list_marker(std::string_view line, std::size_t pos,
                             std::size_t column, bool interrupting) {
    ListMarker result;
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank) return result;
    auto i = ind.first;
    std::size_t width = 0;
    std::uint32_t start = 1;
    ListKind kind = ListKind::bullet;
    char marker = line[i];
    if (marker == '*' || marker == '+' || marker == '-') {
        width = 1;
    } else if (marker >= '0' && marker <= '9') {
        kind = ListKind::ordered;
        const auto digits = i;
        start = 0;
        while (i < line.size() && line[i] >= '0' && line[i] <= '9' && i - digits < 10) {
            start = start * 10U + static_cast<unsigned>(line[i] - '0');
            ++i;
        }
        if (i == digits || i - digits > 9 || i == line.size() ||
            (line[i] != '.' && line[i] != ')')) return result;
        marker = line[i];
        width = i - digits + 1;
        i = digits;
    } else {
        return result;
    }
    const auto after_marker = ind.first + width;
    if (after_marker < line.size() && !ascii_space(line[after_marker])) return result;
    if (interrupting && kind == ListKind::ordered && start != 1) return result;

    auto content = after_marker;
    auto spacing_column = column + ind.columns + width;
    std::size_t spaces = 0;
    while (content < line.size() && ascii_space(line[content]) && spaces <= 4) {
        if (line[content] == ' ') {
            ++spaces;
            ++spacing_column;
        } else {
            const auto tab = 4 - (spacing_column % 4);
            spaces += tab;
            spacing_column += tab;
        }
        ++content;
    }
    const bool blank = content == line.size();
    if (interrupting && blank) return result;

    std::size_t padding_spaces = spaces;
    if (spaces == 0 || spaces > 4 || blank) {
        padding_spaces = 1;
        content = after_marker;
        if (content < line.size() && ascii_space(line[content])) ++content;
        if (blank) content = line.size();
    }
    result.valid = true;
    result.kind = kind;
    result.marker = marker;
    result.start = start;
    result.marker_offset = ind.columns;
    result.width = width;
    result.padding = ind.columns + width + padding_spaces;
    result.content = content;
    auto actual_column = column;
    for (auto cursor = pos; cursor < content; ++cursor) {
        if (line[cursor] == '\t') actual_column += 4 - (actual_column % 4);
        else ++actual_column;
    }
    result.content_column = actual_column;
    const auto structural_column = column + result.padding;
    result.residual_spaces = actual_column > structural_column ? actual_column - structural_column : 0;
    result.content_blank = blank;
    return result;
}

int html_block_start(std::string_view line, std::size_t pos, std::size_t column,
                     bool interrupting) {
    const auto ind = indentation(line, pos, column);
    if (ind.columns > 3 || ind.blank || line[ind.first] != '<') return 0;
    const auto rest = line.substr(ind.first);
    for (const auto tag : {std::string_view("<script"), std::string_view("<pre"),
                           std::string_view("<style"), std::string_view("<textarea")}) {
        if (iequal_prefix(rest, tag) && (rest.size() == tag.size() ||
            ascii_space(rest[tag.size()]) || rest[tag.size()] == '>')) return 1;
    }
    if (rest.starts_with("<!--")) return 2;
    if (rest.starts_with("<?")) return 3;
    if (rest.starts_with("<![CDATA[")) return 5;
    if (rest.size() >= 3 && rest[1] == '!' &&
        ((rest[2] >= 'A' && rest[2] <= 'Z') || (rest[2] >= 'a' && rest[2] <= 'z'))) return 4;

    static constexpr std::array<std::string_view, 62> tags = {
        "address","article","aside","base","basefont","blockquote","body","caption",
        "center","col","colgroup","dd","details","dialog","dir","div","dl","dt",
        "fieldset","figcaption","figure","footer","form","frame","frameset","h1","h2",
        "h3","h4","h5","h6","head","header","hr","html","iframe","legend","li",
        "link","main","menu","menuitem","nav","noframes","ol","optgroup","option","p",
        "param","search","section","summary","table","tbody","td","tfoot","th","thead",
        "title","tr","track","ul"
    };
    auto i = std::size_t{1};
    if (i < rest.size() && rest[i] == '/') ++i;
    const auto begin = i;
    while (i < rest.size() && ((rest[i] >= 'A' && rest[i] <= 'Z') ||
           (rest[i] >= 'a' && rest[i] <= 'z') || (rest[i] >= '0' && rest[i] <= '9') || rest[i] == '-')) ++i;
    if (i > begin) {
        std::string tag(rest.substr(begin, i - begin));
        std::transform(tag.begin(), tag.end(), tag.begin(), lower_ascii);
        const bool boundary = i == rest.size() || ascii_space(rest[i]) || rest[i] == '>' ||
                              (rest[i] == '/' && i + 1 < rest.size() && rest[i + 1] == '>');
        if (boundary && std::find(tags.begin(), tags.end(), tag) != tags.end()) return 6;
    }
    if (interrupting) return 0; // Type 7 cannot interrupt a paragraph.

    // A deliberately small state machine for a complete open/closing tag on this line.
    i = 1;
    bool closing = false;
    if (i < rest.size() && rest[i] == '/') { closing = true; ++i; }
    if (i == rest.size() || !ascii_alpha(static_cast<unsigned char>(rest[i]))) return 0;
    ++i;
    while (i < rest.size() && (ascii_alnum(static_cast<unsigned char>(rest[i])) || rest[i] == '-')) ++i;
    if (closing) {
        while (i < rest.size() && ascii_space(rest[i])) ++i;
        if (i >= rest.size() || rest[i] != '>') return 0;
        ++i;
        while (i < rest.size() && ascii_space(rest[i])) ++i;
        return i == rest.size() ? 7 : 0;
    }
    while (i < rest.size()) {
        bool separated = false;
        while (i < rest.size() && ascii_space(rest[i])) { ++i; separated = true; }
        if (i < rest.size() && rest[i] == '>') {
            ++i;
            while (i < rest.size() && ascii_space(rest[i])) ++i;
            return i == rest.size() ? 7 : 0;
        }
        if (i + 1 < rest.size() && rest[i] == '/' && rest[i + 1] == '>') {
            i += 2;
            while (i < rest.size() && ascii_space(rest[i])) ++i;
            return i == rest.size() ? 7 : 0;
        }
        if (!separated) return 0;
        if (i == rest.size() || !(ascii_alpha(static_cast<unsigned char>(rest[i])) ||
            rest[i] == '_' || rest[i] == ':')) return 0;
        ++i;
        while (i < rest.size() && (ascii_alnum(static_cast<unsigned char>(rest[i])) ||
               rest[i] == '_' || rest[i] == '.' || rest[i] == ':' || rest[i] == '-')) ++i;
        while (i < rest.size() && ascii_space(rest[i])) ++i;
        if (i < rest.size() && rest[i] == '=') {
            ++i;
            while (i < rest.size() && ascii_space(rest[i])) ++i;
            if (i == rest.size()) return 0;
            if (rest[i] == '\'' || rest[i] == '"') {
                const char quote = rest[i++];
                while (i < rest.size() && rest[i] != quote) ++i;
                if (i == rest.size()) return 0;
                ++i;
            } else {
                const auto value = i;
                while (i < rest.size() && !ascii_space(rest[i]) && rest[i] != '>' &&
                       rest[i] != '"' && rest[i] != '\'' && rest[i] != '=' && rest[i] != '<' && rest[i] != '`') ++i;
                if (i == value) return 0;
            }
        }
    }
    return 0;
}

bool html_block_ends(int type, std::string_view line) {
    switch (type) {
    case 1: {
        for (auto pos = line.find('<'); pos != std::string_view::npos; pos = line.find('<', pos + 1)) {
            const auto rest = line.substr(pos);
            if (iequal_prefix(rest, "</script>") || iequal_prefix(rest, "</pre>") ||
                iequal_prefix(rest, "</style>") || iequal_prefix(rest, "</textarea>")) return true;
        }
        return false;
    }
    case 2: return line.find("-->") != std::string_view::npos;
    case 3: return line.find("?>") != std::string_view::npos;
    case 4: return line.find('>') != std::string_view::npos;
    case 5: return line.find("]]>") != std::string_view::npos;
    default: return false;
    }
}

bool blank_between(std::string_view source, std::size_t begin, std::size_t end) {
    if (begin >= end || begin >= source.size()) return false;
    end = std::min(end, source.size());
    bool after_newline = begin == 0 || source[begin - 1] == '\n';
    bool only_ws = true;
    for (auto i = begin; i < end; ++i) {
        if (source[i] == '\n') {
            if (after_newline && only_ws) return true;
            after_newline = true;
            only_ws = true;
        } else if (source[i] != ' ' && source[i] != '\t') {
            only_ws = false;
        }
    }
    return false;
}

bool inline_interrupt(std::string_view line, std::size_t pos, std::size_t column) {
    std::uint32_t level = 0;
    std::size_t a = 0, b = 0;
    char marker = 0;
    std::size_t count = 0, indent = 0, info = 0;
    const auto ind = indentation(line, pos, column);
    if (ind.columns <= 3 && !ind.blank && line[ind.first] == '>') return true;
    if (thematic_break(line, pos, column)) return true;
    if (atx_heading(line, pos, column, level, a, b)) return true;
    if (fence_open(line, pos, column, marker, count, indent, info)) return true;
    if (parse_list_marker(line, pos, column, true).valid) return true;
    return html_block_start(line, pos, column, true) != 0;
}

std::size_t trim_line_end(std::string_view text, std::size_t end) {
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    return end;
}

struct TableCellSlice {
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct TableRowParse {
    std::vector<TableCellSlice> cells;
    bool has_pipe = false;
};

bool escaped_pipe(std::string_view row, std::size_t offset) noexcept {
    std::size_t slashes = 0;
    while (offset > slashes && row[offset - slashes - 1] == '\\') ++slashes;
    return (slashes & 1U) != 0;
}

const TableRowParse& split_table_row(std::string_view row, TableRowParse& result,
                              std::size_t max_cells = std::numeric_limits<std::size_t>::max()) {
    result.cells.clear();
    result.has_pipe = false;
    std::size_t begin = 0;
    std::size_t end = trim_line_end(row, row.size());
    while (begin < end && ascii_space(row[begin])) ++begin;
    if (begin == end) return result;

    std::size_t cell_begin = begin;
    if (row[begin] == '|') {
        result.has_pipe = true;
        cell_begin = begin + 1;
    }
    for (std::size_t i = cell_begin; i < end; ++i) {
        if (row[i] != '|' || escaped_pipe(row, i)) continue;
        result.has_pipe = true;
        auto first = cell_begin;
        auto last = i;
        while (first < last && ascii_space(row[first])) ++first;
        while (last > first && ascii_space(row[last - 1])) --last;
        if (result.cells.size() < max_cells) result.cells.push_back({first, last});
        cell_begin = i + 1;
    }
    if (cell_begin < end || row[end - 1] != '|') {
        auto first = cell_begin;
        auto last = end;
        while (first < last && ascii_space(row[first])) ++first;
        while (last > first && ascii_space(row[last - 1])) --last;
        if (result.cells.size() < max_cells) result.cells.push_back({first, last});
    }
    return result;
}

std::string table_cell_text(std::string_view row, TableCellSlice cell) {
    std::string result;
    result.reserve(cell.end - cell.begin);
    for (auto i = cell.begin; i < cell.end; ++i) {
        if (row[i] == '\\' && i + 1 < cell.end && row[i + 1] == '|') continue;
        result.push_back(row[i]);
    }
    return result;
}

bool parse_table_delimiter(std::string_view row,
                           std::vector<TableAlignment>& alignments, TableRowParse& scratch) {
    const auto& parsed = split_table_row(row, scratch);
    if (!parsed.has_pipe || parsed.cells.empty()) return false;
    alignments.clear();
    alignments.reserve(parsed.cells.size());
    for (const auto cell : parsed.cells) {
        auto value = row.substr(cell.begin, cell.end - cell.begin);
        bool left = false;
        bool right = false;
        if (!value.empty() && value.front() == ':') {
            left = true;
            value.remove_prefix(1);
        }
        if (!value.empty() && value.back() == ':') {
            right = true;
            value.remove_suffix(1);
        }
        if (value.empty() || !std::all_of(value.begin(), value.end(),
            [](char c) { return c == '-'; })) return false;
        alignments.push_back(left && right ? TableAlignment::center :
            (left ? TableAlignment::left : (right ? TableAlignment::right : TableAlignment::none)));
    }
    return true;
}

bool task_whitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

struct ReferenceParse {
    std::size_t consumed = 0;
    std::string label;
    Reference reference;
};

bool escapable(char c) noexcept {
    return (c >= '!' && c <= '/') || (c >= ':' && c <= '@') ||
           (c >= '[' && c <= '`') || (c >= '{' && c <= '~');
}

std::string unescape_backslashes(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size() && escapable(value[i + 1])) ++i;
        out.push_back(value[i]);
    }
    return unescape_entities(out);
}

ReferenceParse parse_reference(std::string_view text) {
    ReferenceParse result;
    if (text.empty() || text[0] != '[') return result;
    std::size_t i = 1;
    std::string raw_label;
    raw_label.reserve(32);
    bool nonspace = false;
    while (i < text.size() && raw_label.size() <= 999) {
        if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == '\n') return {};
        if (text[i] == '\\' && i + 1 < text.size() && escapable(text[i + 1])) {
            raw_label.push_back('\\');
            raw_label.push_back(text[i + 1]);
            if (!ascii_space(text[i + 1]) && text[i + 1] != '\n') nonspace = true;
            i += 2;
            continue;
        }
        if (text[i] == '[') return {};
        if (text[i] == ']') break;
        raw_label.push_back(text[i]);
        if (!ascii_space(text[i]) && text[i] != '\n') nonspace = true;
        ++i;
    }
    if (i >= text.size() || text[i] != ']' || !nonspace || raw_label.size() > 999) return {};
    ++i;
    if (i >= text.size() || text[i++] != ':') return {};
    while (i < text.size() && ascii_space(text[i])) ++i;
    if (i < text.size() && text[i] == '\n') {
        ++i;
        while (i < text.size() && ascii_space(text[i])) ++i;
    }
    if (i >= text.size()) return {};

    std::string destination;
    if (text[i] == '<') {
        ++i;
        const auto begin = i;
        bool escaped = false;
        while (i < text.size() && text[i] != '\n' && (text[i] != '>' || escaped)) {
            if (!escaped && text[i] == '<') return {};
            escaped = text[i] == '\\' && !escaped;
            if (text[i] != '\\') escaped = false;
            ++i;
        }
        if (i >= text.size() || text[i] != '>') return {};
        destination = unescape_backslashes(text.substr(begin, i - begin));
        ++i;
    } else {
        const auto begin = i;
        int depth = 0;
        bool escaped = false;
        while (i < text.size() && text[i] != '\n' && !ascii_space(text[i])) {
            const char c = text[i];
            if (static_cast<unsigned char>(c) < 0x20U || c == '\x7F') return {};
            if (!escaped && c == '(' && ++depth > 32) return {};
            if (!escaped && c == ')' && --depth < 0) break;
            escaped = !escaped && c == '\\';
            if (c != '\\') escaped = false;
            ++i;
        }
        if (i == begin || depth != 0) return {};
        destination = unescape_backslashes(text.substr(begin, i - begin));
    }

    const auto after_destination = i;
    bool had_ws = false;
    while (i < text.size() && ascii_space(text[i])) { had_ws = true; ++i; }
    if (i < text.size() && text[i] == '\n') {
        had_ws = true;
        ++i;
        while (i < text.size() && ascii_space(text[i])) ++i;
    }
    std::string title;
    bool parsed_title = false;
    if (had_ws && i < text.size() && (text[i] == '\'' || text[i] == '"' || text[i] == '(')) {
        const char opener = text[i++];
        const char closer = opener == '(' ? ')' : opener;
        const auto begin = i;
        bool escaped = false;
        while (i < text.size() && (text[i] != closer || escaped)) {
            if (!escaped && opener == '(' && text[i] == '(') break;
            if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == '\n') break;
            escaped = !escaped && text[i] == '\\';
            if (text[i] != '\\') escaped = false;
            ++i;
        }
        if (i < text.size() && text[i] == closer) {
            title = unescape_backslashes(text.substr(begin, i - begin));
            ++i;
            parsed_title = true;
        } else {
            i = after_destination;
        }
    } else {
        i = after_destination;
    }
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i < text.size() && text[i] != '\n' && parsed_title) {
        title.clear();
        i = after_destination;
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    }
    if (i < text.size() && text[i] != '\n') return {};
    if (i < text.size()) ++i;
    result.consumed = i;
    result.label = normalize_reference(raw_label);
    result.reference = {std::move(destination), std::move(title)};
    return result;
}

class BlockParser {
public:
    BlockParser(Document& document, Builder& builder, const ParseOptions& options,
                ParseError& error)
        : source_(detail_access::source(document)), builder_(builder),
          options_(options), error_(error) {
        open_.push_back(0);
    }

    ReferenceMap run() {
        for (std::size_t begin = 0; begin < source_.size() && builder_.ok();) {
            const auto newline = source_.find('\n', begin);
            const auto end = newline == std::string::npos ? source_.size() : newline;
            const auto next = newline == std::string::npos ? end : end + 1;
            process_line({begin, end, next});
            begin = next;
        }
        close_to(1);
        finalize_lists();
        extract_references();
        if (options_.extensions.task_lists) finalize_task_items();
        return std::move(references_);
    }

private:
    void touch_open(std::size_t end) {
        const auto bounded = static_cast<std::uint32_t>(std::min<std::size_t>(end, std::numeric_limits<std::uint32_t>::max()));
        for (const auto id : open_) builder_.get(id).source.end = bounded;
    }

    void close_to(std::size_t size) {
        while (open_.size() > size) {
            const auto id = open_.back();
            auto& node = builder_.get(id);
            if (node.type == NodeType::code_block && !node.fenced) {
                while (node.literal.ends_with("\n\n")) node.literal.pop_back();
            }
            open_.pop_back();
        }
    }

    bool same_list(const Node& node, const ListMarker& marker) const noexcept {
        return node.type == NodeType::list && node.list_kind == marker.kind && node.marker == marker.marker;
    }

    void append_line(Node& node, std::string_view line, std::size_t pos, bool newline = true) {
        if (pos < line.size()) node.literal.append(line.substr(pos));
        if (newline) node.literal.push_back('\n');
    }

    void process_line(const Line& info) {
        const std::string_view line(source_.data() + info.begin, info.end - info.begin);
        std::size_t pos = 0;
        std::size_t column = 0;
        std::size_t virtual_spaces = 0;
        std::size_t matched = 1;

        // Match continuation prefixes of all currently open containers.
        for (std::size_t index = 1; index < open_.size(); ++index) {
            auto& node = builder_.get(open_[index]);
            if (node.type == NodeType::list) {
                matched = index + 1;
                continue;
            }
            if (node.type == NodeType::block_quote) {
                const auto ind = indentation(line, pos, column);
                if (virtual_spaces + ind.columns <= 3 && !ind.blank && line[ind.first] == '>') {
                    virtual_spaces = 0;
                    pos = ind.first + 1;
                    column += ind.columns + 1;
                    if (pos < line.size() && line[pos] == ' ') { ++pos; ++column; }
                    else if (pos < line.size() && line[pos] == '\t') {
                        const auto width = 4 - (column % 4);
                        ++pos;
                        column += width;
                        virtual_spaces += width - 1;
                    }
                    matched = index + 1;
                    continue;
                }
                break;
            }
            if (node.type == NodeType::item) {
                const auto ind = indentation(line, pos, column);
                if (ind.blank) {
                    pos = line.size();
                    column += ind.columns;
                    matched = index + 1;
                    continue;
                }
                if (node.first_child == npos && (!node.tight || blank_pending_)) break;
                if (virtual_spaces + ind.columns >= node.padding) {
                    auto needed = static_cast<std::size_t>(node.padding);
                    const auto from_virtual = std::min(needed, virtual_spaces);
                    virtual_spaces -= from_virtual;
                    needed -= from_virtual;
                    std::string partial;
                    if (needed != 0) consume_columns(line, pos, column, needed, &partial);
                    virtual_spaces += partial.size();
                    matched = index + 1;
                    continue;
                }
                break;
            }
            // A leaf is always the last open node.
            if (node.type == NodeType::code_block) {
                if (node.fenced) {
                    matched = index + 1;
                } else {
                    const auto ind = indentation(line, pos, column);
                    if (ind.blank || ind.columns >= 4) matched = index + 1;
                }
                break;
            }
            if (node.type == NodeType::html_block || node.type == NodeType::paragraph ||
                node.type == NodeType::table) {
                matched = index + 1;
                break;
            }
        }

        const auto remaining = indentation(line, pos, column);
        if (blank_pending_ && !remaining.blank) {
            if (blank_barrier_ != npos) {
                bool barrier_matched = false;
                for (std::size_t index = 0; index < matched; ++index) {
                    if (open_[index] == blank_barrier_) { barrier_matched = true; break; }
                }
                if (!barrier_matched) {
                    blank_pending_ = false;
                    blank_barrier_ = npos;
                }
            }
            NodeId affected = npos;
            for (auto index = blank_pending_ ? matched : 0; index > 0; --index) {
                const auto id = open_[index - 1];
                if (builder_.get(id).type == NodeType::item) { affected = id; break; }
                if (builder_.get(id).type == NodeType::list) {
                    const auto parent = builder_.get(id).parent;
                    const auto marker = parse_list_marker(line, pos, column, false);
                    if (parent == npos || builder_.get(parent).type != NodeType::item ||
                        (marker.valid && same_list(builder_.get(id), marker))) {
                        affected = builder_.get(id).last_child;
                        break;
                    }
                }
            }
            if (affected != npos) builder_.get(affected).tight = false;
            blank_pending_ = false;
            blank_barrier_ = npos;
        }

        NodeId paragraph = npos;
        if (!open_.empty() && builder_.get(open_.back()).type == NodeType::paragraph) paragraph = open_.back();

        if (matched < open_.size()) {
            const auto rest = indentation(line, pos, column);
            bool sibling_list_marker = false;
            if (matched > 0 && builder_.get(open_[matched - 1]).type == NodeType::list)
                sibling_list_marker = parse_list_marker(line, pos, column, false).valid;
            if (paragraph != npos && !rest.blank && !inline_interrupt(line, pos, column) && !sibling_list_marker) {
                auto& p = builder_.get(paragraph);
                p.literal.push_back('\n');
                const auto content = rest.columns <= 3 ? rest.first : pos;
                p.literal.append(line.substr(content));
                touch_open(info.next);
                return;
            }
            close_to(matched);
            paragraph = npos;
        }

        if (!open_.empty()) {
            auto& tip = builder_.get(open_.back());
            if (tip.type == NodeType::code_block) {
                if (tip.fenced) {
                    if (fence_close(line, pos, column, tip.marker, tip.number)) {
                        touch_open(info.next);
                        close_to(open_.size() - 1);
                        return;
                    }
                    std::string partial;
                    consume_columns(line, pos, column, std::min<std::size_t>(tip.marker_offset, indentation(line, pos, column).columns), &partial);
                    tip.literal += partial;
                    append_line(tip, line, pos);
                    touch_open(info.next);
                    return;
                }
                const auto ind = indentation(line, pos, column);
                if (ind.blank) {
                    if (ind.columns >= 4) {
                        std::string partial;
                        consume_columns(line, pos, column, 4, &partial);
                        tip.literal += partial;
                        append_line(tip, line, pos);
                    } else {
                        tip.literal.push_back('\n');
                    }
                } else if (ind.columns >= 4) {
                    std::string partial;
                    consume_columns(line, pos, column, 4, &partial);
                    tip.literal += partial;
                    append_line(tip, line, pos);
                } else {
                    close_to(open_.size() - 1);
                }
                if (!open_.empty() && builder_.get(open_.back()).type == NodeType::code_block) {
                    touch_open(info.next);
                    return;
                }
            } else if (tip.type == NodeType::html_block) {
                const auto ind = indentation(line, pos, column);
                if ((tip.number == 6 || tip.number == 7) && ind.blank) {
                    close_to(open_.size() - 1);
                    return; // blank line is consumed as a separator
                }
                append_line(tip, line, pos);
                touch_open(info.next);
                if (html_block_ends(static_cast<int>(tip.number), line)) close_to(open_.size() - 1);
                return;
            } else if (tip.type == NodeType::table) {
                const auto ind = indentation(line, pos, column);
                const auto content = line.substr(ind.first);
                const bool reference_definition = !content.empty() && content.front() == '[' &&
                    parse_reference(content).consumed != 0;
                if (!ind.blank && ind.columns <= 3 && !inline_interrupt(line, pos, column) &&
                    !reference_definition) {
                    const auto table = open_.back();
                    append_table_body_row(table, content, info.begin + ind.first, info.next);
                    touch_open(info.next);
                    return;
                }
                close_to(open_.size() - 1);
            }
        }

        if (!open_.empty() && builder_.get(open_.back()).type == NodeType::paragraph) {
            auto& p = builder_.get(open_.back());
            const auto ind = indentation(line, pos, column);
            if (ind.blank) {
                for (auto index = open_.size(); index > 0; --index) {
                    const auto id = open_[index - 1];
                    if (builder_.get(id).type == NodeType::block_quote) { blank_barrier_ = id; break; }
                }
                close_to(open_.size() - 1);
                blank_pending_ = true;
                touch_open(info.next);
                return;
            }
            std::uint32_t level = 0;
            if (options_.extensions.tables &&
                try_open_table(open_.back(), line.substr(ind.first), info.begin + ind.first, info.next)) {
                touch_open(info.next);
                return;
            }
            if (setext_underline(line, pos, column, level)) {
                while (!p.literal.empty() && (p.literal.back() == ' ' || p.literal.back() == '\t')) p.literal.pop_back();
                p.type = NodeType::heading;
                p.number = level;
                p.title.assign(line.substr(ind.first));
                p.source.end = static_cast<std::uint32_t>(info.next);
                close_to(open_.size() - 1);
                touch_open(info.next);
                return;
            }
            if (!inline_interrupt(line, pos, column)) {
                p.literal.push_back('\n');
                p.literal.append(line.substr(ind.columns <= 3 ? ind.first : pos));
                touch_open(info.next);
                return;
            }
            close_to(open_.size() - 1);
        }

        // Repeatedly open blockquote and list-item containers.
        bool opened_empty_item = false;
        for (;;) {
            const auto ind = indentation(line, pos, column);
            if (virtual_spaces + ind.columns <= 3 && !ind.blank && line[ind.first] == '>') {
                close_incompatible_list();
                const auto id = builder_.append(open_.back(), NodeType::block_quote,
                    {static_cast<std::uint32_t>(info.begin + ind.first), static_cast<std::uint32_t>(info.next)});
                if (id == npos || !push_open(id, info.begin + ind.first)) return;
                virtual_spaces = 0;
                pos = ind.first + 1;
                column += ind.columns + 1;
                if (pos < line.size() && line[pos] == ' ') { ++pos; ++column; }
                else if (pos < line.size() && line[pos] == '\t') {
                    const auto width = 4 - (column % 4);
                    ++pos;
                    column += width;
                    virtual_spaces += width - 1;
                }
                continue;
            }

            if (thematic_break(line, pos, column)) break;
            const auto marker = parse_list_marker(line, pos, column, false);
            if (!marker.valid || virtual_spaces + marker.marker_offset > 3) break;

            NodeId list = npos;
            if (builder_.get(open_.back()).type == NodeType::list && same_list(builder_.get(open_.back()), marker)) {
                list = open_.back();
            } else {
                if (builder_.get(open_.back()).type == NodeType::list) close_to(open_.size() - 1);
                list = builder_.append(open_.back(), NodeType::list,
                    {static_cast<std::uint32_t>(info.begin + ind.first), static_cast<std::uint32_t>(info.next)});
                if (list == npos) return;
                auto& list_node = builder_.get(list);
                list_node.list_kind = marker.kind;
                list_node.marker = marker.marker;
                list_node.number = marker.start;
                if (!push_open(list, info.begin + ind.first)) return;
            }
            const auto previous_item = builder_.get(list).last_child;
            if (previous_item != npos && !builder_.get(previous_item).tight) builder_.get(list).tight = false;
            const auto item = builder_.append(list, NodeType::item,
                {static_cast<std::uint32_t>(info.begin + ind.first), static_cast<std::uint32_t>(info.next)});
            if (item == npos) return;
            auto& item_node = builder_.get(item);
            item_node.marker_offset = static_cast<std::uint16_t>(std::min<std::size_t>(marker.marker_offset, 65535));
            item_node.padding = static_cast<std::uint16_t>(std::min<std::size_t>(marker.padding + virtual_spaces, 65535));
            if (!push_open(item, info.begin + ind.first)) return;
            pos = marker.content;
            column = marker.content_column;
            virtual_spaces += marker.residual_spaces;
            if (marker.content_blank) { opened_empty_item = true; break; }
        }

        const auto ind = indentation(line, pos, column);
        if (ind.blank) {
            if (!opened_empty_item) {
                blank_pending_ = true;
                blank_barrier_ = npos;
                for (auto index = open_.size(); index > 0; --index) {
                    const auto id = open_[index - 1];
                    if (builder_.get(id).type == NodeType::block_quote) { blank_barrier_ = id; break; }
                }
            }
            touch_open(info.next);
            return;
        }

        // A list container may remain matched after its previous item failed.
        // Non-list leaf blocks start beside that list, never directly inside it.
        if (builder_.get(open_.back()).type == NodeType::list) close_to(open_.size() - 1);

        const auto source_begin = static_cast<std::uint32_t>(info.begin + ind.first);
        const auto source_end = static_cast<std::uint32_t>(info.next);
        if (thematic_break(line, pos, column)) {
            builder_.append(open_.back(), NodeType::thematic_break, {source_begin, source_end});
            touch_open(info.next);
            return;
        }

        std::uint32_t level = 0;
        std::size_t content_begin = 0, content_end = 0;
        if (atx_heading(line, pos, column, level, content_begin, content_end)) {
            const auto id = builder_.append(open_.back(), NodeType::heading, {source_begin, source_end});
            if (id != npos) {
                auto& node = builder_.get(id);
                node.number = level;
                node.literal.assign(line.substr(content_begin, content_end - content_begin));
            }
            touch_open(info.next);
            return;
        }

        char fence_marker = 0;
        std::size_t fence_count = 0, fence_indent = 0, info_begin = 0;
        if (fence_open(line, pos, column, fence_marker, fence_count, fence_indent, info_begin)) {
            const auto id = builder_.append(open_.back(), NodeType::code_block, {source_begin, source_end});
            if (id == npos) return;
            auto& node = builder_.get(id);
            node.fenced = true;
            node.marker = fence_marker;
            node.number = static_cast<std::uint32_t>(fence_count);
            node.marker_offset = static_cast<std::uint16_t>(fence_indent);
            const auto info_end = trim_line_end(line, line.size());
            if (info_begin < info_end) node.title = unescape_backslashes(line.substr(info_begin, info_end - info_begin));
            push_open(id, info.begin + ind.first);
            touch_open(info.next);
            return;
        }

        const int html_type = html_block_start(line, pos, column, false);
        if (html_type != 0) {
            const auto id = builder_.append(open_.back(), NodeType::html_block, {source_begin, source_end});
            if (id == npos) return;
            auto& node = builder_.get(id);
            node.number = static_cast<std::uint32_t>(html_type);
            append_line(node, line, pos);
            if (!html_block_ends(html_type, line)) push_open(id, info.begin + ind.first);
            touch_open(info.next);
            return;
        }

        if (virtual_spaces + ind.columns >= 4) {
            const auto id = builder_.append(open_.back(), NodeType::code_block,
                {static_cast<std::uint32_t>(info.begin + pos), source_end});
            if (id == npos) return;
            auto& node = builder_.get(id);
            node.fenced = false;
            std::string partial;
            auto needed = std::size_t{4};
            const auto from_virtual = std::min(needed, virtual_spaces);
            virtual_spaces -= from_virtual;
            needed -= from_virtual;
            if (needed != 0) consume_columns(line, pos, column, needed, &partial);
            node.literal.append(virtual_spaces, ' ');
            node.literal += partial;
            append_line(node, line, pos);
            push_open(id, info.begin + pos);
            touch_open(info.next);
            return;
        }

        const auto id = builder_.append(open_.back(), NodeType::paragraph, {source_begin, source_end});
        if (id == npos) return;
        builder_.get(id).literal.assign(line.substr(ind.first));
        push_open(id, info.begin + ind.first);
        touch_open(info.next);
    }

    NodeId append_table_cell(NodeId row, std::string_view raw,
                             std::optional<TableCellSlice> slice,
                             std::size_t source_base, TableAlignment alignment) {
        const auto local_begin = slice ? slice->begin : raw.size();
        const auto local_end = slice ? slice->end : raw.size();
        const auto cell = builder_.append(row, NodeType::table_cell,
            {static_cast<std::uint32_t>(source_base + local_begin),
             static_cast<std::uint32_t>(source_base + local_end)});
        if (cell != npos) {
            builder_.get(cell).alignment = alignment;
            if (slice) builder_.get(cell).literal = table_cell_text(raw, *slice);
        }
        return cell;
    }

    bool try_open_table(NodeId paragraph, std::string_view delimiter,
                        std::size_t delimiter_begin, std::size_t delimiter_end) {
        auto& alignments = alignments_;
        if (!parse_table_delimiter(delimiter, alignments, row_scratch_)) return false;

        const std::string_view full_text(builder_.get(paragraph).literal);
        const auto last_newline = full_text.rfind('\n');
        const auto header_offset = last_newline == std::string::npos ? 0 : last_newline + 1;
        auto header_text = full_text.substr(header_offset);
        const auto& header = split_table_row(header_text, row_scratch_);
        if (!header.has_pipe || header.cells.size() != alignments.size()) return false;

        // A failed candidate must not copy the growing paragraph. Once a table
        // is confirmed, retain only its header while the arena is modified.
        const std::string saved_header(header_text);
        header_text = saved_header;

        auto header_source = builder_.get(paragraph).source;
        NodeId table = paragraph;
        if (last_newline != std::string::npos) {
            const auto current_line_break = delimiter_begin == 0 ? std::string::npos :
                source_.rfind('\n', delimiter_begin - 1);
            const auto header_line_end = current_line_break == std::string::npos ? delimiter_begin : current_line_break + 1;
            const auto prior_break = header_line_end <= 1 ? std::string::npos :
                source_.rfind('\n', header_line_end - 2);
            const auto physical_begin = prior_break == std::string::npos ? 0 : prior_break + 1;
            const auto physical_end = current_line_break == std::string::npos ? delimiter_begin : current_line_break;
            const auto physical_line = std::string_view(source_).substr(physical_begin, physical_end - physical_begin);
            const auto found = physical_line.rfind(header_text);
            header_source.begin = static_cast<std::uint32_t>(physical_begin +
                (found == std::string_view::npos ? 0 : found));
            header_source.end = static_cast<std::uint32_t>(header_line_end);

            builder_.get(paragraph).literal.resize(last_newline);
            builder_.get(paragraph).source.end = header_source.begin;
            table = builder_.insert_after(paragraph, NodeType::table);
            if (table == npos) return true;
            builder_.get(table).source = header_source;
            open_.back() = table;
        }
        auto& table_node = builder_.get(table);
        table_node.type = NodeType::table;
        std::string{}.swap(table_node.literal);
        table_node.number = static_cast<std::uint32_t>(alignments.size());
        table_node.source.end = static_cast<std::uint32_t>(delimiter_end);

        const auto head = builder_.append(table, NodeType::table_head, header_source);
        if (head == npos) return true;
        const auto header_row = builder_.append(head, NodeType::table_row, header_source);
        if (header_row == npos) return true;
        for (std::size_t column = 0; column < alignments.size(); ++column) {
            if (append_table_cell(header_row, header_text, header.cells[column],
                                  header_source.begin, alignments[column]) == npos) return true;
        }
        builder_.append(table, NodeType::table_body,
            {static_cast<std::uint32_t>(delimiter_begin), static_cast<std::uint32_t>(delimiter_end)});
        return true;
    }

    void append_table_body_row(NodeId table, std::string_view raw,
                               std::size_t source_begin, std::size_t source_end) {
        const auto head = builder_.get(table).first_child;
        if (head == npos) return;
        const auto body = builder_.get(head).next;
        const auto header_row = builder_.get(head).first_child;
        if (body == npos || header_row == npos) return;

        const auto columns = static_cast<std::size_t>(builder_.get(table).number);
        const auto& parsed = split_table_row(raw, row_scratch_, columns);
        const bool first_body_row = builder_.get(body).first_child == npos;
        const auto row = builder_.append(body, NodeType::table_row,
            {static_cast<std::uint32_t>(source_begin), static_cast<std::uint32_t>(source_end)});
        if (row == npos) return;
        auto header_cell = builder_.get(header_row).first_child;
        for (std::size_t column = 0; column < columns; ++column) {
            const auto alignment = header_cell == npos ? TableAlignment::none : builder_.get(header_cell).alignment;
            const auto slice = column < parsed.cells.size()
                ? std::optional<TableCellSlice>(parsed.cells[column]) : std::nullopt;
            if (append_table_cell(row, raw, slice, source_begin, alignment) == npos) return;
            if (header_cell != npos) header_cell = builder_.get(header_cell).next;
        }
        if (first_body_row) builder_.get(body).source.begin = static_cast<std::uint32_t>(source_begin);
        builder_.get(body).source.end = static_cast<std::uint32_t>(source_end);
        builder_.get(table).source.end = static_cast<std::uint32_t>(source_end);
    }

    bool push_open(NodeId id, std::size_t offset) {
        if (options_.max_nesting != 0 && open_.size() >= options_.max_nesting) {
            error_ = {ErrorCode::nesting_limit, offset, "nesting limit exceeded"};
            return false;
        }
        open_.push_back(id);
        return true;
    }

    void close_incompatible_list() {
        // Lists are left open while matching continuation lines. A quote at the
        // same level cannot become a direct child of a list; it starts after it.
        if (builder_.get(open_.back()).type == NodeType::list) close_to(open_.size() - 1);
    }

    void finalize_lists() {
        for (NodeId id = 0; id < builder_.nodes().size(); ++id) {
            auto& list = builder_.get(id);
            if (list.type != NodeType::list) continue;
            list.tight = true;
            for (auto item_id = list.first_child; item_id != npos; item_id = builder_.get(item_id).next) {
                const auto& item = builder_.get(item_id);
                if (!item.tight && (item.next != npos ||
                    (item.first_child != npos && builder_.get(item.first_child).next != npos))) {
                    list.tight = false;
                }
                for (auto child = item.first_child; child != npos;) {
                    const auto next = builder_.get(child).next;
                    if (next != npos && blank_between(source_, builder_.get(child).source.end,
                                                      builder_.get(next).source.begin)) {
                        list.tight = false;
                    }
                    child = next;
                }
                if (item.next != npos && item.last_child != npos &&
                    blank_between(source_, builder_.get(item.last_child).source.end, item.source.end)) {
                    list.tight = false;
                }
            }
        }
    }

    void extract_references() {
        for (NodeId id = 1; id < builder_.nodes().size(); ++id) {
            auto& node = builder_.get(id);
            if ((node.type != NodeType::paragraph && node.type != NodeType::heading) || node.parent == npos) continue;
            const bool was_heading = node.type == NodeType::heading;
            std::string underline = was_heading ? node.title : std::string{};
            std::size_t consumed = 0;
            for (;;) {
                auto parsed = parse_reference(std::string_view(node.literal).substr(consumed));
                if (parsed.consumed == 0 || parsed.label.empty()) break;
                references_.try_emplace(std::move(parsed.label), std::move(parsed.reference));
                consumed += parsed.consumed;
            }
            if (consumed == 0) continue;
            if (consumed >= node.literal.size()) {
                if (was_heading) {
                    node.type = NodeType::paragraph;
                    node.number = 0;
                    node.title.clear();
                    node.literal = std::move(underline);
                    while (!node.literal.empty() && ascii_space(node.literal.back())) node.literal.pop_back();
                    const auto next = node.next;
                    if (next != npos && builder_.get(next).type == NodeType::paragraph &&
                        !blank_between(source_, node.source.end, builder_.get(next).source.begin)) {
                        node.literal.push_back('\n');
                        node.literal += builder_.get(next).literal;
                        node.source.end = builder_.get(next).source.end;
                        builder_.unlink(next);
                    }
                } else {
                    builder_.unlink(id);
                }
            } else {
                node.literal.erase(0, consumed);
                while (!node.literal.empty() && node.literal.front() == '\n') node.literal.erase(node.literal.begin());
                if (was_heading) node.title.clear();
            }
        }
    }

    void finalize_task_items() {
        for (NodeId id = 1; id < builder_.nodes().size(); ++id) {
            if (builder_.get(id).type != NodeType::item) continue;
            const auto paragraph = builder_.get(id).first_child;
            if (paragraph == npos || builder_.get(paragraph).type != NodeType::paragraph) continue;

            auto& literal = builder_.get(paragraph).literal;
            std::size_t marker = 0;
            while (marker < literal.size() && literal[marker] == ' ') ++marker;
            if (marker + 2 >= literal.size() || literal[marker] != '[' || literal[marker + 2] != ']') continue;
            const char state = literal[marker + 1];
            if (state != 'x' && state != 'X' && !task_whitespace(state)) continue;
            auto consumed = marker + 3;
            if (consumed < literal.size()) {
                if (!task_whitespace(literal[consumed])) continue;
                ++consumed;
            }

            builder_.get(id).task = true;
            builder_.get(id).checked = state == 'x' || state == 'X';
            literal.erase(0, consumed);
            auto& source = builder_.get(paragraph).source;
            source.begin = static_cast<std::uint32_t>(std::min<std::size_t>(
                static_cast<std::size_t>(source.begin) + consumed, source.end));
        }
    }

    std::string& source_;
    Builder& builder_;
    const ParseOptions& options_;
    ParseError& error_;
    std::vector<NodeId> open_;
    bool blank_pending_ = false;
    NodeId blank_barrier_ = npos;
    ReferenceMap references_;
    TableRowParse row_scratch_;
    std::vector<TableAlignment> alignments_;
};

} // namespace

bool valid_utf8(std::string_view input, std::size_t& bad_offset) noexcept {
    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        std::size_t width = 0;
        std::uint32_t value = 0;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { width = 2; value = c & 0x1FU; }
        else if (c >= 0xE0 && c <= 0xEF) { width = 3; value = c & 0x0FU; }
        else if (c >= 0xF0 && c <= 0xF4) { width = 4; value = c & 0x07U; }
        else { bad_offset = i; return false; }
        if (i + width > input.size()) { bad_offset = i; return false; }
        for (std::size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(input[i + j]);
            if ((continuation & 0xC0U) != 0x80U) { bad_offset = i + j; return false; }
            value = (value << 6U) | (continuation & 0x3FU);
        }
        if ((width == 3 && value < 0x800U) || (width == 4 && value < 0x10000U) ||
            (value >= 0xD800U && value <= 0xDFFFU) || value > 0x10FFFFU) {
            bad_offset = i;
            return false;
        }
        i += width;
    }
    return true;
}

void normalize_input(std::string_view input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        const auto special = input.find_first_of(std::string_view("\r\0", 2), i);
        if (special == std::string_view::npos) { output.append(input.substr(i)); break; }
        output.append(input.substr(i, special - i));
        i = special;
        const char c = input[i];
        if (c == '\r') {
            if (i + 1 < input.size() && input[i + 1] == '\n') ++i;
            output.push_back('\n');
        } else if (c == '\0') {
            output.append("\xEF\xBF\xBD");
        }
        ++i;
    }
}

} // namespace chmd::detail

namespace chmd {

ParseResult Parser::parse(std::string_view markdown) const {
    ParseResult result;
    try {
        if ((options_.max_input_bytes != 0 && markdown.size() > options_.max_input_bytes) ||
            markdown.size() > std::numeric_limits<std::uint32_t>::max()) {
            result.error = {ErrorCode::input_too_large, 0, "input exceeds configured or addressable size"};
            return result;
        }
        if (options_.validate_utf8) {
            std::size_t offset = 0;
            if (!detail::valid_utf8(markdown, offset)) {
                result.error = {ErrorCode::invalid_utf8, offset, "invalid UTF-8 input"};
                return result;
            }
        }
        detail::normalize_input(markdown, detail_access::source(result.document));
        const auto& normalized = detail_access::source(result.document);
        if (normalized.size() > std::numeric_limits<std::uint32_t>::max()) {
            result.error = {ErrorCode::input_too_large, 0, "normalized input exceeds addressable size"};
            return result;
        }
        detail::Builder builder(result.document, options_, result.error);
        auto references = detail::BlockParser(result.document, builder, options_, result.error).run();
        if (!result.error) {
            builder.compact(1, 0);
            detail::parse_inlines(builder, references, options_);
        }
        if (!result.error && options_.max_nesting != 0) {
            detail::walk(result.document, [&](NodeId id, std::size_t depth) {
                if (depth >= options_.max_nesting && !result.error)
                    result.error = {ErrorCode::nesting_limit, result.document.node(id).source.begin,
                                    "nesting limit exceeded"};
                return !result.error;
            }, [](NodeId, std::size_t) {});
        }
        auto& nodes = detail_access::nodes(result.document);
        // Retire a large construction arena when only a small result remains
        // (e.g. reference definitions or unmatched punctuation). Normal trees
        // keep their spare capacity and avoid an extra move/allocation.
        if (!result.error && nodes.capacity() > 64 && nodes.size() < nodes.capacity() / 4)
            nodes.shrink_to_fit();
    } catch (const std::bad_alloc&) {
        result.error = {ErrorCode::out_of_memory, 0, "memory allocation failed"};
    }
    return result;
}

ParseError Parser::parse_events(std::string_view markdown, EventHandler& handler) const {
    auto result = parse(markdown);
    if (!result) return result.error;
    walk_events(result.document, handler);
    return {};
}

} // namespace chmd
