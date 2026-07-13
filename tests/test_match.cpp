// Matching-engine correctness: price-time priority, partial fills, resting
// remainders, and market sweeps. Zero-dependency (asserts + exit code).
#include "lob/order_book.hpp"
#include "lob/types.hpp"
#include <cstdio>
#include <vector>

using namespace lob;
using Fill = OrderBook::Fill;

static int failures = 0;
#define CHECK(cond) do { if(!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while(0)

int main() {
    // ---- price-time priority + partial fill + resting remainder ----
    {
        OrderBook b(1000);
        b.add(1, Side::Sell, 100, 5);   // best ask, FIFO first at 100
        b.add(2, Side::Sell, 100, 3);   // FIFO second at 100
        b.add(3, Side::Sell, 101, 4);
        CHECK(b.best_ask() == 100);

        std::vector<Fill> fills;
        uint32_t rem = b.submit(99, Side::Buy, 101, 10, /*market=*/false, fills);

        CHECK(rem == 0);
        CHECK(fills.size() == 3);
        // price-time: id1@100(5), id2@100(3), then id3@101(2)
        CHECK(fills[0].maker_id == 1 && fills[0].tick == 100 && fills[0].qty == 5);
        CHECK(fills[1].maker_id == 2 && fills[1].tick == 100 && fills[1].qty == 3);
        CHECK(fills[2].maker_id == 3 && fills[2].tick == 101 && fills[2].qty == 2);
        CHECK(b.qty_at(Side::Sell, 100) == 0);   // level cleared
        CHECK(b.qty_at(Side::Sell, 101) == 2);   // id3 partially filled, 2 left
        CHECK(b.best_ask() == 101);
    }

    // ---- limit order that does not fully fill rests on the book ----
    {
        OrderBook b(1000);
        b.add(1, Side::Sell, 105, 2);
        std::vector<Fill> fills;
        // buy 6 @105: fills 2, remaining 4 rests as a bid @105
        uint32_t rem = b.submit(9, Side::Buy, 105, 6, false, fills);
        CHECK(fills.size() == 1 && fills[0].qty == 2);
        CHECK(rem == 4);
        CHECK(b.best_bid() == 105 && b.qty_at(Side::Buy, 105) == 4);
        CHECK(b.best_ask() > 1000);               // ask side empty
    }

    // ---- sell crossing the bid side ----
    {
        OrderBook b(1000);
        b.add(10, Side::Buy, 99, 5);
        std::vector<Fill> fills;
        uint32_t rem = b.submit(11, Side::Sell, 99, 3, false, fills);
        CHECK(rem == 0 && fills.size() == 1 && fills[0].tick == 99 && fills[0].qty == 3);
        CHECK(fills[0].taker_side == Side::Sell);
        CHECK(b.qty_at(Side::Buy, 99) == 2);
    }

    // ---- market order sweeps the book, unfilled remainder is dropped (not rested) ----
    {
        OrderBook b(1000);
        b.add(1, Side::Sell, 100, 2);
        b.add(2, Side::Sell, 102, 2);
        std::vector<Fill> fills;
        uint32_t rem = b.submit(7, Side::Buy, 0, 10, /*market=*/true, fills);
        CHECK(fills.size() == 2);                  // took both levels
        CHECK(rem == 6);                           // 10 - 4 unfilled
        CHECK(b.best_ask() > 1000);                // book emptied
        CHECK(b.best_bid() == -1);                 // market remainder not rested
    }

    // ---- limit buy with no crossing asks rests as a bid ----
    {
        OrderBook b(1000);
        std::vector<Fill> fills;
        uint32_t rem = b.submit(1, Side::Buy, 100, 8, false, fills);
        CHECK(fills.empty() && rem == 8 && b.best_bid() == 100 && b.qty_at(Side::Buy, 100) == 8);
    }

    if (failures) { std::printf("FAILED: %d checks\n", failures); return 1; }
    std::printf("OK: matching engine (price-time, partials, resting, market sweep)\n");
    return 0;
}
