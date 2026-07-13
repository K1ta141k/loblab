#pragma once
#include "lob/types.hpp"
#include "lob/id_map.hpp"
#include <vector>
#include <cstdint>
#include <algorithm>

namespace lob {

// Optimized limit order book.
//
// Design (the low-latency techniques worth showing at HRT):
//   - Price levels are a flat array indexed by tick -> O(1) level access, no tree.
//   - Each level is an intrusive FIFO doubly-linked list of order-pool indices
//     (price-time priority), so add/cancel touch a couple of cache lines.
//   - Orders come from a preallocated object pool with a free list -> zero heap
//     allocation on the hot path.
//   - best_bid_/best_ask_ are maintained incrementally.
//
// Single instrument, single-threaded hot path (HFT-typical). Tick range is fixed
// at construction; out-of-range ticks are rejected.
class OrderBook {
public:
    explicit OrderBook(int32_t max_tick, std::size_t pool_capacity = 1u << 20)
        : bid_(static_cast<std::size_t>(max_tick) + 1),
          ask_(static_cast<std::size_t>(max_tick) + 1),
          max_tick_(max_tick) {
        pool_.reserve(pool_capacity);
        id_to_idx_.reserve(pool_capacity);
        best_ask_ = max_tick_ + 1;  // empty ask sentinel
    }

    // Add a resting limit order. Returns false on bad tick / duplicate id.
    bool add(uint64_t id, Side side, int32_t tick, uint32_t qty) {
        if (tick < 0 || tick > max_tick_) return false;
        if (id_to_idx_.find(id) != NIL) return false;   // duplicate id
        uint32_t idx = alloc();
        pool_[idx] = Order{id, qty, tick, side, NIL, NIL};
        id_to_idx_.insert(id, idx);

        Level& lv = level(side, tick);
        // append to tail (time priority)
        pool_[idx].prev = lv.tail;
        if (lv.tail != NIL) pool_[lv.tail].next = idx; else lv.head = idx;
        lv.tail = idx;
        lv.total_qty += qty;
        ++lv.count;

        if (side == Side::Buy) { if (tick > best_bid_) best_bid_ = tick; }
        else                   { if (tick < best_ask_) best_ask_ = tick; }
        return true;
    }

    // Cancel a resting order by id. Returns false if unknown.
    bool cancel(uint64_t id) {
        uint32_t idx = id_to_idx_.find(id);
        if (idx == NIL) return false;
        Order& o = pool_[idx];
        Level& lv = level(o.side, o.tick);

        if (o.prev != NIL) pool_[o.prev].next = o.next; else lv.head = o.next;
        if (o.next != NIL) pool_[o.next].prev = o.prev; else lv.tail = o.prev;
        lv.total_qty -= o.qty;
        --lv.count;

        int32_t tick = o.tick; Side side = o.side;
        id_to_idx_.erase(id);
        free(idx);
        if (lv.count == 0) repair_best(side, tick);
        return true;
    }

    // Modify quantity. A size increase loses time priority (moved to the tail),
    // matching real matching-engine semantics; a decrease keeps priority.
    bool modify(uint64_t id, uint32_t new_qty) {
        uint32_t idx = id_to_idx_.find(id);
        if (idx == NIL) return false;
        Order& o = pool_[idx];
        Level& lv = level(o.side, o.tick);
        if (new_qty <= o.qty) {
            lv.total_qty -= (o.qty - new_qty);
            o.qty = new_qty;
            return true;
        }
        // increase: re-queue at tail
        Side side = o.side; int32_t tick = o.tick;
        cancel(id);
        return add(id, side, tick, new_qty);
    }

    // A trade printed by the matching engine.
    struct Fill { uint64_t taker_id, maker_id; int32_t tick; uint32_t qty; Side taker_side; };

    // Submit an aggressive order and match it against the resting book with
    // price-time priority (best price first, FIFO within a level). `is_market`
    // ignores the price bound; a limit order's unfilled remainder rests in the book.
    // Fills are appended to `fills`. Returns the unfilled remaining quantity
    // (0 if fully filled; for a resting limit it is the size left on the book).
    uint32_t submit(uint64_t id, Side side, int32_t limit_tick, uint32_t qty,
                    bool is_market, std::vector<Fill>& fills) {
        uint32_t rem = qty;
        if (side == Side::Buy) {
            while (rem > 0) {
                int32_t bt = best_ask_;
                if (bt > max_tick_) break;                    // ask side empty
                if (!is_market && bt > limit_tick) break;     // does not cross
                Order& mk = pool_[ask_[bt].head];
                uint32_t trade = std::min(rem, mk.qty);
                fills.push_back({id, mk.id, bt, trade, Side::Buy});
                rem -= trade;
                if (trade == mk.qty) { uint64_t m = mk.id; cancel(m); }   // full fill: remove maker, repair best
                else { mk.qty -= trade; ask_[bt].total_qty -= trade; }    // partial: maker stays (rem now 0)
            }
        } else {
            while (rem > 0) {
                int32_t bt = best_bid_;
                if (bt < 0) break;                            // bid side empty
                if (!is_market && bt < limit_tick) break;     // does not cross
                Order& mk = pool_[bid_[bt].head];
                uint32_t trade = std::min(rem, mk.qty);
                fills.push_back({id, mk.id, bt, trade, Side::Sell});
                rem -= trade;
                if (trade == mk.qty) { uint64_t m = mk.id; cancel(m); }
                else { mk.qty -= trade; bid_[bt].total_qty -= trade; }
            }
        }
        if (rem > 0 && !is_market) add(id, side, limit_tick, rem);  // rest the remainder
        return rem;
    }

    int32_t best_bid() const { return best_bid_; }   // -1 if empty
    int32_t best_ask() const { return best_ask_; }   // max_tick+1 if empty

    uint64_t qty_at(Side side, int32_t tick) const {
        if (tick < 0 || tick > max_tick_) return 0;
        return level(side, tick).total_qty;
    }

    // Summed resting quantity over the best `levels` occupied price levels on a
    // side (walking out from the touch). Used by the order-book-imbalance signal.
    uint64_t top_depth(Side side, int levels) const {
        uint64_t sum = 0; int found = 0;
        if (side == Side::Buy) {
            for (int32_t t = best_bid_; t >= 0 && found < levels; --t)
                if (bid_[t].count) { sum += bid_[t].total_qty; ++found; }
        } else {
            for (int32_t t = best_ask_; t <= max_tick_ && found < levels; ++t)
                if (ask_[t].count) { sum += ask_[t].total_qty; ++found; }
        }
        return sum;
    }

private:
    struct Level { uint32_t head = NIL, tail = NIL, count = 0; uint64_t total_qty = 0; };

    Level& level(Side s, int32_t t) { return s == Side::Buy ? bid_[t] : ask_[t]; }
    const Level& level(Side s, int32_t t) const { return s == Side::Buy ? bid_[t] : ask_[t]; }

    uint32_t alloc() {
        if (!free_list_.empty()) { uint32_t i = free_list_.back(); free_list_.pop_back(); return i; }
        pool_.emplace_back();
        return static_cast<uint32_t>(pool_.size() - 1);
    }
    void free(uint32_t idx) { free_list_.push_back(idx); }

    void repair_best(Side side, int32_t tick) {
        if (side == Side::Buy && tick == best_bid_) {
            int32_t t = tick;
            while (t >= 0 && bid_[t].count == 0) --t;
            best_bid_ = t;
        } else if (side == Side::Sell && tick == best_ask_) {
            int32_t t = tick;
            while (t <= max_tick_ && ask_[t].count == 0) ++t;
            best_ask_ = (t <= max_tick_) ? t : (max_tick_ + 1);
        }
    }

    std::vector<Level> bid_;
    std::vector<Level> ask_;
    std::vector<Order> pool_;
    std::vector<uint32_t> free_list_;
    IdMap id_to_idx_;
    int32_t max_tick_;
    int32_t best_bid_ = -1;
    int32_t best_ask_ = 0;  // real value set in constructor body (max_tick+1)
};

} // namespace lob
