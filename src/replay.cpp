// Replay a recorded real market-data feed (data/feed.csv from record_binance.py)
// through the order book and report latency + throughput on real data.
//
// The CSV holds normalized L2 level updates "side,tick,qty" (qty in micro-units,
// 0 = level cleared). A feed handler turns each level update into a book op:
//   qty>0, level not live -> add ;  qty>0, live -> modify ;  qty==0, live -> cancel
// id is derived from (tick, side) since L2 has one aggregate order per level.
//
// This is a simplified replay (not sequence-synced to a REST snapshot); it exists
// to benchmark the book on real message shape/rates, not to trade.
#include "lob/order_book.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Row { int side; int32_t tick; uint32_t qty; };

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::size_t k = static_cast<std::size_t>(p / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "data/feed.csv";
    std::ifstream in(path);
    if (!in) { std::fprintf(stderr, "cannot open %s (run data/record_binance.py first)\n", path); return 1; }

    std::vector<Row> rows;
    int32_t max_tick = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        int side; long tick; long long qty;
        if (std::sscanf(line.c_str(), "%d,%ld,%lld", &side, &tick, &qty) != 3) continue;
        if (tick < 0) continue;
        rows.push_back({side, (int32_t)tick, (uint32_t)std::min<long long>(qty, 0xFFFFFFFFll)});
        max_tick = std::max<int32_t>(max_tick, (int32_t)tick);
    }
    if (rows.empty()) { std::fprintf(stderr, "no rows parsed\n"); return 1; }

    OrderBook book(max_tick + 16);
    std::unordered_set<uint64_t> live;
    live.reserve(rows.size());
    std::vector<double> lat;
    lat.reserve(rows.size());

    for (const auto& r : rows) {
        uint64_t id = (uint64_t(r.tick) << 1) | uint64_t(r.side);
        Side side = r.side == 0 ? Side::Buy : Side::Sell;
        auto t0 = Clock::now();
        if (r.qty > 0) {
            if (live.insert(id).second) book.add(id, side, r.tick, r.qty);
            else                        book.modify(id, r.qty);
        } else {
            if (live.erase(id))         book.cancel(id);
        }
        auto t1 = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    double total_ns = 0; for (double x : lat) total_ns += x;
    double thr = rows.size() / (total_ns / 1e9);
    std::vector<double> tmp = lat;
    int32_t ask = book.best_ask();
    std::printf("replayed %zu real level updates from %s\n", rows.size(), path);
    std::printf("top of book: bid $%d / ask $%d\n", book.best_bid(),
                ask > max_tick ? -1 : ask);
    std::printf("book op latency: p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns  |  %.2f M op/s\n",
                pct(tmp, 50), pct(tmp, 99), pct(tmp, 99.9), thr / 1e6);
    return 0;
}
