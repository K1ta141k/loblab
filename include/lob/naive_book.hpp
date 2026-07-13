#pragma once
#include "lob/types.hpp"
#include <map>
#include <list>
#include <unordered_map>

namespace lob {

// Deliberately naive baseline for A/B benchmarking: std::map of price -> std::list
// of orders, with per-order heap allocation and tree traversal. This is roughly
// what a hand-written first pass looks like; the optimized OrderBook should beat
// it by a large factor on the hot path.
class NaiveBook {
public:
    explicit NaiveBook(int32_t /*max_tick*/, std::size_t /*pool*/ = 0) {}

    bool add(uint64_t id, Side side, int32_t tick, uint32_t qty) {
        auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto& lst = book[tick];
        lst.push_back(Order{id, qty, tick, side, NIL, NIL});
        auto it = std::prev(lst.end());
        loc_[id] = {side, tick, it};
        return true;
    }

    bool cancel(uint64_t id) {
        auto it = loc_.find(id);
        if (it == loc_.end()) return false;
        auto& [side, tick, node] = it->second;
        auto& book = (side == Side::Buy) ? bids_ : asks_;
        auto b = book.find(tick);
        if (b != book.end()) { b->second.erase(node); if (b->second.empty()) book.erase(b); }
        loc_.erase(it);
        return true;
    }

    bool modify(uint64_t id, uint32_t new_qty) {
        auto it = loc_.find(id);
        if (it == loc_.end()) return false;
        if (new_qty <= it->second.node->qty) { it->second.node->qty = new_qty; return true; }
        Side side = it->second.side; int32_t tick = it->second.tick;  // copy before cancel invalidates it
        cancel(id);
        return add(id, side, tick, new_qty);
    }

    int32_t best_bid() const { return bids_.empty() ? -1 : bids_.rbegin()->first; }
    int32_t best_ask() const { return asks_.empty() ? -1 : asks_.begin()->first; }

private:
    using ListIt = std::list<Order>::iterator;
    struct Loc { Side side; int32_t tick; ListIt node; };
    std::map<int32_t, std::list<Order>> bids_;
    std::map<int32_t, std::list<Order>> asks_;
    std::unordered_map<uint64_t, Loc> loc_;
};

} // namespace lob
