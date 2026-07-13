# Replacing `std::unordered_map` with an open-addressing hash map

Learning notes for the order-book `IdMap`. The book needs one lookup on the hot path:
given an **order id**, find where that order lives (`order_id -> pool index`), so `cancel`
and `modify` can reach it in O(1). We started with `std::unordered_map<uint64_t,uint32_t>`
and replaced it with a hand-rolled open-addressing map. This explains why, and how.

---

## 1. Why `std::unordered_map` is slow on a hot path

`std::unordered_map` is a **chained** hash table. The standard effectively forces:

- **A separate heap-allocated node per element.** Every `insert` calls the allocator;
  every `erase` frees. Allocation is slow and, worse, scatters nodes across memory.
- **Pointer chasing.** A bucket holds a pointer to a linked list of nodes. A lookup
  hashes the key, indexes the bucket array, then follows node pointers comparing keys.
  Each `next` is very likely a **cache miss** (the node is somewhere unrelated in memory).
- **Poor cache locality.** The keys you compare live in scattered nodes, not packed
  together, so the CPU's prefetcher can't help you.

On a trading hot path where you do millions of `cancel`/`modify` lookups per second, those
cache misses dominate. A miss to main memory is ~100+ ns; an L1 hit is ~1 ns.

<div>
<svg width="680" height="250" viewBox="0 0 680 250" xmlns="http://www.w3.org/2000/svg" font-family="ui-monospace,Menlo,monospace" font-size="13">
  <text x="0" y="16" font-weight="bold">Chaining (std::unordered_map): bucket array + pointers to scattered heap nodes</text>
  <!-- bucket array -->
  <text x="0" y="60">buckets:</text>
  <g fill="#eef2ff" stroke="#4f46e5">
    <rect x="70" y="46" width="52" height="26"/><rect x="122" y="46" width="52" height="26"/>
    <rect x="174" y="46" width="52" height="26"/><rect x="226" y="46" width="52" height="26"/>
    <rect x="278" y="46" width="52" height="26"/><rect x="330" y="46" width="52" height="26"/>
  </g>
  <g fill="#334155" text-anchor="middle">
    <text x="96" y="88">0</text><text x="148" y="88">1</text><text x="200" y="88">2</text>
    <text x="252" y="88">3</text><text x="304" y="88">4</text><text x="356" y="88">5</text>
  </g>
  <!-- scattered nodes -->
  <g fill="#fef3c7" stroke="#d97706">
    <rect x="120" y="150" width="120" height="30"/>
    <rect x="330" y="200" width="120" height="30"/>
    <rect x="470" y="120" width="120" height="30"/>
  </g>
  <g fill="#7c2d12" font-size="12">
    <text x="128" y="170">node id=42 -&gt;</text>
    <text x="338" y="220">node id=17 (null)</text>
    <text x="478" y="140">node id=91 (null)</text>
  </g>
  <!-- arrows (bucket -> node, node -> node) -->
  <g stroke="#dc2626" fill="none" stroke-width="1.5" marker-end="url(#ah)">
    <path d="M148,72 C148,110 150,120 180,150"/>
    <path d="M240,165 C290,165 300,200 340,200"/>
    <path d="M356,72 C420,72 500,90 530,120"/>
  </g>
  <defs><marker id="ah" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto">
    <path d="M0,0 L6,3 L0,6 z" fill="#dc2626"/></marker></defs>
  <text x="0" y="245" fill="#dc2626" font-size="12">red = pointer chase; each hop is likely a cache miss (~100 ns)</text>
</svg>
</div>

---

## 2. The idea: open addressing

An **open-addressing** table stores entries **inline in one flat array** - no nodes, no
pointers. When two keys hash to the same slot (a collision), you don't chain; you **probe**
to another slot in the same array by a fixed rule. Our rule is **linear probing**: on a
collision, try the next slot, then the next, wrapping around.

Because everything is in one contiguous array, a lookup touches only a couple of adjacent
cache lines, and the prefetcher loves sequential scans. No allocator on the hot path.

```cpp
enum State : uint8_t { EMPTY = 0, FULL = 1, DELETED = 2 };
struct Slot { uint64_t key; uint32_t val; uint8_t state; };
std::vector<Slot> slot_;   // one flat array, power-of-2 size
```

The array size is a **power of two**, so `hash(key) & (cap - 1)` replaces a modulo (a slow
integer division) with a single AND.

<div>
<svg width="680" height="200" viewBox="0 0 680 200" xmlns="http://www.w3.org/2000/svg" font-family="ui-monospace,Menlo,monospace" font-size="13">
  <text x="0" y="16" font-weight="bold">Open addressing + linear probing: everything inline in one flat array</text>
  <!-- slot array -->
  <g fill="#ecfdf5" stroke="#059669">
    <rect x="40" y="90" width="56" height="30"/><rect x="96" y="90" width="56" height="30"/>
    <rect x="152" y="90" width="56" height="30"/><rect x="208" y="90" width="56" height="30"/>
    <rect x="264" y="90" width="56" height="30"/><rect x="320" y="90" width="56" height="30"/>
    <rect x="376" y="90" width="56" height="30"/><rect x="432" y="90" width="56" height="30"/>
  </g>
  <!-- occupied slots -->
  <g fill="#334155" text-anchor="middle" font-size="12">
    <text x="180" y="109">id 42</text><text x="236" y="109">id 91</text>
  </g>
  <g fill="#64748b" text-anchor="middle" font-size="11">
    <text x="68" y="136">0</text><text x="124" y="136">1</text><text x="180" y="136">2</text>
    <text x="236" y="136">3</text><text x="292" y="136">4</text><text x="348" y="136">5</text>
    <text x="404" y="136">6</text><text x="460" y="136">7</text>
  </g>
  <!-- probe path: id 91 hashes to slot 2 (taken), probes to slot 3 -->
  <g stroke="#2563eb" fill="none" stroke-width="1.5" marker-end="url(#ah2)">
    <path d="M180,55 C180,72 180,80 180,88"/>            <!-- hash lands on slot 2 -->
    <path d="M208,70 C224,70 236,80 236,88"/>            <!-- probe +1 to slot 3 -->
  </g>
  <text x="120" y="45" fill="#2563eb" font-size="12">insert id 91: hash&amp;mask = slot 2 (taken by id 42)...</text>
  <text x="250" y="66" fill="#2563eb" font-size="12">...probe +1 -&gt; slot 3 (empty), place here</text>
  <defs><marker id="ah2" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto">
    <path d="M0,0 L6,3 L0,6 z" fill="#2563eb"/></marker></defs>
  <text x="0" y="180" fill="#059669" font-size="12">find scans adjacent slots (cache-friendly); stop at the key (hit) or EMPTY (miss)</text>
</svg>
</div>

---

## 3. The hash function

Order ids are often sequential (1, 2, 3, ...) or come straight from an exchange. Feeding raw
sequential integers into a table causes clustering. We run the key through the **splitmix64
finalizer** - three xor-shift-multiply steps that "avalanche" the bits so similar inputs land
far apart, for a few cheap ALU ops (no memory access):

```cpp
static uint64_t hash(uint64_t x) {          // splitmix64 finalizer
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27; x *= 0x94d049bb133111ebull;
    x ^= x >> 31; return x;
}
```

---

## 4. Find (the hot path)

Probe forward from the home slot; stop at the key (hit) or an `EMPTY` slot (miss). An `EMPTY`
slot means "nothing was ever placed at or past here in this probe run," so the key can't exist.

```cpp
uint32_t find(uint64_t key) const {
    std::size_t i = hash(key) & (cap_ - 1);
    while (true) {
        const Slot& s = slot_[i];
        if (s.state == EMPTY) return 0xFFFFFFFFu;   // not present
        if (s.state == FULL && s.key == key) return s.val;
        i = (i + 1) & (cap_ - 1);                   // linear probe
    }
}
```

At load factor < 0.7 the average probe length is ~1-2 slots, all cache-adjacent.

---

## 5. Deletion: why tombstones

You cannot just blank a slot on delete - that would insert a false `EMPTY` mid-probe-run and
make later keys unfindable. Two standard fixes:

- **Tombstones (what we use):** mark the slot `DELETED`. `find` treats it as "keep probing";
  `insert` may reuse it. Simple and correct. Downside: tombstones lengthen probe runs over
  time, so we **rehash** to clear them when the table gets crowded.
- **Backward-shift deletion:** on delete, shift subsequent displaced entries back to fill the
  gap, keeping the table tombstone-free. Faster steady-state, more code. A good "next step."

```cpp
bool erase(uint64_t key) {
    std::size_t i = hash(key) & (cap_ - 1);
    while (true) {
        Slot& s = slot_[i];
        if (s.state == EMPTY) return false;
        if (s.state == FULL && s.key == key) { s.state = DELETED; --size_; ++tomb_; return true; }
        i = (i + 1) & (cap_ - 1);
    }
}
```

---

## 6. Load factor and rehash

Probe length blows up as the table fills (that's the birthday-paradox tail). We keep
`(live + tombstones) / capacity < 0.7`; when it crosses that, we allocate a 2x array and
re-insert only the live entries (which also purges tombstones). Rehash is O(n) but rare, so
amortized cost stays O(1). `reserve(n)` presizes so a known workload never rehashes mid-run.

```cpp
void insert(uint64_t key, uint32_t val) {
    if ((size_ + tomb_) * 10 >= cap_ * 7) rehash(cap_ << 1);   // load >= 0.7
    // ... linear-probe to the key (update) or first EMPTY/DELETED slot (place)
}
```

---

## 7. Why it's faster (the summary)

| | `std::unordered_map` | open-addressing `IdMap` |
|---|---|---|
| memory | node per element, scattered | one flat array |
| allocation on hot path | yes (insert/erase) | none |
| lookup | hash + bucket + pointer chase | hash + short cache-adjacent scan |
| cache behavior | misses on node chains | prefetcher-friendly |

**Measured** (this project, Apple clang -O3 -march=native): swapping the map lifted the book
from ~14M to ~20.8M msg/s (speedup over the naive baseline 1.8x -> 2.7x), and on the real BTC
replay it halved median op latency, 84 ns -> 42 ns.

---

## 8. Trade-offs (when NOT to bother)

- **Clustering:** linear probing forms runs; a bad hash makes them long. splitmix64 mitigates.
- **Tombstone buildup** under delete-heavy churn - needs the rehash (or backward-shift).
- **Reserved key values:** open addressing needs sentinels; here `state` avoids stealing a key
  value, but a value-based sentinel scheme would reserve one key.
- For cold paths or small/rarely-touched maps, `std::unordered_map` is perfectly fine and
  saves you the code. This win only matters because it is on the microsecond hot path.

---

## 9. Where to go next

1. **Hand out pool indices as the id** where the feed lets you - then no lookup at all.
2. **Backward-shift deletion** to kill tombstones.
3. **Robin Hood hashing** or a **Swiss table** (SIMD probing, e.g. `absl::flat_hash_map`) for
   even tighter tails.
4. Measure with `perf` - confirm the lookup left the top of the flamegraph.
