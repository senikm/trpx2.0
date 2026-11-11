// Micro benchmarks of Bitqueue and of Terse compression (Terse constructor) and decompression (prolix()).
//
// Usage: terse_benchmark [--filter SUBSTRING] [--repetitions N] [--pixels N] [--list]
//
// Each case runs once untimed, and then N times timed, single-threaded. Reported are the fastest and the median
// time, the time per value and the throughput (uncompressed bytes per second, from the median).
//
// Every case also checks its result: the values must survive the round trip. For the Terse cases, a hash of the
// compressed stream is printed, so that changes that should not alter the compressed output can be verified by
// comparing the hashes of two builds. The synthetic data are generated with a fixed seed. They do not depend on
// the standard library, but they use std::exp and std::log, so compare hashes of builds on the same platform.
//
// The program returns 1 if any check fails.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Concurrent.hpp must be included before Terse.hpp.
#include "Concurrent.hpp"
#include "Bitqueue.hpp"
#include "Terse.hpp"
#include "benchmark_data.hpp"

namespace {

using terse_benchmark::block;
using terse_benchmark::make_frame;
using terse_benchmark::Random;

struct Options {
    std::string filter;
    int repetitions = 10;
    std::size_t pixels = 2048 * 2048;
    bool list = false;
};

struct Result {
    double min_ms;
    double median_ms;
};

bool g_ok = true;
volatile std::uint64_t g_sink = 0; // Keeps the compiler from discarding results.

std::uint64_t fnv1a(std::string const& bytes) {
    std::uint64_t hash = 0xcbf29ce484222325;
    for (char const c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3;
    }
    return hash;
}

template <typename F>
Result measure(Options const& options, F&& function) {
    function(); // Warm up caches, and the allocator.
    std::vector<double> ms;
    for (int i = 0; i < options.repetitions; ++i) {
        auto const start = std::chrono::steady_clock::now();
        function();
        auto const stop = std::chrono::steady_clock::now();
        ms.push_back(std::chrono::duration<double, std::milli>(stop - start).count());
    }
    std::sort(ms.begin(), ms.end());
    return {ms.front(), ms[ms.size() / 2]};
}

bool selected(Options const& options, std::string const& name) {
    if (options.list) {
        std::printf("%s\n", name.c_str());
        return false;
    }
    return name.find(options.filter) != std::string::npos;
}

void print_header() {
    std::printf("%-44s %9s %9s %8s %9s %7s  %-16s\n", "benchmark", "min ms", "median ms", "ns/value", "MB/s", "ratio",
                "hash");
}

void print(std::string const& name, Result const& result, std::size_t const values, std::size_t const bytes,
           double const ratio, std::uint64_t const hash, bool const ok) {
    std::printf("%-44s %9.3f %9.3f %8.3f %9.0f ", name.c_str(), result.min_ms, result.median_ms,
                result.median_ms * 1e6 / static_cast<double>(values),
                static_cast<double>(bytes) / result.median_ms / 1e3);
    if (ratio > 0)
        std::printf("%7.2f  %016llx", ratio, static_cast<unsigned long long>(hash));
    else
        std::printf("%7s  %-16s", "-", "-");
    std::printf("%s\n", ok ? "" : "  FAILED");
    g_ok = g_ok && ok;
}

// Pushes and pops blocks of 12 values of B bits, with the bit width as a template argument (as Terse does when
// decompressing narrow blocks) and as a function argument (as Terse does when compressing).
template <std::uint8_t B>
void bench_bitqueue(Options const& options) {
    std::size_t const n = options.pixels / block * block;
    std::vector<std::uint32_t> values(n);
    Random random(B);
    for (auto& value : values)
        value = static_cast<std::uint32_t>(random() & ((std::uint64_t{1} << B) - 1));
    std::vector<std::uint8_t> bits((n * B + 63) / 64 * 8 + 8);
    std::vector<std::uint32_t> popped(n);
    std::string const prefix = "bitqueue/B=" + std::to_string(B) + "/";

    auto const push_static = [&] {
        jpa::Bitqueue_push_back queue(bits);
        for (std::size_t i = 0; i != n; i += block)
            queue.push_back<B>(std::span<std::uint32_t const, block>(values.data() + i, block));
    };
    auto const push_runtime = [&] {
        jpa::Bitqueue_push_back queue(bits);
        for (std::size_t i = 0; i != n; i += block)
            queue.push_back(B, std::span<std::uint32_t const>(values.data() + i, block));
    };
    auto const pop_static = [&] {
        jpa::Bitqueue_pop queue(bits);
        for (std::size_t i = 0; i != n; i += block)
            queue.pop<B>(std::span<std::uint32_t, block>(popped.data() + i, block));
        g_sink = g_sink + popped[n / 2];
    };
    auto const pop_runtime = [&] {
        jpa::Bitqueue_pop queue(bits);
        for (std::size_t i = 0; i != n; i += block)
            queue.pop(B, std::span<std::uint32_t>(popped.data() + i, block));
        g_sink = g_sink + popped[n / 2];
    };
    struct Case {
        std::string name;
        std::function<void()> run;
        bool pop;
    };
    for (auto const& [name, run, pop] : {Case{prefix + "push_back<B>", push_static, false},
                                         Case{prefix + "push_back(B)", push_runtime, false},
                                         Case{prefix + "pop<B>", pop_static, true},
                                         Case{prefix + "pop(B)", pop_runtime, true}}) {
        if (!selected(options, name))
            continue;
        push_static(); // The pop cases read what was pushed.
        std::fill(popped.begin(), popped.end(), 0u);
        Result const result = measure(options, run);
        if (!pop)
            pop_static();
        print(name, result, n, n * sizeof(std::uint32_t), 0, 0, popped == values);
    }
}

char const* mode_name(jpa::Terse_mode const mode) {
    switch (mode) {
        case jpa::Terse_mode::Signed: return "Signed";
        case jpa::Terse_mode::Unsigned: return "Unsigned";
        case jpa::Terse_mode::Small_unsigned: return "Small_unsigned";
        default: return "Default";
    }
}

template <typename T>
void bench_terse(Options const& options, char const* type_name, jpa::Terse_mode const mode, char const* data_name,
                 double const background) {
    std::string const prefix = std::string("terse/") + data_name + "/" + type_name + "/" + mode_name(mode) + "/";
    std::string const compress_name = prefix + "compress";
    std::string const prolix_name = prefix + "prolix";
    bool const run_compress = selected(options, compress_name);
    bool const run_prolix = selected(options, prolix_name);
    if (!run_compress && !run_prolix)
        return;
    std::vector<T> const data = make_frame<T>(options.pixels, background);
    std::size_t const bytes = data.size() * sizeof(T);
    jpa::Terse<> terse(data.data(), data.size(), block, mode);
    std::vector<T> unpacked(data.size());
    terse.prolix(unpacked.data());
    bool const ok = unpacked == data;
    std::stringstream stream;
    terse.write(stream);
    std::uint64_t const hash = fnv1a(stream.str());
    double const ratio = static_cast<double>(bytes) / static_cast<double>(terse.terse_size());
    if (run_compress) {
        Result const result = measure(options, [&] {
            jpa::Terse<> compressed(data.data(), data.size(), block, mode);
            g_sink = g_sink + compressed.terse_size();
        });
        print(compress_name, result, data.size(), bytes, ratio, hash, ok);
    }
    if (run_prolix) {
        Result const result = measure(options, [&] {
            terse.prolix(unpacked.data());
            g_sink = g_sink + static_cast<std::uint64_t>(unpacked[unpacked.size() / 2]);
        });
        print(prolix_name, result, data.size(), bytes, ratio, hash, ok && unpacked == data);
    }
}

void bench_terse_data(Options const& options, char const* data_name, double const background) {
    bench_terse<std::uint16_t>(options, "uint16", jpa::Terse_mode::Small_unsigned, data_name, background);
    bench_terse<std::uint16_t>(options, "uint16", jpa::Terse_mode::Unsigned, data_name, background);
    bench_terse<std::uint32_t>(options, "uint32", jpa::Terse_mode::Small_unsigned, data_name, background);
    bench_terse<std::uint32_t>(options, "uint32", jpa::Terse_mode::Unsigned, data_name, background);
    bench_terse<std::int32_t>(options, "int32", jpa::Terse_mode::Signed, data_name, background);
}

[[noreturn]] void usage(char const* program) {
    std::fprintf(stderr, "Usage: %s [--filter SUBSTRING] [--repetitions N] [--pixels N] [--list]\n", program);
    std::exit(2);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string const arg = argv[i];
        bool const has_value = i + 1 < argc;
        if (arg == "--filter" && has_value)
            options.filter = argv[++i];
        else if (arg == "--repetitions" && has_value)
            options.repetitions = std::max(1, std::atoi(argv[++i]));
        else if (arg == "--pixels" && has_value)
            options.pixels = std::max<std::size_t>(block, std::strtoull(argv[++i], nullptr, 10));
        else if (arg == "--list")
            options.list = true;
        else
            usage(argv[0]);
    }
    if (!options.list)
        print_header();
    [&]<std::uint8_t... B>(std::integer_sequence<std::uint8_t, B...>) {
        (bench_bitqueue<B>(options), ...);
    }(std::integer_sequence<std::uint8_t, 1, 2, 3, 4, 6, 8, 12, 16>{});
    for (auto const& dataset : terse_benchmark::datasets)
        bench_terse_data(options, dataset.name, dataset.background);
    if (!g_ok)
        std::printf("\nSome checks FAILED.\n");
    return g_ok ? 0 : 1;
}
