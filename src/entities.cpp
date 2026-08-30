#include "internal.hpp"

#include <algorithm>

namespace chmd::detail {
namespace {

#include "generated_tables.inc"

template <std::size_t N>
bool in_ranges(std::uint32_t cp, const Range (&table)[N]) noexcept {
    const auto* found = std::lower_bound(std::begin(table), std::end(table), cp,
        [](const Range& range, std::uint32_t value) { return range.last < value; });
    return found != std::end(table) && cp >= found->first;
}

bool ascii_punctuation(std::uint32_t cp) noexcept {
    return (cp >= 0x21U && cp <= 0x2FU) || (cp >= 0x3AU && cp <= 0x40U) ||
           (cp >= 0x5BU && cp <= 0x60U) || (cp >= 0x7BU && cp <= 0x7EU);
}

std::uint32_t parse_numeric(std::string_view digits, unsigned base, bool& valid) {
    valid = !digits.empty();
    std::uint32_t value = 0;
    for (const char c : digits) {
        unsigned digit = 99;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        if (digit >= base || value > (0x10FFFFU - digit) / base) { valid = false; return 0; }
        value = value * base + digit;
    }
    if (value == 0 || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) return 0xFFFDU;
    return value;
}

} // namespace

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7FU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFU) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0xFFFFU) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::uint32_t decode_utf8_at(std::string_view text, std::size_t offset, std::size_t* width) noexcept {
    if (offset >= text.size()) { if (width) *width = 0; return 0; }
    const auto c = static_cast<unsigned char>(text[offset]);
    std::size_t count = 1;
    std::uint32_t value = c;
    if ((c & 0xE0U) == 0xC0U) { count = 2; value = c & 0x1FU; }
    else if ((c & 0xF0U) == 0xE0U) { count = 3; value = c & 0x0FU; }
    else if ((c & 0xF8U) == 0xF0U) { count = 4; value = c & 0x07U; }
    if (offset + count > text.size()) { if (width) *width = 1; return c; }
    for (std::size_t i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0U) != 0x80U) { if (width) *width = 1; return c; }
        value = (value << 6U) | (next & 0x3FU);
    }
    if (width) *width = count;
    return value;
}

std::uint32_t decode_utf8_before(std::string_view text, std::size_t offset) noexcept {
    if (offset == 0 || offset > text.size()) return 0;
    auto begin = offset - 1;
    std::size_t continuation = 0;
    while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U && continuation < 3) {
        --begin;
        ++continuation;
    }
    return decode_utf8_at(text, begin);
}

bool is_unicode_whitespace(std::uint32_t cp) noexcept {
    return in_ranges(cp, whitespace_ranges);
}

bool is_unicode_punctuation(std::uint32_t cp) noexcept {
    return ascii_punctuation(cp) || in_ranges(cp, punctuation_ranges);
}

std::string unescape_entities(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (input[i] != '&') {
            out.push_back(input[i++]);
            continue;
        }
        const auto semicolon = input.find(';', i + 1);
        if (semicolon == std::string_view::npos || semicolon - i > 33) {
            out.push_back(input[i++]);
            continue;
        }
        const auto body = input.substr(i + 1, semicolon - i - 1);
        if (body.starts_with('#')) {
            const bool hex = body.size() >= 2 && (body[1] == 'x' || body[1] == 'X');
            const auto digits = body.substr(hex ? 2 : 1);
            if ((!hex && digits.size() > 7) || (hex && digits.size() > 6)) {
                out.push_back(input[i++]);
                continue;
            }
            bool valid = false;
            const auto value = parse_numeric(digits, hex ? 16U : 10U, valid);
            if (!valid) { out.push_back(input[i++]); continue; }
            append_utf8(out, value);
            i = semicolon + 1;
            continue;
        }
        const auto* entity = std::lower_bound(std::begin(html_entities), std::end(html_entities), body,
            [](const Entity& item, std::string_view name) { return item.name < name; });
        if (entity == std::end(html_entities) || entity->name != body) {
            out.push_back(input[i++]);
            continue;
        }
        append_utf8(out, entity->a);
        if (entity->count == 2) append_utf8(out, entity->b);
        i = semicolon + 1;
    }
    return out;
}

std::string normalize_reference(std::string_view label) {
    auto decoded = unescape_entities(label);
    std::string out;
    out.reserve(decoded.size());
    bool pending_space = false;
    for (std::size_t i = 0; i < decoded.size();) {
        std::size_t width = 1;
        const auto cp = decode_utf8_at(decoded, i, &width);
        if (is_unicode_whitespace(cp)) {
            if (!out.empty()) pending_space = true;
            i += width;
            continue;
        }
        if (pending_space) { out.push_back(' '); pending_space = false; }
        const auto* fold = std::lower_bound(std::begin(case_folds), std::end(case_folds), cp,
            [](const Fold& item, std::uint32_t value) { return item.source < value; });
        if (fold != std::end(case_folds) && fold->source == cp) {
            append_utf8(out, fold->a);
            if (fold->count > 1) append_utf8(out, fold->b);
            if (fold->count > 2) append_utf8(out, fold->c);
        } else {
            out.append(decoded.substr(i, width));
        }
        i += width;
    }
    return out;
}

std::string clean_url(std::string_view input) { return unescape_entities(input); }
std::string clean_title(std::string_view input) { return unescape_entities(input); }

} // namespace chmd::detail

