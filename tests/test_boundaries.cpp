#include <chmd/chmd.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::size_t validate_subtree(const chmd::Document& document, chmd::NodeId id,
                             std::vector<bool>& active, std::size_t depth = 0) {
    expect(id < document.nodes().size(), "node index is in range");
    if (id >= document.nodes().size()) return 0;
    expect(!active[id], "tree has no cycle or duplicate child");
    active[id] = true;
    const auto& parent = document.node(id);
    chmd::NodeId previous = chmd::npos;
    std::size_t count = 1;
    std::size_t guard = 0;
    for (auto child = parent.first_child; child != chmd::npos; child = document.node(child).next) {
        expect(++guard <= document.nodes().size(), "sibling chain terminates");
        if (guard > document.nodes().size() || child >= document.nodes().size()) break;
        const auto& node = document.node(child);
        expect(node.parent == id, "child points back to parent");
        expect(node.previous == previous, "previous sibling link is consistent");
        expect(node.source.begin <= node.source.end, "source range is ordered");
        count += validate_subtree(document, child, active, depth + 1);
        previous = child;
    }
    expect(previous == parent.last_child, "last child link is consistent");
    expect(depth < 4096, "tree depth stays bounded");
    return count;
}

void validate(std::string_view input) {
    auto result = chmd::Parser().parse(input);
    expect(static_cast<bool>(result), "arbitrary byte input parses in permissive mode");
    if (!result) return;
    std::vector<bool> active(result.document.nodes().size());
    const auto reachable = validate_subtree(result.document, result.document.root(), active);
    expect(reachable >= 1 && reachable <= result.document.nodes().size(), "reachable count is valid");
    (void)chmd::render_html(result.document);
    (void)chmd::render_ast(result.document, false);
    (void)chmd::render_events(result.document);
}

} // namespace

int main() {
    const std::vector<std::string> fixed = {
        "", "\n", "\r", "\r\n", "\0", "\xFF\xFE", "\t\ttext\n",
        "-\t\tfoo\n", ">\t\tfoo\n", "***\n---\n___\n",
        "[x](<a b> \"t\")", "[x]: /u\n\n[x]", "![*a*](u)",
        "<!-- a -->\n\n<a x='>'>z</a>", "`` ` ``", "***a***",
        "- a\n  - b\n\n    c\n", "1. a\n2) b\n", "a  \nb\\\nc\n",
        "| a\\|b | c |\n| :--- | ---: |\n| `x\\|y` | ~~z~~ |\n",
        "- [\t]\tfoo\n- [X] bar\n", "~~~not~~~ ~~yes~~ ~one~\n",
        "- | a | b |\n  | - | - |\n  | c | d |\n",
        "> | a | b |\n> | - | - |\n> | c | d |\n",
        "| a | b |\n| - | - |\nplain\n# stop\n"
    };
    for (const auto& input : fixed) validate(input);
    validate(std::string("a\0b", 3));

    // Deterministic mixed-boundary generation exercises every byte class used
    // by block and inline state machines, including malformed UTF-8 and NUL.
    static constexpr std::string_view alphabet =
        "abcXYZ012 _-*[]()<>!&;#`~+.'\"/:=\\\t\n\r\0";
    std::mt19937_64 random(0x43484D443031ULL);
    for (int sample = 0; sample < 2500; ++sample) {
        const auto length = static_cast<std::size_t>(random() % 384U);
        std::string input;
        input.reserve(length);
        for (std::size_t i = 0; i < length; ++i) input.push_back(alphabet[random() % alphabet.size()]);
        if (sample % 17 == 0) input.push_back(static_cast<char>(0xFF));
        if (sample % 31 == 0) input.push_back('\0');
        validate(input);
    }

    chmd::ParseOptions options;
    options.max_nodes = 8;
    auto nodes = chmd::Parser(options).parse("- a\n- b\n- c\n- d\n");
    expect(!nodes && nodes.error.code == chmd::ErrorCode::node_limit, "node limit fails predictably");
    options = {};
    options.max_nesting = 8;
    auto nesting = chmd::Parser(options).parse("> > > > > > > > > x");
    expect(!nesting && nesting.error.code == chmd::ErrorCode::nesting_limit, "nesting limit fails predictably");

    // Regression guard against accidental quadratic scans in unmatched inline
    // delimiters. The threshold is intentionally generous for debug/sanitized builds.
    std::string hostile;
    hostile.reserve(400000);
    for (int i = 0; i < 50000; ++i) hostile += "[***_`";
    const auto start = std::chrono::steady_clock::now();
    auto parsed = chmd::Parser().parse(hostile);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    expect(static_cast<bool>(parsed), "large unmatched delimiter input parses");
    expect(elapsed < 20.0, "hostile delimiter scan completes in bounded time");

    if (failures != 0) std::cerr << failures << " boundary test(s) failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
