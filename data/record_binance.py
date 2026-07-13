#!/usr/bin/env python3
"""Record a real market-data feed to a normalized CSV the C++ replay can read.

Source: Binance public depth-diff stream (free, no auth). Each update carries the
changed bid/ask price levels. We normalize each changed level to one CSV row:

    side,tick,qty

  side: 0 = bid, 1 = ask
  tick: integer price in dollars (round(price / TICK_SIZE))
  qty:  integer size in micro-units (round(size * 1e6)); 0 means the level cleared

The replay turns these L2 level updates into add/modify/cancel book operations.
This is a simplified replay (not sequence-synced against a REST snapshot); it is
for benchmarking the book on real market message shape and rates, not for trading.

Usage:  python3 record_binance.py [symbol=btcusdt] [seconds=30] [tick_size=1.0]
"""
import asyncio, json, sys, time
import websockets

SYMBOL   = sys.argv[1] if len(sys.argv) > 1 else "btcusdt"
SECONDS  = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
TICK_SZ  = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
OUT      = "feed.csv"

async def main():
    url = f"wss://stream.binance.us:9443/ws/{SYMBOL}@depth"
    rows = 0
    with open(OUT, "w") as f:
        async with websockets.connect(url, ping_interval=20, max_size=None) as ws:
            t_end = time.time() + SECONDS
            while time.time() < t_end:
                msg = await asyncio.wait_for(ws.recv(), timeout=15)
                d = json.loads(msg)
                for side, key in ((0, "b"), (1, "a")):
                    for px, sz in d.get(key, []):
                        tick = int(round(float(px) / TICK_SZ))
                        qty  = int(round(float(sz) * 1_000_000))
                        f.write(f"{side},{tick},{qty}\n")
                        rows += 1
    print(f"wrote {rows} level updates to {OUT} (symbol={SYMBOL}, tick_size={TICK_SZ})")

if __name__ == "__main__":
    asyncio.run(main())
