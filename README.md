# chmd

**English** | [简体中文](README.zh-CN.md)

`chmd` is a zero-runtime-dependency, C++20, UTF-8-first CommonMark parser. Its core is an entirely independent implementation built around a two-pass parsing strategy and an efficient callback interface.

The current release strictly implements CommonMark 0.31.2 and passes all **652/652** examples in the official `spec.json` fixture. The library provides:

- A compact, index-based AST;
- SAX-style enter, leave, and text callbacks, plus stable line-oriented event output;
- CommonMark HTML rendering;
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
CHMD_BUILD_BENCHMARKS=OFF   Build the 1 MiB microbenchmark
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

`Document` stores nodes in one contiguous `std::vector<Node>` arena. Parent, child, and sibling relationships use 32-bit indices, so moving a document does not invalidate its tree relationships and nodes do not need individually allocated child containers. Source text is normalized and stored once: CRLF and CR become LF, while NUL is replaced with U+FFFD as required by the specification.

For structured streaming, derive from `EventHandler` and call `Parser::parse_events()`. Callbacks are strictly nested: non-text nodes receive `enter` and `leave`, while text, code, line breaks, and raw HTML receive `text`.

## Command line

```sh
chmd --to html README.md
chmd --to ast document.md
chmd --to events document.md
chmd --safe untrusted.md       # Escape raw HTML
chmd --validate-utf8 input.md  # Reject invalid UTF-8
```

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

## Implementation and performance

Parsing is divided into block and inline passes. The block scanner maintains a stack of open containers; the inline scanner maintains delimiter stacks for emphasis and links. Backtick runs are pre-indexed, and emphasis matching records lower bounds for opener searches to avoid repeated whole-input rescans. Tabs participate in structural parsing at four-column tab stops, and unconsumed columns crossing structural boundaries are returned to the content as spaces.

The implementation priorities are performance first, memory use second, and binary/source footprint third:

1. Hand-written scanners handle ASCII hot paths without `std::regex`;
2. The AST uses a contiguous arena and index relationships to avoid pointer-tree allocation fragmentation;
3. HTML5 entities and Unicode whitespace, punctuation, and case-folding data are generated before compilation and stored as compact sorted tables;
4. The runtime needs no external data files, while the core source and generated tables remain within a few hundred KiB;
5. `ParseOptions` supplies resource limits for untrusted input.

Run the local microbenchmark with:

```sh
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHMD_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/chmd_benchmark
```

The benchmark repeatedly parses approximately 1 MiB of mixed headings, lists, links, emphasis, code spans, and entities, then reports throughput in MiB/s. HTML serialization time is intentionally excluded from the parsing result.

## Tests and specification fixture

- `tests/spec.json`: all 652 official CommonMark 0.31.2 examples;
- `tests/test_main.cpp`: public API, core semantics, and error-limit unit tests;
- `tests/test_boundaries.cpp`: tree invariants, 2,500 deterministic mixed-byte cases, depth/node limits, and adversarial input;
- `tests/fuzz_parser.cpp`: libFuzzer entry point covering parsing and all three output formats;
- `tools/generate_tables.py`: reproducible HTML5 entity and Unicode classification/folding table generation.

Copyright and attribution information for the conformance fixture is available in [tests/README.md](tests/README.md).

## Security

CommonMark permits raw HTML, and the default HTML renderer preserves it. When rendering untrusted Markdown for use on a web page, enable `HtmlOptions::escape_raw_html` or pass `--safe` to the CLI. This option handles raw HTML only; URL-scheme allowlists, CSP, and application-level content policies remain the caller's responsibility.

## License

The independently developed chmd source is licensed under the MIT License. The CommonMark conformance fixture retains its upstream license; see the test directory notice for details.
