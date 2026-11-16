#include "scene.hpp"
#ifdef AURORA_H700
#include "../../extern/aurora/lib/h700/lifecycle.hpp"
#include "../../extern/aurora/lib/h700/presentation.hpp"
#include "../../extern/aurora/lib/webgpu/gpu.hpp"
#include "../../extern/aurora/lib/window.hpp"
#include <aurora/aurora.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace h700_driver {
namespace {
using namespace aurora;
using namespace aurora::h700;
using namespace aurora::h700::diagnostic;
struct Scene {
  // Every FIFO array stays valid until actual generation quiescence, including deadline failure.
  std::array<std::array<std::array<float,3>,24>,120> positions{};
  alignas(32) std::array<std::array<uint8_t,64>,4> textureBytes{};
  std::array<GXTexObj,4> textures{};
  explicit Scene() {
    VIConfigure(nullptr);
    GXInit(nullptr, 0);
    for (unsigned v = 0; v < 4; ++v) {
      textureBytes[v].fill(255);
      textureBytes[v][31] = static_cast<uint8_t>(v * 32); // unsampled texel; four real content identities
      GXInitTexObj(&textures[v], textureBytes[v].data(), 4,4,GX_TF_RGBA8,GX_CLAMP,GX_CLAMP,GX_FALSE);
      GXInitTexObjFilter(&textures[v],GX_NEAR,GX_NEAR);
    }
  }
  void draw(unsigned gen, unsigned frame) {
    // GX clips z in [-w,0]; the production uniform reverses its projection row.
    // view z=-1/4 -> GX clip=-3/4 -> Dawn depth=3/4 -> fog GX depth=1/4.
    const float projection[4][4]{{2.f/640,0,0,-1},{0,-2.f/480,0,1},{0,0,-1,-1},{0,0,0,1}};
    const float identity[3][4]{{1,0,0,0},{0,1,0,0},{0,0,1,0}};
    GXSetProjection(projection,GX_ORTHOGRAPHIC);
    GXLoadPosMtxImm(identity,GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0,0,640,480,0,1);
    GXSetScissor(0,0,640,480);
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE,GX_ALWAYS,GX_FALSE);
    GXSetBlendMode(GX_BM_NONE,GX_BL_ONE,GX_BL_ZERO,GX_LO_COPY);
    GXSetColorUpdate(GX_TRUE); GXSetAlphaUpdate(GX_TRUE); GXSetDither(GX_FALSE);
    GXSetAlphaCompare(GX_ALWAYS,0,GX_AOP_AND,GX_ALWAYS,0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0,GX_FALSE,GX_SRC_VTX,GX_SRC_VTX,0,GX_DF_NONE,GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen(GX_TEXCOORD0,GX_TG_MTX2x4,GX_TG_TEX0,GX_IDENTITY);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS,GX_INDEX16); GXSetVtxDesc(GX_VA_CLR0,GX_DIRECT); GXSetVtxDesc(GX_VA_TEX0,GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_POS,GX_POS_XYZ,GX_F32,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_CLR0,GX_CLR_RGBA,GX_RGBA8,0);
    GXSetVtxAttrFmt(GX_VTXFMT0,GX_VA_TEX0,GX_TEX_ST,GX_F32,0);
    const std::array<std::array<float,4>,6> rects{{{float(frame&1),120,640,280},{64,0,128,64},
        {528,0,592,64},{64,408,128,472},{528,408,592,472},{288,408,352,472}}};
    for (size_t q=0;q<rects.size();++q) {
      const auto r=rects[q];
      positions[frame][q*4+0]={r[0],r[1],-0.25f}; positions[frame][q*4+1]={r[2],r[1],-0.25f};
      positions[frame][q*4+2]={r[2],r[3],-0.25f}; positions[frame][q*4+3]={r[0],r[3],-0.25f};
    }
    GXSetArray(GX_VA_POS,positions[frame].data(),sizeof(positions[frame]),sizeof(positions[frame][0]),true);
    GXInvalidateVtxCache();
    GXLoadTexObj(&textures[frame%4],GX_TEXMAP0);
    const unsigned stages=1+frame%4;
    GXSetNumTevStages(stages);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD0,GX_TEXMAP0,GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0,GX_MODULATE);
    for (unsigned i=1;i<stages;++i) {
      const auto stage=static_cast<GXTevStageID>(i);
      GXSetTevOrder(stage,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR_NULL);
      GXSetTevColorIn(stage,GX_CC_ZERO,GX_CC_ZERO,GX_CC_ZERO,GX_CC_CPREV);
      GXSetTevAlphaIn(stage,GX_CA_ZERO,GX_CA_ZERO,GX_CA_ZERO,GX_CA_APREV);
      GXSetTevColorOp(stage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
      GXSetTevAlphaOp(stage,GX_TEV_ADD,GX_TB_ZERO,GX_CS_SCALE_1,GX_TRUE,GX_TEVPREV);
    }
    GXFogAdjTable table{};
    for (auto& value:table.r) value=(frame&1)?128:64;
    GXSetFog(GX_FOG_ORTHO_LIN,0,1,0,1,{0,0,0,255});
    GXSetFogRangeAdj(GX_TRUE,320,&table);
    quad(frame,0,{255,255,255,255});
    GXSetFog(GX_FOG_NONE,0,1,0,1,{0,0,0,255});
    GXSetFogRangeAdj(GX_FALSE,0,nullptr);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0,GX_TEXCOORD_NULL,GX_TEXMAP_NULL,GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0,GX_PASSCLR);
    quad(frame,1,{255,0,0,255}); quad(frame,2,{0,255,0,255});
    quad(frame,3,{0,0,255,255}); quad(frame,4,{255,255,0,255}); quad(frame,5,frame_marker(gen,frame));
    ImGui::GetForegroundDrawList()->AddRectFilled({16,16},{48,48},IM_COL32(0,255,255,255));
  }
  void quad(unsigned, unsigned q, Pixel color) {
    GXBegin(GX_QUADS,GX_VTXFMT0,4);
    for (unsigned v=0;v<4;++v) {
      GXPosition1x16(q*4+v); GXColor4u8(color[0],color[1],color[2],color[3]); GXTexCoord2f32(0.125f,0.125f);
    }
    GXEnd();
  }
};
// A deadline/Unknown is not quiescence. Deliberately retain this one bounded owner on
// failure instead of destroying arrays an active FIFO job may still reference.
Scene* pendingScene = nullptr;
struct Results {
  unsigned generation;
  std::array<unsigned,120> seen{};
  bool valid=true;
  uint64_t presented=0;
  std::ofstream samples;
  Results(unsigned gen, const std::filesystem::path& directory) : generation(gen), samples(directory/"pixels.tsv") {
    samples<<"frame\tstage\tx\ty\tactual_rgba\texpected_rgba\n";
    valid=samples.good();
  }
  void consume(const std::shared_ptr<Control>& control, std::shared_ptr<PresentationState> notifications = {}) {
    if (!notifications) notifications=presentation_state();
    if (notifications) for (const auto& n:notifications->take_notifications()) {
      if (n.outcome!=PresentationNotification::Outcome::Presented || n.descriptor.frame!=presented) valid=false;
      else ++presented;
    }
    for (const auto& record:control->take_pixels()) {
      if (record.frame>=seen.size()) { valid=false; continue; }
      const unsigned bit=record.presenter?2:1;
      if (seen[record.frame]&bit) valid=false;
      seen[record.frame]|=bit;
      const unsigned width=generation==1 && record.frame>=60 ? 320:640;
      for (size_t i=0;i<Points.size();++i) {
        const auto wanted=expected(generation,record.frame,width,Points[i]);
        const auto actual=record.values[i];
        samples<<record.frame<<'\t'<<(record.presenter?"after-blit":"native")<<'\t'<<Points[i].x<<'\t'<<Points[i].y<<'\t';
        for (auto value:actual) samples<<unsigned(value)<<',';
        samples<<'\t'; for (auto value:wanted) samples<<unsigned(value)<<','; samples<<'\n';
        if (!matches(record.values[i],wanted,Points[i])) {
          valid=false;
          const auto a=record.values[i];
          std::fprintf(stderr,"pixel FAIL gen=%u frame=%llu stage=%s xy=%u,%u actual=%u,%u,%u,%u expected=%u,%u,%u,%u\n",
              generation,static_cast<unsigned long long>(record.frame),record.presenter?"after-blit":"native",
              Points[i].x,Points[i].y,a[0],a[1],a[2],a[3],wanted[0],wanted[1],wanted[2],wanted[3]);
        }
      }
    }
    valid=valid && samples.good();
    if (!valid) control->cancel();
  }
};
bool trace(const std::filesystem::path& directory, const std::shared_ptr<Control>& control) {
  std::ofstream file(directory/"intervals.tsv");
  file<<"kind\tkey\tns\tthread\tbegin\n";
  for (const auto& e:control->events()) file<<unsigned(e.kind)<<'\t'<<e.key<<'\t'<<e.ns<<'\t'
      <<std::hash<std::thread::id>{}(e.thread)<<'\t'<<e.begin<<'\n';
  file.flush();
  std::ofstream exports(directory/"exports.tsv");
  for (const auto& ids:control->storage()) exports<<ids[0]<<'\t'<<ids[1]<<'\t'<<ids[2]<<'\n';
  exports.flush();
  return file.good() && exports.good();
}
ShutdownResult finish(const std::shared_ptr<Control>& control, Results& results, const std::filesystem::path& dir) {
  const auto state=presentation_state();
  request_generation_shutdown();
  control->release(); // release ONLY our barriers/credit delay, never GPU ownership
  bool contextsWritten=false;
  while (true) {
    const auto result=poll_generation_shutdown();
    results.consume(control,state); // main safe-point: ordered owned results, never from progress
    if (result.stage==ShutdownStage::Exports && !contextsWritten) {
      std::ofstream file(dir/"contexts.tsv");
      const auto snapshot=context_snapshot();
      file<<"trustworthy\t"<<snapshot.trustworthy<<'\n';
      for (const auto& c:snapshot.contexts) file<<reinterpret_cast<uintptr_t>(c.context)<<'\t'<<c.group<<'\t'<<c.destroyed<<'\n';
      for (const auto& e:snapshot.events) file<<"event\t"<<unsigned(e.operation)<<'\t'
          <<std::hash<std::thread::id>{}(e.thread)<<'\t'<<reinterpret_cast<uintptr_t>(e.display)<<'\t'
          <<reinterpret_cast<uintptr_t>(e.context)<<'\t'<<reinterpret_cast<uintptr_t>(e.share)<<'\t'
          <<reinterpret_cast<uintptr_t>(e.draw)<<'\t'<<reinterpret_cast<uintptr_t>(e.read)<<'\t'<<e.result<<'\n';
      file.flush(); if (!file.good()) { results.valid=false; control->cancel(); }
      contextsWritten=true; // taken after WHOLE-group proof, BEFORE exports are destroyed
    }
    if (result.status!=ShutdownStatus::Pending) return result;
    if (Clock::now()>=control->deadline) {
      control->cancel();
      std::fprintf(stderr,"Incomplete generation teardown at stage %u; owners retained\n",unsigned(result.stage));
      return result; // preserve actual Pending stage for fail-stop evidence
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
bool generation(unsigned gen, const std::filesystem::path& dir, Clock::time_point deadline) {
  auto control=std::make_shared<Control>(gen,deadline);
  diagnostic::install(control);
  Results results{gen,dir};
  if (!results.valid) { diagnostic::install({}); return false; }
  const auto user=(dir/"user").string()+"/", cache=(dir/"cache").string()+"/", resources=(dir/"resources").string()+"/";
  if (!std::filesystem::create_directory(user) || !std::filesystem::create_directory(cache) ||
      !std::filesystem::create_directory(resources)) { diagnostic::install({}); return false; }
  AuroraConfig config{};
  config.appName="H700 integration"; config.userPath=user.c_str(); config.cachePath=cache.c_str(); config.resourcesPath=resources.c_str();
  config.desiredBackend=BACKEND_OPENGLES; config.msaa=1; config.maxTextureAnisotropy=1; config.startFullscreen=true;
  config.windowWidth=640; config.windowHeight=480; config.frameBufferScale=1; config.frameStorageScale=1;
  config.pipelineCacheNoSeed=true; config.pipelineCacheLazy=true; config.logLevel=LOG_INFO;
  config.dawnDisableToggles="skip_validation,disable_robustness";
  config.imGuiInitCallback=[](const AuroraWindowSize*) { ImGui::GetIO().IniFilename=nullptr; };
  aurora_disable_config_persistence();
  generationOwnersLive=true; // initialization may create owners before throwing/failing
  const auto info=aurora_initialize(0,nullptr,&config);
  bool good=info.backend==BACKEND_OPENGLES && control->imports.load()==2;
  if (good) {
    window::set_frame_buffer_scale(1.f);
    aurora_set_resampler(SAMPLER_BILINEAR); // the explicit diagnostic sampler override is Nearest, not Linear
    aurora_set_present_sharpness(0.f);
    pendingScene=new Scene;
    control->arm_overlap();
    const unsigned frames=gen==1?120:2; // fixed bound; never extend to manufacture observations
    for (unsigned frame=0;frame<frames && good;++frame) {
      if (!control->healthy()) { good=false; break; }
      if (gen==1 && frame==60) window::set_frame_buffer_scale(0.5f);
      aurora_update();
      const auto size=window::get_window_size();
      const unsigned width=gen==1 && frame>=60?320:640;
      if (size.fb_width!=width || size.fb_height!=width*3/4 || size.native_fb_width!=640 || size.native_fb_height!=480 || control->imports!=2) { good=false; break; }
      control->activeFrame.store(frame,std::memory_order_release);
      if (!aurora_begin_frame()) { good=false; break; }
      pendingScene->draw(gen,frame);
      aurora_end_frame();
      results.consume(control);
      if (gen==1 && frame==1) {
        bool bothSafe=false;
        while (control->healthy() && !bothSafe) {
          (void)main_progress_once(); results.consume(control);
          const auto ownership=presentation_state()->snapshot();
          bothSafe=std::all_of(ownership.slots.begin(),ownership.slots.end(),[](const auto& slot) {
            return slot.descriptor && slot.descriptor->frame<2 && slot.reverse_complete && slot.swap_succeeded;
          });
          std::this_thread::yield();
        }
        if (!bothSafe) { good=false; break; }
      }
      if (gen==2 && frame==0) {
        bool submitted=false;
        while (control->healthy() && !submitted) {
          (void)main_progress_once(); results.consume(control);
          const auto ownership=presentation_state()->snapshot();
          for (const auto& flight:pending_native_frames()) if (flight && flight->descriptor.frame==0 && flight->published) {
            const auto& slot=ownership.slots[flight->descriptor.slot];
            submitted=slot.reverse_complete && slot.swap_succeeded; // actually safe, but credit is diagnostically held
          }
          std::this_thread::yield();
        }
        if (!submitted) { good=false; break; }
        control->arm_stop();
      }
    }
    if (good && gen==2) {
      bool pending=false;
      while (control->healthy() && !pending) {
        (void)main_progress_once(); results.consume(control);
        const auto flights=pending_native_frames();
        bool first=false,second=false;
        for (const auto& f:flights) if (f) {
          if (f->descriptor.frame==0 && f->published) first=true;
          if (f->descriptor.frame==1 && !f->published) second=true;
        }
        pending=first && second && control->compilerHeld.load(std::memory_order_acquire) && control->renderWaiting.load(std::memory_order_acquire)>0 &&
            control->compilerHeldKey.load(std::memory_order_acquire)==control->renderWaitingKey.load(std::memory_order_acquire);
        std::this_thread::yield();
      }
      good=pending;
      if (pending) {
        control->event(Kind::Stop,1,true); // observed boundary, not fake work completion
        std::ofstream snapshot(dir/"stop.tsv");
        snapshot<<"compiler_held\t"<<control->compilerHeld.load()<<"\nrender_waiting\t"<<control->renderWaiting.load()<<'\n';
        snapshot<<"compiler_pipeline\t"<<control->compilerHeldKey.load()<<"\nrender_pipeline\t"<<control->renderWaitingKey.load()<<'\n';
        for (const auto& f:pending_native_frames()) if (f)
          snapshot<<"frame\t"<<f->descriptor.frame<<"\tslot\t"<<unsigned(f->descriptor.slot)
                  <<"\tpublished\t"<<f->published<<"\tfuture\t"<<f->forward.id<<'\n';
        const auto ownership=presentation_state()->snapshot();
        for (const auto& slot:ownership.slots)
          snapshot<<"status\t"<<unsigned(slot.status)<<"\tforward_complete\t"<<slot.forward_complete
                  <<"\treverse_complete\t"<<slot.reverse_complete<<"\tswap_succeeded\t"<<slot.swap_succeeded<<'\n';
        snapshot.flush(); if (!snapshot.good()) good=false;
      }
    }
  }
  if (!good || !results.valid) {
    control->cancel();
    if (const auto state=presentation_state()) state->fail("IG12 scenario incomplete/failed");
  }
  const auto shutdown=finish(control,results,dir);
  const auto status=shutdown.status;
  if (generation_released(status)) generationOwnersLive=false;
  results.samples.flush();
  results.valid=results.valid && results.samples.good();
  if (!trace(dir,control)) results.valid=false;
  if (!generation_released(status)) {
    // Retain pendingScene and installed control; returning would run live global destructors.
    std::fprintf(stderr,"Incomplete generation=%u status=%u stage=%u; owners retained\n",
                 gen,unsigned(status),unsigned(shutdown.stage));
    std::ofstream incomplete(dir/"incomplete.tsv");
    incomplete<<"generation\t"<<gen<<"\nstatus\t"<<unsigned(status)<<"\nstage\t"<<unsigned(shutdown.stage)<<'\n';
    incomplete.flush();
    if (!incomplete.good()) std::fputs("Unable to persist incomplete.tsv; status/stage reported on stderr\n",stderr);
    fail_stop("generation shutdown incomplete");
  }
  delete pendingScene; pendingScene=nullptr;
  diagnostic::install({});
  const unsigned frames=gen==1?120:2;
  for (unsigned i=0;i<frames;++i) if (results.seen[i]!=3) good=false;
  good=good && status==ShutdownStatus::Released && results.valid && results.presented==frames &&
      !control->failed.load() && control->imports==2;
  const int overlap=gen==1?int(real_overlap(control->events())):-1; // -1: gen2 deliberately does not claim overlap
  if (gen==1) good=good && control->fog_variants()==4 && control->overlap_barrier_complete() &&
      overlap==1 && control->pressure_passes()==4 && control->safeCreditsHeld.load()>0;
  else good=good && control->fog_variants()==2;
  std::printf("generation=%u frames=%u complete=%d variants=%u imports=%u presented=%llu pressure_passes=%u real_overlap=%d pixels_ok=%d (runtime evidence only)\n",
      gen,frames,good,control->fog_variants(),control->imports.load(),static_cast<unsigned long long>(results.presented),
      control->pressure_passes(),overlap,results.valid);
  return good;
}
} // namespace

int run_scene(const std::string& directory) {
  const auto deadline=Clock::now()+std::chrono::seconds(60);
  const std::filesystem::path root(directory);
  if (!root.is_absolute() || root==root.root_path() || std::filesystem::exists(root) || !std::filesystem::is_directory(root.parent_path())) {
    std::fputs("A fresh absolute test directory with an existing parent is required; no graphics initialized\n",stderr); return 2;
  }
  if (!std::filesystem::create_directory(root)) return 2; // exclusive creation, never reuse a raced existing directory
  for (unsigned gen=1;gen<=2;++gen) {
    const auto dir=root/("generation-"+std::to_string(gen));
    if (!std::filesystem::create_directory(dir)) return 2;
    if (!generation(gen,dir,deadline)) { std::fputs("IG12 incomplete/FAIL; no live recovery or relaxed acceptance\n",stderr); return 1; }
  }
  std::puts("Diagnostic scenario completed; orientation/UI/menu confirmation remains a separate IG14 acceptance item.");
  return 0;
}
} // namespace h700_driver
#endif
