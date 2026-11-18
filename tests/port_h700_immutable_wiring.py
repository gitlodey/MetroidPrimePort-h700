"""PERF12 source ownership wiring and checked FIFO append contracts, no device."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wiring_helpers import braced

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "extern/aurora/lib"


def body(text, signature):
    return braced(text, signature, signature)


class ImmutableSourceWiring(unittest.TestCase):
    def test_public_registration_requires_exact_h700_opt_in_and_benchmark(self):
        disp = (LIB / "dolphin/gx/GXDispList.cpp").read_text()
        signature = "u64 GXPortRegisterImmutableSource(const void* data, u32 size, GXPortImmutableKind kind)"
        wrapper = signature + body(disp, signature)
        fixture = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <dolphin/gx/GXAurora.h>
namespace aurora::gx::immutable {
enum class SourceKind { Array, DisplayList };
unsigned calls = 0;
SourceKind lastKind = SourceKind::Array;
u64 register_source(const void* data, u32 size, SourceKind kind) noexcept {
  assert(data != nullptr && size == 4);
  ++calls;
  lastKind = kind;
  return 17;
}
}
'''
        assertions = r'''
int main(int argc, char** argv) {
  assert(argc == 2);
  const bool expected = argv[1][0] == '1';
  const u8 data[4] = {1, 2, 3, 4};
  using namespace aurora::gx::immutable;
  assert(GXPortRegisterImmutableSource(data, 4, GX_PORT_IMMUTABLE_ARRAY) == (expected ? 17 : 0));
  assert(calls == (expected ? 1u : 0u) && lastKind == SourceKind::Array);
  assert(GXPortRegisterImmutableSource(data, 4, static_cast<GXPortImmutableKind>(99)) == 0);
  assert(calls == (expected ? 1u : 0u));
  assert(GXPortRegisterImmutableSource(data, 4, GX_PORT_IMMUTABLE_DISPLAY_LIST) == (expected ? 17 : 0));
  assert(calls == (expected ? 2u : 0u));
  if (expected) assert(lastKind == SourceKind::DisplayList);
}
'''
        cases = ((None, None), (None, "1"), (None, "menu"), ("1", None),
                 ("", "1"), ("0", "menu"), ("01", "1"), ("true", "menu"),
                 ("1 ", "1"), ("1\n", "menu"), ("1", ""), ("1", "0"),
                 ("1", "01"), ("1", "MENU"), ("1", "menu "), ("1", "1\n"),
                 ("1", "1"), ("1", "menu"))
        with tempfile.TemporaryDirectory(prefix="immutable-gate-") as temp:
            cpp = Path(temp) / "gate.cpp"
            cpp.write_text(fixture + wrapper + assertions)
            for h700 in (False, True):
                binary = Path(temp) / ("h700" if h700 else "non-h700")
                args = [os.environ.get("CXX", "clang++"), "-std=c++20", "-DTARGET_PC",
                        "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "extern/aurora/include")]
                if h700:
                    args.append("-DAURORA_H700")
                subprocess.run(args + [str(cpp), "-o", str(binary)], check=True, timeout=30)
                for opt_in, bench in cases:
                    env = os.environ.copy()
                    for name, value in (("MP_H700_RETAIL_GEOMETRY", opt_in), ("MP_H700_BENCH", bench)):
                        env.pop(name, None)
                        if value is not None:
                            env[name] = value
                    enabled = h700 and opt_in == "1" and bench in ("1", "menu")
                    with self.subTest(h700=h700, opt_in=opt_in, bench=bench):
                        # A fresh process per case also tests the cached startup gate.
                        subprocess.run([str(binary), "1" if enabled else "0"], env=env,
                                       check=True, timeout=5)

    def test_actual_checkpoint_release_precedes_unchanged_root_guards(self):
        main = (ROOT / "src/MetroidPrime/main.cpp").read_text()
        prepare = body(main, "static void PortH700PrepareRootUnwind(")
        self.assertLess(prepare.index("RootUnwindReady(checkpoint)"),
                        prepare.index("release_immutable_sources_at_cpu_quiescence()"))
        self.assertLess(prepare.index("release_immutable_sources_at_cpu_quiescence()"),
                        prepare.index("RootCleanupAllowed(checkpoint, PortH700CleanupObligationsEmpty)"))
        catch = body(main, "catch (const PortH700::RootTerminalToken& terminal)")
        self.assertIn("RootCleanupAllowed(terminal.checkpoint, PortH700CleanupObligationsEmpty)", catch)
        self.assertNotIn("release_immutable_sources", catch)
        aurora = (LIB / "aurora.cpp").read_text()
        bridge = body(aurora, "bool h700::release_immutable_sources_at_cpu_quiescence()")
        self.assertIn("if (!h700::generation_cpu_quiescence_held()) return false;", bridge)
        self.assertIn("return gx::immutable::release_at_cpu_quiescence();", bridge)
        guard = body(aurora, "bool h700::resident_resources_empty()")
        self.assertLess(guard.index("generation_cpu_quiescence_held()"), guard.index("registries_empty()"))
        self.assertIn("gx::resident::can_destroy_without_graphics_commands()", guard)
        release = body((LIB / "gx/immutable.cpp").read_text(), "bool release_at_cpu_quiescence()")
        self.assertLess(release.index("generation_cpu_quiescence_held()"), release.index("sTerminal = true"))
        for forbidden in ("retained_bytes =", "sRetainedBytes.store", "resident::", "gfx::", "fifo::", "process("):
            self.assertNotIn(forbidden, release)

    def test_mrea_owner_retires_before_move_rebuild_and_free(self):
        area = (ROOT / "src/MetroidPrime/CGameArea.cpp").read_text()
        fill = body(area, "void CGameArea::FillInStaticGeometry()")
        self.assertLess(fill.index("PortReleaseImmutableSources();"), fill.index("x4c_insts.clear()"))
        for name in ("positions", "normals", "colors", "texCoords", "packedTexCoords"):
            self.assertIn(f"PortRegisterImmutableSource({name}, {name}Size, false);", fill)
        self.assertLess(fill.index("ConvertSurfaceHeader("), fill.index("sourceSurface.GetDisplayList()"))
        self.assertIn("dlSize <= uint(section->second) - headerSize", fill)
        move = body(area, "bool CGameArea::TransferTokensToARAM()")
        self.assertLess(move.index("PortReleaseImmutableSources();"), move.index(".first = empty"))
        kill = body(area, "void CGameArea::KillmAreaData()")
        self.assertLess(kill.index("PortReleaseImmutableSources();"), kill.index("CMemory::Free("))
        self.assertIn("KillmAreaData();", body(area, "CGameArea::~CGameArea()"))
        invalidate = body(area, "bool CGameArea::Invalidate(")
        self.assertLess(invalidate.index("PortReleaseImmutableSources();"), invalidate.index("x12c_postConstructed = nullptr"))
        self.assertLess(invalidate.rindex("PortReleaseImmutableSources();"), invalidate.rindex("x12c_postConstructed = nullptr"))
        self.assertNotIn("PortReleaseImmutableSources", body(area, "void CGameArea::RemoveStaticGeometry()"))

    def test_versioned_records_and_cgx_epoch_leave_legacy_payloads_intact(self):
        geometry = (LIB / "dolphin/gx/GXGeometry.cpp").read_text()
        array = body(geometry, "void GXSetArray(")
        self.assertIn("producer_admission(data, size, aurora::gx::immutable::SourceKind::Array)", array)
        self.assertIn("GX_AURORA_LOAD_IMMUTABLE_ARRAY", array)
        self.assertIn("write_checked(record, sizeof(record))", array)
        self.assertIn("GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAYBASE | cpIdx)", array)
        dl = body((LIB / "dolphin/gx/GXDispList.cpp").read_text(), "void GXCallDisplayList(")
        self.assertIn("GX_AURORA_CALL_IMMUTABLE_DL", dl)
        self.assertIn("fifo::write_data(data, nbytes)", dl)
        cgx = (ROOT / "include/Kyoto/Graphics/CGX_Impl.hpp").read_text()
        for signature in ("CGX_INLINE void CGX::SetArray(GXAttr attr, const void* data, uint size",
                          "CGX_INLINE void CGX::SetArrayNative("):
            setter = body(cgx, signature)
            self.assertLess(setter.index("ArrayLifetimeChanged(idx)"), setter.index("if (data == nullptr"))
            self.assertIn("!sourceChanged &&", setter)

    def test_production_checked_append_and_cgx_bodies_cpu_contract(self):
        # Execute the exact two production function bodies with only downstream health/
        # growth supplied by this CPU fixture. Full FIFO worker tests also live in
        # gx_fifo_test.cpp; this test does not claim renderer/worker integration.
        fifo = (LIB / "gx/fifo.cpp").read_text()
        functions = "\n".join(signature + body(fifo, signature) for signature in (
            "bool write_checked(const void* data, uint32_t length) noexcept",
            "bool enqueue_ownership(uint16_t command, uint64_t sourceId) noexcept"))
        cgx = (ROOT / "include/Kyoto/Graphics/CGX_Impl.hpp").read_text()
        functions += "\nnamespace PortGXImmutableDetail { inline bool ArrayLifetimeChanged(uint idx)" + body(
            cgx, "inline bool ArrayLifetimeChanged(uint idx)") + "}\n"
        functions += "\n".join(signature + body(cgx, signature) for signature in (
            "void CGX::SetArray(GXAttr attr, const void* data, uint size, uchar stride)",
            "void CGX::SetArrayNative(GXAttr attr, const void* data, uint size, uchar stride)"))
        fixture = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
namespace detail {
std::array<uint8_t, 128> buffer{};
uint8_t* sBufferData = buffer.data();
uint32_t sBufferSize = 0, sBufferCapacity = 128;
bool sInDisplayList = false, sWritesOpen = true;
}
using uint = unsigned int;
using uchar = unsigned char;
u64 sourceEpoch = 0;
unsigned arrayBinds = 0;
bool lastLittleEndian = false;
extern "C" u64 GXPortImmutableSourceEpoch() { return sourceEpoch; }
void GXSetArray(GXAttr, const void*, u32, u8, bool le) { ++arrayBinds; lastLittleEndian = le; }
struct CGX {
  struct SGXState { const void* x0_arrayPtrs[12]; uint x4_arraySizes[12]; };
  static SGXState sGXState;
  static void SetArray(GXAttr, const void*, uint, uchar);
  static void SetArrayNative(GXAttr, const void*, uint, uchar);
};
CGX::SGXState CGX::sGXState{};
bool downstreamHealthy = true, growAllowed = true;
bool healthy() { return downstreamHealthy; }
bool write_data_grow(const void* bytes, uint32_t size) {
  if (!growAllowed) return false;
  detail::sBufferCapacity = 128;
  std::memcpy(detail::sBufferData + detail::sBufferSize, bytes, size);
  detail::sBufferSize += size;
  return true;
}
'''
        assertions = r'''
int main() {
  const auto id = uint64_t{0x0102030405060708};
  assert(enqueue_ownership(GX_AURORA_REGISTER_IMMUTABLE_SOURCE, id));
  const std::array<uint8_t, 11> expected{GX_AURORA, 0, 0x75, 1, 2, 3, 4, 5, 6, 7, 8};
  assert(detail::sBufferSize == expected.size());
  assert(std::memcmp(detail::sBufferData, expected.data(), expected.size()) == 0);
  const auto before = detail::buffer;
  detail::sWritesOpen = false;
  assert(!enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, id));
  detail::sWritesOpen = true;
  downstreamHealthy = false;
  assert(!enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, id));
  downstreamHealthy = true;
  detail::sInDisplayList = true;
  assert(!enqueue_ownership(GX_AURORA_REGISTER_IMMUTABLE_SOURCE, id));
  detail::sInDisplayList = false;
  assert(!enqueue_ownership(GX_AURORA_REGISTER_IMMUTABLE_SOURCE, 0));
  assert(!enqueue_ownership(GX_AURORA_RESIDENT_RETAIN, id));
  assert(!write_checked(nullptr, 4));
  assert(!write_checked(expected.data(), 0));
  assert(detail::sBufferSize == expected.size() && detail::buffer == before);
  detail::sBufferCapacity = detail::sBufferSize;
  growAllowed = false;
  assert(!enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, id));
  assert(detail::sBufferSize == expected.size() && detail::buffer == before);
  growAllowed = true;
  assert(enqueue_ownership(GX_AURORA_RELEASE_IMMUTABLE_SOURCE, id));
  assert(detail::sBufferSize == 22 && detail::sBufferData[13] == 0x76);
  assert(std::memcmp(detail::sBufferData, before.data(), expected.size()) == 0);
  const uint8_t array[4] = {1, 2, 3, 4};
  CGX::SetArray(GX_VA_POS, array, 4, 1);
  CGX::SetArray(GX_VA_POS, array, 4, 1);
  assert(arrayBinds == 1); // unchanged lifetime keeps exact legacy suppression
  ++sourceEpoch;
  CGX::SetArray(GX_VA_POS, nullptr, 0, 0); // must not consume a rebind epoch
  CGX::SetArray(GX_VA_POS, array, 4, 1);
  assert(arrayBinds == 2 && !lastLittleEndian);
  ++sourceEpoch;
  CGX::SetArray(GX_VA_POS, array, 4, 1);
  assert(arrayBinds == 3); // same pointer/size, different source generation
  ++sourceEpoch;
  CGX::SetArrayNative(GX_VA_POS, nullptr, 0, 0);
  CGX::SetArrayNative(GX_VA_POS, array, 4, 1);
  CGX::SetArrayNative(GX_VA_POS, array, 4, 1);
  assert(arrayBinds == 4 && lastLittleEndian);
  std::puts("production checked FIFO append + CGX epoch bodies: PASS");
}
'''
        with tempfile.TemporaryDirectory(prefix="immutable-fifo-") as temp:
            cpp = Path(temp) / "contract.cpp"
            binary = Path(temp) / "contract"
            cpp.write_text(fixture + functions + assertions)
            args = [os.environ.get("CXX", "clang++"), "-std=c++20", "-DTARGET_PC", "-DAURORA_H700",
                    "-Wall", "-Wextra", "-Werror"]
            # The pinned game builder has no compiler-rt sanitizer runtime. Keep the
            # ordinary contract portable; opt in explicitly on the sanitizer host.
            if os.environ.get("MP_TEST_SANITIZERS") == "1":
                args.append("-fsanitize=address,undefined")
            subprocess.run(args + ["-I" + str(ROOT / "extern/aurora/include"), str(cpp), "-o", str(binary)],
                           check=True, timeout=30)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
