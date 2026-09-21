#include "internal.hpp"

#include <algorithm>

namespace chmd::detail {
namespace {

#include "generated_tables.inc"

constexpr std::size_t entity_count = sizeof(html_entities) / sizeof(html_entities[0]);

// Looking a name up in 2,125 entries costs about eleven string comparisons,
// each ending in a memcmp call. Packing the first four bytes into an integer
// turns the search into plain integer compares and leaves one confirmation
// step for the few entries that share a prefix.
constexpr std::uint32_t pack_name_prefix(std::string_view name) noexcept {
    std::uint32_t key = 0;
    for (std::size_t i = 0; i < 4; ++i)
        key = (key << 8U) | (i < name.size()
            ? static_cast<std::uint32_t>(static_cast<unsigned char>(name[i])) : 0U);
    return key;
}

// Packing is order-preserving here: entity names are ASCII without NUL, so
// zero padding sorts a shorter name before any longer one it prefixes.
constexpr auto entity_prefixes = [] {
    std::array<std::uint32_t, entity_count> keys{};
    for (std::size_t i = 0; i < entity_count; ++i) keys[i] = pack_name_prefix(html_entities[i].name);
    return keys;
}();

const Entity* find_entity(std::string_view name) noexcept {
    const auto key = pack_name_prefix(name);
    const auto* first = std::lower_bound(std::begin(entity_prefixes), std::end(entity_prefixes), key);
    auto index = static_cast<std::size_t>(first - std::begin(entity_prefixes));
    while (index < entity_count && entity_prefixes[index] == key) {
        if (html_entities[index].name == name) return &html_entities[index];
        ++index;
    }
    return nullptr;
}

template <std::size_t N>
bool in_ranges(std::uint32_t cp, const Range (&table)[N]) noexcept {
    const auto* found = std::lower_bound(std::begin(table), std::end(table), cp,
        [](const Range& range, std::uint32_t value) { return range.last < value; });
    return found != std::end(table) && cp >= found->first;
}

std::uint32_t parse_numeric(std::string_view digits, unsigned base, bool& valid) {
    valid = !digits.empty();
    std::uint32_t value = 0;
    for (const char c : digits) {
        unsigned digit = 99;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        // Syntax limits this to seven decimal or six hexadecimal digits, both
        // of which fit uint32_t. Out-of-Unicode-range values become U+FFFD.
        if (digit >= base) { valid = false; return 0; }
        value = value * base + digit;
    }
    if (value == 0 || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) return 0xFFFDU;
    return value;
}

} // namespace

namespace {
char* write_utf8(char* out, std::uint32_t cp) noexcept {
    if (cp <= 0x7FU) {
        *out++ = static_cast<char>(cp);
    } else if (cp <= 0x7FFU) {
        *out++ = static_cast<char>(0xC0U | (cp >> 6U));
        *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
    } else if (cp <= 0xFFFFU) {
        *out++ = static_cast<char>(0xE0U | (cp >> 12U));
        *out++ = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
        *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
    } else {
        *out++ = static_cast<char>(0xF0U | (cp >> 18U));
        *out++ = static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU));
        *out++ = static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU));
        *out++ = static_cast<char>(0x80U | (cp & 0x3FU));
    }
    return out;
}
} // namespace

void append_utf8(std::string& out, std::uint32_t cp) {
    char buffer[4];
    out.append(buffer, static_cast<std::size_t>(write_utf8(buffer, cp) - buffer));
}

std::uint32_t in_unicode_whitespace_ranges(std::uint32_t cp) noexcept {
    return in_ranges(cp, whitespace_ranges) ? 1U : 0U;
}

std::uint32_t in_unicode_punctuation_ranges(std::uint32_t cp) noexcept {
    return in_ranges(cp, punctuation_ranges) ? 1U : 0U;
}

std::size_t append_entity(std::string_view input, char* out, std::size_t& written) {
    written = 0;
    if (input.size() < 3 || input.front() != '&') return 0;
    const auto relative = input.substr(1, 33).find(';');
    if (relative == std::string_view::npos) return 0;
    const auto body = input.substr(1, relative);
    std::uint32_t value = 0;
    unsigned count = 1;
    std::uint32_t second = 0;
    if (body.starts_with('#')) {
        const bool hex = body.size() >= 2 && (body[1] == 'x' || body[1] == 'X');
        const auto digits = body.substr(hex ? 2 : 1);
        if (digits.size() > (hex ? 6U : 7U)) return 0;
        bool valid = false;
        value = parse_numeric(digits, hex ? 16U : 10U, valid);
        if (!valid) return 0;
    } else {
        const auto* entity = find_entity(body);
        if (entity == nullptr) return 0;
        value = entity->a;
        second = entity->b;
        count = entity->count;
    }
    auto* cursor = write_utf8(out, value);
    if (count == 2) cursor = write_utf8(cursor, second);
    written = static_cast<std::size_t>(cursor - out);
    return relative + 2;
}

std::size_t append_entity(std::string_view input, std::string& output) {
    // The longest entity expands to two code points, so eight bytes always fit.
    char buffer[8];
    std::size_t written = 0;
    const auto consumed = append_entity(input, buffer, written);
    if (consumed != 0) output.append(buffer, written);
    return consumed;
}

std::string unescape_markdown(std::string_view input) {
    constexpr std::string_view specials = "\\&";
    auto special = input.find_first_of(specials);
    if (special == std::string_view::npos) return std::string(input);
    std::string out;
    out.reserve(input.size());
    std::size_t begin = 0;
    do {
        out.append(input.substr(begin, special - begin));
        begin = special;
        if (input[begin] == '\\' && begin + 1 < input.size() &&
            ascii_punctuation(static_cast<unsigned char>(input[begin + 1]))) {
            out.push_back(input[begin + 1]);
            begin += 2;
        } else if (const auto consumed = append_entity(input.substr(begin), out)) {
            begin += consumed;
        } else {
            out.push_back(input[begin++]);
        }
        special = input.find_first_of(specials, begin);
    } while (special != std::string_view::npos);
    out.append(input.substr(begin));
    return out;
}

LinkLabel scan_link_label(std::string_view input, std::size_t begin) {
    if (begin >= input.size() || input[begin] != '[') return {};
    std::size_t characters = 0;
    bool escaped = false;
    bool nonspace = false;
    for (auto pos = begin + 1; pos < input.size();) {
        const auto c = input[pos];
        if (!escaped && c == ']')
            return nonspace ? LinkLabel{input.substr(begin + 1, pos - begin - 1), pos + 1} : LinkLabel{};
        if (!escaped && c == '[') return {};
        if (++characters > 999) return {};
        nonspace = nonspace || (c != ' ' && c != '\t' && c != '\n' && c != '\r');
        escaped = !escaped && c == '\\';
        std::size_t width = 1;
        if (static_cast<unsigned char>(c) >= 0x80U) decode_utf8_at(input, pos, &width);
        pos += width;
    }
    return {};
}

void normalize_reference_into(std::string_view label, std::string& out) {
    out.clear();
    out.reserve(label.size());
    bool pending_space = false;
    for (std::size_t i = 0; i < label.size();) {
        std::size_t width = 1;
        const auto cp = decode_utf8_at(label, i, &width);
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
            if (!out.empty()) pending_space = true;
            i += width;
            continue;
        }
        if (pending_space) { out.push_back(' '); pending_space = false; }
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp));
            i += width;
            continue;
        }
        const auto* fold = std::lower_bound(std::begin(case_folds), std::end(case_folds), cp,
            [](const Fold& item, std::uint32_t value) { return item.source < value; });
        if (fold != std::end(case_folds) && fold->source == cp) {
            append_utf8(out, fold->a);
            if (fold->count > 1) append_utf8(out, fold->b);
            if (fold->count > 2) append_utf8(out, fold->c);
        } else {
            out.append(label.substr(i, width));
        }
        i += width;
    }
}

std::string normalize_reference(std::string_view label) {
    std::string out;
    normalize_reference_into(label, out);
    return out;
}

} // namespace chmd::detail
