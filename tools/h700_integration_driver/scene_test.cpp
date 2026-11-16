#include "scene.hpp"
#include <gtest/gtest.h>
#include <cstring>
#include <future>
#include <fstream>
#include <iterator>
#include <latch>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace h700_driver;
using namespace aurora::h700::diagnostic;
using namespace std::chrono_literals;
TEST(DriverSceneOracle, FixedKnownFogValuesAtBothEfbSizes) {
  constexpr std::array<unsigned,4> k1{175,189,189,175}, k2{187,191,191,187};
  for (const unsigned width:{640,320}) for (unsigned i=0;i<4;++i) {
    EXPECT_EQ(expected(1,0,width,Points[i]), (Pixel{uint8_t(k1[i]),uint8_t(k1[i]),uint8_t(k1[i]),255}));
    EXPECT_EQ(expected(1,1,width,Points[i]), (Pixel{uint8_t(k2[i]),uint8_t(k2[i]),uint8_t(k2[i]),255}));
  }
}
TEST(DriverSceneOracle, OrthographicGeometryAccountsForActualReversedZConversion) {
  const double viewZ=-0.25;
  const double gxClip=-viewZ-1.0;
  const double dawnDepth=-gxClip;
  EXPECT_DOUBLE_EQ(dawnDepth,0.75);
  EXPECT_DOUBLE_EQ(1.0-dawnDepth,0.25);
}
TEST(DriverSceneOracle, RejectsMissingRangeDataAndKeepsAlphaExact) {
  const auto wanted=expected(1,0,640,{80,200});
  EXPECT_FALSE(matches({191,191,191,255},wanted,{80,200})); // missing range adjustment (k/infinite or unity)
  auto changed=wanted; ++changed[0]; EXPECT_TRUE(matches(changed,wanted,{80,200}));
  changed[0]+=1; EXPECT_FALSE(matches(changed,wanted,{80,200}));
  changed=wanted; changed[3]=254; EXPECT_FALSE(matches(changed,wanted,{80,200}));
  EXPECT_FALSE(matches({0,254,255,255},expected(1,0,320,{24,24}),{24,24}));
}
TEST(DriverSceneOracle, AsymmetricOrientationAndGenerationFrameIdentity) {
  EXPECT_EQ(expected(1,0,640,{80,40}), (Pixel{255,0,0,255}));
  EXPECT_EQ(expected(1,0,640,{80,440}), (Pixel{0,0,255,255}));
  EXPECT_FALSE(matches(expected(1,0,640,{80,440}),expected(1,0,640,{80,40}),{80,40}));
  EXPECT_EQ(expected(2,1,640,{320,440}), (Pixel{1,128,254,255}));
  EXPECT_NE(expected(1,1,640,{320,440}),expected(2,1,640,{320,440}));
  EXPECT_EQ(expected(1,60,320,{24,24}), (Pixel{0,255,255,255}));
}
TEST(DriverSceneOracle, InvalidCoordinatesOrProfileNeverCalibrateExpectedValues) {
  EXPECT_THROW(expected(1,0,512,{80,200}),std::invalid_argument);
  EXPECT_THROW(expected(1,120,640,{80,200}),std::invalid_argument);
  EXPECT_THROW(expected(1,0,640,{80,201}),std::invalid_argument);
}
TEST(DriverScenePhases, ArtificialBarriersAndPairwiseOnlyIntervalsDoNotProveTripleOverlap) {
  std::array<std::thread::id,3> ids;
  std::latch ready{3}, release{1};
  std::array<std::thread,3> threads;
  for (unsigned i=0;i<3;++i) threads[i]=std::thread([&,i] { ids[i]=std::this_thread::get_id(); ready.count_down(); release.wait(); });
  ready.wait(); release.count_down(); for (auto& thread:threads) thread.join();
  std::vector<Event> events{{Kind::Barrier,1,0,ids[0],true},{Kind::Barrier,1,100,ids[0],false}};
  EXPECT_FALSE(real_overlap(events));
  events={{Kind::Compiler,1,1,ids[0],true},{Kind::Compiler,1,6,ids[0],false},
          {Kind::ResourceTexture,1,2,ids[1],true},{Kind::ResourceTexture,1,7,ids[1],false},
          {Kind::RenderEncoder,1,6,ids[2],true},{Kind::RenderEncoder,1,8,ids[2],false}};
  EXPECT_FALSE(real_overlap(events)); // touching endpoints is not overlap
  events[4].ns=3;
  EXPECT_TRUE(real_overlap(events));
  events[4].thread=ids[0]; events[5].thread=ids[0]; EXPECT_FALSE(real_overlap(events));
}
TEST(DriverScenePhases, FailureReleasesOwnCompilerGateWithoutCreatingFakeWork) {
  auto control=std::make_shared<Control>(2,Clock::now()+2s);
  control->arm_stop();
  auto future=std::async(std::launch::async,[&] { return control->before(Kind::Compiler,1,true); });
  const auto deadline=Clock::now()+1s;
  while (!control->compilerHeld.load() && Clock::now()<deadline) std::this_thread::yield();
  EXPECT_TRUE(control->compilerHeld.load());
  EXPECT_EQ(future.wait_for(1ms),std::future_status::timeout);
  control->cancel();
  EXPECT_FALSE(future.get());
  EXPECT_FALSE(real_overlap(control->events()));
  EXPECT_EQ(control->fog_variants(),0u);
}
TEST(DriverScenePhases, DeadlineAutomaticallyOpensOwnGateAndRetainsFailure) {
  Control control(2,Clock::now()+5ms); control.arm_stop();
  EXPECT_FALSE(control.before(Kind::Compiler,1,true));
  EXPECT_TRUE(control.failed.load());
  EXPECT_FALSE(control.healthy());
}
TEST(DriverScenePhases, FixedNativeStorageIdentityCannotChangeAcrossEfbResize) {
  Control control(1,Clock::now()+2s);
  control.storage(0,11,12,13,640,480); control.storage(1,21,22,23,640,480);
  control.storage(0,11,12,13,640,480);
  EXPECT_FALSE(control.failed.load());
  control.storage(0,31,12,13,640,480);
  EXPECT_TRUE(control.failed.load());
  EXPECT_FALSE(control.hold_credit(0)); // opens ONLY the diagnostic delay, not a GPU ownership transition
}
TEST(DriverScenePhases, ThirdAcquireReleasesDelayAfterFourProgressPassesNotGpuCompletion) {
  Control control(1,Clock::now()+2s);
  EXPECT_TRUE(control.hold_credit(0));
  control.third_acquire();
  for (unsigned i=0;i<3;++i) { control.progress(); EXPECT_TRUE(control.hold_credit(0)); }
  control.progress(); EXPECT_FALSE(control.hold_credit(0)); EXPECT_EQ(control.pressure_passes(),4u);
  EXPECT_FALSE(control.hold_credit(2));
  EXPECT_FALSE(real_overlap(control.events())); // CPU delay release is not a GPU fence result
}
std::string source(const char* file) {
  std::ifstream input(std::string(DRIVER_SOURCE_DIR)+"/"+file);
  EXPECT_TRUE(input.good())<<file;
  return {std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
}
TEST(DriverSceneSource, ObservationsBracketActualCallsAndDefaultPathRemainsOptIn) {
  const auto hooks=source("../../extern/aurora/lib/h700/diagnostics.hpp");
  EXPECT_LT(hooks.find("if (!control) return call()"),hooks.find("control->before(kind"));
  const auto begin=hooks.find("const auto begin = Clock::now()");
  const auto end=hooks.find("const auto end = Clock::now()",begin);
  ASSERT_NE(begin,std::string::npos); ASSERT_NE(end,std::string::npos);
  EXPECT_EQ(hooks.substr(begin,end-begin).find("event("),std::string::npos);
  EXPECT_LT(hooks.find("auto result = call()",begin),end);
  const auto gx=source("../../extern/aurora/lib/gx/gx.cpp");
  EXPECT_NE(gx.find("[&] { return g_device.CreateRenderPipeline(&descriptor); }, config.shaderConfig.fogRangeEnabled"),std::string::npos);
  const auto frame=source("../../extern/aurora/lib/gfx/frame.cpp");
  EXPECT_NE(frame.find("Kind::RenderEncoder"),std::string::npos);
  EXPECT_NE(frame.find("[&] { return g_device.CreateCommandEncoder(&EncoderDescriptor); }"),std::string::npos);
  const auto texture=source("../../extern/aurora/lib/gfx/texture.cpp");
  EXPECT_NE(texture.find("Kind::ResourceTexture"),std::string::npos);
  const auto gpu=source("../../extern/aurora/lib/webgpu/gpu.cpp");
  EXPECT_NE(gpu.find("diagnostic::nearest() ? wgpu::FilterMode::Nearest : wgpu::FilterMode::Linear"),std::string::npos);
}
TEST(DriverSceneSource, NativeCopySharesSubmitAndPresenterReadsBeforeSwapWithGlOrientation) {
  const auto presentation=source("../../extern/aurora/lib/h700/presentation.cpp");
  EXPECT_LT(presentation.find("glReadPixels(point.x, NativeHeight - 1 - point.y"),presentation.find("eglSwapBuffers(backend->display"));
  EXPECT_LT(presentation.find("observation->pixels(pixels); // owned"),presentation.find("flight->reverse = backend->create_sync"));
  EXPECT_NE(presentation.find("poll_diagnostic_pixels(flight, true)"),std::string::npos);
  EXPECT_NE(presentation.find("diagnostic_buffer.GetConstMappedRange()"),std::string::npos);
  EXPECT_LT(presentation.find("!teardown && !flight->published.load(std::memory_order_acquire)"),
            presentation.find("if (!flight->diagnostic_buffer) return true"));
  EXPECT_NE(presentation.find("observation->cancel(); // release own gates on real Dawn failure too"),std::string::npos);
  const auto aurora=source("../../extern/aurora/lib/aurora.cpp");
  EXPECT_LT(aurora.find("h700::diagnostic_copy_native(nativeFrame, encoder)"),aurora.find("encoder.Finish(&cmdBufDescriptor)"));
  const auto scene=source("scene.cpp");
  EXPECT_LT(scene.find("request_generation_shutdown();"),scene.find("control->release();"));
  EXPECT_NE(scene.find("Retain pendingScene and installed control"),std::string::npos);
  EXPECT_EQ(scene.find("build_fog_range_lut"),std::string::npos);
}
TEST(DriverSceneSource, FixedScenarioRequiresRealPendingStatesAndColdVariants) {
  const auto scene=source("scene.cpp");
  for (const auto required:{"gen==1?120:2", "frame==60", "control->imports.load()==2", "control->fog_variants()==4",
       "real_overlap(control->events())", "control->pressure_passes()==4", "control->compilerHeld.load", "control->renderWaiting.load",
       "std::chrono::seconds(60)", "pipelineCacheNoSeed=true", "skip_validation,disable_robustness"})
    EXPECT_NE(scene.find(required),std::string::npos)<<required;
  EXPECT_EQ(scene.find("DVDInit("),std::string::npos);
  EXPECT_NE(scene.find("control->compilerHeldKey.load(std::memory_order_acquire)==control->renderWaitingKey.load"),std::string::npos);
  EXPECT_NE(scene.find("{0,0,-1,-1}"),std::string::npos);
  EXPECT_NE(scene.find("config.resourcesPath=resources.c_str()"),std::string::npos);
  const auto oracle=source("scene.hpp");
  EXPECT_EQ(oracle.find("build_fog_range_lut"),std::string::npos);
}
TEST(DriverSceneExit, OnlyActualReleaseAllowsNormalReturn) {
  using aurora::h700::ShutdownStatus;
  EXPECT_FALSE(generation_released(ShutdownStatus::Pending));
  EXPECT_FALSE(generation_released(ShutdownStatus::Unknown));
  EXPECT_TRUE(generation_released(ShutdownStatus::Released));
  EXPECT_TRUE(generation_released(ShutdownStatus::FailedReleased)); // known release, not PASS/reinit
}
TEST(DriverSceneExit, FailStopBypassesLiveWorkerAndAtExit) {
  // Direct child avoids GTest's stream capture writing to /tmp in the read-only build image.
  const auto child=::fork();
  ASSERT_GE(child,0);
  if (child==0) {
    std::atexit([] { std::_Exit(99); });
    static std::thread liveWorker([] { std::this_thread::sleep_for(1s); });
    // Returning/exit would hit the joinable worker destructor or the registered exit hook.
    fail_stop("CPU-only incomplete ownership regression");
  }
  int status=0;
  ASSERT_EQ(::waitpid(child,&status,0),child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status),EXIT_FAILURE);
}
TEST(DriverSceneOverrides, PresenceConsumersRejectEmptyAndZeroToo) {
  for (const char* name : {"AURORA_STAGING_START_KIB","AURORA_STAGING_START_MASK","AURORA_FULL_DEVICE_BUFFERS"}) {
    for (const char* value : {"", "0", "1", "15"}) {
      const char* rejected=conflicting_override([&](const char* key) { return std::strcmp(key,name)==0 ? value : nullptr; });
      ASSERT_NE(rejected,nullptr); EXPECT_STREQ(rejected,name);
    }
  }
  EXPECT_EQ(conflicting_override([](const char*) -> const char* { return nullptr; }),nullptr);
}
TEST(DriverSceneOverrides, ValueConsumersRejectNonemptyButNotInertEmptyValues) {
  for (const char* name : {"AURORA_OVERDRAW","MP_STORAGE_CLAMP"}) {
    for (const char* value : {"0", "1", "/optional-output"}) {
      const char* rejected=conflicting_override([&](const char* key) { return std::strcmp(key,name)==0 ? value : nullptr; });
      ASSERT_NE(rejected,nullptr); EXPECT_STREQ(rejected,name);
    }
    EXPECT_EQ(conflicting_override([&](const char* key) { return std::strcmp(key,name)==0 ? "" : nullptr; }),nullptr);
  }
  EXPECT_EQ(conflicting_override([](const char* key) { return std::strcmp(key,"MP_PASS_LOG")==0 ? "1" : nullptr; }),nullptr);
}
TEST(DriverSceneSource, IncompleteAndExceptionNeverUnwindLiveOwners) {
  const auto scene=source("scene.cpp"), main=source("main.cpp"), header=source("scene.hpp");
  EXPECT_LT(scene.find("generationOwnersLive=true"),scene.find("aurora_initialize(0,nullptr,&config)"));
  EXPECT_NE(scene.find("if (generation_released(status)) generationOwnersLive=false"),std::string::npos);
  const auto retained=scene.find("if (!generation_released(status))");
  const auto destroy=scene.find("delete pendingScene",retained);
  ASSERT_NE(retained,std::string::npos); ASSERT_NE(destroy,std::string::npos);
  const auto branch=scene.substr(retained,destroy-retained);
  EXPECT_EQ(branch.find("return "),std::string::npos);
  EXPECT_NE(branch.find("incomplete.flush()"),std::string::npos);
  EXPECT_NE(branch.find("unsigned(shutdown.stage)"),std::string::npos);
  EXPECT_NE(branch.find("fail_stop(\"generation shutdown incomplete\")"),std::string::npos);
  EXPECT_EQ(scene.find("return ShutdownStatus::Pending"),std::string::npos);
  EXPECT_NE(scene.find("return result; // preserve actual Pending stage"),std::string::npos);
  EXPECT_LT(main.find("if (h700_driver::generationOwnersLive)"),main.find("h700_driver::fail_stop("));
  EXPECT_NE(main.find("catch (...) { return scene_exception"),std::string::npos);
  EXPECT_LT(header.find("std::fflush(nullptr)"),header.find("std::_Exit(EXIT_FAILURE)"));
  EXPECT_EQ(header.find("std::exit("),std::string::npos);
}
TEST(DriverSceneSource, OverrideGuardPrecedesGraphicsAndMatchesActualConsumers) {
  const auto main=source("main.cpp");
  EXPECT_LT(main.find("h700_driver::conflicting_override("),main.find("h700_driver::run_scene(argv[3])"));
  const auto frame=source("../../extern/aurora/lib/gfx/frame.cpp");
  EXPECT_NE(frame.find("if (const char* kib = std::getenv(\"AURORA_STAGING_START_KIB\"))"),std::string::npos);
  EXPECT_NE(frame.find("maskEnv != nullptr ?"),std::string::npos);
  EXPECT_NE(frame.find("std::getenv(\"AURORA_FULL_DEVICE_BUFFERS\") != nullptr"),std::string::npos);
  const auto overdraw=source("../../extern/aurora/lib/gfx/overdraw.cpp");
  EXPECT_NE(overdraw.find("path != nullptr && *path != '\\0'"),std::string::npos);
  const auto shader=source("../../extern/aurora/lib/gx/shader.cpp");
  EXPECT_NE(shader.find("force != nullptr && *force != '\\0'"),std::string::npos);
}
} // namespace
