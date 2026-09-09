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
    std::cout << name << ',' << input.size() << ',' << rounds << ','
              << std::fixed << std::setprecision(6) << parse_seconds << ','
              << (input.size() / 1048576.0 * rounds / parse_seconds) << ','
              << render_seconds << ',' << parsed.document.size() << ','
              << retained << ',' << peak << ',' << allocations << ',' << checksum << std::endl;
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
    std::cout << "case,input_bytes,rounds,parse_seconds,parse_mib_s,html_seconds,nodes,retained_bytes,peak_bytes,allocations,checksum\n";
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
    chmd::ParseOptions unlimited;
    unlimited.max_nesting = 0;
    measure("nested_images", repeated("![", 24000) + "x" + repeated("](u)", 48000), rounds, unlimited);
    measure("nested_emphasis", repeated("*a ", 36000) + "x" + repeated(" b*", 36000), rounds, unlimited);
}
