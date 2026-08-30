#include <chmd/chmd.hpp>

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

void usage(std::ostream& out) {
    out << "usage: chmd [--to html|ast|events] [--safe] [--validate-utf8] [FILE]\n";
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string format = "html";
    std::string filename;
    chmd::ParseOptions parse_options;
    chmd::HtmlOptions html_options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--to" && i + 1 < argc) format = argv[++i];
        else if (arg == "--safe") html_options.escape_raw_html = true;
        else if (arg == "--validate-utf8") parse_options.validate_utf8 = true;
        else if (arg == "--version") { std::cout << "chmd " << chmd::version() << '\n'; return 0; }
        else if (arg == "--help" || arg == "-h") { usage(std::cout); return 0; }
        else if (!arg.empty() && arg.front() == '-') { usage(std::cerr); return 2; }
        else if (filename.empty()) filename = arg;
        else { usage(std::cerr); return 2; }
    }
    if (format != "html" && format != "ast" && format != "events") {
        std::cerr << "chmd: unknown output format: " << format << '\n';
        return 2;
    }

    std::string input;
    if (filename.empty() || filename == "-") {
        std::cin.sync_with_stdio(false);
        input.assign(std::istreambuf_iterator<char>(std::cin), {});
    } else {
        std::ifstream stream(filename, std::ios::binary);
        if (!stream) { std::cerr << "chmd: cannot open " << filename << '\n'; return 2; }
        input.assign(std::istreambuf_iterator<char>(stream), {});
    }

    auto result = chmd::Parser(parse_options).parse(input);
    if (!result) {
        std::cerr << "chmd: parse error at byte " << result.error.offset << ": " << result.error.message << '\n';
        return 1;
    }
    if (format == "html") std::cout << chmd::render_html(result.document, html_options);
    else if (format == "ast") std::cout << chmd::render_ast(result.document);
    else std::cout << chmd::render_events(result.document);
    return std::cout ? 0 : 1;
}
