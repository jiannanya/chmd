#include <chmd/chmd.hpp>
#include <algorithm>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>
#include <string_view>

// Requested heap bytes: excludes allocator headers, stack and caller input.
namespace memory {
struct alignas(std::max_align_t) alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) Header { std::size_t bytes; };
std::size_t live = 0, peak = 0, allocations = 0;
void* allocate(std::size_t bytes) {
    if (bytes > static_cast<std::size_t>(-1) - sizeof(Header)) throw std::bad_alloc();
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + bytes));
    if (!header) throw std::bad_alloc();
    header->bytes = bytes;
    live += bytes;
    peak = std::max(peak, live);
    ++allocations;
    return header + 1;
}
void release(void* ptr) noexcept {
    if (!ptr) return;
    auto* header = static_cast<Header*>(ptr) - 1;
    live -= header->bytes;
    std::free(header);
}
}
void* operator new(std::size_t n) { return memory::allocate(n); }
void* operator new[](std::size_t n) { return memory::allocate(n); }
void operator delete(void* p) noexcept { memory::release(p); }
void operator delete[](void* p) noexcept { memory::release(p); }
void operator delete(void* p, std::size_t) noexcept { memory::release(p); }
void operator delete[](void* p, std::size_t) noexcept { memory::release(p); }

namespace {
using Clock = std::chrono::steady_clock;
double seconds(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}
std::string repeated(std::string_view unit, std::size_t bytes) {
    std::string out;
    out.reserve(bytes + unit.size());
    while (out.size() < bytes) out += unit;
    return out;
}
std::string repeat_n(std::string_view unit, std::size_t count) {
    std::string out;
    out.reserve(unit.size() * count);
    for (std::size_t i = 0; i < count; ++i) out += unit;
    return out;
}
void measure(std::string_view name, const std::string& input, int rounds, chmd::ParseOptions options = {}) {
    { auto warm = chmd::Parser(options).parse(input); if (!warm) std::exit(1); }
    std::size_t checksum = 0;
    const auto start = Clock::now();
    for (int round = 0; round < rounds; ++round) {
        auto parsed = chmd::Parser(options).parse(input);
        if (!parsed) { std::cerr << parsed.error.message << '\n'; std::exit(1); }
        checksum += parsed.document.size();
    }
    const double parse_seconds = seconds(start);
    const auto before = memory::live;
    const auto allocations_before = memory::allocations;
    memory::peak = before;
    auto parsed = chmd::Parser(options).parse(input);
    const auto retained = memory::live - before;
    const auto peak = memory::peak - before;
    const auto allocations = memory::allocations - allocations_before;
    const auto render_start = Clock::now();
    for (int round = 0; round < rounds; ++round)
        checksum += chmd::render_html(parsed.document).size();
    const double render_seconds = seconds(render_start);
    const auto ast_start = Clock::now();
    for (int round = 0; round < rounds; ++round)
        checksum += chmd::render_ast(parsed.document, false).size();
    const double ast_seconds = seconds(ast_start);
    const auto events_start = Clock::now();
    for (int round = 0; round < rounds; ++round)
        checksum += chmd::render_events(parsed.document).size();
    const double events_seconds = seconds(events_start);
    std::cout << name << ',' << input.size() << ',' << rounds << ','
              << std::fixed << std::setprecision(6) << parse_seconds << ','
              << (input.size() / 1048576.0 * rounds / parse_seconds) << ','
              << render_seconds << ',' << parsed.document.size() << ','
              << retained << ',' << peak << ',' << allocations << ',' << checksum << ','
              << ast_seconds << ',' << events_seconds << std::endl;
}
}
int main(int argc, char** argv) {
    int rounds = 10;
    if (argc > 2) return 2;
    if (argc == 2) {
        const std::string_view value(argv[1]);
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), rounds);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || rounds <= 0) return 2;
    }
    constexpr std::size_t mib = 1024 * 1024;
    std::cout << "case,input_bytes,rounds,parse_seconds,parse_mib_s,html_seconds,nodes,retained_bytes,peak_bytes,allocations,checksum,ast_seconds,events_seconds\n";
    measure("mixed", repeated("## Heading\n\n- [link](https://example.com) and **strong** text\n- `code` &amp; text\n\n", mib), rounds);
    measure("plain", repeated("Ordinary prose with no markup and a short line.\n", mib), rounds);
    measure("blank_lines", std::string(mib, '\n'), rounds);
    measure("tables", repeated("| a | b |\n| :- | -: |\n| **c** | `d` |\n\n", mib), rounds);
    measure("references", repeated("[ref]: /url \"title\"\n\n[ref] **text**\n\n", mib), rounds);
    measure("html_comment", "<!--\n" + repeated("comment text\n", mib) + "-->\n", rounds);
    measure("ampersands", std::string(65536, '&'), rounds);
    measure("open_angles", "x " + std::string(65536, '<'), rounds);
    measure("unmatched_brackets", "x " + std::string(32768, '[') + std::string(32768, ']'), rounds);
    measure("unmatched_strikes", repeated("a~ ", 65536), rounds);
    measure("failed_tables", repeated("| - | - |\n| - |\n", mib / 2), rounds);
    measure("reference_definitions", repeated("[ref]: /url\n\n", mib), rounds);
    measure("text_fragments", repeated("] text ", 65536), rounds);
    measure("large_table_header", "| " + std::string(mib, 'a') + " |\n| - |\n", rounds);
    std::string unique_references;
    for (int i = 0; i < 10000; ++i)
        unique_references += "[ref" + std::to_string(i) + "]: /url\n\n";
    measure("unique_references", unique_references, rounds);
    measure("entities", repeated("&amp; &lt; &#169; &NotEqualTilde; ", mib), rounds);
    measure("plain_long", std::string(mib, 'a'), rounds);
    measure("code_block", "```text\n" + repeated("plain code content ", mib) + "\n```\n", rounds);
    measure("excess_table_cells", "| a | b |\n| - | - |\n| c | d |" + repeated(" tail |", mib) + "\n", rounds);
    measure("unicode", repeated("\xE4\xB8\xAD\xE6\x96\x87\xE6\xAE\xB5\xE8\x90\xBD\xE4\xB8\x8E\x20\x55\x6E\x69\x63\x6F\x64\x65\x20\xE7\xAC\xA6\xE5\x8F\xB7\x20\xF0\x9F\x98\x80\xE3\x80\x82", mib), rounds);
    chmd::ParseOptions unlimited;
    unlimited.max_nesting = 0;
    measure("nested_images", repeated("![", 24000) + "x" + repeated("](u)", 48000), rounds, unlimited);
    measure("nested_emphasis", repeated("*a ", 36000) + "x" + repeated(" b*", 36000), rounds, unlimited);

    // --- Extended coverage: structure, tables, inline features, HTML blocks,
    // references, rendering, UTF-8 and list-specific workloads. ---

    // Structure (width/depth).
    measure("deep_blockquote", repeat_n("> ", 500) + "deep\n", rounds);
    measure("deep_list", repeat_n("- ", 400) + "deep\n", rounds);
    {
        std::string wide_list;
        for (int i = 0; i < 10000; ++i) { wide_list += "- item "; wide_list += std::to_string(i); wide_list += "\n"; }
        measure("wide_list", wide_list, rounds);
    }
    measure("many_paragraphs", repeat_n("Paragraph text here for benchmarking.\n\n", 10000), rounds);
    measure("sparse_inline_tail", std::string(mib / 4, 'a') + " *tail emphasis* \n", rounds);

    // Tables.
    {
        std::string wide_table = "|";
        std::string separator = "|";
        for (int c = 0; c < 200; ++c) { wide_table += " c" + std::to_string(c) + " |"; separator += " - |"; }
        wide_table += "\n" + separator + "\n";
        for (int r = 0; r < 50; ++r) {
            wide_table += "|";
            for (int c = 0; c < 200; ++c) wide_table += " v |";
            wide_table += "\n";
        }
        measure("wide_table", wide_table, rounds);
    }
    measure("many_small_tables", repeat_n("| a | b |\n| - | - |\n| c | d |\n\n", 500), rounds);

    // Inline-heavy.
    measure("autolinks", repeated("<https://example.com/a/b/c> ", mib), rounds);
    measure("raw_html_tags", repeated("<span class=\"x\">text</span> ", mib), rounds);
    measure("backslash_escapes", repeated("\\*\\_\\`\\[\\]\\(\\)\\! ", mib), rounds);
    measure("long_link", "[text](<" + std::string(65536, 'a') + "> \"" + std::string(1024, 't') + "\")\n", rounds);
    measure("many_images", repeated("![alt text here](https://example.com/img.png) ", mib), rounds);
    measure("hard_breaks", repeated("line ends with break  \n", mib), rounds);
    measure("cjk_emphasis", repeated("\xE4\xB8\xAD\xE6\x96\x87**\xE5\x8A\xA0\xE7\xB2\x97**\xE4\xB8\x8E*\xE5\xBC\xBA\xE8\xB0\x83* ", mib), rounds);
    measure("nested_emphasis_valid", repeat_n("*", 300) + "word" + repeat_n("*", 300) + "\n", rounds);

    // Realistic mixed workload.
    {
        const std::string readme_unit =
            "# Project Title\n\n"
            "This project does a thing. See the [documentation](https://example.com/docs) for details.\n\n"
            "## Features\n\n"
            "- Fast\n- Small\n- Portable\n\n"
            "## Example\n\n"
            "```cpp\nint main() { return 0; }\n```\n\n"
            "| Option | Default |\n| - | - |\n| `verbose` | false |\n\n";
        measure("readme_mixed", repeat_n(readme_unit, 300), rounds);
    }

    // HTML blocks.
    measure("html_block_pre", "<pre>\n" + repeated("html body content line\n", mib) + "</pre>\n", rounds);
    measure("html_block_div", "<div>\n" + repeated("<p>content</p>\n", mib) + "</div>\n", rounds);

    // References.
    measure("reference_reuse", "[r0]: /a\n[r1]: /b\n[r2]: /c\n[r3]: /d\n[r4]: /e\n\n" +
        repeated("[r0] [r1] [r2] [r3] [r4] ", mib), rounds);
    measure("forward_references", repeated("[r0] [r1] [r2] ", mib / 2) +
        "\n[r0]: /a\n[r1]: /b\n[r2]: /c\n", rounds);

    // Rendering / traversal.
    {
        const std::string attributes_unit =
            "1. ordered item\n2. another\n\n"
            "- [x] done\n- [ ] todo\n\n"
            "| a | b |\n| :- | -: |\n| x | y |\n\n";
        measure("attributes_heavy", repeat_n(attributes_unit, 3000), rounds);
    }
    measure("tiny_doc_overhead", "# Hi\n\nSmall paragraph with *em* and a [link](/u).\n", rounds * 500);

    // UTF-8 / encoding.
    measure("astral_heavy", repeated("\xF0\x9F\x98\x80\xF0\x9F\x8E\x89\xF0\x9F\x9A\x80\xF0\x9F\x8C\x9F ", mib), rounds);
    {
        chmd::ParseOptions validate_opts;
        validate_opts.validate_utf8 = true;
        measure("utf8_validation",
                repeated("\xE4\xB8\xAD\xE6\x96\x87\xE6\xAE\xB5\xE8\x90\xBD\xE4\xB8\x8E\x20"
                         "\x55\x6E\x69\x63\x6F\x64\x65\x20\xE7\xAC\xA6\xE5\x8F\xB7\x20\xF0\x9F\x98\x80\xE3\x80\x82", mib),
                rounds, validate_opts);
    }

    // Lists.
    {
        std::string ordered_big = "999999999. start\n";
        for (int i = 0; i < 10000; ++i) ordered_big += "1. item\n";
        measure("ordered_list_renumber", ordered_big, rounds);
    }
    measure("tight_loose_alternating", repeat_n("- a\n- b\n\n- c\n\n- d\n\n\n", 2000), rounds);

    // Line endings / misc block features.
    measure("crlf_heavy", repeated("Ordinary prose with a short line.\r\n", mib), rounds);
    measure("thematic_setext_flood", repeat_n("Heading\n-------\n\n***\n\n", 5000), rounds);
    {
        std::string tasks;
        for (int i = 0; i < 10000; ++i) {
            tasks += (i % 2 == 0 ? "- [x] done " : "- [ ] todo ");
            tasks += std::to_string(i);
            tasks += "\n";
        }
        measure("task_list_flood", tasks, rounds);
    }
    measure("empty_blockquote_run", repeat_n(">\n", 50000), rounds);
}
