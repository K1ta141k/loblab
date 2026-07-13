// Multi-threaded benchmark of the lock-free SPSC feed->book handoff.
//
// Two honest measurements (they trade off against each other by Little's law):
//   1. Handoff latency  — one message in flight (depth 1): the true time for a
//      message to cross the ring and be applied, no queuing behind a backlog.
//   2. Sustained throughput — flood mode: how many msg/s the pipeline sustains
//      when the producer is allowed to run flat out.
//
// On x86 Linux, pin the two threads to isolated cores (taskset -c) and swap
// steady_clock for rdtsc for cleaner tails; cross-core numbers on a laptop OS are
// noisier than a tuned server.
#include "lob/order_book.hpp"
#include "lob/spsc_ring.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using namespace lob;
using Clock = std::chrono::steady_clock;

struct Stamped { Message msg; uint64_t ts; };

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count();
}

static std::vector<Message> gen_flow(std::size_t n, int32_t max_tick, uint32_t seed = 42) {
    std::mt19937 rng(seed);
    std::vector<Message> out; out.reserve(n);
    std::vector<uint64_t> live; live.reserve(n);
    uint64_t next_id = 1; int32_t mid = max_tick / 2;
    std::normal_distribution<double> walk(0.0, 3.0);
    std::uniform_int_distribution<int> act(0, 99), spread(1, 20);
    for (std::size_t i = 0; i < n; ++i) {
        mid = std::clamp(mid + (int32_t)walk(rng), 50, max_tick - 50);
        int a = act(rng);
        if (a < 70 || live.empty()) {
            bool buy = rng() & 1;
            int32_t tick = std::clamp(buy ? mid - spread(rng) : mid + spread(rng), 0, max_tick);
            out.push_back({MsgType::Add, buy ? Side::Buy : Side::Sell, tick, 1u + (rng() % 100), next_id});
            live.push_back(next_id++);
        } else if (a < 90) {
            std::size_t k = rng() % live.size();
            out.push_back({MsgType::Cancel, Side::Buy, 0, 0, live[k]});
            live[k] = live.back(); live.pop_back();
        } else {
            std::size_t k = rng() % live.size();
            out.push_back({MsgType::Modify, Side::Buy, 0, 1u + (rng() % 100), live[k]});
        }
    }
    return out;
}

static double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::size_t k = (std::size_t)(p / 100.0 * (v.size() - 1));
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

struct Result { double thr, p50, p99, p999; };

// depth = max messages in flight (SIZE_MAX == flood / unbounded up to ring size)
static Result run(const std::vector<Message>& flow, int32_t max_tick, std::size_t depth) {
    const std::size_t n = flow.size();
    SpscRing<Stamped> ring(1u << 16);
    std::vector<double> lat(n);
    std::atomic<bool> start{false};
    std::atomic<std::size_t> consumed{0};

    std::thread consumer([&] {
        OrderBook book(max_tick);
        std::size_t got = 0;
        while (!start.load(std::memory_order_acquire)) {}
        Stamped s;
        while (got < n) {
            if (ring.pop(s)) {
                uint64_t t = now_ns();
                switch (s.msg.type) {
                    case MsgType::Add:    book.add(s.msg.id, s.msg.side, s.msg.tick, s.msg.qty); break;
                    case MsgType::Cancel: book.cancel(s.msg.id); break;
                    case MsgType::Modify: book.modify(s.msg.id, s.msg.qty); break;
                }
                lat[got++] = double(t - s.ts);
                consumed.store(got, std::memory_order_release);
            }
        }
    });

    auto t0 = Clock::now();
    std::thread producer([&] {
        start.store(true, std::memory_order_release);
        for (std::size_t i = 0; i < n; ++i) {
            while (i - consumed.load(std::memory_order_acquire) >= depth) { /* bound in-flight */ }
            Stamped s{flow[i], now_ns()};
            while (!ring.push(s)) { /* spin: ring full */ }
        }
    });

    producer.join(); consumer.join();
    auto t1 = Clock::now();
    double wall = std::chrono::duration<double>(t1 - t0).count();
    std::vector<double> tmp = lat;
    return {n / wall / 1e6, pct(tmp, 50), pct(tmp, 99), pct(tmp, 99.9)};
}

int main(int argc, char** argv) {
    const std::size_t n = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int32_t max_tick = 100'000;
    auto flow = gen_flow(n, max_tick);
    std::printf("SPSC handoff: producer -> lock-free ring -> book consumer\n\n");

    // pure handoff latency: one message in flight
    Result lat = run(flow, max_tick, 1);
    std::printf("handoff latency (1 in flight): p50 %.0f ns  p99 %.0f ns  p99.9 %.0f ns\n",
                lat.p50, lat.p99, lat.p999);

    // sustained throughput: flood
    Result flood = run(flow, max_tick, SIZE_MAX);
    std::printf("sustained throughput (flood):  %.2f M msg/s\n", flood.thr);
    return 0;
}
