# Does the signal make money? An honest P&L backtest

Learning notes for `src/backtest.cpp`. The signal bench proved we can go from book to order in
tens of nanoseconds. Speed is necessary, not sufficient - the real question is whether acting on
order-book imbalance **pays after costs**. This is the backtest, and its most valuable output is
a *negative-sounding* result stated honestly, which is exactly what a research interviewer wants
to see you reason about.

---

## 1. The one hard truth: signal vs spread

Every taker (a strategy that crosses the spread to trade *now*) pays the same tax: you buy at
the **ask** and later sell at the **bid**, so a round trip forfeits roughly the **full spread**
before the market moves at all. A directional signal only makes money if the move it predicts is
**bigger than that spread**.

<div>
<svg width="700" height="230" viewBox="0 0 700 230" xmlns="http://www.w3.org/2000/svg" font-family="ui-monospace,Menlo,monospace" font-size="13">
  <text x="0" y="16" font-weight="bold">A taker round trip: pay the spread first, hope the move covers it</text>
  <!-- price axis -->
  <line x1="60" y1="60" x2="60" y2="180" stroke="#94a3b8"/>
  <!-- entry -->
  <line x1="60" y1="90" x2="360" y2="90" stroke="#b91c1c" stroke-dasharray="4 3"/>
  <text x="150" y="84" fill="#b91c1c" font-size="12">ask  (you BUY here)</text>
  <line x1="60" y1="120" x2="360" y2="120" stroke="#065f46" stroke-dasharray="4 3"/>
  <text x="150" y="135" fill="#065f46" font-size="12">bid  (you'd SELL here)</text>
  <text x="370" y="108" fill="#334155" font-size="12">spread = cost paid up front</text>
  <path d="M366,90 L366,120" stroke="#334155" marker-start="url(#d)" marker-end="url(#d)"/>
  <!-- move -->
  <line x1="380" y1="70" x2="660" y2="70" stroke="#2563eb"/>
  <text x="470" y="64" fill="#2563eb" font-size="12">mid drifts up (the edge)</text>
  <path d="M640,120 L640,70" stroke="#2563eb" marker-start="url(#e)" marker-end="url(#e)"/>
  <text x="648" y="98" fill="#2563eb" font-size="11">move</text>
  <text x="0" y="210" fill="#334155">Profit needs (move) &gt; (spread). OBI's move is small -&gt; that is the whole fight.</text>
  <defs>
    <marker id="d" markerWidth="8" markerHeight="8" refX="4" refY="4" orient="auto"><path d="M4,0 L4,8 M1,2 L4,0 L7,2 M1,6 L4,8 L7,6" stroke="#334155" fill="none"/></marker>
    <marker id="e" markerWidth="8" markerHeight="8" refX="4" refY="4" orient="auto"><path d="M4,0 L4,8 M1,2 L4,0 L7,2 M1,6 L4,8 L7,6" stroke="#2563eb" fill="none"/></marker>
  </defs>
</svg>
</div>

---

## 2. Building a flow where the signal *could* work

A backtest is only meaningful if the data has the effect you're testing for. We synthesize the
flow from a **latent fair value** `FV` that random-walks. Order arrivals lean toward it: when the
book is underpriced (`FV > mid`), buyers dominate and aggressive buys **eat the offer** - which
does two things at once:

- lifts the **mid** up toward `FV` (the price move), and
- leaves the book **bid-heavy** (the imbalance).

So the imbalance genuinely *leads* the price - the real economic reason OBI works - rather than a
circular `mid = alpha * OBI` identity baked in by hand. **This is a simulator, not the market:**
its imbalance-to-price link is far cleaner than reality, which is why the accuracy below looks
almost perfect. The machinery is what transfers; the numbers must be re-earned on live data.

---

## 3. Method: separate the market from the strategy

**Phase 1** runs the sim once and records `{bid, ask, obi}` at every step. **Phase 2** replays
that tape for several thresholds `T`. On `|OBI| > T` we open one unit crossing the spread and
exit `H = 20` steps later crossing back. For each signal we log:

- **directional accuracy** - of the signals whose mid actually moved, the share that moved *our* way;
- **gross mid-move** - the mid change we predicted, in ticks (the raw edge, before costs);
- **net P&L / trade** - after crossing the spread both ways;
- a **random-direction control** on the *same* trigger times - if OBI has real edge, it must beat this coin flip.

---

## 4. What it found

```
  thr | trades | dir-acc | grossMid |  spread | net/trade | ctrl/trade |  edge
  ----+--------+---------+----------+---------+-----------+------------+-------
  0.20 |  69697 |   99.8% |   +0.097 |   1.014 |    -0.917 |     -1.014 | +0.097
  0.30 |  48363 |   99.8% |   +0.135 |   1.020 |    -0.885 |     -1.020 | +0.135
  0.40 |  15279 |   99.9% |   +0.315 |   1.040 |    -0.725 |     -1.038 | +0.313
  0.50 |    461 |   99.7% |   +0.818 |   1.087 |    -0.269 |     -0.996 | +0.727
  0.60 |      8 |  100.0% |   +1.750 |   1.250 |    +0.500 |     -2.000 | +2.500
```

Read it left to right as conviction rises:

1. **The signal is real.** Directional accuracy sits far above 50%, and every threshold beats the
   random control (positive `edge`). Higher conviction → bigger predicted move (`grossMid` climbs
   +0.10 → +1.75 ticks). That monotonic curve is the signature of a genuine signal.
2. **But it loses as a taker** everywhere with a meaningful sample. `net/trade` is negative until
   `T ≈ 0.55`, because `grossMid < spread`: the move is real but smaller than the ~1-tick toll.
3. **Profit only appears at the extremes**, where `grossMid` finally exceeds the spread - but only
   8 trades qualify, far too few to trust.

---

## 5. The conclusion worth stating

> Order-book imbalance is a **directional** signal, not a **tradeable-by-crossing** one. The move
> it predicts is smaller than the spread, so a taker pays more to enter than the edge is worth.

The edge is real, so where does the money live? Two honest answers:

- **Provide, don't take.** Post a limit order on the favored side and *earn* the spread instead of
  paying it. Now imbalance tells you which side to quote - the sign flips from cost to income.
- **Use it as a filter/tilt**, not a standalone trade: skew an existing maker's quotes, or gate a
  different taker signal, so you never pay the spread just for the imbalance.

That is the real lesson of microstructure alpha: a true signal and a *profitable* one are
different claims, separated by the spread. Being able to show that gap - with a control, a
threshold sweep, and a stated caveat about the simulator - is worth more than a cherry-picked
green P&L.

---

## 6. What would make this live-grade

- Replay **recorded L2 data** (the repo already records Binance.US depth) instead of a simulator,
  and expect accuracy to fall toward ~55%.
- Add a **maker/queue model** to test the "provide, don't take" version that this result points to.
- Report P&L in **currency with fees**, add inventory limits and a decay/half-life study of the
  signal, and bootstrap confidence intervals on `net/trade`.
