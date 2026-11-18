// CPU-only tests of the actual production budget and opt-in helper. No Dawn device,
// cache implementation substitute, environment mutation or renderer initialization.
// Production cache/packet/native-slot integration is checked by port_h700_decoded_wiring.py.
#include <gtest/gtest.h>

#include "gx/decoded_budget.hpp"
#include "gx/decoded_geometry.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace aurora::gx::decoded {
namespace {

static_assert(!std::is_copy_constructible_v<RetainedBudget>);
static_assert(!std::is_move_constructible_v<RetainedBudget>);
static_assert(!std::is_copy_constructible_v<RetainedBudget::Reservation>);
static_assert(!std::is_copy_assignable_v<RetainedBudget::Reservation>);
static_assert(std::is_nothrow_move_constructible_v<RetainedBudget::Reservation>);
static_assert(std::is_nothrow_move_assignable_v<RetainedBudget::Reservation>);
static_assert(noexcept(std::declval<RetainedBudget&>().reserve(1)));
static_assert(noexcept(std::declval<const RetainedBudget&>().used()));
static_assert(requested("1", "1") && requested("1", "menu"));
static_assert(!requested(nullptr, "1") && !requested("1", nullptr));
static_assert(!requested("01", "menu") && !requested("1\n", "1"));
static_assert(!requested("1", "MENU") && !requested("1", "menu "));

TEST(H700DecodedLifetime, BudgetRejectsZeroOversizeAndOneByteOverWithoutMutation) {
  RetainedBudget budget{GpuBudgetBytes};
  EXPECT_FALSE(budget.reserve(0));
  EXPECT_FALSE(budget.reserve(GpuBudgetBytes + 1));
  EXPECT_EQ(budget.used(), 0u);
  auto all = budget.reserve(GpuBudgetBytes);
  ASSERT_TRUE(all);
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_FALSE(budget.reserve(1));
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  all.reset();
  EXPECT_EQ(budget.used(), 0u);
  auto recovered = budget.reserve(GpuBudgetBytes);
  ASSERT_TRUE(recovered);
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
}

TEST(H700DecodedLifetime, BudgetArithmeticDoesNotWrapAtUint64Max) {
  constexpr auto Max = std::numeric_limits<uint64_t>::max();
  RetainedBudget budget{Max};
  auto almost = budget.reserve(Max - 1);
  ASSERT_TRUE(almost);
  EXPECT_FALSE(budget.reserve(2));
  EXPECT_EQ(budget.used(), Max - 1);
  auto last = budget.reserve(1);
  ASSERT_TRUE(last);
  EXPECT_EQ(budget.used(), Max);
  EXPECT_FALSE(budget.reserve(1));
  EXPECT_FALSE(budget.reserve(Max));
  last.reset();
  almost.reset();
  EXPECT_EQ(budget.used(), 0u);
  RetainedBudget zero{0};
  EXPECT_FALSE(zero.reserve(0));
  EXPECT_FALSE(zero.reserve(1));
  EXPECT_EQ(zero.used(), 0u);
}

TEST(H700DecodedLifetime, MoveConstructionTransfersChargeExactlyOnce) {
  RetainedBudget budget{64};
  auto source = budget.reserve(48);
  ASSERT_TRUE(source);
  {
    RetainedBudget::Reservation moved{std::move(*source)};
    source.reset(); // Engaged moved-from optional must not release the new owner's charge.
    EXPECT_EQ(budget.used(), 48u);
    EXPECT_FALSE(budget.reserve(17));
    auto& alias = moved;
    moved = std::move(alias);
    EXPECT_EQ(budget.used(), 48u);
    auto remaining = budget.reserve(16);
    ASSERT_TRUE(remaining);
    EXPECT_EQ(budget.used(), 64u);
  }
  EXPECT_EQ(budget.used(), 0u);
}

TEST(H700DecodedLifetime, MoveAssignmentReleasesPreviousBudgetAndKeepsNewOwner) {
  RetainedBudget first{32}, second{64};
  auto destination = first.reserve(24);
  auto source = second.reserve(48);
  ASSERT_TRUE(destination);
  ASSERT_TRUE(source);
  *destination = std::move(*source);
  EXPECT_EQ(first.used(), 0u);
  EXPECT_EQ(second.used(), 48u);
  source.reset();
  EXPECT_EQ(second.used(), 48u);
  EXPECT_FALSE(second.reserve(17));
  destination.reset();
  EXPECT_EQ(first.used(), 0u);
  EXPECT_EQ(second.used(), 0u);
}

TEST(H700DecodedLifetime, AssigningMovedFromReservationReleasesDestinationWithoutDoubleRelease) {
  RetainedBudget budget{64};
  auto source = budget.reserve(16);
  auto destination = budget.reserve(24);
  ASSERT_TRUE(source);
  ASSERT_TRUE(destination);
  {
    RetainedBudget::Reservation held{std::move(*source)};
    *destination = std::move(*source); // Only destination's former 24-byte charge is released.
    EXPECT_EQ(budget.used(), 16u);
    source.reset();
    destination.reset();
    EXPECT_EQ(budget.used(), 16u);
  }
  EXPECT_EQ(budget.used(), 0u);
}

TEST(H700DecodedLifetime, ConcurrentWholeBudgetReservationsHaveExactlyOneHeldWinner) {
  constexpr size_t Workers = 12;
  RetainedBudget budget{GpuBudgetBytes};
  std::barrier start{Workers + 1}, held{Workers + 1}, release{Workers + 1};
  std::atomic<unsigned> winners{0};
  std::vector<std::thread> threads;
  threads.reserve(Workers);
  for (size_t i = 0; i < Workers; ++i) {
    threads.emplace_back([&] {
      start.arrive_and_wait();
      auto reservation = budget.reserve(GpuBudgetBytes);
      if (reservation) winners.fetch_add(1, std::memory_order_relaxed);
      held.arrive_and_wait();
      release.arrive_and_wait();
    });
  }
  start.arrive_and_wait();
  held.arrive_and_wait();
  EXPECT_EQ(winners.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_FALSE(budget.reserve(1));
  release.arrive_and_wait();
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(budget.used(), 0u);
}

TEST(H700DecodedLifetime, ConcurrentMixedReservationsMatchAllHeldChargesAndRecoverEachRound) {
  constexpr size_t Workers = 8;
  constexpr unsigned Rounds = 64;
  constexpr uint64_t Limit = 64;
  RetainedBudget budget{Limit};
  std::barrier start{Workers + 1}, held{Workers + 1}, release{Workers + 1}, freed{Workers + 1};
  std::array<uint64_t, Workers> charges{};
  std::atomic<bool> exceeded{false};
  std::vector<std::thread> threads;
  threads.reserve(Workers);
  for (size_t i = 0; i < Workers; ++i) {
    threads.emplace_back([&, i] {
      for (unsigned round = 0; round < Rounds; ++round) {
        start.arrive_and_wait();
        const uint64_t size = 9 + (i + round) % 8;
        auto reservation = budget.reserve(size);
        charges[i] = reservation ? size : 0;
        if (budget.used() > Limit) exceeded.store(true, std::memory_order_relaxed);
        held.arrive_and_wait();
        release.arrive_and_wait();
        reservation.reset();
        freed.arrive_and_wait();
      }
    });
  }
  for (unsigned round = 0; round < Rounds; ++round) {
    start.arrive_and_wait();
    held.arrive_and_wait();
    uint64_t expected = 0;
    for (auto charge : charges) expected += charge;
    EXPECT_GT(expected, 0u);
    EXPECT_LE(expected, Limit);
    EXPECT_EQ(budget.used(), expected);
    EXPECT_FALSE(budget.reserve(Limit - expected + 1));
    release.arrive_and_wait();
    freed.arrive_and_wait();
    EXPECT_EQ(budget.used(), 0u);
  }
  for (auto& thread : threads) thread.join();
  EXPECT_FALSE(exceeded.load(std::memory_order_relaxed));
  EXPECT_EQ(budget.used(), 0u);
}

// Only the holder graph is a fixture: every byte charge/move/release below is the
// production RetainedBudget. No GPU buffer is manufactured as completion evidence.
struct HeldAllocation {
  RetainedBudget::Reservation charge;
  std::atomic<unsigned>& destroyed;
  HeldAllocation(RetainedBudget::Reservation reservation, std::atomic<unsigned>& counter)
  : charge(std::move(reservation)), destroyed(counter) {}
  ~HeldAllocation() { destroyed.fetch_add(1, std::memory_order_relaxed); }
};

TEST(H700DecodedLifetime, CacheEvictionAndPacketResetCannotSpendTwoNativeSlotsHeldBytes) {
  RetainedBudget budget{GpuBudgetBytes};
  std::atomic<unsigned> destroyed{0};
  auto reservation = budget.reserve(GpuBudgetBytes);
  ASSERT_TRUE(reservation);
  std::vector<std::shared_ptr<HeldAllocation>> cache;
  cache.push_back(std::make_shared<HeldAllocation>(std::move(*reservation), destroyed));
  reservation.reset();
  std::weak_ptr<HeldAllocation> weak = cache.front();
  std::vector<std::shared_ptr<void>> packetPins{cache.front()};
  std::array<std::vector<std::shared_ptr<void>>, 2> nativePins;
  cache.clear();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_FALSE(budget.reserve(1));
  nativePins[0] = std::move(packetPins);
  packetPins = {};
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0u);
  nativePins[1].push_back(nativePins[0].front()); // A second in-flight frame reuses the mesh.
  nativePins[0].clear();
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_FALSE(budget.reserve(1));
  EXPECT_FALSE(weak.expired());
  nativePins[1].clear();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(budget.used(), 0u);
  auto replacement = budget.reserve(GpuBudgetBytes);
  EXPECT_TRUE(replacement);
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
}

TEST(H700DecodedLifetime, FinalWorkerPinReleaseReturnsBudgetAfterProducerEviction) {
  RetainedBudget budget{GpuBudgetBytes};
  std::atomic<unsigned> destroyed{0};
  auto reservation = budget.reserve(GpuBudgetBytes);
  ASSERT_TRUE(reservation);
  auto cacheOwner = std::make_shared<HeldAllocation>(std::move(*reservation), destroyed);
  std::weak_ptr<HeldAllocation> weak = cacheOwner;
  std::shared_ptr<void> nativePin = cacheOwner;
  cacheOwner.reset();
  std::barrier held{2}, release{2};
  std::thread worker([&, pin = std::move(nativePin)]() mutable {
    held.arrive_and_wait();
    release.arrive_and_wait();
    pin.reset();
  });
  held.arrive_and_wait();
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(budget.used(), GpuBudgetBytes);
  EXPECT_FALSE(budget.reserve(1));
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0u);
  release.arrive_and_wait();
  worker.join();
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1u);
  EXPECT_EQ(budget.used(), 0u);
}

TEST(H700DecodedLifetime, OptInRequiresExactOneAndExactSupportedBenchmarkValue) {
  constexpr std::array<const char*, 17> Values{
      nullptr, "", "0", "00", "01", "true", "TRUE", "yes", "1 ", " 1", "1\n", "1\r", "1\t", "1x", "-1", "1", "menu"};
  constexpr std::array<const char*, 18> Bench{
      nullptr, "", "0", "00", "01", "true", "MENU", "Menu", "menu ", " menu", "menu\n", "menu\r", "menu\t", "1 ", " 1", "1\n", "1", "menu"};
  for (const char* value : Values) {
    for (const char* bench : Bench) {
      SCOPED_TRACE(::testing::Message() << "value=" << (value ? value : "<unset>")
                                      << " bench=" << (bench ? bench : "<unset>"));
      const bool expected = value && std::string_view{value} == "1" && bench &&
                            (std::string_view{bench} == "1" || std::string_view{bench} == "menu");
      EXPECT_EQ(requested(value, bench), expected);
    }
  }
}

} // namespace
} // namespace aurora::gx::decoded
