# Market making: turning the signal into money you can keep

Learning notes for `src/market_maker.cpp`. The taker backtest ended on a cliffhanger: order-book
imbalance predicts direction, but the move is smaller than the spread, so you cannot profit by
*crossing* it. The natural question is the one a market-making desk lives on: what if you
*provide* the spread instead of paying it? This is that experiment, and it closes the loop.

---

## 1. The two ways to trade a signal

- **Taker** (backtest.cpp): cross the spread to get in now. You pay ~1 tick round-trip and need
  the move to exceed it. OBI's move does not, so a taker loses.
- **Maker** (this file): post a bid and an ask and wait to get hit. Now the spread is your
  *income*, not your cost. But you inherit a new enemy: **adverse selection** - the informed
  traders pick you off, buying from you right before a rise and selling to you right before a
  fall. A maker's whole job is to earn the spread faster than adverse selection bleeds it back.

The signal changes role too: a taker used OBI to *pick a direction*; a maker uses it to *decide
which quote to defend*.

---

## 2. The reservation price

We quote around a reservation price, not the raw mid (the Avellaneda-Stoikov idea):

```
 r = mid  -  gamma * inventory  +  theta * OBI
              \-----------/          \--------/
            inventory skew          signal skew
   bid = r - delta        ask = r + delta      (delta = base half-spread)
```

- **Inventory skew** (`- gamma * inventory`): if we are long, `r` drops, pulling *both* quotes
  down - our ask gets hit more (we sell down the position), our bid less. This mean-reverts
  inventory toward zero. It is pure **risk management**, and a hard position limit `+/- Qmax`
  backstops it.
- **Signal skew** (`+ theta * OBI`): when imbalance says price is about to rise, `r` rises,
  lifting our ask out of the way of informed buyers (fewer bad sells) and our bid up so we lean
  gently long *before* the move. When it says fall, the mirror. This is the signal defending the
  inventory.

<div>
<svg width="700" height="250" viewBox="0 0 700 250" xmlns="http://www.w3.org/2000/svg" font-family="ui-monospace,Menlo,monospace" font-size="13">
  <text x="0" y="16" font-weight="bold">OBI &gt; 0: skew the whole quote UP to dodge informed buyers</text>
  <!-- mid line -->
  <line x1="40" y1="150" x2="545" y2="150" stroke="#94a3b8" stroke-dasharray="3 3"/>
  <text x="550" y="154" fill="#64748b" font-size="11">mid</text>
  <!-- no-skew quotes, centered on mid -->
  <text x="110" y="86" fill="#64748b" font-size="12">no skew</text>
  <rect x="105" y="120" width="80" height="20" fill="#eef2f7" stroke="#94a3b8"/><text x="132" y="134" font-size="11">ask</text>
  <rect x="105" y="160" width="80" height="20" fill="#eef2f7" stroke="#94a3b8"/><text x="132" y="174" font-size="11">bid</text>
  <text x="70" y="212" fill="#b91c1c" font-size="11">informed buyers lift the grey ask = a bad sell</text>
  <!-- +OBI quotes, shifted up -->
  <text x="355" y="70" fill="#2563eb" font-size="12">+OBI skew</text>
  <rect x="350" y="88" width="80" height="20" fill="#dbeafe" stroke="#2563eb"/><text x="377" y="102" font-size="11">ask</text>
  <rect x="350" y="128" width="80" height="20" fill="#dbeafe" stroke="#2563eb"/><text x="377" y="142" font-size="11">bid</text>
  <text x="445" y="102" fill="#065f46" font-size="11">ask up: fewer bad sells</text>
  <text x="445" y="142" fill="#065f46" font-size="11">bid up: lean long early</text>
  <g stroke="#2563eb" fill="none" stroke-width="1.3" marker-end="url(#u)"><path d="M188,126 C270,110 300,100 345,98"/></g>
  <defs><marker id="u" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto"><path d="M0,0 L6,3 L0,6 z" fill="#2563eb"/></marker></defs>
</svg>
</div>

---

## 3. Making adverse selection real

A backtest that assumes fills are random would show a maker printing free spread forever - useless.
The fills here are generated from the loblab sim so the danger is real. Fill intensity on each
side **decays with quote distance** (further out, less likely to trade) and **tilts with the
hidden informed pressure** (`FV - mid`):

```cpp
double up = clamp(presc * pressure, -0.95, 0.95);     // FV - mid: the informed direction
double bid_rate = base * exp(-k*bidDist) * (1 - beta*up);   // fewer bids hit when price rising
double ask_rate = base * exp(-k*askDist) * (1 + beta*up);   // more asks hit when price rising
if (inv >=  Qmax) bid_rate = 0;   // position limit
if (inv <= -Qmax) ask_rate = 0;
```

The crucial asymmetry: **the maker never sees `FV`** (the truth driving informed flow). It only
sees `OBI`, a noisy proxy. So the question is whether skewing on that proxy actually helps.

---

## 4. The result (A/B over the same path and fill luck)

```
A: inventory only    PnL +384544 t   adverse +65986   avg|inv| 8.3   spread-retention 85.4%
B: inventory + OBI   PnL +396911 t   adverse +49982   avg|inv| 6.9   spread-retention 88.8%
```

Same market, same random fills - the only difference is whether the maker skews on OBI. Doing so:

- lifts **PnL +3.2%** (+12,367 ticks),
- cuts **adverse selection ~24%** (65,986 -> 49,982),
- *lowers* average inventory (8.3 -> 6.9), so it earns **more while holding less risk**,
- raises **spread retention** (share of captured spread kept after adverse selection) 85.4% -> 88.8%.

Every axis improves at once. That is the signature of a real edge, not a variance artifact.

---

## 5. The punchline that ties the repo together

> The *same* order-book-imbalance signal that **loses money crossing the spread** (taker backtest)
> **makes money providing it** (this market maker).

That is not a contradiction - it is the whole lesson of microstructure alpha. A signal's value is
inseparable from *how you trade it*. Crossing the spread, OBI's edge is smaller than the toll.
Quoting the spread, the same edge becomes a way to keep more of the spread you already earn, by
stepping out of the way of informed flow. Being able to show both halves - and connect them - is
worth more than either number alone.

**Caveats (honest, as before):** synthetic latent-fair-value flow, a simplified fill model (no
explicit queue position), and PnL in ticks without fees or a live spread. The transferable result
is the *framework and the direction of every effect*; the magnitudes must be re-earned on recorded
data with a queue model. Next steps live in the README roadmap.
