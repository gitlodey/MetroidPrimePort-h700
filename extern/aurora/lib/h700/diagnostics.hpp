#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aurora::h700::diagnostic {
using Clock = std::chrono::steady_clock;
enum class Kind { Compiler, ResourceBuffer, ResourceTexture, RenderEncoder, RenderWait, Barrier, Submitted, Stop };
struct Event { Kind kind; uint64_t key; uint64_t ns; std::thread::id thread; bool begin; };
struct Point { uint32_t x, y; };
inline constexpr std::array<Point, 10> Points{{{80,200},{240,200},{400,200},{560,200},
    {80,40},{560,40},{80,440},{560,440},{320,440},{24,24}}};
using Pixel = std::array<uint8_t, 4>;
struct Pixels { uint64_t frame; bool presenter; std::array<Pixel, Points.size()> values; };

// Fixed IG12 phases only. No caller callbacks, GPU completion substitution or general scheduler.
// Install before producers, keep alive through generation release, uninstall after joins/callbacks.
class Control {
public:
  const unsigned generation;
  const Clock::time_point deadline;
  std::atomic_bool failed{false}, compilerHeld{false};
  std::atomic_uint renderWaiting{0};
  std::atomic_uint64_t compilerHeldKey{0}, renderWaitingKey{0};
  std::atomic_uint activeFrame{0};
  std::atomic_uint safeCreditsHeld{0}, imports{0};
  explicit Control(unsigned gen, Clock::time_point end) : generation(gen), deadline(end) {}
  void event(Kind kind, uint64_t key, bool begin) {
    std::lock_guard lock(m_mutex);
    if (m_events.size() == 32768) { cancel_locked(); return; }
    m_events.push_back({kind, key, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count()), std::this_thread::get_id(), begin});
  }
  void interval(Kind kind, uint64_t key, Clock::time_point begin, Clock::time_point end) {
    std::lock_guard lock(m_mutex);
    if (m_events.size() + 2 > 32768) { cancel_locked(); return; }
    bool first = true;
    for (const auto point : {begin, end}) {
      m_events.push_back({kind, key,
          static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(point.time_since_epoch()).count()),
          std::this_thread::get_id(), first});
      first = false;
    }
  }
  void storage(unsigned slot, uintptr_t image, uintptr_t source, uintptr_t fbo, unsigned width, unsigned height) {
    std::lock_guard lock(m_mutex);
    if (slot >= 2 || width != 640 || height != 480) { cancel_locked(); return; }
    const std::array<uintptr_t,3> value{image,source,fbo};
    if (!m_storage[slot][0]) m_storage[slot]=value;
    else if (m_storage[slot]!=value) cancel_locked();
  }
  std::array<std::array<uintptr_t,3>,2> storage() { std::lock_guard lock(m_mutex); return m_storage; }
  void compiled(uint64_t key) {
    std::lock_guard lock(m_mutex);
    for (unsigned i = 0; i < m_fogCount; ++i) if (m_fog[i] == key) return;
    if (m_fogCount == m_fog.size()) { cancel_locked(); return; }
    m_fog[m_fogCount++] = key;
  }
  unsigned fog_variants() { std::lock_guard lock(m_mutex); return m_fogCount; }
  void cancel() { std::lock_guard lock(m_mutex); cancel_locked(); }
  bool healthy() {
    if (Clock::now() >= deadline) cancel();
    return !failed.load(std::memory_order_acquire);
  }
  void arm_overlap() { std::lock_guard lock(m_mutex); m_overlap = generation == 1; }
  void arm_stop() { std::lock_guard lock(m_mutex); m_stop = generation == 2; }
  void release() {
    std::lock_guard lock(m_mutex);
    m_released = true;
    m_cv.notify_all();
  }
  bool before(Kind kind, uint64_t key, bool fog = false) {
    if (!healthy()) return false;
    bool barrier = false;
    {
      std::unique_lock lock(m_mutex);
      if (m_overlap && !m_overlapDone) {
        unsigned role = kind == Kind::Compiler && fog ? 1 : kind == Kind::ResourceTexture ? 2 :
                        kind == Kind::RenderEncoder ? 4 : 0;
        if (role) {
          barrier = true;
          m_roles |= role;
          if (m_roles == 7) { m_overlapDone = true; m_cv.notify_all(); }
        }
      }
      if (m_stop && kind == Kind::Compiler && fog && !m_released) {
        compilerHeldKey.store(key, std::memory_order_release);
        compilerHeld.store(true, std::memory_order_release);
        barrier = true;
      }
    }
    if (barrier) {
      event(Kind::Barrier, key, true);
      std::unique_lock lock(m_mutex);
      if (!m_cv.wait_until(lock, deadline, [&] { return m_released || failed.load() ||
          ((!m_overlap || m_overlapDone) && !(m_stop && kind == Kind::Compiler && fog)); })) cancel_locked();
      lock.unlock();
      event(Kind::Barrier, key, false);
      compilerHeld.store(false, std::memory_order_release);
    }
    return healthy();
  }
  void third_acquire() { m_third.store(true, std::memory_order_release); }
  void progress() {
    if (!healthy()) return;
    if (generation == 1 && m_third.load(std::memory_order_acquire)) {
      const auto passes = m_progress.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (passes >= 4) { std::lock_guard lock(m_mutex); m_creditRelease = true; m_third.store(false); }
    }
  }
  bool hold_credit(uint64_t frame) {
    std::lock_guard lock(m_mutex);
    const bool hold = !failed.load() && !m_released &&
        ((generation == 1 && frame < 2 && !m_creditRelease) || (generation == 2 && frame == 0));
    if (hold) safeCreditsHeld.fetch_add(1, std::memory_order_relaxed);
    return hold;
  }
  unsigned pressure_passes() const { return m_progress.load(std::memory_order_acquire); }
  bool overlap_barrier_complete() { std::lock_guard lock(m_mutex); return m_overlapDone; }
  void pixels(Pixels value) {
    std::lock_guard lock(m_mutex);
    if (m_pixels.size() == 256) { cancel_locked(); return; }
    m_pixels.push_back(value);
  }
  std::vector<Pixels> take_pixels() { std::lock_guard lock(m_mutex); std::vector<Pixels> result; result.swap(m_pixels); return result; }
  std::vector<Event> events() { std::lock_guard lock(m_mutex); return m_events; }
private:
  void cancel_locked() { failed.store(true, std::memory_order_release); m_released = true; m_cv.notify_all(); }
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_overlap = false, m_overlapDone = false, m_stop = false, m_released = false, m_creditRelease = false;
  unsigned m_roles = 0, m_fogCount = 0;
  std::array<uint64_t, 4> m_fog{};
  std::array<std::array<uintptr_t,3>,2> m_storage{};
  std::atomic_bool m_third{false};
  std::atomic_uint m_progress{0};
  std::vector<Event> m_events;
  std::vector<Pixels> m_pixels;
};
inline std::atomic<std::shared_ptr<Control>> installed;
inline std::shared_ptr<Control> current() { return installed.load(std::memory_order_acquire); }
inline void install(std::shared_ptr<Control> value) { installed.store(std::move(value), std::memory_order_release); }
inline bool nearest() { return bool(current()); }

template <class F> auto dawn_call(Kind kind, uint64_t key, F call, bool fog = false) {
  const auto control = current();
  if (!control) return call();
  (void)control->before(kind, key, fog); // failure opens OUR gate; an executing accepted call still returns its real result
  // No trace lock, event allocation or artificial wait lies inside these endpoints.
  const auto begin = Clock::now();
  auto result = call();
  const auto end = Clock::now();
  control->interval(kind, key, begin, end);
  if (fog && result) control->compiled(key);
  return result;
}
} // namespace aurora::h700::diagnostic
