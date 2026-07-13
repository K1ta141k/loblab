#pragma once
#include <cstdint>
#include <vector>

namespace lob {

// Open-addressing (linear-probing) hash map<uint64_t order_id -> uint32_t pool idx>,
// specialized for the book's hot path. Replaces std::unordered_map to kill node
// allocation and pointer chasing on cancel/modify lookups:
//   - flat power-of-2 slot array, no per-element heap allocation
//   - splitmix64 finalizer for good distribution on sequential/sparse exchange ids
//   - tombstone deletes with rehash when load (live+tombstones) crosses ~70%
//
// value NIL (0xFFFFFFFF) is returned by find() for "not present".
class IdMap {
public:
    explicit IdMap(std::size_t cap_pow2 = 1u << 16) { init(cap_pow2); }

    void reserve(std::size_t n) {
        std::size_t need = 8;
        while (need * 7 < n * 10) need <<= 1;   // keep load < 0.7
        if (need > cap_) rehash(need);
    }

    void insert(uint64_t key, uint32_t val) {
        if ((size_ + tomb_) * 10 >= cap_ * 7) rehash(cap_ << 1);
        std::size_t mask = cap_ - 1;
        std::size_t i = hash(key) & mask;
        std::size_t first_tomb = SIZE_MAX;
        while (true) {
            Slot& s = slot_[i];
            if (s.state == EMPTY) {
                std::size_t at = (first_tomb != SIZE_MAX) ? first_tomb : i;
                if (first_tomb != SIZE_MAX) --tomb_;
                slot_[at] = {key, val, FULL};
                ++size_;
                return;
            }
            if (s.state == FULL && s.key == key) { s.val = val; return; }
            if (s.state == DELETED && first_tomb == SIZE_MAX) first_tomb = i;
            i = (i + 1) & mask;
        }
    }

    uint32_t find(uint64_t key) const {
        std::size_t mask = cap_ - 1;
        std::size_t i = hash(key) & mask;
        while (true) {
            const Slot& s = slot_[i];
            if (s.state == EMPTY) return 0xFFFFFFFFu;
            if (s.state == FULL && s.key == key) return s.val;
            i = (i + 1) & mask;
        }
    }

    bool erase(uint64_t key) {
        std::size_t mask = cap_ - 1;
        std::size_t i = hash(key) & mask;
        while (true) {
            Slot& s = slot_[i];
            if (s.state == EMPTY) return false;
            if (s.state == FULL && s.key == key) { s.state = DELETED; --size_; ++tomb_; return true; }
            i = (i + 1) & mask;
        }
    }

private:
    enum State : uint8_t { EMPTY = 0, FULL = 1, DELETED = 2 };
    struct Slot { uint64_t key; uint32_t val; uint8_t state; };

    static uint64_t hash(uint64_t x) {  // splitmix64 finalizer
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27; x *= 0x94d049bb133111ebull;
        x ^= x >> 31; return x;
    }

    void init(std::size_t cap) {
        cap_ = cap; size_ = 0; tomb_ = 0;
        slot_.assign(cap_, Slot{0, 0, EMPTY});
    }

    void rehash(std::size_t new_cap) {
        std::vector<Slot> old = std::move(slot_);
        init(new_cap);
        for (const Slot& s : old)
            if (s.state == FULL) insert(s.key, s.val);
    }

    std::vector<Slot> slot_;
    std::size_t cap_ = 0, size_ = 0, tomb_ = 0;
};

} // namespace lob
