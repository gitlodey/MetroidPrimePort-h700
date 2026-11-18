"""Focused PERF12 cache/pin integration hooks; no device, CMake, or renderer rebuild.

Runtime budget/concurrency/move/opt-in tests live in h700_decoded_lifetime_test.cpp.
These are production source-hook checks, not a simulated GPU-completion test.
"""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wiring_helpers import WiringCase, braced, index, squash

ROOT = Path(__file__).resolve().parents[1]
LIB = ROOT / "extern/aurora/lib"


def body(text, signature):
    return braced(text, signature, signature)


def source(path):
    return (LIB / path).read_text()


class DecodedLifetimeWiring(WiringCase):
    def test_cache_key_covers_source_identity_and_all_decode_layout_inputs(self):
        text = source("gx/decoded_geometry.cpp")
        key = body(text, "struct Key")
        equal = body(key, "bool operator==(const Key& rhs)")
        for field in ("list", "offset", "count", "primitive", "stride", "arrays", "sizes", "bases"):
            self.assertIn(f"{field} == rhs.{field}", equal)
        self.assertIn("std::memcmp(attrs.data(), rhs.attrs.data(), sizeof(attrs)) == 0", equal)
        hashed = body(key, "friend H AbslHashValue(")
        self.assertIn("key.list, key.offset, key.count, key.primitive, key.stride", hashed)
        self.assertIn("reinterpret_cast<const uint8_t*>(key.attrs.data()), sizeof(key.attrs)", hashed)
        self.assertIn("key.arrays, key.sizes, key.bases", hashed)
        self.assertNotIn("original_base", key)
        acquire = body(text, "std::optional<Admission> acquire(")
        for assignment in ("key.list = list.id()", "key.offset = vertexOffset", "key.count = input.vertexCount",
                           "key.primitive = input.primitive", "key.stride = input.config.vtxStride",
                           "key.attrs = input.config.attrs", "key.arrays[attr] = source.id()",
                           "key.sizes[attr] = source.size()", "key.bases[attr] = input.baseIndices[attr]"):
            self.assertIn(assignment, acquire)
        self.assertIn("immutable::active(list.id())", acquire)
        self.assertIn("immutable::active(source.id())", acquire)
        self.assertIn("decoded_native_eligible(input.config)", acquire)
        self.assertIn("limits_valid(make_layout(input.config)", acquire)
        self.assertIn("input.vertices.data() != list.data() + vertexOffset", acquire)
        self.assertLess(index(acquire, "sCache.find(key)"), index(acquire, "decode(input, cpu, failure)"))

    def test_admission_precedes_raw_bounds_scan_uploads_and_cannot_merge_frame_indices(self):
        text = source("gx/command_processor.cpp")
        draw = body(text, "static void draw_prim(")
        for later in ("check_indexed_arrays(", "gfx::push_verts("):
            self.assertLess(index(draw, "decoded::acquire("), index(draw, later))
        self.assertIn("populate_pipeline_config(candidate, prim, fmt)", draw)
        self.assertIn("candidate.shaderConfig.nativeVertex = 0", draw)
        self.assertIn("g_gxState.arrays[attr].baseIndex", draw)
        self.assertIn("sImmutableArrays, bases", draw)
        admission = body(draw, "if (const auto mesh = decoded::acquire(")
        self.assertIn("reader.skip(totalVtxBytes)", admission)
        self.assertIn("push_gx_draw(prim, fmt, vtxCount, {}, {}, mesh->indexCount, &*mesh)", admission)
        self.assertIn("sDrawCache.lastDrawFmt = GX_MAX_VTXFMT", admission)
        self.assertIn("return;", admission)
        merge = body(source("gx/pipeline.hpp"), "inline bool native_vertex_merge_compatible(")
        self.assertIn("!draw.decodedGeometry && !config.decodedNative", merge)
        push = body(text, "static void push_gx_draw(")
        decoded = body(push, "if (decodedMesh)")
        self.assertIn("cache.config.shaderConfig.decodedNative = 1", decoded)
        self.assertIn("if (decodedMesh ||", push)
        self.assertIn("draw.decodedGeometry = true", push)
        self.assertIn("draw.decodedMesh = decodedMesh->ref", push)

    def test_budget_is_owned_by_buffers_and_not_released_by_cache_invalidation(self):
        text = source("gx/decoded_geometry.cpp")
        mesh = body(text, "struct Mesh")
        self.assertLess(index(mesh, "RetainedBudget::Reservation charge"), index(mesh, "wgpu::Buffer vertex"))
        self.assertLess(index(mesh, "RetainedBudget::Reservation charge"), index(mesh, "wgpu::Buffer index"))
        acquire = body(text, "std::optional<Admission> acquire(")
        self.assertIn("((vertexBytes + 3) & ~uint64_t{3}) + ((indexBytes + 3) & ~uint64_t{3})", acquire)
        self.assertIn("sBudget.reserve(bytes)", acquire)
        self.assertIn("std::make_shared<Mesh>(std::move(*reservation))", acquire)
        self.assertLess(index(acquire, "sBudget.reserve(bytes)"), index(acquire, "mesh->vertex = make_buffer("))
        self.assertLess(index(acquire, "sBudget.reserve(bytes)"), index(acquire, "mesh->index = make_buffer("))
        invalidate = body(text, "void invalidate_source(")
        self.assertIn("it->first.uses(id)", invalidate)
        self.assertIn("sCache.erase(it++)", invalidate)
        self.assertNotIn("sBudget", invalidate)
        self.assertNotIn("sSlots", invalidate)
        self.assertNotIn("Destroy(", invalidate)
        evict = body(text, "void evict_one()")
        self.assertIn("it->second.mesh.use_count() != 1", evict)
        self.assertNotIn("sBudget", evict)
        shutdown = body(text, "void shutdown() noexcept")
        self.assertIn("slot.mesh.expired()", shutdown)
        self.assertIn("sBudget.used() == 0", shutdown)
        self.assertNotIn("sBudget =", text)
        release = body(source("gx/command_processor.cpp"), "bool handle_aurora(ByteReader& reader) noexcept {")
        self.assertLess(index(release, "immutable::consume_release(id)"),
                        index(release, "decoded::invalidate_source(id)"))

    def test_weak_slot_requires_matching_nonzero_generation_and_never_wraps(self):
        text = source("gx/decoded_geometry.cpp")
        self.assertIn("std::weak_ptr<Mesh> mesh", body(text, "struct Slot"))
        resolve = body(text, "std::shared_ptr<Mesh> resolve(")
        self.assertIn("std::lock_guard lock{sMutex}", resolve)
        self.assertIn("ref.slot >= sSlots.size()", resolve)
        self.assertIn("ref.generation == 0", resolve)
        self.assertIn("sSlots[ref.slot].generation != ref.generation", resolve)
        self.assertIn("return sSlots[ref.slot].mesh.lock()", resolve)
        acquire = body(text, "std::optional<Admission> acquire(")
        self.assertIn("sSlots[slot].mesh.expired() && sSlots[slot].generation != UINT32_MAX", acquire)
        self.assertIn("mesh->ref = {slot, ++sSlots[slot].generation}", acquire)
        self.assertIn("sSlots[slot].mesh = mesh", acquire)
        bind = body(text, "void bind(MeshRef ref,")
        self.assertIn("const auto mesh = resolve(ref)", bind)
        self.assertIn("AURORA_ASSERT(mesh && mesh->vertex && mesh->index", bind)
        self.assertIn("wgpu::IndexFormat::Uint16", bind)
        self.assertNotIn("gfx::push_", bind)

    def test_cache_hit_and_new_mesh_admissions_both_require_strong_recording_pin(self):
        acquire = body(source("gx/decoded_geometry.cpp"), "std::optional<Admission> acquire(")
        hit = body(acquire, "if (auto it = sCache.find(key);")
        for branch in (hit, squash(acquire)[index(acquire, "++sMisses"):]):
            self.assertIn("if (!gfx::detail::retain_geometry(mesh, mesh->lastPinFrame)) return fail()", branch)
            self.assertLess(index(branch, "retain_geometry("), index(branch, "return Admission{"))
        self.assertIn("geometry_pins_supported()", acquire)
        pin = body(source("gfx/recording.cpp"), "bool retain_geometry(")
        self.assertIn("if (!owner || !geometry_pins_supported()) return false", pin)
        self.assertIn("if (lastPinFrame == packet.frameId) return true", pin)
        self.assertIn("packet.geometryPins.push_back(std::move(owner))", pin)
        self.assertLess(index(pin, "packet.geometryPins.push_back("), index(pin, "lastPinFrame = packet.frameId"))
        self.assertIn("catch (const std::bad_alloc&)", pin)
        self.assertIn("return false", pin)
        gate = body(source("gfx/recording.cpp"), "bool geometry_pins_supported()")
        self.assertIn("g_recorder.active() && g_recorder.packet->geometryNativeSlot != UINT32_MAX", gate)
        self.assertIn("std::vector<std::shared_ptr<void>> geometryPins", source("gfx/frame_packet.hpp"))

    def test_packet_pins_move_before_packet_reset_and_submit_callback_not_after_submit_release(self):
        text = source("gfx/frame.cpp")
        end = body(text, "bool end_frame(EndFrameCallback callback,")
        self.assertLess(index(end, "pins = std::move(packet.geometryPins)"), index(end, "packet = {}"))
        self.assertLess(index(end, "pins = std::move(packet.geometryPins)"), index(end, "callback(encoder,"))
        self.assertIn("packet.geometryNativeSlot < g_geometrySlotPins.size()", end)
        self.assertIn("auto& pins = g_geometrySlotPins[packet.geometryNativeSlot]", end)
        self.assertIn("AURORA_ASSERT(pins.empty()", end)
        self.assertNotIn("pins.clear()", end)
        for function in ("void after_submit() noexcept", "bool retire_cancelled_frames_after_quiescence()"):
            self.assertNotIn("g_geometrySlotPins", body(text, function))
        finish = body(source("gfx/recording.cpp"), "RecordedFrame end_recording()")
        self.assertNotIn("geometryPins.clear", finish)
        self.assertNotIn("geometryPins =", finish)

    def test_slot_pins_release_only_after_current_native_descriptor_validation(self):
        begin = body(source("gfx/frame.cpp"), "bool begin_frame(std::optional<h700::FrameDescriptor> nativeFrame)")
        self.assertIn("frame.geometryNativeSlot = nativeFrame->slot", begin)
        self.assertLess(index(begin, "enqueue_begin_frame("), index(begin, "!h700::native_slot(*nativeFrame)"))
        self.assertLess(index(begin, "!h700::native_slot(*nativeFrame)"),
                        index(begin, "g_geometrySlotPins[nativeFrame->slot].clear()"))
        self.assertIn("nativeFrame->generation == 0", begin)
        self.assertIn("nativeFrame->generation != g_geometryGeneration", begin)
        self.assertIn("nativeFrame->frame <= *g_geometryLastFrame", begin)
        self.assertIn("fail_frame_work(frameSlot)", begin)
        self.assertLess(index(begin, "g_geometrySlotPins[nativeFrame->slot].clear()"),
                        index(begin, "g_geometryLastFrame = nativeFrame->frame"))
        slot = body(source("h700/presentation.cpp"), "const NativeSlot* native_slot(")
        self.assertIn("descriptor.generation != backend->generation", slot)
        self.assertIn("!current || *current != descriptor", slot)
        state = source("h700/presentation_state.cpp")
        reserve = body(state, "std::optional<FrameDescriptor> PresentationState::try_reserve(")
        self.assertIn("frame <= *m_last_frame", reserve)
        self.assertIn("if (slot.status == SlotStatus::Free)", reserve)
        self.assertIn("slot.status = SlotStatus::Reserved", reserve)
        forward = body(state, "bool PresentationState::complete_forward(")
        self.assertIn("slot->status = SlotStatus::Ready", forward)
        self.assertNotIn("release_locked", forward)
        recycle = body(state, "bool PresentationState::recycle(")
        for required in ("slot->status != SlotStatus::Retiring", "!slot->reverse_complete", "!slot->swap_succeeded"):
            self.assertIn(required, recycle)
        self.assertLess(index(recycle, "!slot->reverse_complete"), index(recycle, "release_locked(*slot)"))

    def test_guarded_holder_release_clears_all_pins_before_cache_shutdown(self):
        release = body(source("gfx/frame.cpp"), "bool release_frame_holders_after_quiescence()")
        for guard in ("gx::fifo::is_quiescent()", "render_worker::is_quiescent()",
                      "pipeline_cache_threads_joined()", "pending_internal_maps()"):
            self.assertLess(index(release, guard), index(release, "g_geometrySlotPins.fill({})"))
        self.assertLess(index(release, "g_geometrySlotPins.fill({})"), index(release, "gx::decoded::shutdown()"))
        self.assertLess(index(release, "packet.geometryPins.clear()"), index(release, "gx::decoded::shutdown()"))
        lifecycle = body(source("aurora.cpp"), "h700::ShutdownResult poll_h700_shutdown(h700::ShutdownGoal goal) noexcept {")
        self.assertLess(index(lifecycle, "case Stage::Native:"), index(lifecycle, "case Stage::Holders:"))
        self.assertLess(index(lifecycle, "webgpu::poll_h700_queue_drain()"),
                        index(lifecycle, "gfx::release_frame_holders_after_quiescence()"))

    def test_enabled_uses_exact_helper_only_in_native_h700_production(self):
        text = source("gx/decoded_geometry.cpp")
        enabled = body(text, "bool enabled() noexcept")
        self.assertIn("#if defined(AURORA_H700) && !defined(AURORA_FIFO_TESTING)", enabled)
        self.assertIn('requested(std::getenv("MP_H700_RETAIL_GEOMETRY"), std::getenv("MP_H700_BENCH"))', enabled)
        self.assertIn("#else", enabled)
        self.assertIn("return false", enabled)
        self.assertNotIn("atoi(", enabled)

    def test_transient_allocation_failure_does_not_poison_immutable_cache(self):
        # A one-shot CPU allocation failure is not an immutable tuple property.
        # Admission must distinguish transient failures from stable tuple rejections.
        acquire = body(source("gx/decoded_geometry.cpp"), "std::optional<Admission> acquire(")
        rejected = squash(body(acquire, "if (!decode(input, cpu, failure))"))
        if "sCache.emplace(" not in rejected:
            return
        self.assertIn("DecodeFailure::AllocationFailed", rejected,
                      "AllocationFailed is currently memoized as a permanent null mesh: subsequent warm hits never retry")
        early_return = "if(failure==DecodeFailure::AllocationFailed)return fail();"
        braced_return = "if(failure==DecodeFailure::AllocationFailed){return fail();}"
        conditional_cache = "if(failure!=DecodeFailure::AllocationFailed)"
        guarded = (early_return in rejected or braced_return in rejected or conditional_cache in rejected)
        self.assertTrue(guarded, "Do not negative-cache a transient AllocationFailed result")
        if conditional_cache not in rejected:
            self.assertLess(rejected.index("DecodeFailure::AllocationFailed"), rejected.index("sCache.emplace("))


if __name__ == "__main__":
    unittest.main()
