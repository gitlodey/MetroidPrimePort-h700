#include "fifo.hpp"

#include "../thread.hpp"
#include "../gfx/render_worker.hpp"
#include "command_processor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstring>
#include <limits>
#include <mutex>

#include <tracy/Tracy.hpp>

namespace aurora::gx::fifo {
namespace detail {
std::atomic_bool sFailed{false};
bool sWritesOpen = true;
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
constexpr auto kProcessingMode = ProcessingMode::Thread;
constexpr uint32_t kDrawBatchSize = 1;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
std::atomic_bool sWorkerExited{true};
std::atomic_bool sWorkerJoined{true};
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();
}

bool fail(const char* reason) {
  if (!detail::sFailed.exchange(true, std::memory_order_acq_rel)) Log.error("{}", reason);
  gfx::render_worker::notify_failure();
  wake_worker();
  return false;
}

bool healthy() {
  if (detail::sFailed.load(std::memory_order_acquire)) return false;
  return gfx::render_worker::wait_healthy() || fail("FIFO downstream wait failed");
}

bool progress_wait() {
  if (!healthy()) return false;
  if (!gfx::render_worker::progress_wait()) return fail("FIFO progress failed");
  std::this_thread::sleep_for(std::chrono::milliseconds{1});
  return healthy();
}

bool lock_buffer(std::unique_lock<std::mutex>& lock) {
  while (healthy()) {
    if (lock.try_lock()) {
      if (healthy()) return true;
      lock.unlock();
      return false;
    }
    // The FIFO processor can itself be blocked on downstream capacity while holding
    // sBufferMutex. Never take that lock before servicing the main presenter.
    if (!progress_wait()) return false;
  }
  return false;
}

bool process_to(uint64_t target, std::memory_order order) noexcept {
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    if (!healthy()) return false;
    ProcessResult result{};
    {
      std::lock_guard lock{sBufferMutex};
      // The producer may already be appending the next batch. Only its atomic
      // published watermark is shared with us; sBufferSize is producer-owned.
      const uint64_t published = sPublished.load(std::memory_order_acquire);
      AURORA_ASSERT(processed >= sStreamBase && target <= published,
                    "FIFO processing range [{}, {}) is outside published range [{}, {})", processed, target, sStreamBase,
                    published);
      const auto start = static_cast<uint32_t>(processed - sStreamBase);
      const auto size = static_cast<uint32_t>(target - processed);
      result = process(detail::sBufferData + start, size);
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= target - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  target - processed);
    if (!healthy()) return false;
    if (result.drawDone) {
      dispatch_draw_done();
    }
    // A cancelled waiter does not retire the range while its callback is executing.
    if (!healthy()) return false;
    processed += result.bytesProcessed;
    sProcessed.store(processed, order);
    sProcessed.notify_all();
  }
  return true;
}

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, wake_worker};
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    if (!healthy()) break;
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    if (published != processed) {
      if (!process_to(published, std::memory_order_release)) break;
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    sWorkerWake.wait(event, std::memory_order_acquire);
  }
  sWorkerExited.store(true, std::memory_order_release);
}

void start_worker() {
  if (kProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerExited.store(false, std::memory_order_release);
  sWorkerJoined.store(false, std::memory_order_release);
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                 },
                                 worker_main};
}

bool stop_worker() {
  if (!sWorkerThread.joinable()) return true;
  sWorkerThread.request_stop();
  while (!sWorkerExited.load(std::memory_order_acquire)) {
    if (!progress_wait()) return false;
  }
  sWorkerThread.join();
  sWorkerJoined.store(true, std::memory_order_release);
  return true;
}
} // namespace

ProcessingMode processing_mode() noexcept { return kProcessingMode; }

bool init() {
  if (!stop_worker() || !gfx::render_worker::wait_healthy()) return false;

  constexpr uint32_t initialCapacity = 64 * 1024;
  auto* buffer = static_cast<uint8_t*>(malloc(initialCapacity));
  if (!buffer) return fail("fifo::init: allocation failed");
  free(detail::sBufferData);
  detail::sBufferData = buffer;
  detail::sFailed.store(false, std::memory_order_release);
  detail::sWritesOpen = true;
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
  return true;
}

bool shutdown() {
  if (!stop_worker()) return false;
  detail::sWritesOpen = false;
  return true;
}
bool is_quiescent() noexcept { return sWorkerJoined.load(std::memory_order_acquire); }
bool failed() noexcept { return detail::sFailed.load(std::memory_order_acquire); }
uint64_t published_bytes() noexcept { return sPublished.load(std::memory_order_acquire); }
uint64_t processed_bytes() noexcept { return sProcessed.load(std::memory_order_acquire); }

bool begin_frame() noexcept {
  if (!healthy() || sFrameActive || (kProcessingMode == ProcessingMode::Thread && !sWorkerThread.joinable()))
    return false;
  if (!gfx::render_worker::root_admission_open()) {
    detail::sWritesOpen = false;
    return false;
  }
  detail::sWritesOpen = true;
  sFrameActive = true;
  return true;
}

bool end_frame() noexcept {
  if (!healthy()) return false;
  sFrameActive = false;
  clear_draw_cache(); // command_processor; only after a successful drain
  detail::sWritesOpen = gfx::render_worker::root_admission_open();
  return true;
}

bool frame_active() noexcept { return sFrameActive; }

bool write_data_grow(const void* data, uint32_t length) {
  if (!healthy() || !detail::sWritesOpen) return false;
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  if (needed64 > std::numeric_limits<uint32_t>::max()) return fail("FIFO buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  std::unique_lock lock{sBufferMutex, std::defer_lock};
  if (!lock_buffer(lock)) return false;
  auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
  if (!resized) return fail("FIFO buffer allocation failed");
  detail::sBufferData = resized;
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
  return true;
}

void publish() noexcept {
  if (!healthy() || !detail::sWritesOpen || !sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
    if (kProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!healthy() || !sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  if (!healthy()) return;
  AURORA_ASSERT(!detail::sInDisplayList && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  const uint32_t bytesWritten = detail::sDlWritePos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

bool drain() {
  if (!healthy()) return false;
  if (detail::sBufferSize == 0) return true;

  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (kProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    if (!process_to(target, std::memory_order_relaxed)) return false;
    break;
  case ProcessingMode::Thread: {
    if (!sWorkerThread.joinable()) return fail("FIFO drain requires a live processor; buffer retained");
    sPublished.store(target, std::memory_order_release);
    wake_worker();

    while (sProcessed.load(std::memory_order_acquire) < target) {
      if (!progress_wait()) return false;
    }
    break;
  }
  }

  std::unique_lock lock{sBufferMutex, std::defer_lock};
  if (!lock_buffer(lock)) return false;
  sStreamBase = target;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
  return true;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

bool clear_buffer() {
  if (!healthy()) return false;
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  std::unique_lock lock{sBufferMutex, std::defer_lock};
  if (!lock_buffer(lock)) return false;
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
  return true;
}

#ifdef AURORA_FIFO_TESTING
bool testing::try_buffer_lock() {
  std::unique_lock lock{sBufferMutex, std::try_to_lock};
  return lock.owns_lock();
}
#endif

} // namespace aurora::gx::fifo
