// Synthetic data shared by the benchmarks in this directory.

#ifndef TERSE_BENCHMARK_DATA_HPP
#define TERSE_BENCHMARK_DATA_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace terse_benchmark {

// The number of values that Terse encodes per block by default.
inline constexpr std::size_t block = 12;

// The mean background counts per pixel of the synthetic frames. Sparse is typical of fast, low-dose experiments.
struct Dataset {
    char const* name;
    double background;
};
inline constexpr Dataset datasets[] = {{"sparse", 0.1}, {"weak", 2.0}, {"strong", 50.0}};

// splitmix64: a small, fast generator that gives the same sequence on all platforms.
class Random {
public:
    explicit Random(std::uint64_t seed) : d_state(seed) {}
    std::uint64_t operator()() {
        std::uint64_t z = (d_state += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }
    // Uniform in [0, 1).
    double uniform() { return static_cast<double>((*this)() >> 11) * 0x1.0p-53; }

private:
    std::uint64_t d_state;
};

// Draws Poisson distributed values with the inverse of the cumulative distribution.
class Poisson {
public:
    explicit Poisson(double mean) {
        double p = std::exp(-mean);
        double cumulative = p;
        d_cdf.push_back(cumulative);
        for (std::size_t k = 1; cumulative < 1.0 - 1e-15 && k < 100000; ++k) {
            p *= mean / static_cast<double>(k);
            cumulative += p;
            d_cdf.push_back(cumulative);
        }
    }
    std::uint64_t operator()(Random& random) const {
        double const u = random.uniform();
        return static_cast<std::uint64_t>(std::lower_bound(d_cdf.begin(), d_cdf.end(), u) - d_cdf.begin());
    }

private:
    std::vector<double> d_cdf;
};

// A diffraction-like frame: Poisson background of the given mean, Bragg spots with exponentially distributed
// intensities, and (for unsigned types) masked module gaps with the maximum value of T, as written by detectors.
template <typename T>
std::vector<T> make_frame(std::size_t const pixels, double const background) {
    std::size_t const width = static_cast<std::size_t>(std::sqrt(static_cast<double>(pixels)));
    std::size_t const height = pixels / width;
    std::vector<double> frame(width * height);
    Random random(42);
    Poisson const poisson(background);
    for (auto& value : frame)
        value = static_cast<double>(poisson(random));
    std::size_t const spots = frame.size() / 6000;
    for (std::size_t spot = 0; spot != spots; ++spot) {
        std::size_t const x = random() % width;
        std::size_t const y = random() % height;
        double const intensity = -2000.0 * std::log(1.0 - random.uniform());
        for (std::ptrdiff_t dy = -2; dy <= 2; ++dy)
            for (std::ptrdiff_t dx = -2; dx <= 2; ++dx) {
                std::ptrdiff_t const px = static_cast<std::ptrdiff_t>(x) + dx;
                std::ptrdiff_t const py = static_cast<std::ptrdiff_t>(y) + dy;
                if (px >= 0 && px < static_cast<std::ptrdiff_t>(width) && py >= 0 && py < static_cast<std::ptrdiff_t>(height)) {
                    double const r2 = static_cast<double>(dx * dx + dy * dy);
                    frame[static_cast<std::size_t>(py) * width + static_cast<std::size_t>(px)] +=
                        std::round(intensity * std::exp(-r2 / 2.0));
                }
            }
    }
    double const max_value = std::is_signed_v<T> ? static_cast<double>(std::numeric_limits<T>::max() / 2)
                                                 : static_cast<double>(std::numeric_limits<T>::max() - 1);
    std::vector<T> result(frame.size());
    for (std::size_t i = 0; i != frame.size(); ++i)
        result[i] = static_cast<T>(std::min(frame[i], max_value));
    if constexpr (std::is_unsigned_v<T>)
        for (std::size_t y = 0; y != height; ++y)
            for (std::size_t x = 0; x != width; ++x)
                if (y % 514 < 10 || x % 1030 < 8)
                    result[y * width + x] = std::numeric_limits<T>::max();
    return result;
}

} // namespace terse_benchmark

#endif // TERSE_BENCHMARK_DATA_HPP
