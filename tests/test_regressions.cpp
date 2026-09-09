#include <chmd/chmd.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) {
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
void html(std::string_view input, std::string_view expected) {
    auto result = chmd::Parser().parse(input);
    expect(result && chmd::render_html(result.document) == expected, input);
}
std::string repeat(std::string_view text, std::size_t count) {
    std::string result;
    result.reserve(text.size() * count);
    while (count-- != 0) result += text;
    return result;
}
struct Validator : chmd::EventHandler {
    const chmd::Document& document;
    std::vector<const chmd::Node*> stack;
    std::size_t visited = 0;
    explicit Validator(const chmd::Document& doc) : document(doc) {}
    void check_node(const chmd::Node& node) {
        ++visited;
        expect(node.source.begin <= node.source.end && node.source.end <= document.source().size(), "bounded source range");
        if (node.parent != chmd::npos) {
            expect(!stack.empty() && &document.node(node.parent) == stack.back(), "correct parent");
            if (node.previous != chmd::npos) expect(document.node(node.previous).next == &node - document.nodes().data(), "previous/next symmetry");
            if (node.next != chmd::npos) expect(document.node(node.next).previous == &node - document.nodes().data(), "next/previous symmetry");
        }
    }
    void enter(const chmd::Node& node) override { check_node(node); stack.push_back(&node); }
    void leave(const chmd::Node& node) override {
        expect(!stack.empty() && stack.back() == &node, "balanced traversal");
        if (!stack.empty()) stack.pop_back();
    }
    void text(const chmd::Node& node) override { check_node(node); }
};
void validate(const chmd::Document& doc) {
    Validator validator(doc);
    chmd::walk_events(doc, validator);
    expect(validator.stack.empty() && validator.visited == doc.size(), "arena contains exactly reachable nodes");
}
}

int main() {
    html("&#1114112; &#9999999; &#x110000; &#xFFFFFF;", "<p>\xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD \xEF\xBF\xBD</p>\n");
    html("&#12345678; &#x1234567; &#xG;", "<p>&amp;#12345678; &amp;#x1234567; &amp;#xG;</p>\n");
    html("x\n<prefix>\ny\n", "<p>x\n<prefix>\ny</p>\n");
    html("<scripture>\n\n**bold**\n", "<scripture>\n<p><strong>bold</strong></p>\n");
    html("<SCRIPT>\na\n</ScRiPt>\n**b**\n", "<SCRIPT>\na\n</ScRiPt>\n<p><strong>b</strong></p>\n");
    html("<a@b.-c> <a@b-.c>", "<p>&lt;a@b.-c&gt; &lt;a@b-.c&gt;</p>\n");
    html("before ![a *b* ![c](v)](u) after", "<p>before <img src=\"u\" alt=\"a b c\" /> after</p>\n");
    html("[x](u) [y](v) ![a](w)", "<p><a href=\"u\">x</a> <a href=\"v\">y</a> <img src=\"w\" alt=\"a\" /></p>\n");
    html("![foo] \n[]\n\n[foo]: /url \"title\"\n", "<p><img src=\"/url\" alt=\"foo\" title=\"title\" />\n[]</p>\n");
    html("[x](u\x01v)", "<p>[x](u\x01v)</p>\n");
    html("[x](<u\x01v>)", "<p><a href=\"u%01v\">x</a></p>\n");
    html("[x](u (a(b))", "<p>[x](u (a(b))</p>\n");
    html("[x](u (a\\(b))", "<p><a href=\"u\" title=\"a(b\">x</a></p>\n");
    html("[x](a\\ )", "<p><a href=\"a%5C\">x</a></p>\n");
    html("[x](a\\\n)", "<p><a href=\"a%5C\">x</a></p>\n");
    for (const auto input : {"[x]: <a<b>\n\n[x]", "[x]: u\x01v\n\n[x]", "[x]: u (a(b)\n\n[x]"}) {
        auto parsed = chmd::Parser().parse(input);
        expect(parsed && chmd::render_html(parsed.document).find("<a href=") == std::string::npos,
               "invalid destinations/titles do not define reference links");
    }

    auto blank = chmd::Parser().parse(std::string(1024 * 1024, '\n'));
    expect(blank && blank.document.size() == 1 && blank.document.capacity() <= 64, "blank lines do not reserve an input-sized node arena");
    auto definitions = chmd::Parser().parse(repeat("[ref]: /url\n\n", 20000));
    expect(definitions && definitions.document.size() == 1 && definitions.document.capacity() <= 64,
           "reference-only documents release their construction arena");
    const auto fragment_input = repeat("] ~x * [ ", 8000);
    auto fragments = chmd::Parser().parse(fragment_input);
    expect(fragments && fragments.document.size() == 3 && fragments.document.capacity() <= 64,
           "unmatched inline fragments coalesce into one text node");
    if (fragments) {
        validate(fragments.document);
        expect(chmd::render_html(fragments.document) == "<p>" + fragment_input.substr(0, fragment_input.size() - 1) + "</p>\n",
               "coalescing preserves literal punctuation");
    }
    auto compact = chmd::Parser().parse("[unused]: /url\n\n" + repeat("**x** [y](u) ~~z~~\n\n", 200));
    expect(static_cast<bool>(compact), "mixed compacted document parses");
    if (compact) {
        validate(compact.document);
        const auto expected = chmd::render_html(compact.document);
        compact.document.shrink_to_fit();
        validate(compact.document);
        expect(chmd::render_html(compact.document) == expected, "compaction preserves rendering");
        std::string output(100000, 'x');
        chmd::render_html_to(compact.document, output);
        expect(output == expected, "reusable HTML output replaces existing content");
        chmd::render_ast_to(compact.document, output, false);
        expect(output == chmd::render_ast(compact.document, false), "reusable AST output");
        chmd::render_events_to(compact.document, output);
        expect(output == chmd::render_events(compact.document), "reusable events output");
    }

    chmd::ParseOptions options;
    options.max_nesting = 8;
    expect(!chmd::Parser(options).parse("**************x**************"), "inline nesting limit includes text leaves");
    auto rejected = chmd::Parser(options).parse(repeat("![", 20) + "x" + repeat("](u)", 20));
    expect(!rejected && rejected.error.code == chmd::ErrorCode::nesting_limit, "nested image limit enforced");
    options.max_nesting = 0;
    const auto deep_input = std::string(24000, '*') + "x" + std::string(24000, '*');
    auto deep = chmd::Parser(options).parse(deep_input);
    expect(static_cast<bool>(deep), "explicit unlimited nesting parses");
    if (deep) {
        validate(deep.document);
        expect(chmd::render_html(deep.document) == "<p>" + repeat("<strong>", 12000) + "x" + repeat("</strong>", 12000) + "</p>\n", "deep HTML is stack independent");
        expect(chmd::render_ast(deep.document, false).find("\"strong\"") != std::string::npos, "deep compact AST is stack independent");
        expect(!chmd::render_events(deep.document).empty(), "deep events are stack independent");
    }
    const auto images_input = repeat("![", 12000) + "x" + repeat("](u)", 12000);
    auto images = chmd::Parser(options).parse(images_input);
    expect(static_cast<bool>(images), "deep image delimiter chains parse with unlimited nesting");
    if (images) {
        validate(images.document);
        expect(chmd::render_html(images.document) == "<p><img src=\"u\" alt=\"x\" /></p>\n", "nested images preserve alt text");
    }
    options.max_nesting = 16;
    auto early = chmd::Parser(options).parse(images_input);
    expect(!early && early.error.code == chmd::ErrorCode::nesting_limit && early.document.size() < 12100,
           "depth limits stop inline construction before all wrappers are allocated");
    options.max_nesting = 3;
    expect(static_cast<bool>(chmd::Parser(options).parse("plain")), "plain text at exact depth budget");
    expect(static_cast<bool>(chmd::Parser(options).parse("[](u)")), "empty link at exact depth budget");
    expect(!chmd::Parser(options).parse("[x](u)"), "link text exceeds depth budget");
    options.max_nesting = 5;
    expect(static_cast<bool>(chmd::Parser(options).parse("> *x*")), "inline depth includes containing block quote");
    expect(!chmd::Parser(options).parse("> > *x*"), "inline depth respects outer containers");

    // Wall-clock thresholds are deliberately generous under debug/sanitizers.
    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::string> hostile = {
        std::string(262144, '&'), "x " + std::string(262144, '<'),
        "<!--\n" + repeat("comment text\n", 40000) + "-->\n",
        "x " + repeat("<?", 30000) + ">", repeat("a~ ", 30000),
        "x " + std::string(30000, '[') + std::string(30000, ']'),
        repeat("[x](u) ", 20000),
        repeat("| - | - |\n| - |\n", 32768)
    };
    for (const auto& input : hostile) {
        auto parsed = chmd::Parser().parse(input);
        expect(static_cast<bool>(parsed), "adversarial input parses");
        if (parsed) validate(parsed.document);
    }
    expect(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 20.0, "adversarial scans stay bounded");
    for (std::size_t limit = 1; limit < 80; ++limit) {
        options = {};
        options.max_nodes = limit;
        auto parsed = chmd::Parser(options).parse("[ref]: u\n\n- **a** [ref] ~~b~~\n- [x] c\n\n| a | b |\n| - | - |\n| c | d |\n");
        expect(parsed || parsed.error.code == chmd::ErrorCode::node_limit, "node limits report stable errors");
        if (parsed) validate(parsed.document);
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
