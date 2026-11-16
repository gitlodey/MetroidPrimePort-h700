#include "h700/presentation_state.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>

namespace aurora::h700 {
namespace {
std::string source(const char* name) {
  std::ifstream file(std::string(H700_PRESENT_SOURCE_DIR) + "/" + name);
  EXPECT_TRUE(file.good()) << name;
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string section(const std::string& text, std::string_view begin, std::string_view end) {
  const auto first = text.find(begin);
  const auto last = first == std::string::npos ? std::string::npos : text.find(end, first + begin.size());
  if (first == std::string::npos || last == std::string::npos) {
    ADD_FAILURE() << "Missing source section: " << begin << " .. " << end;
    return {};
  }
  return text.substr(first, last - first);
}
void ordered(const std::string& text, std::initializer_list<std::string_view> pieces) {
  size_t next = 0;
  for (const auto piece : pieces) {
    const auto position = text.find(piece, next);
    ASSERT_NE(position, std::string::npos) << piece;
    next = position + piece.size();
  }
}

TEST(H700PresentationIntegration, DescriptorOutlivesCpuPacketAndAbortReturnsCredit) {
  PresentationState state(7);
  auto packetDescriptor = state.try_reserve(10);
  ASSERT_TRUE(packetDescriptor);
  const auto descriptor = *packetDescriptor;
  std::function<void()> submit = [descriptor = *packetDescriptor, &state] {
    EXPECT_TRUE(state.mark_submitted(descriptor));
    EXPECT_TRUE(state.publish_submitted(descriptor, 55));
  };
  packetDescriptor.reset(); // same by-value lifetime contract as the actual end-frame capture below
  submit();
  EXPECT_EQ(state.front_submission()->descriptor, descriptor);
  const auto other = state.try_reserve(11);
  ASSERT_TRUE(other);
  ASSERT_TRUE(state.abort_reservation(*other));
  EXPECT_EQ(state.snapshot().slots[other->slot].status, SlotStatus::Free);
  EXPECT_TRUE(state.try_reserve(12));
}

TEST(H700PresentationIntegration, BothCompletionsAndSwapRequiredAndOldFrameCannotTouchReverse) {
  PresentationState state(1);
  const auto descriptor = *state.try_reserve(1);
  ASSERT_TRUE(state.mark_submitted(descriptor));
  ASSERT_TRUE(state.publish_submitted(descriptor, 100));
  EXPECT_FALSE(state.try_begin_read());
  ASSERT_TRUE(state.complete_forward(descriptor, 100));
  ASSERT_TRUE(state.try_begin_read());
  ASSERT_TRUE(state.begin_retiring(descriptor, 200));
  ASSERT_TRUE(state.complete_swap(descriptor));
  EXPECT_FALSE(state.recycle(descriptor));
  ASSERT_TRUE(state.complete_reverse(descriptor, 200));
  ASSERT_TRUE(state.recycle(descriptor));
  const auto next = *state.try_reserve(2);
  ASSERT_EQ(next.slot, descriptor.slot);
  ASSERT_TRUE(state.mark_submitted(next));
  ASSERT_TRUE(state.publish_submitted(next, 101));
  ASSERT_TRUE(state.complete_forward(next, 101));
  ASSERT_TRUE(state.try_begin_read());
  ASSERT_TRUE(state.begin_retiring(next, 200)); // a driver can reuse a destroyed sync's handle
  EXPECT_FALSE(state.complete_reverse(descriptor, 200));
  EXPECT_FALSE(state.complete_reverse({2, next.frame, next.slot}, 200));
  EXPECT_FALSE(state.complete_reverse({1, next.frame, static_cast<uint8_t>(1 - next.slot)}, 200));
  EXPECT_FALSE(state.recycle(next));
  ASSERT_TRUE(state.complete_reverse(next, 200));
  EXPECT_FALSE(state.recycle(next));
  ASSERT_TRUE(state.complete_swap(next));
  EXPECT_TRUE(state.recycle(next));
}

TEST(H700PresentationIntegration, FailedLatePublicationRetainsBothFuturesWithoutReadingOrReuse) {
  PresentationState state(1);
  const auto a = *state.try_reserve(0);
  const auto b = *state.try_reserve(1);
  ASSERT_TRUE(state.mark_submitted(a));
  ASSERT_TRUE(state.mark_submitted(b));
  state.begin_draining();
  ASSERT_TRUE(state.publish_submitted(a, 10));
  ASSERT_TRUE(state.quarantine(a, "real work-done status failed"));
  ASSERT_TRUE(state.publish_submitted(b, 11)); // accepted result still arrives after failure
  const auto snapshot = state.snapshot();
  ASSERT_EQ(snapshot.mailbox_size, 2u);
  EXPECT_EQ(snapshot.slots[a.slot].forward, 10u);
  EXPECT_EQ(snapshot.slots[b.slot].forward, 11u);
  EXPECT_FALSE(state.try_begin_read());
  EXPECT_FALSE(state.recycle(a));
  EXPECT_FALSE(state.try_reserve(2));
  const auto notifications = state.take_notifications();
  ASSERT_EQ(notifications.size(), 1u);
  EXPECT_EQ(notifications[0].descriptor, a);
  EXPECT_EQ(notifications[0].outcome, PresentationNotification::Outcome::Quarantined);
  EXPECT_EQ(state.snapshot().mailbox_size, 2u); // owned notifications do not consume futures/credits
}

TEST(H700PresentationIntegration, ActualAdmissionAndCompositionCaptureAreIndependentOfPacket) {
  const auto aurora = source("aurora.cpp");
  const auto admission = section(aurora, "bool begin_frame() noexcept", "void end_frame(bool fifoDrained");
  ordered(admission, {"h700::acquire_frame()", "g_nativeFrame.emplace(*nativeFrame)", "if (!gfx::begin_frame())",
                      "h700::abort_frame(*g_nativeFrame)", "imgui::new_frame"});
  const auto callback = section(aurora, "gfx::end_frame([", "TracyPlotConfig(\"aurora: lastVertSize\"");
  ordered(callback, {"nativeFrame = *nativeFrame", "h700::native_slot(nativeFrame)", "currentView = native->view",
                     "webgpu::resample_present_source", "pass.SetPipeline(webgpu::g_CopyPipeline)",
                     "if (rmlBindGroup && rmlOverlay)", "imgui::render(pass, imguiDrawData)", "encoder.Finish",
                     "h700::submit_frame(nativeFrame, buffer)"});
  EXPECT_NE(callback.find("if (rmlBindGroup && !rmlOverlay)"), std::string::npos);
  EXPECT_EQ(callback.find("g_nativeFrame"), std::string::npos);
  EXPECT_EQ(callback.find("recorded.packet"), std::string::npos);
  EXPECT_EQ(callback.find("packet."), std::string::npos);
  const auto frame = source("gfx/frame.cpp");
  const auto completion = section(frame, "auto afterSubmitCallbacks = std::move(packet.afterSubmitCallbacks)",
                                  "expire_cached_bind_groups(finishedFrame)");
  ordered(completion, {"packet = {}", "callback(encoder, std::move(afterSubmitCallbacks))", "g_frameSlots.release(frameSlot)"});
}

TEST(H700PresentationIntegration, ActualSubmitPublishesStatusBearingFutureWithoutPresenterWait) {
  const auto presentation = source("h700/presentation.cpp");
  const auto submit = section(presentation, "bool submit_frame(", "bool main_progress_once()");
  ordered(submit, {"backend->state->mark_submitted(descriptor)", "webgpu::g_queue.Submit(1, &commands)",
                   "OnSubmittedWorkDone(", "wgpu::CallbackMode::WaitAnyOnly", "flight->status.store(status",
                   "backend->state->publish_submitted(descriptor, flight->forward.id)", "flight->published.store(true"});
  EXPECT_EQ(submit.find(".WaitAny("), std::string::npos);
  EXPECT_EQ(submit.find("SwapWindow("), std::string::npos);
  EXPECT_EQ(submit.find("eglSwapBuffers("), std::string::npos);
  EXPECT_EQ(submit.find("acquire_frame("), std::string::npos);
  const auto forward = section(presentation, "void poll_forward(", "void poll_reverse(");
  ordered(forward, {"WaitAny(1, &info, 0)", "wait == wgpu::WaitStatus::TimedOut", "!info.completed",
                    "flight->callback_called.load", "wgpu::QueueWorkDoneStatus::Success", "complete_forward("});
}

TEST(H700PresentationIntegration, ActualMainProgressAndReverseOrderingNeverFakeCompletion) {
  const auto presentation = source("h700/presentation.cpp");
  const auto progress = section(presentation, "bool main_progress_once()", "std::array<std::optional<NativePending>");
  ordered(progress, {"backend->progressing", "webgpu::g_instance.ProcessEvents()", "poll_forward(flight)", "present_ready()"});
  for (const auto forbidden : {"enqueue_", "window::poll_events(", "gfx::begin_frame(", "shutdown(", "callback()"}) {
    EXPECT_EQ(progress.find(forbidden), std::string::npos) << forbidden;
  }
  const auto present = section(presentation, "void present_ready()", "std::optional<FrameDescriptor> acquire_frame()");
  ordered(present, {"restore_presenter()", "glBlitFramebuffer(", "backend->create_sync(", "begin_retiring(",
                    "glFlush()", "if (eglSwapBuffers(backend->display, backend->draw) != EGL_TRUE)",
                    "complete_swap(", "gfx::after_present()"});
  EXPECT_EQ(present.find("SDL_GL_SwapWindow("), std::string::npos);
  const auto swapFailure = section(present, "if (eglSwapBuffers(backend->display, backend->draw) != EGL_TRUE)",
                                   "if (!backend->state->complete_swap(*descriptor))");
  ordered(swapFailure, {"const EGLint error = eglGetError()", "quarantine_frame(*descriptor,",
                        "H700 eglSwapBuffers failed: EGL error", "return;"});
  EXPECT_EQ(swapFailure.find("complete_swap("), std::string::npos);
  EXPECT_EQ(swapFailure.find("gfx::after_present()"), std::string::npos);
  const auto reverse = section(presentation, "void poll_reverse(", "void present_ready()");
  ordered(reverse, {"flight_for(flight->descriptor)", "*owned.reverse != reinterpret_cast<uintptr_t>(flight->reverse)",
                    "client_wait_sync(backend->display, flight->reverse, 0, 0)",
                    "EGL_TIMEOUT_EXPIRED_KHR", "EGL_CONDITION_SATISFIED_KHR", "destroy_sync(",
                    "complete_reverse(", "recycle_safe(flight)"});
  const auto recycle = section(presentation, "void recycle_safe(", "void poll_reverse(");
  ordered(recycle, {"flight_for(flight->descriptor)", "owned.reverse_complete", "owned.swap_succeeded",
                    "observation->hold_credit", "backend->state->recycle(flight->descriptor)"});
  const auto transport = section(presentation, "void poll_forward(", "ContextSnapshot context_snapshot()");
  for (const auto forbidden : {"glFinish(", "WrapExternalEGLImage("}) {
    EXPECT_EQ(transport.find(forbidden), std::string::npos) << forbidden;
  }
  const auto diagnosticRead = section(present, "if (const auto observation = diagnostic::current())", "const EGLint attrs[]");
  ordered(diagnosticRead, {"glReadPixels(", "glGetError()", "observation->pixels(pixels)"});
  EXPECT_EQ(transport.find("glReadPixels("), transport.rfind("glReadPixels(")); // single opt-in observation site only
  EXPECT_NE(presentation.find("pending native ownership/futures/reverse fences (IG-11 retirement required)"), std::string::npos);
}
} // namespace
} // namespace aurora::h700
