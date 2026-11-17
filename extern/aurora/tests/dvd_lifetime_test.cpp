// CPU-only GM-11b regression. Include the actual implementation to start its
// private worker without opening a retail image or adding a production test API.
// Do NOT also compile dvd.cpp into this executable; aurora::dvd supplies its
// existing dependencies/FST object. CResFactory.cpp and CDvdRequest.cpp are real
// production TUs, linked with function/data sections + --gc-sections.
#include "../lib/dolphin/dvd/dvd.cpp"

#include "Kyoto/CARAMManager.hpp"
#include "Kyoto/CDvdRequest.hpp"
#include "Kyoto/CResFactory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <new>
#include <stdexcept>

#ifndef AURORA_H700
#error This regression must exercise the H700 retirement path
#endif

namespace {
using namespace std::chrono_literals;
using LoadingData = CResFactory::SLoadingData;
constexpr size_t kBytes = 32;

// Observe the actual delete[] in SLoadingData's member tail. Quarantine just
// this allocation until the test ends so an ordering regression is a defined
// assertion failure, not a test-induced use-after-free in the worker callback.
struct Destination;
std::atomic<Destination*> s_destination{nullptr};
struct Destination {
  uchar* const bytes = new uchar[kBytes]{};
  std::atomic_int releases{0};
  Destination() { s_destination.store(this); }
  ~Destination() {
    s_destination.store(nullptr);
    ::operator delete(bytes);
  }
};

bool ObserveArrayDelete(void* ptr) {
  Destination* destination = s_destination.load();
  if (destination == nullptr || ptr != destination->bytes) return false;
  destination->releases.fetch_add(1);
  return true;
}

struct Gate {
  Destination& destination;
  std::mutex mutex;
  std::condition_variable cv;
  bool allowRead = false;
  bool allowCallback = false;
  bool readEntered = false;
  bool callbackEntered = false;
  bool callbackReturned = false;
  bool requestDestructorEntered = false;
  bool handleClosed = false;
  bool ownerDestroyed = false;
  bool readSawLiveDestination = false;
  bool callbackSawLiveDestination = false;
  bool timedOut = false;
  bool failRead = false;

  explicit Gate(Destination& destination) : destination(destination) {}

  bool Wait(bool Gate::*flag) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 2s, [&] { return this->*flag; });
  }
  void Set(bool Gate::*flag) {
    std::lock_guard lock(mutex);
    this->*flag = true;
    cv.notify_all();
  }
  void ReleaseAll() {
    std::lock_guard lock(mutex);
    allowRead = true;
    allowCallback = true;
    cv.notify_all();
  }
};

class GatedRead final : public CommandDataBase {
public:
  Gate& gate;
  explicit GatedRead(Gate& gate) : gate(gate) {}
  ~GatedRead() override { gate.Set(&Gate::handleClosed); }
  int64_t seek(int64_t offset, int32_t) override { return offset; }
  int64_t read(uint8_t* bytes, size_t size) override {
    std::unique_lock lock(gate.mutex);
    gate.readEntered = true;
    gate.cv.notify_all();
    if (!gate.cv.wait_for(lock, 2s, [&] { return gate.allowRead; })) gate.timedOut = true;
    gate.readSawLiveDestination = gate.destination.releases.load() == 0;
    if (gate.failRead) return -1;
    std::memset(bytes, 0x51, size);
    return static_cast<int64_t>(size);
  }

  static void Callback(s32, DVDFileInfo* file) {
    auto& gate = static_cast<GatedRead*>(static_cast<CommandDataBase*>(file->cb.userData))->gate;
    std::unique_lock lock(gate.mutex);
    gate.callbackEntered = true;
    gate.cv.notify_all();
    if (!gate.cv.wait_for(lock, 2s, [&] { return gate.allowCallback; })) gate.timedOut = true;
    gate.callbackSawLiveDestination = gate.destination.releases.load() == 0;
    // Even END/FATAL_ERROR status is published BEFORE this final destination use.
    gate.destination.bytes[1] = 0xa7;
    gate.callbackReturned = true;
    gate.cv.notify_all();
  }
};

class ObservedRequest final : public CRealDvdRequest {
  Gate& gate;
public:
  explicit ObservedRequest(Gate& gate) : gate(gate) {}
  ~ObservedRequest() override { gate.Set(&Gate::requestDestructorEntered); }
  bool IsComplete() override { throw std::runtime_error("destructor polled completion"); }
  void WaitUntilComplete() override { throw std::runtime_error("disc destructor used throwing wait"); }
};

std::unique_ptr<LoadingData> MakeLoadingData(CDvdRequest* request, Destination& destination) {
  return std::make_unique<LoadingData>(SObjectTag(0x4d4c564c, 1), request, nullptr,
      destination.bytes, kBytes, CResLoader::kCompressionType_Uncompressed, CVParamTransfer::Null());
}

ObservedRequest* StartRead(Gate& gate) {
  auto* request = new ObservedRequest(gate);
  DVDFileInfo* file = request->FileInfo();
  file->length = kBytes;
  file->cb.userData = static_cast<CommandDataBase*>(new GatedRead(gate));
  DVDReadAsyncPrio(file, gate.destination.bytes, kBytes, 0, GatedRead::Callback, 2);
  return request;
}

class Retirement {
  Gate& gate;
  std::thread thread;
public:
  Retirement(Gate& gate, std::unique_ptr<LoadingData> owner)
  : gate(gate), thread([&gate, owner = std::move(owner)]() mutable {
      owner.reset(); // actual SLoadingData destructor and real DVDClose
      gate.Set(&Gate::ownerDestroyed);
    }) {}
  ~Retirement() {
    gate.ReleaseAll();
    thread.join();
  }
};

class H700DvdLifetime : public testing::Test {
protected:
  void SetUp() override { s_worker.start(); }
  void TearDown() override { s_worker.stop(); }

  static uchar* InitializeAram() {
    static u32 allocationStack[8]{};
    aurora::g_config.mem2Size = 64 * 1024;
    ARInit(allocationStack, 8);
    return static_cast<uchar*>(ARGetStorageAddress());
  }

  void CheckCallbackLifetime(bool failRead) {
    Destination destination;
    Gate gate(destination);
    gate.allowRead = true;
    gate.failRead = failRead;
    auto* request = StartRead(gate);
    auto owner = MakeLoadingData(request, destination);
    ASSERT_TRUE(gate.Wait(&Gate::callbackEntered));
    // Actual backend terminal state, while the worker is still in its callback.
    EXPECT_EQ(DVDGetFileInfoStatus(request->FileInfo()), failRead ? DVD_STATE_FATAL_ERROR : DVD_STATE_END);
    if (failRead) {
      EXPECT_THROW(request->CRealDvdRequest::IsComplete(), std::runtime_error);
    } else {
      EXPECT_TRUE(request->CRealDvdRequest::IsComplete());
    }
    Retirement retirement(gate, std::move(owner));
    ASSERT_TRUE(gate.Wait(&Gate::requestDestructorEntered));
    {
      std::unique_lock lock(gate.mutex);
      EXPECT_FALSE(gate.cv.wait_for(lock, 50ms, [&] { return gate.ownerDestroyed; }));
      EXPECT_FALSE(gate.handleClosed);
    }
    EXPECT_EQ(destination.releases.load(), 0);
    gate.Set(&Gate::allowCallback);
    ASSERT_TRUE(gate.Wait(&Gate::ownerDestroyed));
    EXPECT_TRUE(gate.callbackReturned);
    EXPECT_TRUE(gate.callbackSawLiveDestination);
    EXPECT_TRUE(gate.handleClosed);
    EXPECT_FALSE(gate.timedOut);
    EXPECT_EQ(destination.bytes[1], 0xa7);
    EXPECT_EQ(destination.releases.load(), 1);
  }
};

TEST_F(H700DvdLifetime, PendingReadDrainsBeforeDestinationRelease) {
  Destination destination;
  Gate gate(destination);
  auto owner = MakeLoadingData(StartRead(gate), destination);
  ASSERT_TRUE(gate.Wait(&Gate::readEntered));
  Retirement retirement(gate, std::move(owner));
  ASSERT_TRUE(gate.Wait(&Gate::requestDestructorEntered));
  EXPECT_EQ(destination.releases.load(), 0);
  gate.Set(&Gate::allowRead);
  ASSERT_TRUE(gate.Wait(&Gate::callbackEntered));
  EXPECT_EQ(destination.releases.load(), 0);
  {
    std::lock_guard lock(gate.mutex);
    EXPECT_FALSE(gate.ownerDestroyed);
    EXPECT_FALSE(gate.handleClosed);
  }
  gate.Set(&Gate::allowCallback);
  ASSERT_TRUE(gate.Wait(&Gate::ownerDestroyed));
  EXPECT_TRUE(gate.readSawLiveDestination);
  EXPECT_TRUE(gate.callbackSawLiveDestination);
  EXPECT_TRUE(gate.callbackReturned);
  EXPECT_TRUE(gate.handleClosed);
  EXPECT_FALSE(gate.timedOut);
  EXPECT_EQ(destination.bytes[0], 0x51);
  EXPECT_EQ(destination.bytes[1], 0xa7);
  EXPECT_EQ(destination.releases.load(), 1);
}

TEST_F(H700DvdLifetime, EndStateStillWaitsForCallbackReturn) { CheckCallbackLifetime(false); }
TEST_F(H700DvdLifetime, FatalStateDestructorDoesNotCallThrowingCompletionCheck) { CheckCallbackLifetime(true); }

TEST_F(H700DvdLifetime, TransferredLoadingDataDoesNotRetireBorrowedRequestOrBuffer) {
  Destination destination;
  Gate gate(destination);
  auto source = MakeLoadingData(StartRead(gate), destination);
  ASSERT_TRUE(gate.Wait(&Gate::readEntered));
  auto owner = std::make_unique<LoadingData>(*source);
  EXPECT_FALSE(source->x8_dvdReq.owner());
  EXPECT_FALSE(source->x14_buffer.owner());
  EXPECT_TRUE(owner->x8_dvdReq.owner());
  source.reset(); // must not cancel/wait/free somebody else's pending read
  EXPECT_EQ(destination.releases.load(), 0);
  {
    std::lock_guard lock(gate.mutex);
    EXPECT_FALSE(gate.requestDestructorEntered);
    EXPECT_FALSE(gate.handleClosed);
  }
  Retirement retirement(gate, std::move(owner));
  ASSERT_TRUE(gate.Wait(&Gate::requestDestructorEntered));
  EXPECT_EQ(destination.releases.load(), 0);
  gate.ReleaseAll();
  ASSERT_TRUE(gate.Wait(&Gate::ownerDestroyed));
  EXPECT_TRUE(gate.readSawLiveDestination);
  EXPECT_TRUE(gate.callbackSawLiveDestination);
  EXPECT_FALSE(gate.timedOut);
  EXPECT_EQ(destination.releases.load(), 1);
}

TEST_F(H700DvdLifetime, CompletedRequestAndNullOwnerAreSafe) {
  Destination destination;
  Gate gate(destination);
  gate.ReleaseAll();
  auto* request = StartRead(gate);
  auto owner = MakeLoadingData(request, destination);
  s_worker.wait(&request->FileInfo()->cb); // includes actual callback return
  EXPECT_TRUE(request->CRealDvdRequest::IsComplete());
  EXPECT_EQ(destination.releases.load(), 0);
  owner.reset();
  EXPECT_TRUE(gate.handleClosed);
  EXPECT_EQ(destination.releases.load(), 1);
  LoadingData empty(SObjectTag(0x4d4c564c, 2), nullptr, nullptr, nullptr, 0,
                    CResLoader::kCompressionType_Uncompressed, CVParamTransfer::Null());
}

TEST_F(H700DvdLifetime, AramDestinationRetirementWaitsForActualDeferredDmaCallback) {
  // Real Aurora ARQ implementation: memcpy now, callback deferred until ARQPoll.
  // No fake completed handles, manager wait or replacement ARQ callback.
  auto* aram = InitializeAram();
  ASSERT_NE(aram, nullptr);
  std::memset(aram + 0x4000, 0x6b, kBytes);
  Destination destination;
  const uint handle = CARAMManager::DMAToMRAM(reinterpret_cast<void*>(uintptr_t{0x4000}),
      destination.bytes, kBytes, CARAMManager::kDMAPrio_One);
  ASSERT_NE(handle, CARAMManager::GetInvalidDMAHandle());
  EXPECT_FALSE(CARAMManager::IsDMACompleted(handle)); // actual deferred obligation
  auto owner = MakeLoadingData(new CARAMDvdRequest(handle), destination);
  owner.reset();
  EXPECT_TRUE(CARAMManager::IsDMACompleted(handle));
  EXPECT_EQ(destination.bytes[0], 0x6b);
  EXPECT_EQ(destination.releases.load(), 1);
  ARQPoll(); // cleanup remains safe; no callback may still name a retired request
  CARAMManager::WaitForDMACompletion(handle);
}

TEST_F(H700DvdLifetime, InvalidAndAlreadyWaitedAramRequestsAreSafe) {
  CARAMDvdRequest invalid(CARAMManager::GetInvalidDMAHandle());
  invalid.WaitUntilComplete();
  EXPECT_TRUE(invalid.IsComplete());

  auto* aram = InitializeAram();
  ASSERT_NE(aram, nullptr);
  std::memset(aram + 0x4000, 0x38, kBytes);
  Destination destination;
  const uint handle = CARAMManager::DMAToMRAM(reinterpret_cast<void*>(uintptr_t{0x4000}),
      destination.bytes, kBytes, CARAMManager::kDMAPrio_One);
  auto* request = new CARAMDvdRequest(handle);
  request->WaitUntilComplete();
  EXPECT_TRUE(CARAMManager::IsDMACompleted(handle));
  EXPECT_TRUE(request->IsComplete());
  request->WaitUntilComplete();
  auto owner = MakeLoadingData(request, destination);
  owner.reset(); // already invalidated by the actual wait; no second DMA wait
  EXPECT_EQ(destination.bytes[0], 0x38);
  EXPECT_EQ(destination.releases.load(), 1);
}

TEST_F(H700DvdLifetime, NullRequestStillReleasesOwnedDestinationExactlyOnce) {
  Destination destination;
  auto owner = MakeLoadingData(nullptr, destination);
  owner.reset();
  EXPECT_EQ(destination.releases.load(), 1);
}
} // namespace

// Pair all test array allocations with scalar allocation/deallocation. Only the
// watched destination is quarantined, and its watcher frees it exactly once.
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* ptr) noexcept {
  if (!ObserveArrayDelete(ptr)) ::operator delete(ptr);
}
void operator delete[](void* ptr, size_t) noexcept {
  if (!ObserveArrayDelete(ptr)) ::operator delete(ptr);
}

// CPU harness globals only; production request/DMA implementations are linked.
namespace rstl {
CRefData CRefData::sNull(nullptr, 0x1000000 - 1);
}
extern "C" void OSYieldThread() { std::this_thread::yield(); }
