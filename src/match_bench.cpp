// Matching-engine latency bench: submit marketable orders that cross a live book
// and measure submit() latency and fill rate. Liquidity is replenished so the book
// keeps depth. steady_clock timing (swap rdtsc on x86 for tighter tails).
#include "lob/order_book.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;
using Fill = OrderBook::Fill;

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::size_t k = (std::size_t)(p / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

int main(int argc, char** argv) {
    const std::size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 1'000'000;
    const int32_t max_tick = 100'000, mid = max_tick / 2;
    OrderBook book(max_tick);
    std::mt19937 rng(1);
    uint64_t next_id = 1;
    std::uniform_int_distribution<int> depth(1, 8), sz(1, 20), cross(1, 4);

    // seed a deep book: 20 levels each side, several orders per level
    auto replenish = [&] {
        for (int d = 1; d <= 20; ++d) {
            book.add(next_id++, Side::Buy,  mid - d, 1u + sz(rng));
            book.add(next_id++, Side::Sell, mid + d, 1u + sz(rng));
        }
    };
    replenish();

    std::vector<double> lat; lat.reserve(n);
    std::vector<Fill> fills; fills.reserve(64);
    uint64_t total_fills = 0;

    for (std::size_t i = 0; i < n; ++i) {
        bool buy = rng() & 1;
        // marketable limit that crosses ~1-4 levels
        int32_t px = buy ? mid + cross(rng) : mid - cross(rng);
        uint32_t qty = 1u + sz(rng) * (uint32_t)depth(rng);
        fills.clear();
        auto t0 = Clock::now();
        book.submit(next_id++, buy ? Side::Buy : Side::Sell, px, qty, /*market=*/false, fills);
        auto t1 = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
        total_fills += fills.size();
        // top up liquidity periodically so the book keeps depth
        if ((i & 1023) == 0) replenish();
    }

    double total_ns = 0; for (double x : lat) total_ns += x;
    std::vector<double> tmp = lat;
    std::printf("matching bench: %zu marketable orders\n", n);
    std::printf("submit latency: p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  |  %.2f M submit/s\n",
                pct(tmp, 50), pct(tmp, 99), pct(tmp, 99.9), n / (total_ns / 1e9) / 1e6);
    std::printf("avg fills/order: %.2f  (%llu fills total)\n",
                double(total_fills) / n, (unsigned long long)total_fills);
    return 0;
}
