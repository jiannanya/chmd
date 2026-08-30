# chmd

`chmd` 是一个零运行时依赖、C++20、UTF-8 优先的 CommonMark 解析库。核心为完全独立的自研实现，采用两遍解析策略并提供高效的回调接口。

当前版本严格实现 CommonMark 0.31.2，并通过官方 `spec.json` 的全部 **652/652** 个示例。库同时提供：

- 索引化紧凑 AST；
- SAX 风格的进入、离开、文本回调，以及稳定的行式事件输出；
- CommonMark HTML 渲染；
- 命令行工具，支持 `html`、`ast`、`events` 三种输出；
- 输入字节数、节点数、嵌套深度限制和可选严格 UTF-8 校验；
- 官方规范测试、单元测试、确定性混合边界测试、对抗性退化测试和 libFuzzer 入口。

## 构建

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

项目只要求 CMake 3.20+ 和 C++20 编译器。运行库不依赖正则库、ICU、第三方 Markdown 实现或动态数据文件。官方规范测试需要 Python 3；没有 Python 时仍可构建库和原生测试。

常用构建开关：

```text
CHMD_BUILD_TESTS=ON          构建单元、边界和规范测试
CHMD_BUILD_BENCHMARKS=OFF   构建 1 MiB 微基准
CHMD_ENABLE_SANITIZERS=OFF  Linux/macOS 启用 ASan+UBSan；MSVC 启用 ASan；Windows Clang 启用 UBSan
CHMD_BUILD_FUZZER=OFF       Clang 下构建 libFuzzer 目标
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

`Document` 使用一个连续 `std::vector<Node>` 作为节点 arena。父子和兄弟关系均为 32 位索引，移动文档不会让节点关系失效，也没有每个节点单独分配的子节点容器。源文本仅规范化并保存一份；CRLF/CR 统一为 LF，NUL 按规范替换为 U+FFFD。

需要结构化流时，可继承 `EventHandler` 并调用 `Parser::parse_events()`。回调顺序严格嵌套：非文本节点收到 `enter` / `leave`，文本、代码、换行和原始 HTML 收到 `text`。

## 命令行

```sh
chmd --to html README.md
chmd --to ast document.md
chmd --to events document.md
chmd --safe untrusted.md       # 转义原始 HTML
chmd --validate-utf8 input.md  # 拒绝非法 UTF-8
```

不指定文件或使用 `-` 时从标准输入读取。Windows 下标准输入输出以二进制模式打开，因此 HTML 与规范测试始终使用 LF，不被 CRT 改写成 CRLF。

事件输出示例：

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

## 实现与性能取向

解析分为块级和行内两层：块级扫描维护开放容器栈，行内扫描维护强调/链接定界符栈。反引号运行预索引，强调匹配记录 opener 下界，避免常见的重复全串回扫。制表符按 4 列 tab stop 参与结构判断；跨越结构边界时未消费的列会回流为内容空格。

优先级按“性能、内存、产物空间”排序：

1. ASCII 热路径使用手写扫描器，不使用 `std::regex`；
2. AST 采用连续 arena 和索引关系，避免指针树的碎片化分配；
3. HTML5 实体、Unicode 空白/标点和 case-fold 表在构建前生成并以排序紧凑表编译进库；
4. 运行时无外部数据文件，核心源代码和生成表保持在数百 KiB 量级；
5. `ParseOptions` 可为不可信输入设置资源上限。

运行本机微基准：

```sh
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHMD_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/chmd_benchmark
```

微基准会重复解析约 1 MiB 的标题、列表、链接、强调、代码跨度和实体混合输入，并输出 MiB/s。它不把 HTML 序列化耗时混入解析数字。

## 测试与规范来源

- `tests/spec.json`：CommonMark 0.31.2 官方 652 个示例；
- `tests/test_main.cpp`：公共 API、主要语义和错误限制单元测试；
- `tests/test_boundaries.cpp`：树关系不变量、2500 组确定性随机混合字节、深度/节点限制和对抗性输入；
- `tests/fuzz_parser.cpp`：解析及三种输出的 libFuzzer 入口；
- `tools/generate_tables.py`：可重复生成 HTML5 实体和 Unicode 分类/折叠表。

规范测试数据的版权与归属说明见 [tests/README.md](tests/README.md)。

## 安全说明

CommonMark 允许原始 HTML，默认 HTML 渲染会原样保留它。处理不可信 Markdown 且输出将进入网页时，请启用 `HtmlOptions::escape_raw_html` 或 CLI 的 `--safe`。该选项只负责原始 HTML；URL 协议白名单、CSP 和应用层内容策略仍由调用方决定。

## 许可证

chmd 自研代码采用 MIT 许可证。CommonMark 规范测试夹具沿用其上游许可，详见测试目录说明。
