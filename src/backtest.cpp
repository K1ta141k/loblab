// P&L backtest for the order-book-imbalance (OBI) signal.
//
// signal_bench measures how *fast* we can act; this measures whether acting *makes money*.
//
// Two phases, cleanly separated:
//   PHASE 1 (market sim). Synthesize a flow driven by a latent fair value FV (random walk).
//     Order arrivals lean toward FV: when underpriced (FV > mid) buyers dominate and
//     aggressive buys eat the offer, which BOTH lifts the mid toward FV AND leaves the book
//     bid-heavy. So imbalance genuinely *leads* price — the real economic basis for OBI, not
//     a planted mid = alpha*OBI identity. We record {bid, ask, obi} at every step.
//     (Disclosed: this is a realistic simulator, not live data; edge must be re-validated live.)
//   PHASE 2 (strategy eval). Replay the recorded series for several thresholds. For each
//     signal we know the *directional accuracy* (did the mid move our way over H steps) and
//     the *taker P&L* net of the spread we cross to get in and out. A RANDOM-direction control
//     on the identical trigger times isolates signal edge from the harness.
//
// Headline: OBI predicts direction (accuracy > 50%, gross mid-move > 0). Whether that beats
// the spread is a separate, threshold-dependent question — the honest crux of taker alpha.
#include "lob/order_book.hpp"
#include "lob/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace lob;

int main(int argc, char** argv) {
    const std::size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int    K = 5;         // levels for imbalance
    const int    H = 20;        // holding horizon (updates)
    const int32_t MAX = 100'000;

    OrderBook book(MAX);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    std::normal_distribution<double> fvstep(0.0, 0.5);
    std::uniform_int_distribution<int> qd(1, 20);
    std::vector<OrderBook::Fill> fills; fills.reserve(16);
    const std::size_t TARGET = 3000;

    int32_t mid0 = MAX / 2; uint64_t id = 1;
    std::vector<uint64_t> live;
    for (int i = 1; i <= 200; ++i) {
        book.add(id, Side::Buy,  mid0 - i, 1u + qd(rng)); live.push_back(id++);
        book.add(id, Side::Sell, mid0 + i, 1u + qd(rng)); live.push_back(id++);
    }
    double FV = mid0;

    std::vector<float> bidv(N), askv(N), obiv(N);   // the recorded tape
    auto drop = [&](std::size_t k){ book.cancel(live[k]); live[k] = live.back(); live.pop_back(); };

    // ---------- PHASE 1: simulate + record ----------
    for (std::size_t i = 0; i < N; ++i) {
        FV = std::clamp(FV + fvstep(rng), 1000.0, (double)MAX - 1000.0);
        int32_t b = book.best_bid(), a = book.best_ask();
        if (b < 0 || a > MAX) {
            book.add(id, Side::Buy, (int32_t)FV - 1, 5u); live.push_back(id++);
            book.add(id, Side::Sell, (int32_t)FV + 1, 5u); live.push_back(id++);
            b = book.best_bid(); a = book.best_ask();
        }
        double m = 0.5 * (b + a), drift = FV - m;
        double pbuy = std::clamp(0.5 + 0.05 * drift, 0.05, 0.95);

        int32_t bt = std::clamp(a - 1 - (int32_t)(-std::log(U(rng) + 1e-9) * 2), 1, a - 1);
        book.add(id, Side::Buy, bt, 1u + qd(rng)); live.push_back(id++);
        int32_t at = std::clamp(b + 1 + (int32_t)(-std::log(U(rng) + 1e-9) * 2), b + 1, MAX - 1);
        book.add(id, Side::Sell, at, 1u + qd(rng)); live.push_back(id++);

        double paggr = std::clamp(0.12 + 0.03 * std::fabs(drift), 0.0, 0.85);
        if (U(rng) < paggr) {
            bool buy = U(rng) < pbuy;
            int cross = 1 + (int)(U(rng) * 3);
            int32_t lim = buy ? std::min(a + cross, MAX - 1) : std::max(b - cross, 1);
            fills.clear();
            book.submit(id++, buy ? Side::Buy : Side::Sell, lim, 3u + qd(rng), false, fills);
        }
        if (!live.empty() && U(rng) < 0.15) drop(rng() % live.size());
        while (live.size() > TARGET) drop(rng() % live.size());

        b = book.best_bid(); a = book.best_ask();
        uint64_t bd = book.top_depth(Side::Buy, K), ad = book.top_depth(Side::Sell, K);
        uint64_t tot = bd + ad;
        bidv[i] = (float)b; askv[i] = (float)a;
        obiv[i] = (b < 0 || a > MAX || a <= b || !tot) ? 0.0f
                  : (float)((double(bd) - double(ad)) / double(tot));
    }

    // ---------- PHASE 2: evaluate strategies over the recorded tape ----------
    auto mid = [&](std::size_t i){ return 0.5 * (bidv[i] + askv[i]); };
    auto valid = [&](std::size_t i){ return bidv[i] > 0 && askv[i] <= MAX && askv[i] > bidv[i]; };

    std::printf("OBI P&L backtest — %zu updates, top-%d imbalance, hold %d, unit=1, taker\n", N, K, H);
    std::printf("latent-fair-value flow; P&L in price ticks; control = same triggers, random side\n\n");
    std::printf("  thr | trades | dir-acc | grossMid |  spread | net/trade | ctrl/trade |  edge\n");
    std::printf("  ----+--------+---------+----------+---------+-----------+------------+-------\n");

    for (double T : {0.20, 0.30, 0.40, 0.50, 0.60, 0.70}) {
        long n = 0, correct = 0, wrong = 0; double gross = 0, spread = 0, net = 0, cnet = 0;
        std::size_t next = 0;                       // single open position: no entry before this
        std::mt19937 crng(99);
        for (std::size_t i = 0; i + H < N; ++i) {
            if (i < next || !valid(i) || !valid(i + H)) continue;
            double s = obiv[i] > T ? +1.0 : (obiv[i] < -T ? -1.0 : 0.0);
            if (s == 0.0) continue;
            double entry = (s > 0) ? askv[i]     : bidv[i];        // cross to enter
            double exit  = (s > 0) ? bidv[i + H] : askv[i + H];    // cross to exit
            double pnl   = s * (exit - entry);
            double gm    = s * (mid(i + H) - mid(i));              // directional mid move
            ++n; if (gm > 0) ++correct; else if (gm < 0) ++wrong;  // ties (gm==0) excluded
            gross += gm; spread += (gm - pnl); net += pnl;
            double cs = (crng() & 1) ? +1.0 : -1.0;               // control: random side
            double centry = (cs > 0) ? askv[i] : bidv[i];
            double cexit  = (cs > 0) ? bidv[i + H] : askv[i + H];
            cnet += cs * (cexit - centry);
            next = i + H;                                          // hold, then free to re-enter
        }
        if (!n) continue;
        long moved = correct + wrong;
        std::printf("  %.2f | %6ld | %6.1f%% | %+8.3f | %7.3f | %+9.3f | %+10.3f | %+.3f\n",
                    T, n, moved ? 100.0 * correct / moved : 0.0, gross / n, spread / n,
                    net / n, cnet / n, (net - cnet) / n);
    }
    std::printf("\ndir-acc = of signals whose mid actually moved over H, %% that moved OUR way (>50 = edge).\n");
    std::printf("net/trade = taker P&L after crossing the spread both ways; edge = net - random control.\n");
    std::printf("Read: OBI is directionally right and gross rises with conviction, but the move only\n");
    std::printf("clears the ~1-tick spread at extreme thresholds (thin samples) -> a taker barely\n");
    std::printf("monetizes it; the edge is real but belongs to a liquidity-provider / filter, not a\n");
    std::printf("spread-crosser. (Synthetic flow; re-validate on live data.)\n");
    return 0;
}
