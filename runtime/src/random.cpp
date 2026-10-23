// The seeds std.math's default generators start from, one for each thread that draws a number
// before SeedRandom, different on each run.

#include "fb/runtime.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>

namespace {

/// How many seeds the program has made, so two made on the same tick still differ.
std::atomic<std::uint64_t> g_seeds = 0;

/// 64 bits from the OS's randomness: rand_s on Windows, and getrandom or /dev/urandom elsewhere.
std::uint64_t device_bits() {
    std::random_device device;
    const std::uint64_t high = device();
    return (high << 32U) | device();
}

} // namespace

extern "C" {

std::uint64_t fb_random_seed() {
    const auto ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::uint64_t seed = fb_hash_integer(device_bits());
    seed = fb_hash_combine(seed, fb_hash_integer(static_cast<std::uint64_t>(ticks)));
    return fb_hash_combine(seed, fb_hash_integer(g_seeds.fetch_add(1)));
}

} // extern "C"
