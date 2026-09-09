#include <chmd/chmd.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
void usage(std::ostream& out) {
    out << "usage: chmd [OPTIONS] [--] [FILE|-]\n"
           "  --to html|ast|events     Output format (default: html)\n"
           "  --safe                   Escape raw HTML\n"
           "  --validate-utf8          Reject invalid UTF-8\n"
           "  --commonmark             Disable all extensions\n"
           "  --no-tables              Disable pipe tables\n"
           "  --no-strikethrough       Disable strikethrough\n"
           "  --no-task-lists          Disable task lists\n"
           "  --compact                Compact AST JSON\n"
           "  --html5                  Use HTML5 void tags\n"
           "  --soft-break-as-space    Render soft breaks as spaces\n"
           "  --max-input-bytes N      Bound input before parsing (0: unlimited)\n"
           "  --max-nodes N            Bound parser arena slots (0: unlimited)\n"
           "  --max-nesting N          Bound tree depth (default: 1000; 0: unlimited)\n"
           "  --help, -h               Show this help\n"
           "  --version                Show version\n";
}

bool read_input(std::istream& stream, std::string& input, std::size_t limit) {
    std::array<char, 65536> buffer;
    // The normalized source uses 32-bit offsets, even with no configured limit.
    if (limit == 0) limit = std::numeric_limits<std::uint32_t>::max();
    for (;;) {
        const auto amount = std::min(buffer.size(), limit - input.size());
        if (amount == 0) {
            if (stream.peek() == std::char_traits<char>::eof() && !stream.bad()) return true;
            std::cerr << "chmd: input limit exceeded\n";
            return false;
        }
        stream.read(buffer.data(), static_cast<std::streamsize>(amount));
        input.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
        if (stream.bad() || (stream.fail() && !stream.eof())) {
            std::cerr << "chmd: input read failed\n";
            return false;
        }
        if (stream.eof()) return true;
    }
}

int run(int argc, char** argv) {
    std::string_view format = "html";
    std::string filename;
    bool has_filename = false, positional = false, pretty = true;
    chmd::ParseOptions parse_options;
    chmd::HtmlOptions html_options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (!positional && arg == "--") { positional = true; continue; }
        if (!positional && arg == "--to") {
            if (++i == argc) { std::cerr << "chmd: --to requires a format\n"; return 2; }
            format = argv[i];
        } else if (!positional && (arg == "--max-input-bytes" || arg == "--max-nodes" || arg == "--max-nesting")) {
            if (++i == argc) { std::cerr << "chmd: " << arg << " requires a nonnegative integer\n"; return 2; }
            const std::string_view value(argv[i]);
            std::size_t number = 0;
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), number);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
                std::cerr << "chmd: invalid integer for " << arg << '\n'; return 2;
            }
            if (arg == "--max-input-bytes") parse_options.max_input_bytes = number;
            else if (arg == "--max-nodes") parse_options.max_nodes = number;
            else parse_options.max_nesting = number;
        } else if (!positional && arg == "--safe") html_options.escape_raw_html = true;
        else if (!positional && arg == "--validate-utf8") parse_options.validate_utf8 = true;
        else if (!positional && arg == "--commonmark") parse_options.extensions = {false, false, false};
        else if (!positional && arg == "--no-tables") parse_options.extensions.tables = false;
        else if (!positional && arg == "--no-strikethrough") parse_options.extensions.strikethrough = false;
        else if (!positional && arg == "--no-task-lists") parse_options.extensions.task_lists = false;
        else if (!positional && arg == "--compact") pretty = false;
        else if (!positional && arg == "--html5") html_options.xhtml = false;
        else if (!positional && arg == "--soft-break-as-space") html_options.soft_break_as_space = true;
        else if (!positional && arg == "--version") { std::cout << "chmd " << chmd::version() << '\n'; return 0; }
        else if (!positional && (arg == "--help" || arg == "-h")) { usage(std::cout); return 0; }
        else if (!positional && arg != "-" && !arg.empty() && arg.front() == '-') {
            std::cerr << "chmd: unknown option: " << arg << '\n'; return 2;
        } else if (!has_filename) { filename = arg; has_filename = true; }
        else { std::cerr << "chmd: only one input file is supported\n"; return 2; }
    }
    if (format != "html" && format != "ast" && format != "events") {
        std::cerr << "chmd: unknown output format: " << format << '\n'; return 2;
    }
    std::string input;
    if (!has_filename || filename == "-") {
        if (!read_input(std::cin, input, parse_options.max_input_bytes)) return 1;
    } else {
        std::ifstream stream(filename, std::ios::binary);
        if (!stream) { std::cerr << "chmd: cannot open " << filename << '\n'; return 2; }
        if (!read_input(stream, input, parse_options.max_input_bytes)) return 1;
    }
    auto result = chmd::Parser(parse_options).parse(input);
    // Reuse the input allocation for output after parsing has taken ownership
    // of its normalized source. This avoids a third large live string.
    if (!result) {
        std::cerr << "chmd: parse error at byte " << result.error.offset << ": " << result.error.message << '\n';
        return 1;
    }
    if (format == "html") chmd::render_html_to(result.document, input, html_options);
    else if (format == "ast") chmd::render_ast_to(result.document, input, pretty);
    else chmd::render_events_to(result.document, input);
    std::cout.write(input.data(), static_cast<std::streamsize>(input.size()));
    std::cout.flush();
    if (!std::cout) { std::cerr << "chmd: output write failed\n"; return 1; }
    return 0;
}
}
int main(int argc, char** argv) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    try { return run(argc, argv); }
    catch (const std::bad_alloc&) { std::cerr << "chmd: memory allocation failed\n"; }
    catch (const std::length_error&) { std::cerr << "chmd: size limit exceeded\n"; }
    return 1;
}
