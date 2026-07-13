// Correctness cross-check: replay the same synthetic flow through both books and
// assert the top-of-book agrees at every step. Zero-dependency (asserts + exit code).
#include "lob/order_book.hpp"
#include "lob/naive_book.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

using namespace lob;

int main() {
    const int32_t max_tick = 2000;
    OrderBook opt(max_tick);
    NaiveBook nai(max_tick);

    std::mt19937 rng(7);
    std::vector<uint64_t> live;
    uint64_t next_id = 1;
    int32_t mid = 1000;
    std::normal_distribution<double> walk(0.0, 2.0);
    int failures = 0;

    for (int i = 0; i < 200000; ++i) {
        mid = std::clamp(mid + (int)walk(rng), 50, max_tick - 50);
        int a = rng() % 100;
        if (a < 70 || live.empty()) {
            bool buy = rng() & 1;
            Side side = buy ? Side::Buy : Side::Sell;
            int32_t tick = std::clamp(buy ? mid - (int)(1 + rng() % 15)
                                          : mid + (int)(1 + rng() % 15), 0, max_tick);
            uint32_t q = 1 + rng() % 50;             // one draw, identical to both books
            uint64_t id = next_id++;
            opt.add(id, side, tick, q);
            nai.add(id, side, tick, q);
            live.push_back(id);
        } else if (a < 90) {
            std::size_t k = rng() % live.size();
            opt.cancel(live[k]); nai.cancel(live[k]);
            live[k] = live.back(); live.pop_back();
        } else {
            std::size_t k = rng() % live.size();
            uint32_t q = 1 + rng() % 50;
            opt.modify(live[k], q); nai.modify(live[k], q);
        }

        int32_t ob = opt.best_bid();
        int32_t nb = nai.best_bid();
        int32_t oa = (opt.best_ask() == max_tick + 1) ? -1 : opt.best_ask();
        int32_t na = nai.best_ask();
        if (ob != nb || oa != na) {
            if (failures < 5)
                std::printf("MISMATCH @%d  bid opt=%d nai=%d  ask opt=%d nai=%d\n",
                            i, ob, nb, oa, na);
            ++failures;
        }
    }
    if (failures) { std::printf("FAILED: %d mismatches\n", failures); return 1; }
    std::printf("OK: top-of-book matched across 200k messages\n");
    return 0;
}
