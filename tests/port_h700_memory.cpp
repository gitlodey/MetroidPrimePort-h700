// Include the production implementation to exercise its private checks without a public test API.
// Build this TU alone, with AURORA_H700 and the platform/Aurora include directories; no GPU is used.
#if !defined(__linux__) || !defined(AURORA_H700)
#error "H700 memory tests require Linux and AURORA_H700 (not a no-op platform path)"
#endif
#include "../platform/port_memory_log.cpp"
#include "port_test.h"

#include <array>
#include <string>
#include <vector>

namespace {
std::vector<uint32_t> sTrims;
std::vector<std::array<int64_t, 3>> sTexturePressure;
unsigned sGpuSamples = 0;
unsigned sTextureStatsSamples = 0;
}

extern "C" {
bool aurora_get_gpu_memory_budget(uint64_t*, uint64_t*) {
  ++sGpuSamples;
  return false;
}
void aurora_get_texture_stats(AuroraTextureStats* out) {
  ++sTextureStatsSamples;
  *out = {};
}
bool aurora_get_dawn_memory(AuroraDawnMemory*) { return false; }
void aurora_trim_pipelines(uint32_t idleFrames) { sTrims.push_back(idleFrames); }
void aurora_set_memory_pressure(int64_t headroom, int64_t moderate, int64_t severe) {
  sTexturePressure.push_back({headroom, moderate, severe});
}
}

namespace {
constexpr uint64_t kSecond = 1000000000ull;
constexpr int64_t kMiBInKiB = 1024;
constexpr int64_t kMiBInBytes = 1024 * 1024;

void TestPipelinePressure() {
  using namespace PortMemoryLog;
  SetPipelinePressure(0, 0);
  CheckPipelinePressure(kSecond, 0);
  SetPipelinePressure(-192, -128);
  CheckPipelinePressure(kSecond, 0);
  CHECK(sTrims.empty());
  CHECK_EQ(sPressureModerateKb, 0);
  CHECK_EQ(sPressureSevereKb, 0);

  SetPipelinePressure(192, 128);
  CHECK_EQ(sPressureModerateKb, 192 * kMiBInKiB);
  CHECK_EQ(sPressureSevereKb, 128 * kMiBInKiB);
  // One monotonic sequence preserves the production function's static poll/level/repeat state.
  const auto check = [](uint64_t now, int64_t available, size_t count, uint32_t lastIdle) {
    CheckPipelinePressure(now, available);
    CHECK_EQ(sTrims.size(), count);
    if (count != 0 && !sTrims.empty()) {
      CHECK_EQ(sTrims.back(), lastIdle);
    }
  };
  check(1 * kSecond, 192 * kMiBInKiB, 0, 0);         // equality is not moderate
  check(6 * kSecond, 192 * kMiBInKiB - 1, 1, 3600);  // one KiB below: moderate
  check(11 * kSecond, 128 * kMiBInKiB, 1, 3600);     // equality is not severe
  check(16 * kSecond - 1, 128 * kMiBInKiB - 1, 1, 3600); // before the 5 s poll
  check(16 * kSecond, 128 * kMiBInKiB - 1, 2, 1800); // escalation need not wait 30 s
  check(21 * kSecond, 0, 2, 1800);
  check(41 * kSecond, 0, 2, 1800);                  // 25 s since the trim
  check(46 * kSecond - 1, 0, 2, 1800);
  check(46 * kSecond, 0, 3, 1800);                  // exactly 30 s: repeat severe
  check(51 * kSecond, -1, 3, 1800);                // unknown: no trim and no poll consumed
  check(51 * kSecond, 192 * kMiBInKiB, 3, 1800);   // off, resets the level
  check(56 * kSecond, 128 * kMiBInKiB, 4, 3600);   // moderate re-entry before 30 s
  check(61 * kSecond, 128 * kMiBInKiB - 1, 5, 1800);
  check(66 * kSecond, 192 * kMiBInKiB - 1, 5, 1800); // downgrade does not trim immediately
  check(86 * kSecond, 192 * kMiBInKiB - 1, 5, 1800);
  check(91 * kSecond - 1, 192 * kMiBInKiB - 1, 5, 1800);
  check(91 * kSecond, 192 * kMiBInKiB - 1, 6, 3600); // exactly 30 s: repeat moderate
  check(96 * kSecond, 192 * kMiBInKiB, 6, 3600);
  check(101 * kSecond, -123, 6, 3600);
  check(101 * kSecond, 0, 7, 1800);

  SetPipelinePressure(0, 0);
  check(131 * kSecond, 0, 7, 1800);
  // These stubs observe only Aurora requests, not eviction or GLOBAL retention inside Aurora.
}

void TestTexturePressure() {
  using namespace PortMemoryLog;
  const size_t trimCount = sTrims.size();
  SetTexturePressure(0, 0);
  CheckTexturePressure(0);
  SetTexturePressure(-192, -128);
  CheckTexturePressure(0);
  CHECK(sTexturePressure.empty());
  CHECK_EQ(sTextureModerateKb, 0);
  CHECK_EQ(sTextureSevereKb, 0);

  SetTexturePressure(192, 128);
  for (int64_t headroom : {192 * kMiBInKiB, 192 * kMiBInKiB - 1,
                           128 * kMiBInKiB, 128 * kMiBInKiB - 1, int64_t{0}}) {
    const size_t before = sTexturePressure.size();
    CheckTexturePressure(headroom);
    REQUIRE(sTexturePressure.size() == before + 1);
    const auto& call = sTexturePressure.back();
    CHECK_EQ(call[0], headroom * 1024);
    CHECK_EQ(call[1], 192 * kMiBInBytes);
    CHECK_EQ(call[2], 128 * kMiBInBytes);
  }
  const size_t before = sTexturePressure.size();
  CheckTexturePressure(-1);
  CheckTexturePressure(-123);
  CHECK_EQ(sTexturePressure.size(), before);
  CHECK_EQ(sTrims.size(), trimCount);
  SetTexturePressure(0, 0);
}

int64_t HighWaterFromText(const char* text) {
  // tmpfile() uses /tmp rather than TMPDIR on glibc; the approved container
  // rootfs is read-only. Exercise the same FILE parser with an in-memory file.
  std::string buffer = text;
  FILE* file = fmemopen(buffer.data(), buffer.size(), "r");
  REQUIRE(file != nullptr);
  const int64_t result = PortMemoryLog::ReadHighWaterKb(file);
  std::fclose(file);
  return result;
}

void TestProcDiagnostics() {
  using namespace PortMemoryLog;
  CHECK_EQ(ReadHighWaterKb(nullptr), -1); // missing /proc is unknown, not zero or success
  CHECK_EQ(HighWaterFromText("Name:\tgame\nVmRSS:\t1024 kB\nVmHWM:\t2049 kB\n"), 2049);
  CHECK_EQ(HighWaterFromText("VmRSS:\t1024 kB\n"), -1);
  CHECK_EQ(HighWaterFromText("VmHWM:\tinvalid kB\n"), -1);
  CHECK_EQ(HighWaterFromText("VmHWM:\t-1 kB\n"), -1);
  CHECK_EQ(HighWaterFromText("VmHWM:\t0 kB\n"), 0);
  CHECK_EQ(ReadProcKb("MemAvailable: 98304 kB\n", "MemAvailable:"), 98304);
  CHECK_EQ(ReadProcKb("MemAvailable: invalid kB\n", "MemAvailable:"), -1);
  CHECK_EQ(ReadProcKb("MemFree: 98304 kB\n", "MemAvailable:"), -1);
  CHECK_EQ(ReadProcKb("VmHWM: 42 MB\n", "VmHWM:"), -1);
  CHECK_EQ(ReadProcKb("VmHWM: 42\n", "VmHWM:"), -1);
  CHECK_EQ(ReadProcKb("VmHWM: 42 kB trailing\n", "VmHWM:"), -1);
  CHECK_EQ(ReadProcKb("VmHWM: 99999999999999999999999999 kB\n", "VmHWM:"), -1);

  H700MemoryDiagnostics diagnostics;
  ProcessMemory process{.residentKb = 1024, .highWaterKb = 2049};
  SystemMemory system{.availableKb = 192 * kMiBInKiB};
  CHECK_EQ(std::string(diagnostics.GateStatus()), "unknown");
  diagnostics.Sample(process, system);
  CHECK_EQ(diagnostics.minAvailableKb, system.availableKb);
  CHECK_EQ(std::string(diagnostics.GateStatus()), "sampled-ok");
  system.availableKb = 96 * kMiBInKiB;
  diagnostics.Sample(process, system);
  CHECK_EQ(diagnostics.minAvailableKb, 96 * kMiBInKiB);
  CHECK_EQ(std::string(diagnostics.GateStatus()), "sampled-ok"); // equality is allowed
  system.availableKb -= 1;
  diagnostics.Sample(process, system);
  CHECK_EQ(std::string(diagnostics.GateStatus()), "below-96MiB");
  system.availableKb = 256 * kMiBInKiB;
  diagnostics.Sample(process, system);
  CHECK_EQ(diagnostics.minAvailableKb, 96 * kMiBInKiB - 1);
  CHECK_EQ(std::string(diagnostics.GateStatus()), "below-96MiB"); // recovery cannot erase the minimum
  system.availableKb = -1;
  diagnostics.Sample(process, system);
  CHECK_EQ(diagnostics.minAvailableKb, 96 * kMiBInKiB - 1);
  CHECK_EQ(std::string(diagnostics.GateStatus()), "below-96MiB"); // known breach remains decisive

  for (int missing = 0; missing != 3; ++missing) {
    H700MemoryDiagnostics incomplete;
    ProcessMemory validProcess{.residentKb = 1024, .highWaterKb = 2049};
    SystemMemory validSystem{.availableKb = 192 * kMiBInKiB};
    incomplete.Sample(validProcess, validSystem);
    auto missingProcess = validProcess;
    auto missingSystem = validSystem;
    if (missing == 0) missingSystem.availableKb = -1;
    if (missing == 1) missingProcess.residentKb = -1;
    if (missing == 2) missingProcess.highWaterKb = -1;
    incomplete.Sample(missingProcess, missingSystem);
    CHECK_EQ(std::string(incomplete.GateStatus()), "unknown");
    incomplete.Sample(validProcess, validSystem);
    CHECK_EQ(std::string(incomplete.GateStatus()), "unknown"); // a later read cannot fill the gap
  }
}

void TestTickCadence() {
  using namespace PortMemoryLog;
  // The real Tick may read host /proc, but its contents never count as device headroom evidence.
  // GPU stubs count samples and log emissions independently of those readings (including unknown).
  const unsigned samples = sGpuSamples;
  const unsigned lines = sTextureStatsSamples;
  Tick(0);
  CHECK_EQ(sGpuSamples, samples + 1);
  CHECK_EQ(sTextureStatsSamples, lines + 1);
  Tick(0);
  Tick(kSecond - 1);
  CHECK_EQ(sGpuSamples, samples + 1);
  CHECK_EQ(sTextureStatsSamples, lines + 1);
  Tick(kSecond);
  CHECK_EQ(sGpuSamples, samples + 2);
  CHECK_EQ(sTextureStatsSamples, lines + 2);
  Tick(30 * kSecond); // delayed frame: one sample/line, not a catch-up loop
  CHECK_EQ(sGpuSamples, samples + 3);
  CHECK_EQ(sTextureStatsSamples, lines + 3);
}
}

int main() {
  TestPipelinePressure();
  TestTexturePressure();
  TestProcDiagnostics();
  TestTickCadence();
  return PORT_TEST_RESULT();
}
