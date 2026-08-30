#include <chmd/chmd.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string render(std::string_view markdown, chmd::ParseOptions options = {}) {
    auto result = chmd::Parser(options).parse(markdown);
    expect(static_cast<bool>(result), "extension input parses");
    return result ? chmd::render_html(result.document) : std::string{};
}

void expect_html(std::string_view markdown, std::string_view expected,
                 chmd::ParseOptions options = {}) {
    const auto actual = render(markdown, options);
    if (actual != expected) {
        std::cerr << "FAIL markdown: " << markdown << "\nexpected: " << expected
                  << "\nactual: " << actual << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    expect_html("| a |  | b |\n--- | --- | ---\n| c || d |\n",
                "<table>\n<thead>\n<tr>\n<th>a</th>\n<th></th>\n<th>b</th>\n</tr>\n"
                "</thead>\n<tbody>\n<tr>\n<td>c</td>\n<td></td>\n<td>d</td>\n</tr>\n"
                "</tbody>\n</table>\n");
    expect_html("| a |\n--- |\n",
                "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n</table>\n");
    expect_html("| a | b |\n| - | - |\n| c | d |\nplain\n\nafter\n",
                "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n"
                "<tbody>\n<tr>\n<td>c</td>\n<td>d</td>\n</tr>\n<tr>\n<td>plain</td>\n"
                "<td></td>\n</tr>\n</tbody>\n</table>\n<p>after</p>\n");
    expect_html("alpha\n| a | b |\n| --- | --- |\n| [x] | [ref] |\n[ref]: /url \"title\"\n",
                "<p>alpha</p>\n<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n"
                "</thead>\n<tbody>\n<tr>\n<td>[x]</td>\n"
                "<td><a href=\"/url\" title=\"title\">ref</a></td>\n</tr>\n"
                "</tbody>\n</table>\n");
    expect_html("| a\\|b | `c\\|d` |\n| :--- | ---: |\n| **x\\|y** | e |\n",
                "<table>\n<thead>\n<tr>\n<th align=\"left\">a|b</th>\n"
                "<th align=\"right\"><code>c|d</code></th>\n</tr>\n</thead>\n<tbody>\n"
                "<tr>\n<td align=\"left\"><strong>x|y</strong></td>\n"
                "<td align=\"right\">e</td>\n</tr>\n</tbody>\n</table>\n");

    expect_html("- [@] normal\n- [ ] task\n    - [x] nested\n",
                "<ul>\n<li>[@] normal</li>\n<li><input disabled=\"\" type=\"checkbox\"> task\n"
                "<ul>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> nested</li>\n"
                "</ul>\n</li>\n</ul>\n");
    expect_html("1. [X]\tchecked\n2. [ ] unchecked\n",
                "<ol>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> checked</li>\n"
                "<li><input disabled=\"\" type=\"checkbox\"> unchecked</li>\n</ol>\n");

    expect_html("~~a ~b~ c~~\n", "<p><del>a <del>b</del> c</del></p>\n");
    expect_html("x ~~~a~~~ ~~b~ ~c~~\n", "<p>x ~~~a~~~ <del>b~ ~c</del></p>\n");
    std::string long_run(200, '~');
    long_run += "text";
    long_run.append(200, '~');
    expect(render(long_run).find("<del>") == std::string::npos,
           "long tilde runs remain literal and bounded");

    chmd::ParseOptions options;
    options.extensions.tables = false;
    expect_html("| a |\n| - |\n", "<p>| a |\n| - |</p>\n", options);
    options = {};
    options.extensions.strikethrough = false;
    expect_html("~~a~~\n", "<p>~~a~~</p>\n", options);
    options = {};
    options.extensions.task_lists = false;
    expect_html("- [x] a\n", "<ul>\n<li>[x] a</li>\n</ul>\n", options);

    const auto parsed = chmd::Parser().parse("| a |\n| :-: |\n| ~~b~~ |\n\n- [x] done\n");
    expect(static_cast<bool>(parsed), "metadata sample parses");
    if (parsed) {
        const auto ast = chmd::render_ast(parsed.document);
        const auto events = chmd::render_events(parsed.document);
        expect(ast.find("\"table_cell\"") != std::string::npos,
               "AST exposes table cells");
        expect(ast.find("\"strikethrough\"") != std::string::npos,
               "AST exposes strikethrough spans");
        expect(ast.find("\"task\": true") != std::string::npos &&
                   ast.find("\"checked\": true") != std::string::npos,
               "AST exposes task state");
        expect(events.find("alignment=center") != std::string::npos &&
                   events.find("task=checked") != std::string::npos,
               "event output exposes extension metadata");
    }

    if (failures != 0) std::cerr << failures << " extension test(s) failed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
