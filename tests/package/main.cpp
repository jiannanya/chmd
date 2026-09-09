#include <chmd/chmd.hpp>
#include <string>

int main() {
    auto parsed = chmd::Parser().parse("# package **works**\n");
    if (!parsed) return 1;
    parsed.document.shrink_to_fit();
    std::string html;
    chmd::render_html_to(parsed.document, html);
    if (html != "<h1>package <strong>works</strong></h1>\n") return 2;
    chmd::render_ast_to(parsed.document, html, false);
    chmd::render_events_to(parsed.document, html);
    return html.starts_with("enter document\n") ? 0 : 3;
}
