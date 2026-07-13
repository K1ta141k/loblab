// Latency + throughput benchmark: replays a synthetic order-flow stream through
// both the optimized OrderBook and the naive baseline, reports per-op latency
// percentiles and the speedup.
//
// NOTE on timing: this uses std::chrono::steady_clock so it builds portably
// (incl. Apple Silicon). On HRT's x86 Linux, swap in rdtsc + a cycles->ns
// calibration and pin the thread to an isolated core (taskset) for cleaner tails.
#include "lob/order_book.hpp"
#include "lob/naive_book.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

static std::vector<Message> gen_flow(std::size_t n, int32_t max_tick, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::vector<Message> out;
    out.reserve(n);
    std::vector<uint64_t> live;           // currently resting ids
    live.reserve(n);
    uint64_t next_id = 1;
    int32_t mid = max_tick / 2;
    std::normal_distribution<double> walk(0.0, 3.0);
    std::uniform_int_distribution<int> act(0, 99);
    std::uniform_int_distribution<int> spread(1, 20);

    for (std::size_t i = 0; i < n; ++i) {
        mid = std::clamp(mid + static_cast<int32_t>(walk(rng)), 50, max_tick - 50);
        int a = act(rng);
        if (a < 70 || live.empty()) {              // 70% adds
            bool buy = (rng() & 1);
            int32_t tick = buy ? mid - spread(rng) : mid + spread(rng);
            tick = std::clamp(tick, 0, max_tick);
            out.push_back({MsgType::Add, buy ? Side::Buy : Side::Sell, tick,
                           1 + (rng() % 100), next_id});
            live.push_back(next_id++);
        } else if (a < 90) {                        // 20% cancels
            std::size_t k = rng() % live.size();
            out.push_back({MsgType::Cancel, Side::Buy, 0, 0, live[k]});
            live[k] = live.back(); live.pop_back();
        } else {                                    // 10% modifies
            std::size_t k = rng() % live.size();
            out.push_back({MsgType::Modify, Side::Buy, 0, 1 + (rng() % 100), live[k]});
        }
    }
    return out;
}

template <class Book>
static std::vector<double> run(Book& book, const std::vector<Message>& flow) {
    std::vector<double> lat;
    lat.reserve(flow.size());
    for (const auto& m : flow) {
        auto t0 = Clock::now();
        switch (m.type) {
            case MsgType::Add:    book.add(m.id, m.side, m.tick, m.qty); break;
            case MsgType::Cancel: book.cancel(m.id); break;
            case MsgType::Modify: book.modify(m.id, m.qty); break;
        }
        auto t1 = Clock::now();
        lat.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return lat;
}

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::size_t k = static_cast<std::size_t>(p / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

template <class Book>
static double report(const char* name, const std::vector<Message>& flow, int32_t max_tick) {
    Book book(max_tick);
    auto lat = run(book, flow);
    double total_ns = 0; for (double x : lat) total_ns += x;
    double thr = flow.size() / (total_ns / 1e9);
    std::vector<double> tmp = lat;
    double p50 = pct(tmp, 50), p99 = pct(tmp, 99), p999 = pct(tmp, 99.9);
    std::printf("%-14s  p50 %6.0f ns  p99 %7.0f ns  p99.9 %8.0f ns  |  %6.2f M msg/s\n",
                name, p50, p99, p999, thr / 1e6);
    return thr;
}

int main(int argc, char** argv) {
    std::size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    int32_t max_tick = 100'000;
    std::printf("Replaying %zu synthetic messages (70%% add / 20%% cancel / 10%% modify)\n\n", n);
    auto flow = gen_flow(n, max_tick);

    double thr_opt = report<OrderBook>("OrderBook",  flow, max_tick);
    double thr_nai = report<NaiveBook>("NaiveBook",  flow, max_tick);
    std::printf("\nspeedup (throughput): %.1fx\n", thr_opt / thr_nai);
    return 0;
}
