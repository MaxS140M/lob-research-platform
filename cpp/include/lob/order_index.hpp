#pragma once

// Open-addressing hash map from OrderId to a 32-bit value (an order's pool slot).
//
// Compared with std::unordered_map: one flat array of slots instead of a heap node per entry
// and a bucket array, so a lookup is usually a single cache line and inserts never allocate
// except when the table grows.
//
// - Linear probing, power-of-two capacity, load factor kept at or below 1/2.
// - Fibonacci hashing (multiply by 2^64 / phi and keep the top bits), which spreads the
//   sequential order references ITCH uses evenly across the table.
// - Backward-shift deletion: no tombstones, so probe lengths do not degrade over a day of
//   adds and deletes.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "lob/types.hpp"

namespace lob {

class OrderIndex {
public:
    static constexpr std::uint32_t kMissing = UINT32_MAX;  // never a valid value

    OrderIndex() = default;
    explicit OrderIndex(std::size_t expected) { reserve(expected); }

    // The value for `key`, or kMissing.
    [[nodiscard]] std::uint32_t find(OrderId key) const noexcept {
        if (slots_.empty()) return kMissing;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            const Slot& s = slots_[i];
            if (s.value == kMissing) return kMissing;
            if (s.key == key) return s.value;
        }
    }

    [[nodiscard]] bool contains(OrderId key) const noexcept { return find(key) != kMissing; }

    // Inserts `key` and returns a pointer to its value slot for the caller to fill in, or
    // nullptr if the key is already present. The pointer is valid until the next insert or
    // erase. The value must be set to something other than kMissing before then.
    std::uint32_t* try_insert(OrderId key) {
        if ((size_ + 1) * 2 > slots_.size()) grow();
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.value == kMissing) {
                s.key = key;
                s.value = 0;
                ++size_;
                return &s.value;
            }
            if (s.key == key) return nullptr;
        }
    }

    // Removes `key`. Returns false if it was not present.
    bool erase(OrderId key) noexcept {
        if (slots_.empty()) return false;
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].value == kMissing) return false;
            if (slots_[i].key == key) break;
        }
        // Backward shift: pull later entries of the probe run into the hole if that moves
        // them no further from their home slot, so lookups never need tombstones.
        for (std::size_t j = (i + 1) & mask_;; j = (j + 1) & mask_) {
            if (slots_[j].value == kMissing) break;
            const std::size_t h = home(slots_[j].key);
            // Can slot j's entry move to i? Only if its home is not in the range (i, j].
            const bool home_in_gap = i <= j ? (h > i && h <= j) : (h > i || h <= j);
            if (!home_in_gap) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].value = kMissing;
        --size_;
        return true;
    }

    void reserve(std::size_t expected) {
        if (expected * 2 > slots_.size())
            rehash(std::bit_ceil(std::max<std::size_t>(16, expected * 2)));
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

private:
    struct Slot {
        OrderId key;
        std::uint32_t value = kMissing;
    };

    [[nodiscard]] std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> shift_);
    }

    void grow() { rehash(slots_.empty() ? 16 : slots_.size() * 2); }

    void rehash(std::size_t capacity) {
        std::vector<Slot> old(capacity);
        old.swap(slots_);
        mask_ = capacity - 1;
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(capacity));
        for (const Slot& s : old) {
            if (s.value == kMissing) continue;
            std::size_t i = home(s.key);
            while (slots_[i].value != kMissing) i = (i + 1) & mask_;
            slots_[i] = s;
        }
    }

    std::vector<Slot> slots_;
    std::size_t mask_ = 0;
    unsigned shift_ = 64;
    std::size_t size_ = 0;
};

}  // namespace lob
