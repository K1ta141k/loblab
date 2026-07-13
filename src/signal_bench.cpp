// Order-book-imbalance (OBI) strategy on top of the book, end to end:
//   apply market update -> read the book -> compute the signal -> decide -> submit.
// We time the signal->order path (read book to submit returns) per update.
//
// OBI over the top-K levels:  obi = (bid_depth - ask_depth) / (bid_depth + ask_depth)
//   obi > +T  => bids dominate, expect an uptick  => take the offer (aggressive BUY)
//   obi < -T  => asks dominate, expect a downtick => hit the bid    (aggressive SELL)
// (Illustrative microstructure signal, not a calibrated alpha.)
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
    const int    K = 5;        // levels for the imbalance
    const double T = 0.30;     // fire threshold
    const int32_t max_tick = 100'000;

    OrderBook book(max_tick);
    std::mt19937 rng(3);
    std::vector<uint64_t> live; live.reserve(n);
    uint64_t next_id = 1; int32_t mid = max_tick / 2;
    std::normal_distribution<double> walk(0.0, 3.0);
    std::uniform_int_distribution<int> act(0, 99), spread(1, 12), sz(1, 30);

    std::vector<double> lat; lat.reserve(n);
    std::vector<Fill> fills; fills.reserve(16);
    uint64_t fired = 0, trades = 0;

    for (std::size_t i = 0; i < n; ++i) {
        // --- one market-data update maintaining a two-sided book ---
        mid = std::clamp(mid + (int32_t)walk(rng), 100, max_tick - 100);
        int a = act(rng);
        if (a < 70 || live.empty()) {
            bool buy = rng() & 1;
            int32_t tick = std::clamp(buy ? mid - spread(rng) : mid + spread(rng), 1, max_tick - 1);
            uint64_t id = next_id++;
            book.add(id, buy ? Side::Buy : Side::Sell, tick, 1u + sz(rng));
            live.push_back(id);
        } else if (a < 88) {
            std::size_t k = rng() % live.size();
            book.cancel(live[k]); live[k] = live.back(); live.pop_back();
        } else {
            std::size_t k = rng() % live.size();
            book.modify(live[k], 1u + sz(rng));
        }

        // --- signal -> order (timed) ---
        auto t0 = Clock::now();
        uint64_t bd = book.top_depth(Side::Buy, K);
        uint64_t ad = book.top_depth(Side::Sell, K);
        uint64_t tot = bd + ad;
        if (tot) {
            double obi = (double(bd) - double(ad)) / double(tot);
            if (obi > T && book.best_ask() <= max_tick) {
                fills.clear();
                book.submit(next_id++, Side::Buy, book.best_ask(), 1, /*market=*/false, fills);
                ++fired; trades += fills.size();
            } else if (obi < -T && book.best_bid() >= 0) {
                fills.clear();
                book.submit(next_id++, Side::Sell, book.best_bid(), 1, false, fills);
                ++fired; trades += fills.size();
            }
        }
        auto t1 = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    std::vector<double> tmp = lat;
    std::printf("OBI strategy: %zu updates, top-%d imbalance, threshold %.2f\n", n, K, T);
    std::printf("signal->order latency: p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns\n",
                pct(tmp, 50), pct(tmp, 99), pct(tmp, 99.9));
    std::printf("fired %llu orders (%.1f%% of updates), %llu fills\n",
                (unsigned long long)fired, 100.0 * fired / n, (unsigned long long)trades);
    return 0;
}
