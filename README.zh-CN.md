# chmd

[English](README.md) | **简体中文**

`chmd` 是一个零运行时依赖、C++20、UTF-8 优先的 CommonMark 解析库。核心为完全独立的自研实现，并提供回调接口。

当前版本完整实现 CommonMark 0.31.2 核心，并通过官方 `spec.json` 的全部 **652/652** 个示例。扩展 Markdown 语法默认开启，既可整体关闭，也可逐项控制。库同时提供：

- 索引化紧凑 AST；
- SAX 风格的进入、离开、文本回调，以及稳定的行式事件输出；
- CommonMark HTML 渲染；
- 支持列对齐、转义竖线、单元格行内语法及正文列数规范化的管道表格；
- 单波浪线与双波浪线删除线，以及可任意嵌套的选中/未选中任务列表；
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
CHMD_BUILD_BENCHMARKS=OFF   构建微基准套件
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

`Document` 使用 `std::vector<Node>` 作为节点 arena。父子和兄弟关系均为 32 位索引，移动文档不会让节点关系失效。源文本经过规范化：CRLF/CR 统一为 LF，NUL 按规范替换为 U+FFFD。

`_to` 渲染接口将结果写入调用方提供的字符串：

```cpp
std::string output;
chmd::render_html_to(result.document, output);
chmd::render_ast_to(result.document, output, false);
chmd::render_events_to(result.document, output);
```

三个 `_to` 接口均覆盖已有内容，也可使用返回 `std::string` 的渲染接口。`document.shrink_to_fit()` 请求收缩节点和字符串容量；该操作保留节点索引，但可能使节点引用和 `span` 失效。`document.capacity()` 返回节点 arena 的容量（以节点为单位）。

`max_nesting` 同时约束块和最终行内树，根节点计为一层，文本叶节点也计入层数；零表示不限。`parse_events()` 仍先构建完整文档再回调，不能作为增量解析器使用。格式化 AST 的缩进空间随嵌套深度增长，极深文档应使用 `render_ast(document, false)` 或 `--compact`。

需要结构化流时，可继承 `EventHandler` 并调用 `Parser::parse_events()`。回调顺序严格嵌套：非文本节点收到 `enter` / `leave`，文本、代码、换行和原始 HTML 收到 `text`。文本回调的分段不作为固定接口约定；调用方应按事件顺序组合文本内容。

表格、删除线和任务列表在 `ParseOptions::extensions` 中默认开启，并可逐项关闭。通过 API 请求严格 CommonMark 解析：

```cpp
chmd::ParseOptions options;
options.extensions = {false, false, false};
auto result = chmd::Parser(options).parse(markdown);
```

## 命令行

```sh
chmd --to html README.md
chmd --to ast document.md
chmd --to events document.md
chmd --safe untrusted.md       # 转义原始 HTML
chmd --validate-utf8 input.md  # 拒绝非法 UTF-8
chmd --commonmark input.md     # 关闭全部扩展语法
chmd --no-tables input.md      # 单独关闭表格
```

还支持：

```sh
chmd -                         # 显式读取标准输入
chmd -- --input.md              # 读取以连字符开头的文件名
chmd --to ast --compact input.md
chmd --html5 --soft-break-as-space input.md
chmd --max-input-bytes 1048576 --max-nodes 100000 --max-nesting 128 input.md
```

输入字节限制在读取阶段就生效。资源限制的零值表示不限，节点限制包含解析期间的临时 arena 槽位。参数错误/文件打开失败返回 2，解析、读写或内存错误返回 1，成功返回 0。宽松 UTF-8 模式下，AST JSON 和事件字符串中的非法字节以 `\ufffd` 输出，保证结构化输出仍为合法 UTF-8；严格模式会拒绝输入。

另外两个独立开关为 `--no-strikethrough` 和 `--no-task-lists`。

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

## 基准测试

运行本机微基准：

```sh
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DCHMD_BUILD_BENCHMARKS=ON
cmake --build build-bench --parallel
./build-bench/chmd_benchmark
```

微基准默认每组重复 10 次，可用 `chmd_benchmark 20` 修改轮数。CSV 覆盖混合文本、普通段落、空行、表格、引用、HTML 注释以及多种不匹配定界符。解析与 HTML 渲染分别计时，同时输出节点数、保留堆字节、峰值堆字节和分配次数。堆指标为基准程序通过普通 `new`/`delete` 记录的申请量，排除调用方输入、分配器元数据和线程栈；它不是进程 RSS。

## 测试与规范来源

- `tests/spec.json`：CommonMark 0.31.2 官方 652 个示例；
- `tests/test_main.cpp`：公共 API、主要语义、扩展语法和错误限制单元测试；
- `tests/test_extensions.cpp`：表格、任务列表、删除线、功能开关和结构化输出元数据测试；
- `tests/test_boundaries.cpp`：树关系不变量、2500 组确定性随机混合字节、深度/节点限制和对抗性输入；
- `tests/test_regressions.cpp`：深层非递归遍历、arena 可达性、内存容量和退化扫描回归；
- `tests/test_cli.py`：参数、资源限制、文件与标准输入、JSON 输出验证；
- `tests/package`：已安装 CMake 包的下游消费测试；
- `tests/fuzz_parser.cpp`：解析及三种输出的 libFuzzer 入口；
- `tools/generate_tables.py`：可重复生成 HTML5 实体和 Unicode 分类/折叠表。

规范测试数据的版权与归属说明见 [tests/README.md](tests/README.md)。

## 安全说明

CommonMark 允许原始 HTML，默认 HTML 渲染会原样保留它。处理不可信 Markdown 且输出将进入网页时，请启用 `HtmlOptions::escape_raw_html` 或 CLI 的 `--safe`。该选项只负责原始 HTML；URL 协议白名单、CSP 和应用层内容策略仍由调用方决定。

## 许可证

chmd 自研代码采用 MIT 许可证。CommonMark 规范测试夹具沿用其上游许可，详见测试目录说明。
