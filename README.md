# chmd

**English** | [简体中文](README.zh-CN.md)

`chmd` is a zero-runtime-dependency, C++20, UTF-8-first CommonMark parser. Its core is an entirely independent implementation with a callback interface.

The current release implements the complete CommonMark 0.31.2 core and passes all **652/652** examples in the official `spec.json` fixture. Extended Markdown syntax is enabled by default and can be disabled as a bundle or feature by feature. The library provides:

- A compact, index-based AST;
- SAX-style enter, leave, and text callbacks, plus stable line-oriented event output;
- CommonMark HTML rendering;
- Pipe tables with column alignment, escaped pipes, inline cell content, and body column normalization;
- Single- and double-tilde strikethrough, plus nested checked and unchecked task list items;
- A command-line tool with `html`, `ast`, and `events` output modes;
- Configurable input-size, node-count, and nesting-depth limits, with optional strict UTF-8 validation;
- Official conformance tests, unit tests, deterministic mixed-boundary tests, adversarial complexity tests, and a libFuzzer entry point.

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The project requires only CMake 3.20 or newer and a C++20 compiler. The runtime library does not depend on a regular-expression library, ICU, another Markdown implementation, or external data files. Python 3 is required only for the official conformance suite; the library and native tests can still be built without it.

Common build options:

```text
CHMD_BUILD_TESTS=ON          Build unit, boundary, and conformance tests
CHMD_BUILD_BENCHMARKS=OFF   Build the microbenchmark suite
CHMD_ENABLE_SANITIZERS=OFF  ASan+UBSan on Linux/macOS; ASan on MSVC; UBSan on Windows Clang
CHMD_BUILD_FUZZER=OFF       Build the libFuzzer target with Clang
```

## C++ API

```cpp
#include <chmd/chmd.hpp>
#include <iostream>

int main() {
    auto result = chmd::Parser().parse("# hello *world*\n");
    if (!result) {
        std::cerr << result.error.message << '\n';
        return 1;
    }

    std::cout << chmd::render_html(result.document);
    std::cout << chmd::render_ast(result.document);
    std::cout << chmd::render_events(result.document);
}
```

`Document` stores nodes in a `std::vector<Node>` arena. Parent, child, and sibling relationships use 32-bit indices; moving a document preserves those relationships. Source text is normalized: CRLF and CR become LF, while NUL is replaced with U+FFFD as required by the specification.

The `_to` rendering functions write into a supplied string, replacing its contents:

```cpp
std::string output;
chmd::render_html_to(result.document, output);
chmd::render_ast_to(result.document, output, false);
chmd::render_events_to(result.document, output);
```

`Document::capacity()` reports arena capacity in nodes. `Document::shrink_to_fit()` requests a reduction in node and string capacity; it preserves node indices but may invalidate node references and spans.

`max_nesting` limits the complete block and inline tree, counting the root and text leaves. Zero disables the limit. Pretty AST indentation grows with nesting depth; use `render_ast(document, false)` or `--compact` for deep documents.

For structured streaming, derive from `EventHandler` and call `Parser::parse_events()`. Callbacks are strictly nested: non-text nodes receive `enter` and `leave`, while text, code, line breaks, and raw HTML receive `text`. `parse_events()` builds the full document before invoking callbacks; it is not incremental parsing. Text callback boundaries are not a fixed tokenization contract; combine text in event order as needed.

Tables, strikethrough, and task lists are enabled in `ParseOptions::extensions` by default. Each feature can be switched independently. To request strict CommonMark parsing through the API:

```cpp
chmd::ParseOptions options;
options.extensions = {false, false, false};
auto result = chmd::Parser(options).parse(markdown);
```

## Command line

```sh
chmd --to html README.md
chmd --to ast document.md
chmd --to events document.md
chmd --safe untrusted.md       # Escape raw HTML
chmd --validate-utf8 input.md  # Reject invalid UTF-8
chmd --commonmark input.md     # Disable all extended syntax
chmd --no-tables input.md      # Disable one extension independently
```

Additional CLI options:

```sh
chmd -
chmd -- --input.md
chmd --to ast --compact input.md
chmd --html5 --soft-break-as-space input.md
chmd --max-input-bytes 1048576 --max-nodes 100000 --max-nesting 128 input.md
```

The byte limit applies while reading input. Zero disables a configured limit. Node limits include temporary arena slots during parsing. Exit codes are 0 for success, 1 for parsing/resource/I/O errors, and 2 for invalid arguments or file-open failures. In permissive UTF-8 mode, JSON/event strings replace invalid bytes with `\ufffd`.

The other individual switches are `--no-strikethrough` and `--no-task-lists`.

When no file is specified, or when the input is `-`, the command reads from standard input. On Windows, standard input and output use binary mode so HTML and conformance results retain LF line endings instead of being rewritten to CRLF by the CRT.

Example event output:

```text
enter document
enter paragraph
text text "hello "
enter emphasis
text text "world"
leave emphasis
leave paragraph
leave document
```

## Benchmarks

Run the local microbenchmark with:

```sh
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHMD_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/chmd_benchmark
```

The benchmark covers 16 workloads and defaults to 10 rounds per case; pass a positive integer as its first argument to change the round count. CSV output includes separate parsing and HTML timings, node counts, and requested heap allocation statistics. Heap counters exclude caller input, allocator metadata, and stack; they are not process RSS.

## Tests and specification fixture

- `tests/spec.json`: all 652 official CommonMark 0.31.2 examples;
- `tests/test_main.cpp`: public API, core semantics, extended syntax, and error-limit unit tests;
- `tests/test_extensions.cpp`: tables, task lists, strikethrough, feature switches, and structured-output metadata;
- `tests/test_boundaries.cpp`: tree invariants, 2,500 deterministic mixed-byte cases, depth/node limits, and adversarial input;
- `tests/fuzz_parser.cpp`: libFuzzer entry point covering parsing and all three output formats;
- `tools/generate_tables.py`: reproducible HTML5 entity and Unicode classification/folding table generation.

Copyright and attribution information for the conformance fixture is available in [tests/README.md](tests/README.md).

## Security

CommonMark permits raw HTML, and the default HTML renderer preserves it. When rendering untrusted Markdown for use on a web page, enable `HtmlOptions::escape_raw_html` or pass `--safe` to the CLI. This option handles raw HTML only; URL-scheme allowlists, CSP, and application-level content policies remain the caller's responsibility.

## License

The independently developed chmd source is licensed under the MIT License. The CommonMark conformance fixture retains its upstream license; see the test directory notice for details.
