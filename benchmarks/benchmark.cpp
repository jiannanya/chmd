#include <chmd/chmd.hpp>

#include <chrono>
#include <iostream>
#include <string>

int main() {
    std::string input;
    input.reserve(1024 * 1024);
    while (input.size() < 1024 * 1024) {
        input += "## Heading\n\n- [link](https://example.com) and **strong** text\n- `code` &amp; text\n\n";
    }
    constexpr int rounds = 20;
    std::size_t nodes = 0;
    std::size_t last_nodes = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < rounds; ++i) {
        auto result = chmd::Parser().parse(input);
        if (!result) return 1;
        nodes += result.document.size();
        last_nodes = result.document.size();
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    const auto mib = static_cast<double>(input.size() * rounds) / (1024.0 * 1024.0);
    std::cout << "parsed " << mib << " MiB in " << elapsed << " s (" << mib / elapsed
              << " MiB/s), node checksum " << nodes << '\n'
              << "last AST: " << last_nodes << " arena slots, sizeof(Node)=" << sizeof(chmd::Node)
              << ", structural lower bound=" << (last_nodes * sizeof(chmd::Node)) / 1024 << " KiB\n";
}
