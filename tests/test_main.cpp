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

void html_with_options(std::string_view markdown, std::string_view expected,
                       chmd::ParseOptions options) {
    auto parsed = chmd::Parser(options).parse(markdown);
    check(static_cast<bool>(parsed), "parse with options succeeds");
    if (!parsed) return;
    const auto actual = chmd::render_html(parsed.document);
    if (actual != expected) {
        std::cerr << "FAIL markdown with options: " << markdown << "\nexpected: " << expected
                  << "\nactual: " << actual << '\n';
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

    html("| foo | bar |\n| --- | --- |\n| baz | bim |\n",
         "<table>\n<thead>\n<tr>\n<th>foo</th>\n<th>bar</th>\n</tr>\n</thead>\n"
         "<tbody>\n<tr>\n<td>baz</td>\n<td>bim</td>\n</tr>\n</tbody>\n</table>\n");
    html("| abc | defghi |\n:-: | -----------:\nbar | baz\n",
         "<table>\n<thead>\n<tr>\n<th align=\"center\">abc</th>\n<th align=\"right\">defghi</th>\n"
         "</tr>\n</thead>\n<tbody>\n<tr>\n<td align=\"center\">bar</td>\n<td align=\"right\">baz</td>\n"
         "</tr>\n</tbody>\n</table>\n");
    html(R"md(| f\|oo  |
| ------ |
| b `\|` az |
| b **\|** im |
)md",
         "<table>\n<thead>\n<tr>\n<th>f|oo</th>\n</tr>\n</thead>\n<tbody>\n"
         "<tr>\n<td>b <code>|</code> az</td>\n</tr>\n"
         "<tr>\n<td>b <strong>|</strong> im</td>\n</tr>\n</tbody>\n</table>\n");
    html("| abc | def |\n| --- | --- |\n| bar |\n| bar | baz | ignored |\n",
         "<table>\n<thead>\n<tr>\n<th>abc</th>\n<th>def</th>\n</tr>\n</thead>\n<tbody>\n"
         "<tr>\n<td>bar</td>\n<td></td>\n</tr>\n<tr>\n<td>bar</td>\n<td>baz</td>\n</tr>\n"
         "</tbody>\n</table>\n");
    html("| abc | def |\n| --- | --- |\n",
         "<table>\n<thead>\n<tr>\n<th>abc</th>\n<th>def</th>\n</tr>\n</thead>\n</table>\n");
    html("| abc | def |\n| --- |\n| bar |\n",
         "<p>| abc | def |\n| --- |\n| bar |</p>\n");
    html("| abc | def |\n| --- | --- |\n| bar | baz |\n> quote\n",
         "<table>\n<thead>\n<tr>\n<th>abc</th>\n<th>def</th>\n</tr>\n</thead>\n<tbody>\n"
         "<tr>\n<td>bar</td>\n<td>baz</td>\n</tr>\n</tbody>\n</table>\n"
         "<blockquote>\n<p>quote</p>\n</blockquote>\n");

    html("- [ ] foo\n- [x] bar\n",
         "<ul>\n<li><input disabled=\"\" type=\"checkbox\"> foo</li>\n"
         "<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> bar</li>\n</ul>\n");
    html("- [x] foo\n  - [ ] bar\n  - [X] baz\n- [ ] bim\n",
         "<ul>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> foo\n<ul>\n"
         "<li><input disabled=\"\" type=\"checkbox\"> bar</li>\n"
         "<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> baz</li>\n"
         "</ul>\n</li>\n<li><input disabled=\"\" type=\"checkbox\"> bim</li>\n</ul>\n");
    html("- [x] first\n\n  second\n",
         "<ul>\n<li>\n<p><input checked=\"\" disabled=\"\" type=\"checkbox\"> first</p>\n"
         "<p>second</p>\n</li>\n</ul>\n");

    html("~~Hi~~ Hello, ~there~ world!\n",
         "<p><del>Hi</del> Hello, <del>there</del> world!</p>\n");
    html("This ~~has a\n\nnew paragraph~~.\n",
         "<p>This ~~has a</p>\n<p>new paragraph~~.</p>\n");
    html("This will ~~~not~~~ strike.\n",
         "<p>This will ~~~not~~~ strike.</p>\n");
    html("*~~nested~~* and ~~**strong**~~\n",
         "<p><em><del>nested</del></em> and <del><strong>strong</strong></del></p>\n");

    chmd::ParseOptions commonmark_only;
    commonmark_only.extensions = {false, false, false};
    html_with_options("~~plain~~\n", "<p>~~plain~~</p>\n", commonmark_only);
    html_with_options("- [x] plain\n", "<ul>\n<li>[x] plain</li>\n</ul>\n", commonmark_only);

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
    auto extensions = chmd::Parser().parse("| a |\n| :-: |\n| ~~b~~ |\n\n- [x] done\n");
    const auto extension_ast = extensions ? chmd::render_ast(extensions.document) : std::string{};
    const auto extension_events = extensions ? chmd::render_events(extensions.document) : std::string{};
    check(extensions && extension_ast.find("\"table_cell\"") != std::string::npos &&
          extension_ast.find("\"strikethrough\"") != std::string::npos,
          "AST contains table and strikethrough nodes");
    check(extensions && extension_ast.find("\"task\": true") != std::string::npos &&
          extension_ast.find("\"checked\": true") != std::string::npos,
          "AST contains checked task metadata");
    check(extensions && extension_events.find("alignment=center") != std::string::npos &&
          extension_events.find("task=checked") != std::string::npos,
          "event output contains extension metadata");
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
