# loblab - a low-latency C++ limit order book and microstructure lab

A single-instrument limit order book in modern C++ (C++17), benchmarked A/B against a
naive `std::map` baseline, built up layer by layer: a custom open-addressing id map, a
lock-free single-producer/single-consumer feed handoff, a price-time matching engine, and
an order-book-imbalance signal studied end to end (signal-to-order latency plus a P&L
backtest with an honest, spread-aware conclusion). Every layer has a short explainer PDF
in `docs/`. Timing uses `std::chrono::steady_clock` for portability; see the notes on
`rdtsc` + core pinning for x86 Linux.

## Build & run
```bash
cmake -S . -B build && cmake --build build -j
./build/test_book        # correctness: optimized book vs naive baseline
./build/bench 2000000    # latency + throughput benchmark

# or without cmake:
clang++ -std=c++17 -O3 -march=native -Iinclude src/bench.cpp -o bench && ./bench
```

## Real market-data replay
```bash
# record ~75s of real L2 depth from Binance.US (free, no auth, US-accessible)
python3 data/record_binance.py btcusdt 75 1.0     # -> data/feed.csv
clang++ -std=c++17 -O3 -march=native -Iinclude src/replay.cpp -o replay
./replay data/feed.csv
```
Sample run on real BTC-USD data (1,616 level updates):
```
top of book: bid $62022 / ask $62033
book op latency: p50 84 ns  p99 333 ns  p99.9 2416 ns  |  8.84 M op/s
```
`record_binance.py` normalizes the depth-diff stream to `side,tick,qty` rows; `replay.cpp`
is a feed handler that turns each L2 level update into an add/modify/cancel book op
(id derived from tick+side). Note: simplified replay (not sequence-synced to a REST
snapshot) - it benchmarks the book on real message shape/rates, not for trading.

## Lock-free SPSC feed -> book handoff
```bash
clang++ -std=c++17 -O3 -march=native -pthread -Iinclude src/bench_mt.cpp -o bench_mt
./bench_mt 2000000
```
A single-producer/single-consumer lock-free ring (`include/lob/spsc_ring.hpp`) hands
messages from a feed thread to the book thread - the realistic feed-handler split.
```
handoff latency (1 in flight): p50 125 ns  p99 167 ns  p99.9 250 ns
sustained throughput (flood):  ~8.2 M msg/s
```
Two honest numbers that trade off by Little's law: the p50 125 ns is the true time for
one message to cross the ring and be applied (no queuing); flood throughput is what the
pipeline sustains when the producer runs flat out. On x86 Linux, pin both threads to
isolated cores (`taskset -c`) and use `rdtsc` for tighter tails.

## Matching engine
```bash
clang++ -std=c++17 -O3 -march=native -Iinclude src/match_bench.cpp -o match_bench && ./match_bench
./build/test_match     # correctness
```
`OrderBook::submit(...)` matches an aggressive order against the resting book with
**price-time priority** (best price first, FIFO within a level), emits `Fill`s, and rests
a limit order's unfilled remainder. Market orders sweep and drop the remainder.
```
submit latency: p50 42 ns  p99 334 ns  p99.9 667 ns  |  17.3 M submit/s
```
`tests/test_match.cpp` verifies price-time ordering, partial fills, resting remainders,
and market sweeps.

## Order-book-imbalance signal (market data -> trade)
```bash
clang++ -std=c++17 -O3 -march=native -Iinclude src/signal_bench.cpp -o signal_bench && ./signal_bench
```
The strategy layer closes the loop: after each market update it reads the top-5 depth on
each side, computes the imbalance `obi = (bidD - askD)/(bidD + askD)`, and if `|obi| > 0.30`
`submit()`s a small aggressive order in the signal's direction - timing the whole
**read -> decide -> submit** path.
```
signal->order latency: p50 0 ns  p99 42 ns  p99.9 83 ns
fired 5866 orders (0.6% of updates)
```
The read+decide is so cheap it falls below this arm64 laptop's `steady_clock` resolution
(~40 ns), so p50 reads 0; p99/p99.9 (42/83 ns) capture the updates that actually fire an
order (two `top_depth` scans + a `submit`). On x86 Linux, `rdtsc` resolves the sub-40 ns
path directly. `top_depth(side, K)` is O(K) over occupied levels off the maintained touch.
(Illustrative microstructure signal, not a calibrated alpha.)

## Does the signal make money? (P&L backtest)
```bash
clang++ -std=c++17 -O3 -march=native -Iinclude src/backtest.cpp -o backtest && ./backtest
```
`signal_bench` shows how *fast* we act; `backtest` shows whether acting *pays*. It simulates a
flow driven by a latent fair value (so imbalance genuinely *leads* price - the real basis for
OBI), records the tape, then sweeps thresholds. Per signal it reports directional accuracy and
the **taker P&L net of the spread crossed both ways**, against a random-direction control on the
same triggers:
```
  thr | trades | dir-acc | grossMid |  spread | net/trade | ctrl/trade |  edge
  0.20 |  69697 |   99.8% |   +0.097 |   1.014 |    -0.917 |     -1.014 | +0.097
  0.40 |  15279 |   99.9% |   +0.315 |   1.040 |    -0.725 |     -1.038 | +0.313
  0.50 |    461 |   99.7% |   +0.818 |   1.087 |    -0.269 |     -0.996 | +0.727
  0.60 |      8 |  100.0% |   +1.750 |   1.250 |    +0.500 |     -2.000 | +2.500
```
The honest read: OBI is **directionally right** (gross mid-move is positive and rises with
conviction; edge over the control is always positive), but the move only clears the ~1-tick
spread at extreme thresholds where the sample is tiny. So a pure **spread-crossing taker barely
monetizes it** - the edge really belongs to a liquidity *provider* (post, don't cross) or as a
filter/tilt. That crux (signal < spread) is the whole game in taker alpha. **Caveat:** the ~99%
accuracy reflects this simulator's tight imbalance→price coupling; on live data expect ~55% -
re-validate before trusting any number here.

## What's here
- `include/lob/order_book.hpp` - the optimized book: flat array of price levels
  indexed by tick (O(1) level access), intrusive FIFO doubly-linked list per level
  (price-time priority), a preallocated order pool with a free list (no heap
  allocation on the hot path), incrementally maintained best bid/ask, and a
  price-time **matching engine** (`submit()` -> `Fill`s).
- `include/lob/spsc_ring.hpp` - single-producer/single-consumer lock-free ring buffer
  (acquire/release atomics, cache-line-padded cursors) for the feed->book handoff.
- `include/lob/id_map.hpp` - custom open-addressing hash map (order_id to pool idx)
  (flat, linear-probing, splitmix64, tombstone deletes) replacing `std::unordered_map`.
- `include/lob/naive_book.hpp` - a deliberately naive `std::map` + `std::list`
  baseline (per-order heap allocation, tree traversal) for the A/B speedup number.
- `src/bench.cpp` - replays synthetic order flow (70% add / 20% cancel / 10% modify),
  reports per-op latency p50/p99/p99.9 (ns) and throughput, and the speedup.
- `tests/test_book.cpp` - cross-checks top-of-book between the two books over 200k
  messages.

## Current numbers (Apple clang -O3 -march=native, arm64)
```
OrderBook   p50 42 ns   p99 209 ns   p99.9 959 ns   ~20.8 M msg/s
NaiveBook   p50 42 ns   p99 542 ns   p99.9 1333 ns  ~7.6 M msg/s
speedup ~2.7x   (was ~1.8x before the custom id map)
```
The custom open-addressing `IdMap` (see below) replaced `std::unordered_map` and lifted
throughput from ~14M to ~20.8M msg/s (2.7x over the naive book); on the real-data replay
it halved p50 (84 ns -> 42 ns). More headroom remains in the roadmap.

## Optimization roadmap (drive the speedup up)
1. **[DONE] Killed the `std::unordered_map`.** `include/lob/id_map.hpp` is a flat
   open-addressing (linear-probing, splitmix64) map with no per-element allocation:
   1.8x -> 2.7x, real-data p50 84 -> 42 ns. Next: hand out pool indices as the external id
   to drop the lookup entirely where the feed allows.
2. **Real market data.** Add a parser + deterministic replay for a recorded Coinbase/
   Binance L2/L3 stream (or LOBSTER / Databento for US equities). Verify against snapshots.
3. **[DONE] Lock-free SPSC ring** between the feed parser (producer) and the book
   (consumer) - `include/lob/spsc_ring.hpp` + `src/bench_mt.cpp`: handoff p50 125 ns,
   ~8.2M msg/s sustained. Next: pin threads to isolated cores + rdtsc for tighter tails.
4. **rdtsc timing** on x86 Linux (HRT's env) with a cycles->ns calibration, instead of
   `steady_clock`; report the tail (p99/p99.9) which is what matters for HFT.
5. **Cache/branch work.** Pack hot fields, arena the nodes, `__builtin_expect` cold
   branches, `-O3 -march=native`, profile with `perf` + flamegraphs.
6. **[DONE] Matching engine.** `OrderBook::submit()` - price-time matching with `Fill`s,
   partial fills, resting remainders, market sweeps; ~17M submit/s, p50 42 ns
   (`src/match_bench.cpp`, `tests/test_match.cpp`).
7. **[DONE] Order-book-imbalance signal.** `top_depth()` + `src/signal_bench.cpp`: reads
   the top-5 depth, fires an aggressive order when `|imbalance| > 0.30`, measures the
   read->decide->submit path (p99 42 ns).
8. **[DONE] P&L backtest.** `src/backtest.cpp`: latent-fair-value sim + threshold sweep +
   random-direction control; net taker P&L vs the spread. Finding: OBI is directionally
   right but the move rarely clears the spread (a provider/filter edge, not a taker's).
   Next: rdtsc/core-pinned timing on x86 Linux for clean tails; micro-price signal;
   validate on recorded L2 data.

## Note
Timing here uses `std::chrono::steady_clock` so it builds portably (incl. Apple
Silicon). On HRT's x86 Linux, swap in `rdtsc` and pin to an isolated core for clean tails.
