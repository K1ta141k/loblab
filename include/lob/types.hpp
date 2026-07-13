#pragma once
#include <cstdint>

namespace lob {

// Sentinel index for the intrusive free-list / linked-list nodes.
inline constexpr uint32_t NIL = 0xFFFFFFFFu;

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class MsgType : uint8_t { Add = 0, Cancel = 1, Modify = 2 };

// A normalized book message. Prices are integer ticks (price / tick_size).
struct Message {
    MsgType type;
    Side    side;
    int32_t tick;   // price in ticks (valid for Add/Modify)
    uint32_t qty;   // quantity (Add: size; Modify: new size)
    uint64_t id;    // exchange order id
};

// An order node lives in the OrderBook's object pool. prev/next are pool
// indices forming the intrusive FIFO list within a price level (no heap
// allocation on the hot path).
struct Order {
    uint64_t id;
    uint32_t qty;
    int32_t  tick;
    Side     side;
    uint32_t prev;  // pool index, or NIL
    uint32_t next;  // pool index, or NIL
};

} // namespace lob
