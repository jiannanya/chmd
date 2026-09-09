#include <chmd/chmd.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(reinterpret_cast<const char*>(data), size);
    chmd::ParseOptions options;
    options.max_input_bytes = 1024 * 1024;
    options.max_nodes = 200000;
    options.max_nesting = 256;
    if (size != 0) {
        options.extensions = {(data[0] & 1) != 0, (data[0] & 2) != 0, (data[0] & 4) != 0};
        options.validate_utf8 = (data[0] & 8) != 0;
    }
    auto result = chmd::Parser(options).parse(input);
    if (result) {
        for (const auto& node : result.document.nodes()) {
            for (const auto id : {node.parent, node.first_child, node.last_child, node.previous, node.next})
                if (id != chmd::npos && id >= result.document.size()) std::abort();
            if ((node.first_child == chmd::npos) != (node.last_child == chmd::npos)) std::abort();
            if (node.next != chmd::npos && result.document.node(node.next).previous !=
                static_cast<chmd::NodeId>(&node - result.document.nodes().data())) std::abort();
        }
        (void)chmd::render_html(result.document);
        (void)chmd::render_ast(result.document, false);
        (void)chmd::render_events(result.document);
    }
    return 0;
}
