#include <chmd/chmd.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = std::string_view(reinterpret_cast<const char*>(data), size);
    auto result = chmd::Parser().parse(input);
    if (result) {
        (void)chmd::render_html(result.document);
        (void)chmd::render_ast(result.document, false);
        (void)chmd::render_events(result.document);
    }
    return 0;
}

