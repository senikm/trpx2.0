// Google Benchmark micro benchmarks of Bitqueue and of Terse compression (Terse constructor) and decompression
// (prolix()), single-threaded. Built by CMake with -DBUILD_BENCHMARKS=ON when Google Benchmark is found.
//
// Besides the options of Google Benchmark (see --help), it takes --pixels=N, the number of pixels of the synthetic
// frames of the Terse benchmarks (default 2048 x 2048). Examples:
//
//   terse_gbenchmark --benchmark_filter=prolix
//   terse_gbenchmark --benchmark_repetitions=10 --benchmark_report_aggregates_only=true
//   terse_gbenchmark --benchmark_out=before.json --benchmark_out_format=json
//
// Two JSON outputs can be compared with tools/compare.py of Google Benchmark, which also tests whether differences
// are significant. The counter 'per_value' is the time per encoded value; bytes_per_second refers to the
// uncompressed data. The Bitqueue benchmarks use a buffer that fits in the L2 cache, so that they measure the bit
// packing rather than memory bandwidth.

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Concurrent.hpp must be included before Terse.hpp.
#include "Concurrent.hpp"
#include "Bitqueue.hpp"
#include "Terse.hpp"
#include "benchmark_data.hpp"

namespace {

using terse_benchmark::block;
using terse_benchmark::Dataset;

// The number of values of the Bitqueue benchmarks: 192 kB of std::uint32_t.
constexpr std::size_t bitqueue_values = 4096 * block;

void set_counters(benchmark::State& state, std::size_t const values, std::size_t const bytes) {
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(bytes));
    state.counters["per_value"] = benchmark::Counter(static_cast<double>(values),
                                                     benchmark::Counter::kIsIterationInvariantRate |
                                                         benchmark::Counter::kInvert);
}

std::vector<std::uint32_t> random_values(std::uint8_t const bits) {
    std::vector<std::uint32_t> values(bitqueue_values);
    terse_benchmark::Random random(bits);
    for (auto& value : values)
        value = static_cast<std::uint32_t>(random() & ((std::uint64_t{1} << bits) - 1));
    return values;
}

// Room for the packed values, and for the last word that Bitqueue_push_back flushes.
std::vector<std::uint8_t> bit_buffer(std::uint8_t const bits) {
    return std::vector<std::uint8_t>((bitqueue_values * bits + 63) / 64 * 8 + 8);
}

// Pushes blocks of 12 values, with the bit width as a template argument (STATIC) or as a function argument
// (as Terse does when compressing).
template <std::uint8_t B, bool STATIC>
void bitqueue_push_back(benchmark::State& state) {
    std::vector<std::uint32_t> const values = random_values(B);
    std::vector<std::uint8_t> bits = bit_buffer(B);
    for (auto _ : state) {
        {
            jpa::Bitqueue_push_back queue(bits);
            for (std::size_t i = 0; i != values.size(); i += block)
                if constexpr (STATIC)
                    queue.push_back<B>(std::span<std::uint32_t const, block>(values.data() + i, block));
                else
                    queue.push_back(B, std::span<std::uint32_t const>(values.data() + i, block));
        }
        benchmark::ClobberMemory();
    }
    set_counters(state, values.size(), values.size() * sizeof(std::uint32_t));
}

// Pops blocks of 12 values, with the bit width as a template argument (STATIC, as Terse does when decompressing
// blocks of at most 8 bits) or as a function argument.
template <std::uint8_t B, bool STATIC>
void bitqueue_pop(benchmark::State& state) {
    std::vector<std::uint32_t> const values = random_values(B);
    std::vector<std::uint8_t> bits = bit_buffer(B);
    {
        jpa::Bitqueue_push_back queue(bits);
        queue.push_back(B, std::span<std::uint32_t const>(values));
    }
    std::vector<std::uint32_t> popped(values.size());
    for (auto _ : state) {
        jpa::Bitqueue_pop queue(bits);
        for (std::size_t i = 0; i != popped.size(); i += block)
            if constexpr (STATIC)
                queue.pop<B>(std::span<std::uint32_t, block>(popped.data() + i, block));
            else
                queue.pop(B, std::span<std::uint32_t>(popped.data() + i, block));
        std::uint32_t* data = popped.data();
        benchmark::DoNotOptimize(data);
        benchmark::ClobberMemory();
    }
    if (popped != values)
        state.SkipWithError("The popped values differ from the pushed values");
    set_counters(state, values.size(), values.size() * sizeof(std::uint32_t));
}

template <std::uint8_t B>
void register_bitqueue() {
    std::string const prefix = "bitqueue/B=" + std::to_string(B) + "/";
    benchmark::RegisterBenchmark((prefix + "push_back<B>").c_str(), bitqueue_push_back<B, true>)
        ->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark((prefix + "push_back(B)").c_str(), bitqueue_push_back<B, false>)
        ->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark((prefix + "pop<B>").c_str(), bitqueue_pop<B, true>)->Unit(benchmark::kMicrosecond);
    benchmark::RegisterBenchmark((prefix + "pop(B)").c_str(), bitqueue_pop<B, false>)->Unit(benchmark::kMicrosecond);
}

// The frames are generated once per type and dataset, the first time that a benchmark that uses them runs.
template <typename T>
std::vector<T> const& frame(Dataset const& dataset, std::size_t const pixels) {
    static std::map<std::string, std::vector<T>> frames;
    auto [it, inserted] = frames.try_emplace(dataset.name);
    if (inserted)
        it->second = terse_benchmark::make_frame<T>(pixels, dataset.background);
    return it->second;
}

template <typename T>
void terse_compress(benchmark::State& state, Dataset const dataset, jpa::Terse_mode const mode,
                    std::size_t const pixels) {
    std::vector<T> const& data = frame<T>(dataset, pixels);
    std::size_t terse_size = 0;
    for (auto _ : state) {
        jpa::Terse<> terse(data.data(), data.size(), block, mode);
        terse_size = terse.terse_size();
        benchmark::DoNotOptimize(terse_size);
    }
    set_counters(state, data.size(), data.size() * sizeof(T));
    state.counters["ratio"] = static_cast<double>(data.size() * sizeof(T)) / static_cast<double>(terse_size);
}

template <typename T>
void terse_prolix(benchmark::State& state, Dataset const dataset, jpa::Terse_mode const mode,
                  std::size_t const pixels) {
    std::vector<T> const& data = frame<T>(dataset, pixels);
    jpa::Terse<> terse(data.data(), data.size(), block, mode);
    std::vector<T> unpacked(data.size());
    for (auto _ : state) {
        terse.prolix(unpacked.data());
        T* unpacked_data = unpacked.data();
        benchmark::DoNotOptimize(unpacked_data);
        benchmark::ClobberMemory();
    }
    if (unpacked != data)
        state.SkipWithError("The decompressed data differ from the original data");
    set_counters(state, data.size(), data.size() * sizeof(T));
    state.counters["ratio"] = static_cast<double>(data.size() * sizeof(T)) / static_cast<double>(terse.terse_size());
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
void register_terse(Dataset const& dataset, char const* type_name, jpa::Terse_mode const mode,
                    std::size_t const pixels) {
    std::string const prefix = std::string("terse/") + dataset.name + "/" + type_name + "/" + mode_name(mode) + "/";
    benchmark::RegisterBenchmark((prefix + "compress").c_str(), terse_compress<T>, dataset, mode, pixels)
        ->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark((prefix + "prolix").c_str(), terse_prolix<T>, dataset, mode, pixels)
        ->Unit(benchmark::kMillisecond);
}

} // namespace

int main(int argc, char** argv) {
    // Takes --pixels=N out of the arguments, and leaves the others to Google Benchmark.
    std::size_t pixels = 2048 * 2048;
    int kept = 1;
    for (int i = 1; i < argc; ++i) {
        std::string_view const arg = argv[i];
        if (arg.starts_with("--pixels="))
            pixels = std::max<std::size_t>(block, std::strtoull(argv[i] + 9, nullptr, 10));
        else
            argv[kept++] = argv[i];
    }
    argc = kept;

    [&]<std::uint8_t... B>(std::integer_sequence<std::uint8_t, B...>) {
        (register_bitqueue<B>(), ...);
    }(std::integer_sequence<std::uint8_t, 1, 2, 3, 4, 6, 8, 12, 16>{});
    for (Dataset const& dataset : terse_benchmark::datasets) {
        register_terse<std::uint16_t>(dataset, "uint16", jpa::Terse_mode::Small_unsigned, pixels);
        register_terse<std::uint16_t>(dataset, "uint16", jpa::Terse_mode::Unsigned, pixels);
        register_terse<std::uint32_t>(dataset, "uint32", jpa::Terse_mode::Small_unsigned, pixels);
        register_terse<std::uint32_t>(dataset, "uint32", jpa::Terse_mode::Unsigned, pixels);
        register_terse<std::int32_t>(dataset, "int32", jpa::Terse_mode::Signed, pixels);
    }

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
        return 1;
    benchmark::AddCustomContext("pixels", std::to_string(pixels));
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
