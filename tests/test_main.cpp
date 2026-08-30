#include <chmd/chmd.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void html(std::string_view markdown, std::string_view expected) {
    auto parsed = chmd::Parser().parse(markdown);
    check(static_cast<bool>(parsed), "parse succeeds");
    if (!parsed) return;
    const auto actual = chmd::render_html(parsed.document);
    if (actual != expected) {
        std::cerr << "FAIL markdown: " << markdown << "\nexpected: " << expected << "\nactual: " << actual << '\n';
        ++failures;
    }
}

struct Counter : chmd::EventHandler {
    int enters = 0;
    int leaves = 0;
    int texts = 0;
    void enter(const chmd::Node&) override { ++enters; }
    void leave(const chmd::Node&) override { ++leaves; }
    void text(const chmd::Node&) override { ++texts; }
};

} // namespace

int main() {
    html("# hello *world*\n", "<h1>hello <em>world</em></h1>\n");
    html("- one\n- two\n", "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n");
    html("[x](https://example.com \"title\")", "<p><a href=\"https://example.com\" title=\"title\">x</a></p>\n");
    html("` a  b `", "<p><code>a  b</code></p>\n");
    html("&copy; &#0;", "<p>\xC2\xA9 \xEF\xBF\xBD</p>\n");
    html("> a\n>\n> b\n", "<blockquote>\n<p>a</p>\n<p>b</p>\n</blockquote>\n");
    html("~~~ cpp\n<&\n~~~\n", "<pre><code class=\"language-cpp\">&lt;&amp;\n</code></pre>\n");
    html("[foo]: /url \"t\"\n\n[foo]", "<p><a href=\"/url\" title=\"t\">foo</a></p>\n");

    std::string with_nul("a\0b", 3);
    auto nul = chmd::Parser().parse(with_nul);
    check(nul && chmd::render_html(nul.document) == "<p>a\xEF\xBF\xBD" "b</p>\n", "NUL is replaced");

    chmd::ParseOptions limited;
    limited.max_input_bytes = 2;
    check(!chmd::Parser(limited).parse("abc"), "input limit is enforced");
    limited = {};
    limited.validate_utf8 = true;
    check(!chmd::Parser(limited).parse(std::string("\xFF", 1)), "invalid UTF-8 is rejected on request");

    auto ast = chmd::Parser().parse("**x**");
    check(ast && chmd::render_ast(ast.document).find("\"strong\"") != std::string::npos, "AST contains strong node");
    check(ast && chmd::render_events(ast.document).find("enter strong") != std::string::npos, "event text contains strong span");
    Counter counter;
    check(!chmd::Parser().parse_events("hello", counter), "event parse succeeds");
    check(counter.enters == counter.leaves && counter.texts == 1, "callback events are balanced");

    // Mixed-boundary stress: deep delimiters, CRLF/CR, tabs, empty containers,
    // and large unmatched runs must all remain valid input.
    std::string adversarial(20000, '*');
    adversarial += "x\r\n>\t- ````` unmatched\r";
    auto stress = chmd::Parser().parse(adversarial);
    check(static_cast<bool>(stress), "mixed adversarial input parses");

    if (failures != 0) std::cerr << failures << " test(s) failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
