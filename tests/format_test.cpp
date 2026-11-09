// Checks that compression produces exactly the bytes of the streams below, and that these streams decompress into
// the original data.
//
// Changes of the file format must be deliberate: files that were written earlier must remain readable, and the other
// readers of the format (e.g. ImageJ/TRPX_Reader.java and DIALS_integration/FormatTRPX.py) must be able to read new
// files. The expected streams, in format_test_streams.inc, were written by the Terse.hpp of commit 8044e46.
//
// The data of each case consist of blocks of the size that Terse encodes, each with a prescribed maximum, so that all
// ways in which blocks are encoded occur: in particular, for Terse_mode::Small_unsigned, blocks with maxima of 0 to 6
// (packed as the digits of a single number in base max + 1), the encoding of the maximum relative to that of the
// previous block, blocks with more significant bits (relative to the previous block, and absolute in all ranges), and
// runs of masked blocks (with pixels of which all bits are set), in which the blocks are encoded after adding 1 to
// all values. The last block of each frame is incomplete.
//
// Run with --print to print the streams of the current implementation in the format of format_test_streams.inc, e.g.
// when adding a case.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Concurrent.hpp must be included before Terse.hpp.
#include "Concurrent.hpp"
#include "Terse.hpp"

namespace {

// splitmix64: a small generator that gives the same sequence on all platforms.
class Random {
public:
    explicit Random(std::uint64_t seed) : d_state(seed) {}
    std::uint64_t operator()() {
        std::uint64_t z = (d_state += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t d_state;
};

// A block of the test data. The values are drawn from [0, max], or from [-max, max] for signed types, and at least
// one of them is max (or -max). Masked blocks contain pixels with all bits set (for unsigned types only) as well: all
// pixels if max is all_masked, otherwise about a third of them.
struct Block {
    std::uint64_t max;
    bool masked = false;
};
constexpr std::uint64_t all_masked = ~std::uint64_t{0};

using Profile = std::vector<Block>;

Profile operator+(Profile a, Profile const& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// Maxima below 7: repeated zeros, changes of +1 and -1, the change from 6 to 4, and arbitrary changes.
Profile const weak = {{0}, {0}, {0}, {1}, {1}, {0}, {2}, {3}, {4}, {5}, {6}, {4}, {6}, {5}, {4}, {3}, {2}, {1},
                      {0}, {0}, {2}, {6}, {3}, {1}, {5}, {0}};

// Strong blocks with up to 7 significant bits, alternating with weak blocks.
Profile const strong_7 = {{7}, {8}, {9}, {15}, {16}, {31}, {32}, {30}, {63}, {64}, {100}, {127}, {126},
                          {3}, {7}, {2}, {0}, {6}, {100}, {4}, {65}, {7}};

Profile const strong_15 = strong_7 + Profile{{255}, {256}, {511}, {600}, {1023}, {1024}, {2000}, {4095}, {8191},
                                             {16383}, {32767}, {32000}, {9}, {1000}, {3}, {20000}};

Profile const strong_31 = strong_15 + Profile{{65535}, {65536}, {131071}, {1u << 20}, {1u << 24}, {(1u << 31) - 1},
                                              {1u << 30}, {5}, {1u << 18}, {6}};

Profile const strong_63 = strong_31 + Profile{{std::uint64_t{1} << 32}, {std::uint64_t{1} << 40},
                                              {(std::uint64_t{1} << 63) - 1}, {std::uint64_t{1} << 62}, {2},
                                              {std::uint64_t{1} << 50}};

// Masked blocks: all masked, with other values of up to 0 to 6 (blocks with maxima of 1 to 7 after adding 1), and with
// larger values. A block without masked pixels ends the run of masked blocks. The last run reaches the end of the frame.
Profile masked(std::uint64_t const large) {
    return {{all_masked, true}, {0, true}, {1, true}, {2, true}, {3, true}, {4, true}, {5, true}, {6, true},
            {all_masked, true}, {large, true}, {3, true}, {3}, {6}, {all_masked, true}, {large / 2, true}, {4},
            {large}, {2, true}, {all_masked, true}, {5, true}};
}

// The magnitudes of signed values.
Profile const signed_7 = {{0}, {0}, {1}, {1}, {0}, {2}, {3}, {6}, {7}, {8}, {15}, {16}, {100}, {127}, {1}, {0}, {64}};
Profile const signed_15 = signed_7 + Profile{{128}, {255}, {1000}, {16383}, {32767}, {3}, {20000}, {0}};
Profile const signed_31 = signed_15 + Profile{{65535}, {1u << 20}, {(1u << 31) - 1}, {1u << 30}, {2}, {1u << 25}};

template <typename T>
std::vector<T> make_data(Profile const& profile, std::size_t const block, std::uint64_t const seed) {
    Random random(seed);
    std::vector<T> data;
    for (Block const& b : profile) {
        std::size_t const start = data.size();
        for (std::size_t i = 0; i != block; ++i) {
            if (b.masked && (b.max == all_masked || random() % 3 == 0))
                data.push_back(std::numeric_limits<T>::max());
            else if constexpr (std::is_signed_v<T>) {
                auto const magnitude = static_cast<std::int64_t>(random() % (b.max + 1));
                data.push_back(static_cast<T>(random() % 2 ? -magnitude : magnitude));
            }
            else
                data.push_back(static_cast<T>(random() % (b.max + 1)));
        }
        if (b.max != all_masked) {
            std::size_t const i = start + random() % block;
            if constexpr (std::is_signed_v<T>)
                data[i] = static_cast<T>(random() % 2 ? -static_cast<std::int64_t>(b.max) : static_cast<std::int64_t>(b.max));
            else
                data[i] = static_cast<T>(b.max);
            if (b.masked)
                data[start + (i - start + 1) % block] = std::numeric_limits<T>::max();
        }
    }
    data.resize(data.size() - block / 2); // An incomplete last block.
    return data;
}

std::string to_hex(std::string const& bytes) {
    static char const digits[] = "0123456789abcdef";
    std::string hex;
    for (char const c : bytes) {
        auto const byte = static_cast<unsigned char>(c);
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

std::string from_hex(std::string_view const hex) {
    auto const value = [](char const c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    std::string bytes;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        bytes += static_cast<char>(value(hex[i]) * 16 + value(hex[i + 1]));
    return bytes;
}

struct Expected {
    char const* name;
    char const* hex;
};

// The expected streams, generated with --print.
Expected const expected[] = {
#include "format_test_streams.inc"
};

bool g_print = false;
bool g_ok = true;

// Reports a failure of the current case.
bool g_case_ok = true;
void fail(char const* name, std::string const& message) {
    std::printf("%-30s FAILED: %s\n", name, message.c_str());
    g_ok = g_case_ok = false;
}

// Decompresses frame 'frame' of 'terse' into values of type OUT and compares these with 'data'.
template <typename OUT, typename T>
bool decompresses_to(jpa::Terse<>& terse, std::size_t const frame, std::vector<T> const& data) {
    std::vector<OUT> unpacked(data.size());
    terse.prolix(unpacked.begin(), frame);
    for (std::size_t i = 0; i != data.size(); ++i)
        if (unpacked[i] != static_cast<OUT>(data[i]))
            return false;
    return true;
}

// Compresses the frames, compares the stream with the expected one, and decompresses the expected stream into values
// of type T and of the types OUT.
template <typename T, typename... OUT>
void check(char const* name, jpa::Terse_mode const mode, std::size_t const block, std::vector<std::vector<T>> const& frames) {
    jpa::Terse<> terse;
    terse.block_size(block);
    for (auto const& frame : frames)
        terse.push_back(frame.data(), frame.size(), mode);
    std::ostringstream stream;
    terse.write(stream);
    std::string const bytes = stream.str();
    if (g_print) {
        std::printf("    {\"%s\",\n", name);
        std::string const hex = to_hex(bytes);
        for (std::size_t i = 0; i < hex.size(); i += 112)
            std::printf("     \"%s\"%s\n", hex.substr(i, 112).c_str(), i + 112 < hex.size() ? "" : "},");
        return;
    }
    g_case_ok = true;
    char const* expected_hex = nullptr;
    for (Expected const& e : expected)
        if (std::string_view(e.name) == name)
            expected_hex = e.hex;
    if (expected_hex == nullptr)
        return fail(name, "no expected stream");
    std::string const expected_bytes = from_hex(expected_hex);
    if (bytes != expected_bytes) {
        std::size_t i = 0;
        while (i < bytes.size() && i < expected_bytes.size() && bytes[i] == expected_bytes[i])
            ++i;
        fail(name, "the stream differs from the expected stream at byte " + std::to_string(i) + " (sizes " +
                       std::to_string(bytes.size()) + " and " + std::to_string(expected_bytes.size()) + ")");
    }
    std::istringstream expected_stream(expected_bytes);
    jpa::Terse<> read(expected_stream);
    if (read.number_of_frames() != frames.size())
        return fail(name, "the expected stream has " + std::to_string(read.number_of_frames()) + " frames");
    for (std::size_t frame = 0; frame != frames.size(); ++frame) {
        if (!decompresses_to<T>(read, frame, frames[frame]))
            fail(name, "frame " + std::to_string(frame) + " decompresses into different values");
        if (!(decompresses_to<OUT>(read, frame, frames[frame]) && ...))
            fail(name, "frame " + std::to_string(frame) + " decompresses into different values of a wider type");
    }
    if (g_case_ok)
        std::printf("%-30s ok (%zu bytes)\n", name, bytes.size());
}

template <typename T, typename... OUT>
void check(char const* name, jpa::Terse_mode const mode, std::size_t const block, Profile const& profile) {
    check<T, OUT...>(name, mode, block, std::vector<std::vector<T>>{make_data<T>(profile, block, 1)});
}

} // namespace

int main(int argc, char** argv) {
    g_print = argc > 1 && std::string_view(argv[1]) == "--print";
    using Mode = jpa::Terse_mode;
    // Terse_mode::Small_unsigned, for all unsigned types, and for blocks sizes of which the decoder uses a block size
    // that is known at compile time (12, 20), or not (7).
    check<std::uint8_t>("small_unsigned/uint8", Mode::Small_unsigned, 12, weak + strong_7 + masked(100));
    check<std::uint16_t>("small_unsigned/uint16", Mode::Small_unsigned, 12, weak + strong_15 + masked(5000));
    check<std::uint32_t>("small_unsigned/uint32", Mode::Small_unsigned, 12, weak + strong_31 + masked(1u << 20));
    check<std::uint64_t>("small_unsigned/uint64", Mode::Small_unsigned, 12, weak + strong_63 + masked(1u << 30));
    check<std::uint16_t>("small_unsigned/uint16/block7", Mode::Small_unsigned, 7, weak + strong_15 + masked(5000));
    check<std::uint16_t>("small_unsigned/uint16/block20", Mode::Small_unsigned, 20, weak + strong_15 + masked(5000));
    // Decompressed into wider types, without masked pixels, which do not decompress into the same values in a wider
    // type. Not into signed types: unsigned values decompress into signed values as if they were signed.
    check<std::uint16_t, std::uint32_t, std::uint64_t, double>(
        "small_unsigned/uint16/wider", Mode::Small_unsigned, 12, weak + strong_15);
    // A run of masked blocks that is long enough to make the compressed frame grow during the run.
    check<std::uint16_t>("small_unsigned/uint16/long_run", Mode::Small_unsigned, 12,
                         weak + Profile(24, Block{5000, true}) + weak);
    check<std::uint16_t>("small_unsigned/uint16/frames", Mode::Small_unsigned, 12,
                         std::vector<std::vector<std::uint16_t>>{
                             make_data<std::uint16_t>(weak + strong_15 + masked(5000), 12, 2),
                             make_data<std::uint16_t>(masked(5000) + weak + strong_15, 12, 3)});
    // Terse_mode::Unsigned.
    check<std::uint8_t>("unsigned/uint8", Mode::Unsigned, 12, weak + strong_7 + masked(100));
    check<std::uint16_t>("unsigned/uint16", Mode::Unsigned, 12, weak + strong_15 + masked(5000));
    check<std::uint32_t>("unsigned/uint32", Mode::Unsigned, 12, weak + strong_31 + masked(1u << 20));
    check<std::uint16_t, std::uint32_t, std::uint64_t, double>(
        "unsigned/uint16/wider", Mode::Unsigned, 12, weak + strong_15);
    // Terse_mode::Signed.
    check<std::int8_t>("signed/int8", Mode::Signed, 12, signed_7);
    check<std::int16_t>("signed/int16", Mode::Signed, 12, signed_15);
    check<std::int32_t, std::int64_t, double>("signed/int32", Mode::Signed, 12, signed_31);
    check<std::int16_t>("signed/int16/block7", Mode::Signed, 7, signed_15);
    if (!g_print)
        std::printf(g_ok ? "All streams are as expected.\n" : "Some checks FAILED.\n");
    return g_ok ? 0 : 1;
}
