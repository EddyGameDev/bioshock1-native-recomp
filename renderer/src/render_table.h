#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace bs1::renderer {

/**
 * Fast open-addressing hash table mapping fixed-size uint32_t keys to dense
 * integer handles (1-based; 0 means absent).
 */
template <int KeyWords>
class KeyTable {
 public:
  KeyTable() { Clear(); }

  void Clear() {
    keys_.assign(64 * KeyWords, 0);
    handles_.assign(64, 0);
    tombstone_.assign(64, 0);
    used_ = 0;
    tombs_ = 0;
  }

  uint32_t Size() const { return used_; }

  uint32_t Find(const uint32_t* key) const {
    const size_t mask = handles_.size() - 1;
    size_t slot = static_cast<size_t>(Hash(key)) & mask;
    for (;;) {
      const uint32_t h = handles_[slot];
      if (h == 0) {
        if (!tombstone_[slot]) return 0;
      } else if (Equal(&keys_[slot * KeyWords], key)) {
        return h;
      }
      slot = (slot + 1) & mask;
    }
  }

  void Insert(const uint32_t* key, uint32_t handle) {
    if ((used_ + tombs_ + 1) * 2 > handles_.size()) {
      Rehash(used_ * 4 < handles_.size() ? handles_.size() : handles_.size() * 2);
    }
    const size_t mask = handles_.size() - 1;
    size_t slot = static_cast<size_t>(Hash(key)) & mask;
    size_t first_tomb = SIZE_MAX;
    for (;;) {
      const uint32_t h = handles_[slot];
      if (h == 0) {
        if (!tombstone_[slot]) {
          const size_t dst = first_tomb != SIZE_MAX ? first_tomb : slot;
          if (first_tomb != SIZE_MAX) {
            tombstone_[dst] = 0;
            --tombs_;
          }
          std::memcpy(&keys_[dst * KeyWords], key, sizeof(uint32_t) * KeyWords);
          handles_[dst] = handle;
          ++used_;
          return;
        }
        if (first_tomb == SIZE_MAX) first_tomb = slot;
      } else if (Equal(&keys_[slot * KeyWords], key)) {
        handles_[slot] = handle;
        return;
      }
      slot = (slot + 1) & mask;
    }
  }

  void Erase(const uint32_t* key) {
    const size_t mask = handles_.size() - 1;
    size_t slot = static_cast<size_t>(Hash(key)) & mask;
    for (;;) {
      const uint32_t h = handles_[slot];
      if (h == 0) {
        if (!tombstone_[slot]) return;
      } else if (Equal(&keys_[slot * KeyWords], key)) {
        handles_[slot] = 0;
        tombstone_[slot] = 1;
        --used_;
        ++tombs_;
        return;
      }
      slot = (slot + 1) & mask;
    }
  }

 private:
  static uint64_t Hash(const uint32_t* key) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < KeyWords; ++i) {
      h ^= key[i];
      h *= 1099511628211ULL;
    }
    return h ^ (h >> 29);
  }

  static bool Equal(const uint32_t* a, const uint32_t* b) {
    for (int i = 0; i < KeyWords; ++i) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  void Rehash(size_t new_cap) {
    if (new_cap < 64) new_cap = 64;
    std::vector<uint32_t> old_keys = std::move(keys_);
    std::vector<uint32_t> old_handles = std::move(handles_);
    keys_.assign(new_cap * KeyWords, 0);
    handles_.assign(new_cap, 0);
    tombstone_.assign(new_cap, 0);
    used_ = 0;
    tombs_ = 0;
    for (size_t i = 0; i < old_handles.size(); ++i) {
      if (old_handles[i] != 0) {
        Insert(&old_keys[i * KeyWords], old_handles[i]);
      }
    }
  }

  std::vector<uint32_t> keys_;
  std::vector<uint32_t> handles_;
  std::vector<uint8_t> tombstone_;
  uint32_t used_ = 0;
  uint32_t tombs_ = 0;
};

}  // namespace bs1::renderer
