#include "../src/core.hpp"
#include <chrono>
#include <iostream>
using namespace atomx;
int main(int argc, char **argv) {
    try {
        int n = argc > 1 ? std::stoi(argv[1]) : 64;
        auto begin = std::chrono::steady_clock::now();
        auto d = crystal(n);
        auto count = d.sourceCount;
        writeXYZ("build/benchmark.xyz", d);
        d = {};
        auto written = std::chrono::steady_clock::now();
        auto index = indexXYZ("build/benchmark.xyz");
        auto indexed = std::chrono::steady_clock::now();
        auto loaded = readXYZ("build/benchmark.xyz", index[0], 2000000);
        auto read = std::chrono::steady_clock::now();
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::cout << "source_atoms=" << count << "\npreview_atoms=" << loaded.atoms.size()
                  << "\nstride=" << loaded.stride
                  << "\nfile_bytes=" << std::filesystem::file_size("build/benchmark.xyz")
                  << "\ngenerate_write_ms=" << ms(begin, written)
                  << "\nindex_ms=" << ms(written, indexed) << "\nread_ms=" << ms(indexed, read)
                  << "\n";
        return loaded.sourceCount == count ? 0 : 1;
    } catch (const std::exception &e) {
        std::cerr << e.what();
        return 1;
    }
}
