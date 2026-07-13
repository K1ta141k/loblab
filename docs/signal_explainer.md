# The order-book-imbalance signal (market data to trade)

Learning notes for `src/signal_bench.cpp`. The matching engine gave us a book you can
*trade against*. A **signal** closes the loop: read the book, decide, and `submit` an
order - then measure how fast that whole path runs. This one uses **order-book imbalance
(OBI)**, one of the oldest and most robust microstructure signals.

---

## 1. The intuition

A limit order book quotes two queues: buyers resting on the **bid**, sellers resting on the
**ask**. When the resting size is lopsided, the thin side tends to get eaten first, so the
mid-price drifts *toward the thin side*:

- **More size on the bid than the ask** → buyers are eager, sellers are scarce → price tends
  to tick **up**. A short-horizon trader wants to **buy** now.
- **More size on the ask** → the mirror: price tends to tick **down** → **sell**.

It's a pressure gauge, not a crystal ball - a well-known effect that decays in milliseconds,
which is exactly why the *latency* of acting on it matters.

---

## 2. The number

Sum the resting quantity over the best **K** price levels on each side (here K = 5), then:

```
        bidDepth - askDepth
 OBI =  -------------------        in  [-1, +1]
        bidDepth + askDepth
```

`OBI = +1` means all the size is on the bid; `-1` means all on the ask; `0` is balanced. We
only act on a **conviction** move, so we gate on a threshold `T = 0.30`:

- `OBI >  +0.30` → aggressive **BUY** (take the offer)
- `OBI <  -0.30` → aggressive **SELL** (hit the bid)
- otherwise → do nothing

<div>
<svg width="700" height="300" viewBox="0 0 700 300" xmlns="http://www.w3.org/2000/svg" font-family="ui-monospace,Menlo,monospace" font-size="13">
  <text x="0" y="16" font-weight="bold">Top-5 depth each side  ->  imbalance  ->  decision</text>
  <!-- bid stack -->
  <text x="60" y="52" fill="#065f46" font-weight="bold">BID (buyers)</text>
  <g fill="#d1fae5" stroke="#065f46">
    <rect x="40" y="62" width="150" height="20"/>
    <rect x="55" y="86" width="135" height="20"/>
    <rect x="70" y="110" width="120" height="20"/>
    <rect x="88" y="134" width="102" height="20"/>
    <rect x="104" y="158" width="86" height="20"/>
  </g>
  <text x="200" y="128" fill="#065f46" font-size="12">sum = 820</text>
  <!-- ask stack -->
  <text x="430" y="52" fill="#7f1d1d" font-weight="bold">ASK (sellers)</text>
  <g fill="#fee2e2" stroke="#7f1d1d">
    <rect x="430" y="62" width="70" height="20"/>
    <rect x="430" y="86" width="60" height="20"/>
    <rect x="430" y="110" width="80" height="20"/>
    <rect x="430" y="134" width="55" height="20"/>
    <rect x="430" y="158" width="48" height="20"/>
  </g>
  <text x="520" y="128" fill="#7f1d1d" font-size="12">sum = 300</text>
  <!-- formula -->
  <text x="0" y="212" font-size="13">OBI = (820 - 300) / (820 + 300) = 520 / 1120 = <tspan font-weight="bold" fill="#065f46">+0.46</tspan></text>
  <text x="0" y="238" font-size="13">+0.46 &gt; +0.30 threshold  -&gt;  <tspan font-weight="bold" fill="#065f46">BUY</tspan> (take the offer)</text>
  <!-- arrow -->
  <g stroke="#334155" fill="none" stroke-width="1.5" marker-end="url(#a)">
    <path d="M300,120 L420,120"/>
  </g>
  <text x="305" y="112" fill="#64748b" font-size="11">bid-heavy</text>
  <text x="0" y="278" fill="#64748b" font-size="11">Bids outweigh asks ~2.7:1 over the top 5 levels - lean long before the thin ask lifts.</text>
  <defs><marker id="a" markerWidth="9" markerHeight="9" refX="6" refY="3" orient="auto">
    <path d="M0,0 L6,3 L0,6 z" fill="#334155"/></marker></defs>
</svg>
</div>

---

## 3. Reading the depth cheaply

The whole point of the array-of-levels book is that this read is fast. `top_depth` walks out
from the maintained touch (`best_bid_` / `best_ask_`), summing the first K *occupied* levels -
O(K), no tree, no allocation:

```cpp
uint64_t top_depth(Side side, int levels) const {
    uint64_t sum = 0; int found = 0;
    if (side == Side::Buy)
        for (int32_t t = best_bid_; t >= 0 && found < levels; --t)
            if (bid_[t].count) { sum += bid_[t].total_qty; ++found; }
    else
        for (int32_t t = best_ask_; t <= max_tick_ && found < levels; ++t)
            if (ask_[t].count) { sum += ask_[t].total_qty; ++found; }
    return sum;
}
```

Each level already keeps a running `total_qty` (maintained by add/cancel/modify), so a level's
depth is a single field read - we never walk the per-order lists here.

---

## 4. The timed path

Per market update we run exactly the loop a real strategy runs - read, decide, act - and time
just that span:

```cpp
auto t0 = Clock::now();
uint64_t bd = book.top_depth(Side::Buy, K);       // read
uint64_t ad = book.top_depth(Side::Sell, K);
uint64_t tot = bd + ad;
if (tot) {
    double obi = (double(bd) - double(ad)) / double(tot);   // decide
    if (obi > T)  book.submit(next_id++, Side::Buy,  book.best_ask(), 1, false, fills);  // act
    else if (obi < -T) book.submit(next_id++, Side::Sell, book.best_bid(), 1, false, fills);
}
auto t1 = Clock::now();     // <- signal-to-order latency
```

This is the number a trading desk cares about: **from seeing the book to the order leaving**.

---

## 5. What it measures

```
signal->order latency: p50 0 ns   p99 42 ns   p99.9 83 ns
fired 5866 orders (0.6% of 1,000,000 updates)
```

Two honest caveats:

- **p50 reads 0** because most updates *don't* fire (the imbalance sits inside ±0.30), and the
  read+compare alone is below this arm64 laptop's `steady_clock` resolution (~40 ns). It isn't
  literally zero - the clock just can't see under its own tick. On x86 Linux you'd use `rdtsc`
  (sub-nanosecond) and pin the thread to an isolated core to resolve it.
- **p99 / p99.9 (42 / 83 ns)** are the updates that *do* fire: two `top_depth` scans plus a
  `submit` that crosses and prints a fill. That's the real cost of acting on the signal.

The 0.6% fire rate is the threshold doing its job - trade only on conviction, not on noise.

---

## 6. Why this is the right capstone

It ties every earlier piece together into one pipeline: the **SPSC ring** feeds updates in,
the **array book + id-map** apply them in ~40 ns, `top_depth` reads the state, and the
**matching engine** executes the decision - all with zero hot-path allocation. That is the
shape of a real low-latency trading loop, and the whole read-to-order path lands in tens of
nanoseconds.

**Not a trading strategy.** OBI is illustrative - real use needs P&L backtesting, transaction
costs, a decay/half-life study, and calibration on live data. What's demonstrated here is the
**systems** result: the signal-to-order path is fast and allocation-free. The alpha is a
separate research problem; the plumbing is what this repo proves.
