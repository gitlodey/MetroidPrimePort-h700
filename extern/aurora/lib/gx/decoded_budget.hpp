#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

namespace aurora::gx::decoded {

// The allocation, not cache membership, owns the charge. Eviction cannot spend
// bytes still held by a recorded packet or a native presentation slot twice.
class RetainedBudget {
public:
  class Reservation {
  public:
    Reservation(Reservation&& other) noexcept
    : m_owner(std::exchange(other.m_owner, nullptr)), m_size(other.m_size) {}
    Reservation& operator=(Reservation&& other) noexcept {
      if (this != &other) {
        if (m_owner) m_owner->m_used.fetch_sub(m_size, std::memory_order_acq_rel);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_size = other.m_size;
      }
      return *this;
    }
    Reservation(const Reservation&) = delete;
    ~Reservation() { if (m_owner) m_owner->m_used.fetch_sub(m_size, std::memory_order_acq_rel); }
  private:
    friend class RetainedBudget;
    Reservation(RetainedBudget* owner, uint64_t size) : m_owner(owner), m_size(size) {}
    RetainedBudget* m_owner;
    uint64_t m_size;
  };

  explicit RetainedBudget(uint64_t limit) : m_limit(limit) {}
  std::optional<Reservation> reserve(uint64_t size) noexcept {
    auto used = m_used.load(std::memory_order_acquire);
    do {
      if (size == 0 || used > m_limit || size > m_limit - used) return std::nullopt;
    } while (!m_used.compare_exchange_weak(used, used + size, std::memory_order_acq_rel));
    return Reservation{this, size};
  }
  uint64_t used() const noexcept { return m_used.load(std::memory_order_acquire); }
private:
  const uint64_t m_limit;
  std::atomic<uint64_t> m_used{0};
};

} // namespace aurora::gx::decoded
