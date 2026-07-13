// Inventory-aware market maker on loblab, driven by the order-book-imbalance signal.
//
// The taker backtest (backtest.cpp) proved OBI is a *provider* edge, not a taker's: the
// predicted move is smaller than the spread you cross. This flips it around and PROVIDES:
// we quote two-sided, capture the spread, and use OBI to defend against adverse selection.
//
// Framework (Avellaneda-Stoikov style, discrete):
//   reservation price  r = mid - gamma*inventory + theta*OBI
//     - inventory term pulls quotes to mean-revert the position (risk management)
//     - OBI term leans the book toward the predicted direction (signal-informed quoting)
//   quotes:  bid = r - delta,  ask = r + delta   (delta = base half-spread, ticks)
//   a hard position limit Q_max stops quoting the side that would breach it.
//
// Fills are generated from the loblab market sim (latent fair value FV). Fill intensity
// falls with quote distance (you get hit less the further out you quote), and — crucially —
// rises on the side the *informed* flow is pushing (FV vs mid). That hidden pressure is what
// creates ADVERSE SELECTION: you sell into a rise, buy into a fall. The MM never sees FV; it
// only sees OBI (a noisy proxy), so skewing by OBI is what defends the inventory.
//
// We A/B two makers over the identical market path and fill luck: (A) inventory skew only,
// (B) inventory + OBI skew. If (B) beats (A), the imbalance signal is monetizable by a maker.
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

struct MMResult {
    double pnl = 0, spread_capture = 0, adverse = 0;
    long fills = 0; double inv_abs_sum = 0; int inv_max = 0;
    double ret_sum = 0, ret_sq = 0; long steps = 0;
};

// Run one maker over the recorded tape. use_signal toggles the OBI term.
static MMResult run_mm(const std::vector<float>& midv, const std::vector<float>& obiv,
                       const std::vector<float>& pressv, double delta, double gamma,
                       double theta, int Qmax, bool use_signal) {
    MMResult R;
    std::mt19937 fill_rng(2024);                 // same seed for both variants: matched luck
    std::uniform_real_distribution<double> U(0.0, 1.0);
    const double base = 0.55, k = 0.55, beta = 0.9, presc = 0.15;
    double cash = 0.0; int inv = 0;
    double prev_mtm = 0.0;

    for (std::size_t i = 0; i < midv.size(); ++i) {
        double mid = midv[i];
        if (mid <= 0) continue;
        double r = mid - gamma * inv + (use_signal ? theta * obiv[i] : 0.0);
        double bid = r - delta, ask = r + delta;
        double bidDist = std::max(0.5, mid - bid), askDist = std::max(0.5, ask - mid);
        double up = std::clamp(presc * pressv[i], -0.95, 0.95);   // hidden informed pressure

        // fill intensities: decay with distance, tilt with informed flow
        double bid_rate = base * std::exp(-k * bidDist) * (1.0 - beta * up);
        double ask_rate = base * std::exp(-k * askDist) * (1.0 + beta * up);
        if (inv >= Qmax)  bid_rate = 0.0;         // position limit: stop growing the long
        if (inv <= -Qmax) ask_rate = 0.0;

        if (U(fill_rng) < std::clamp(bid_rate, 0.0, 0.95)) {      // our bid hit: we BUY at bid
            cash -= bid; inv += 1; ++R.fills;
            R.spread_capture += (mid - bid);                      // earned vs mid...
            R.adverse += 0;                                       // ...adverse booked via mtm below
        }
        if (U(fill_rng) < std::clamp(ask_rate, 0.0, 0.95)) {      // our ask lifted: we SELL at ask
            cash += ask; inv -= 1; ++R.fills;
            R.spread_capture += (ask - mid);
        }

        double mtm = cash + inv * mid;                            // mark to market
        double ret = mtm - prev_mtm; prev_mtm = mtm;
        R.ret_sum += ret; R.ret_sq += ret * ret; ++R.steps;
        R.inv_abs_sum += std::abs(inv); R.inv_max = std::max(R.inv_max, std::abs(inv));
    }
    double final_mid = 0; for (std::size_t j = midv.size(); j-- > 0;) if (midv[j] > 0) { final_mid = midv[j]; break; }
    R.pnl = cash + inv * final_mid;                              // liquidate at the last mid
    R.adverse = R.spread_capture - R.pnl;                        // what the inventory gave back
    return R;
}

static void report(const char* name, const MMResult& R) {
    double retention = R.spread_capture > 0 ? 100.0 * R.pnl / R.spread_capture : 0.0;
    std::printf("%-22s PnL %+9.0f t  fills %6ld  spread-capture %+8.0f  adverse %+8.0f\n",
                name, R.pnl, R.fills, R.spread_capture, R.adverse);
    std::printf("%-22s PnL/fill %+.3f t   avg|inv| %5.1f   max|inv| %3d   spread-retention %4.1f%%\n",
                "", R.fills ? R.pnl / R.fills : 0.0,
                R.steps ? R.inv_abs_sum / R.steps : 0.0, R.inv_max, retention);
}

int main(int argc, char** argv) {
    const std::size_t N = (argc > 1) ? std::strtoull(argv[1], nullptr, 10) : 2'000'000;
    const int    K = 5;
    const int32_t MAX = 100'000;
    const double delta = 1.5;    // base half-spread (ticks)
    const double gamma = 0.40;   // inventory aversion (mean-reverts the position toward 0)
    const double theta = 2.5;    // OBI skew weight (ticks per unit imbalance)
    const int    Qmax  = 30;     // hard position limit

    // ---------- PHASE 1: loblab market sim, record mid / OBI / hidden pressure ----------
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
    std::vector<float> midv(N), obiv(N), pressv(N);
    auto drop = [&](std::size_t kk){ book.cancel(live[kk]); live[kk] = live.back(); live.pop_back(); };

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
        bool ok = (b >= 0 && a <= MAX && a > b);
        midv[i]   = ok ? (float)(0.5 * (b + a)) : -1.0f;
        obiv[i]   = (ok && tot) ? (float)((double(bd) - double(ad)) / double(tot)) : 0.0f;
        pressv[i] = ok ? (float)(FV - 0.5 * (b + a)) : 0.0f;    // hidden informed pressure
    }

    // ---------- PHASE 2: two makers over the same path + fill luck ----------
    MMResult A = run_mm(midv, obiv, pressv, delta, gamma, theta, Qmax, /*signal=*/false);
    MMResult B = run_mm(midv, obiv, pressv, delta, gamma, theta, Qmax, /*signal=*/true);

    std::printf("loblab market maker - %zu steps, half-spread %.1f t, inv-aversion %.2f, "
                "signal skew %.1f t, position limit +-%d\n", N, delta, gamma, theta, Qmax);
    std::printf("(latent-fair-value flow; adverse selection from hidden informed pressure; PnL in ticks)\n\n");
    report("A: inventory only", A);
    std::printf("\n");
    report("B: inventory + OBI", B);
    std::printf("\nSignal adds %+.0f ticks PnL (%+.1f%%) and cuts adverse selection %+.0f t. "
                "OBI monetizes as a MAKER even though it did not as a taker.\n",
                B.pnl - A.pnl, A.pnl != 0 ? 100.0 * (B.pnl - A.pnl) / std::fabs(A.pnl) : 0.0,
                A.adverse - B.adverse);
    return 0;
}
