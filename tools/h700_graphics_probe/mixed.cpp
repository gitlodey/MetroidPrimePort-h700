#include "mixed.hpp"

#include "contract.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace h700_probe::mixed {
namespace {

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 64;
constexpr uint32_t kRowBytes = kWidth * 4;
constexpr uint32_t kFetchWidth = 4096;
constexpr uint32_t kOldWords = kFetchWidth * 2;
constexpr uint32_t kNewWords = kFetchWidth * 4;
constexpr uint32_t kStates = 4;
constexpr uint32_t kDraws = kStates * 2;
constexpr uint64_t kReadbackBytes = uint64_t(kRowBytes) * kHeight;
using Pixel = std::array<uint8_t, 4>;
using Immediate = std::array<uint32_t, 16>;
using Uniform = std::array<uint32_t, 4>;
static_assert(sizeof(Immediate) == 64 && sizeof(Uniform) == 16);
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

constexpr std::array<const char*, kStates> kStateNames{
    "initial", "update-existing", "grow-preserved", "update-grown"};

void require(bool ok, std::string_view message) {
  if (!ok) throw std::runtime_error(std::string(message));
}

uint32_t word_count(uint32_t state) {
  require(state < kStates, "Mixed state outside bounded sequence");
  return state < 2 ? kOldWords : kNewWords;
}

uint32_t probe_index(uint32_t state) {
  (void)word_count(state);
  return state == 3 ? 12288u : 8191u;
}

// Expectations use integer arithmetic, not decoding the bytes uploaded to Dawn.
uint32_t expected_numerator(uint32_t index, uint32_t state) {
  require(index < word_count(state), "Mixed source index outside buffer");
  if (state == 2 && index >= kOldWords) return 0;
  const uint32_t epoch = state == 2 ? 1 : state;
  const uint32_t data_epoch = state == 3 ? 2 : epoch;
  return 1 + (index * 17 + data_epoch * 29) % 200;
}

std::vector<uint32_t> upload_words(uint32_t count, uint32_t epoch) {
  require((count == kOldWords || count == kNewWords) && epoch <= 2,
          "Mixed upload size/epoch outside bounded workload");
  std::vector<uint32_t> words(count);
  for (uint32_t i = 0; i < count; ++i) {
    const float value = float(1 + (i * 17 + epoch * 29) % 200) / 256.0f;
    words[i] = std::bit_cast<uint32_t>(value);
  }
  return words;
}

Immediate immediates(uint32_t draw) {
  require(draw < kDraws, "Mixed draw outside bounded sequence");
  Immediate result{};
  for (uint32_t i = 0; i < result.size(); ++i) result[i] = 5 + draw * 19 + i * 3;
  return result;
}

Uniform uniform_values(uint32_t state, bool shadow) {
  const uint32_t draw = state * 2 + uint32_t(shadow);
  return {11 + state * 7, probe_index(state), 31 + draw * 3, 3 + state};
}

Pixel expected_pixel(uint32_t x, uint32_t y, uint32_t state, bool shadow) {
  require(x < kWidth && y < kHeight, "Mixed pixel outside readback");
  const uint32_t draw = state * 2 + uint32_t(shadow);
  const uint32_t vertex_words = expected_numerator(4094, state) + expected_numerator(4095, state) +
                                expected_numerator(4096, state) + expected_numerator(probe_index(state), state);
  const uint32_t immediate_tag = 16 * (5 + draw * 19) + 3 * 120;
  const uint32_t green = (vertex_words + 11 + state * 7 + immediate_tag + 31 + draw * 3 +
                          y * (3 + state)) % 256;
  return {static_cast<uint8_t>(expected_numerator(4094 + x, state)),
          static_cast<uint8_t>(green), 136, 255};
}

void verify_source(std::span<const uint32_t> words, uint32_t state) {
  require(words.size() == word_count(state), "Mixed source readback size mismatch");
  for (uint32_t i = 0; i < words.size(); ++i) {
    const float expected = float(expected_numerator(i, state)) / 256.0f;
    if (words[i] != std::bit_cast<uint32_t>(expected)) {
      throw std::runtime_error("Mixed source mismatch state=" + std::string(kStateNames[state]) +
                               " word=" + std::to_string(i) + " actual=" + std::to_string(words[i]) +
                               " expected=" + std::to_string(std::bit_cast<uint32_t>(expected)));
    }
  }
}

void verify_pixels(std::span<const uint8_t> bytes, uint32_t state, bool shadow) {
  require(bytes.size() == kReadbackBytes, "Mixed pixel readback size mismatch");
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      Pixel actual{};
      std::memcpy(actual.data(), bytes.data() + y * kRowBytes + x * 4, actual.size());
      const auto expected = expected_pixel(x, y, state, shadow);
      if (actual != expected) {
        std::ostringstream message;
        message << "Mixed pixel mismatch state=" << kStateNames[state]
                << " variant=" << (shadow ? "shadow" : "normal") << " x=" << x << " y=" << y;
        for (size_t channel = 0; channel < actual.size(); ++channel) {
          message << " channel" << channel << '=' << unsigned(actual[channel])
                  << " expected=" << unsigned(expected[channel]);
        }
        throw std::runtime_error(message.str());
      }
    }
  }
}

// Never turn a validation error into a PASS; the parent's uncaptured/device-lost
// diagnostics remain active as well. Pop even when a CPU assertion throws.
template <typename F>
void validated(const wgpu::Device& device, const std::function<void(wgpu::Future)>& wait,
               std::string_view label, F&& work) {
  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  std::exception_ptr failure;
  try {
    work();
  } catch (...) {
    failure = std::current_exception();
  }
  struct Result {
    bool called = false;
    wgpu::PopErrorScopeStatus status{};
    wgpu::ErrorType type{};
    std::string message;
  };
  auto result = std::make_shared<Result>();
  const auto future = device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
      [result](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
        result->called = true;
        result->status = status;
        result->type = type;
        result->message = std::string(std::string_view(message));
      });
  wait(future);
  require(result->called && result->status == wgpu::PopErrorScopeStatus::Success,
          "Mixed validation scope callback failed");
  require(result->type == wgpu::ErrorType::NoError,
          std::string(label) + ": " + result->message);
  if (failure) std::rethrow_exception(failure);
}

wgpu::Buffer buffer(const wgpu::Device& device, uint64_t size, wgpu::BufferUsage usage,
                    const char* label) {
  wgpu::BufferDescriptor desc{};
  desc.label = label;
  desc.size = size;
  desc.usage = usage;
  auto result = device.CreateBuffer(&desc);
  require(bool(result), "Mixed buffer creation failed");
  return result;
}

wgpu::Texture texture(const wgpu::Device& device, wgpu::TextureFormat format,
                      wgpu::TextureDimension dimension, wgpu::TextureViewDimension binding_dimension,
                      wgpu::Extent3D size, wgpu::TextureUsage usage, const char* label) {
  wgpu::TextureBindingViewDimension binding{};
  binding.textureBindingViewDimension = binding_dimension;
  wgpu::TextureDescriptor desc{};
  desc.nextInChain = &binding;
  desc.label = label;
  desc.dimension = dimension;
  desc.format = format;
  desc.size = size;
  desc.usage = usage;
  auto result = device.CreateTexture(&desc);
  require(bool(result), "Mixed texture creation failed");
  return result;
}

wgpu::TextureView texture_view(const wgpu::Texture& resource, wgpu::TextureViewDimension dimension,
                               uint32_t layers = 1, uint32_t base_layer = 0) {
  wgpu::TextureViewDescriptor desc{};
  desc.dimension = dimension;
  desc.mipLevelCount = 1;
  desc.baseArrayLayer = base_layer;
  desc.arrayLayerCount = layers;
  auto result = resource.CreateView(&desc);
  require(bool(result), "Mixed texture view creation failed");
  return result;
}

wgpu::BindGroup bind_group(const wgpu::Device& device, const wgpu::BindGroupLayout& layout,
                           std::span<const wgpu::BindGroupEntry> entries, const char* label) {
  wgpu::BindGroupDescriptor desc{};
  desc.label = label;
  desc.layout = layout;
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  auto result = device.CreateBindGroup(&desc);
  require(bool(result), "Mixed bind group creation failed");
  return result;
}

constexpr char kVertexShader[] = R"wgsl(
@group(0) @binding(0) var vertex_positions: texture_2d<u32>;
@group(0) @binding(1) var vertex_mirror: texture_2d<u32>;
@group(0) @binding(2) var<storage, read> fragment_source: array<u32>;
@group(1) @binding(0) var<uniform> settings: vec4<u32>;
struct Immediate {
  t0: u32, t1: u32, t2: u32, t3: u32,
  t4: u32, t5: u32, t6: u32, t7: u32,
  t8: u32, t9: u32, t10: u32, t11: u32,
  t12: u32, t13: u32, t14: u32, t15: u32,
};
var<immediate> tags: Immediate;

fn vertex_word(index: u32) -> u32 {
  let word = textureLoad(vertex_mirror, vec2<i32>(i32(index % 4096u), i32(index / 4096u)), 0).x;
  return u32(round(bitcast<f32>(word) * 256.0));
}
struct VertexOutput {
  @builtin(position) position: vec4<f32>,
  // Every vertex computes the same checksum; Compatibility permits either vertex.
  @location(0) @interpolate(flat, either) signature: u32,
};
@vertex fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
  let x = bitcast<f32>(textureLoad(vertex_positions, vec2<i32>(i32(index * 2u), 0), 0).x);
  let y = bitcast<f32>(textureLoad(vertex_positions, vec2<i32>(i32(index * 2u + 1u), 0), 0).x);
  var signature = vertex_word(4094u) + vertex_word(4095u) + vertex_word(4096u) +
                  vertex_word(settings.y) + settings.x;
  signature += tags.t0 + tags.t1 + tags.t2 + tags.t3 + tags.t4 + tags.t5 + tags.t6 + tags.t7;
  return VertexOutput(vec4<f32>(x, y, 0.0, 1.0), signature);
}
)wgsl";

std::string shader_source(bool shadow) {
  std::string source = kVertexShader;
  for (uint32_t i = 0; i < 8; ++i) {
    source += "@group(2) @binding(" + std::to_string(i * 2) + ") var tex" + std::to_string(i) +
              ": texture_2d<f32>;\n@group(2) @binding(" + std::to_string(i * 2 + 1) + ") var samp" +
              std::to_string(i) + ": sampler;\n";
  }
  source += R"wgsl(
@group(2) @binding(16) var probe_cube: texture_cube<f32>;
@group(2) @binding(17) var probe_sampler: sampler;
@group(2) @binding(18) var volume0: texture_3d<f32>;
@group(2) @binding(19) var volume1: texture_3d<f32>;
@group(2) @binding(20) var volume2: texture_3d<f32>;
@group(2) @binding(21) var volume3: texture_3d<f32>;
@group(2) @binding(22) var volume4: texture_3d<f32>;
@group(2) @binding(23) var brdf: texture_2d<f32>;
)wgsl";
  source += shadow ? R"wgsl(
@group(2) @binding(24) var shadow_depth: texture_depth_2d_array;
@group(2) @binding(25) var shadow_sampler: sampler_comparison;
)wgsl" : R"wgsl(
@group(2) @binding(24) var froxel: texture_3d<f32>;
@group(2) @binding(25) var froxel_sampler: sampler;
)wgsl";
  source += R"wgsl(
fn red_byte(value: f32) -> u32 { return u32(round(value * 255.0)); }
@fragment fn fs_main(input: VertexOutput) -> @location(0) vec4<f32> {
  let uv = vec2<f32>(0.5, 0.5);
  let uvw = vec3<f32>(0.5, 0.5, 0.5);
  var blue = 0u;
)wgsl";
  for (uint32_t i = 0; i < 8; ++i) {
    const auto number = std::to_string(i);
    source += "  blue += red_byte(textureSampleLevel(tex" + number + ", samp" + number + ", uv, 0.0).r);\n";
  }
  source += "  blue += red_byte(textureSampleLevel(probe_cube, probe_sampler, vec3<f32>(1.0, 0.25, 0.5), 0.0).r);\n";
  for (uint32_t i = 0; i < 5; ++i) {
    source += "  blue += red_byte(textureSampleLevel(volume" + std::to_string(i) +
              ", probe_sampler, uvw, 0.0).r);\n";
  }
  source += "  blue += red_byte(textureSampleLevel(brdf, probe_sampler, uv, 0.0).r);\n";
  source += shadow ?
      "  blue += u32(round(textureSampleCompareLevel(shadow_depth, shadow_sampler, uv, 1, 0.5) * 16.0));\n" :
      "  blue += red_byte(textureSampleLevel(froxel, froxel_sampler, uvw, 0.0).r);\n";
  source += R"wgsl(
  let red = bitcast<f32>(fragment_source[4094u + u32(input.position.x)]) * 256.0;
  var green = input.signature + settings.z + u32(input.position.y) * settings.w;
  green += tags.t8 + tags.t9 + tags.t10 + tags.t11 + tags.t12 + tags.t13 + tags.t14 + tags.t15;
  return vec4<f32>(red / 255.0, f32(green & 255u) / 255.0, f32(blue) / 255.0, 1.0);
}
)wgsl";
  return source;
}

struct Variant {
  contract::MixedLayouts layouts;
  wgpu::RenderPipeline pipeline;
  wgpu::BindGroup group0;
  wgpu::BindGroup group1;
  wgpu::BindGroup group2;
  std::array<wgpu::Texture, 16> textures;
  std::array<wgpu::TextureView, 16> views;
  wgpu::Sampler filtering;
  wgpu::Sampler last_sampler;

  void init_pipeline(const wgpu::Device& device, bool shadow) {
    layouts = contract::mixed_layout(device, shadow);
    const auto code = shader_source(shadow);
    wgpu::ShaderSourceWGSL source{};
    source.code = code.c_str();
    wgpu::ShaderModuleDescriptor shader_desc{};
    shader_desc.nextInChain = &source;
    auto shader = device.CreateShaderModule(&shader_desc);
    wgpu::ColorTargetState target{};
    target.format = wgpu::TextureFormat::RGBA8Unorm;
    target.writeMask = wgpu::ColorWriteMask::All;
    wgpu::FragmentState fragment{};
    fragment.module = shader;
    fragment.entryPoint = "fs_main";
    fragment.targetCount = 1;
    fragment.targets = &target;
    wgpu::RenderPipelineDescriptor render{};
    render.label = shadow ? "Mixed shadow GX-shaped pipeline" : "Mixed normal GX-shaped pipeline";
    render.layout = layouts.pipeline;
    render.vertex.module = shader;
    render.vertex.entryPoint = "vs_main";
    render.fragment = &fragment;
    render.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    render.primitive.frontFace = wgpu::FrontFace::CCW;
    render.primitive.cullMode = wgpu::CullMode::None;
    render.multisample.count = 1;
    pipeline = device.CreateRenderPipeline(&render);
    require(bool(shader) && bool(pipeline), "Mixed shader/pipeline creation failed");
  }

  void init(const wgpu::Device& device, const wgpu::Queue& queue, const wgpu::Buffer& uniforms, bool shadow) {
    init_pipeline(device, shadow);
    wgpu::BindGroupEntry uniform_entry{};
    uniform_entry.binding = 0;
    uniform_entry.buffer = uniforms;
    uniform_entry.size = sizeof(Uniform);
    group1 = bind_group(device, layouts.groups[1], std::span(&uniform_entry, 1), "Mixed dynamic uniform");

    wgpu::SamplerDescriptor sampler{};
    sampler.label = "Mixed filtering sampler";
    sampler.addressModeU = wgpu::AddressMode::ClampToEdge;
    sampler.addressModeV = wgpu::AddressMode::ClampToEdge;
    sampler.addressModeW = wgpu::AddressMode::ClampToEdge;
    sampler.magFilter = wgpu::FilterMode::Linear;
    sampler.minFilter = wgpu::FilterMode::Linear;
    sampler.mipmapFilter = wgpu::MipmapFilterMode::Nearest;
    filtering = device.CreateSampler(&sampler);
    if (shadow) {
      sampler.label = "Mixed LessEqual comparison sampler";
      sampler.compare = wgpu::CompareFunction::LessEqual;
      last_sampler = device.CreateSampler(&sampler);
    } else last_sampler = filtering;
    require(bool(filtering) && bool(last_sampler), "Mixed sampler creation failed");

    for (uint32_t i = 0; i < textures.size(); ++i) {
      const bool depth = shadow && i == 15;
      const auto dimension = (i >= 9 && i <= 13) || (!shadow && i == 15)
                                 ? wgpu::TextureDimension::e3D : wgpu::TextureDimension::e2D;
      const auto view_dimension = depth ? wgpu::TextureViewDimension::e2DArray :
                                  i == 8 ? wgpu::TextureViewDimension::Cube :
                                  dimension == wgpu::TextureDimension::e3D ? wgpu::TextureViewDimension::e3D :
                                  wgpu::TextureViewDimension::e2D;
      const uint32_t slices = depth ? 2 : i == 8 ? 6 : 1;
      textures[i] = texture(device, depth ? wgpu::TextureFormat::Depth32Float : wgpu::TextureFormat::RGBA8Unorm,
                            dimension, view_dimension, {1, 1, slices}, wgpu::TextureUsage::TextureBinding |
                            (depth ? wgpu::TextureUsage::RenderAttachment : wgpu::TextureUsage::CopyDst),
                            "Mixed original group2 texture");
      views[i] = texture_view(textures[i], view_dimension, slices);
      if (depth) {
        auto encoder = device.CreateCommandEncoder();
        for (uint32_t layer = 0; layer < slices; ++layer) {
          wgpu::RenderPassDepthStencilAttachment attachment{};
          attachment.view = texture_view(textures[i], wgpu::TextureViewDimension::e2D, 1, layer);
          attachment.depthLoadOp = wgpu::LoadOp::Clear;
          attachment.depthStoreOp = wgpu::StoreOp::Store;
          attachment.depthClearValue = 0.75f;
          wgpu::RenderPassDescriptor pass_desc{};
          pass_desc.depthStencilAttachment = &attachment;
          auto pass = encoder.BeginRenderPass(&pass_desc);
          pass.End();
        }
        auto command = encoder.Finish();
        queue.Submit(1, &command);
      } else {
        std::array<uint8_t, 24> data{};
        for (uint32_t layer = 0; layer < slices; ++layer) {
          data[layer * 4] = static_cast<uint8_t>(i + 1);
          data[layer * 4 + 3] = 255;
        }
        wgpu::TexelCopyTextureInfo to{};
        to.texture = textures[i];
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = 4;
        layout.rowsPerImage = 1;
        const wgpu::Extent3D extent{1, 1, slices};
        queue.WriteTexture(&to, data.data(), size_t(slices) * 4, &layout, &extent);
      }
    }
    std::array<wgpu::BindGroupEntry, 26> entries{};
    for (uint32_t i = 0; i < entries.size(); ++i) entries[i].binding = i;
    for (uint32_t i = 0; i < 8; ++i) {
      entries[i * 2].textureView = views[i];
      entries[i * 2 + 1].sampler = filtering;
    }
    entries[16].textureView = views[8];
    entries[17].sampler = filtering;
    for (uint32_t i = 0; i < 5; ++i) entries[18 + i].textureView = views[9 + i];
    entries[23].textureView = views[14];
    entries[24].textureView = views[15];
    entries[25].sampler = last_sampler;
    group2 = bind_group(device, layouts.groups[2], entries, "Mixed full original group2 (no lightmap)");
  }

  void bind_source(const wgpu::Device& device, const wgpu::TextureView& positions,
                   const wgpu::TextureView& mirror, const wgpu::Buffer& source, uint64_t size) {
    std::array<wgpu::BindGroupEntry, 3> entries{};
    entries[0].binding = 0;
    entries[0].textureView = positions;
    entries[1].binding = 1;
    entries[1].textureView = mirror;
    entries[2].binding = 2;
    entries[2].buffer = source;
    entries[2].size = size;
    group0 = bind_group(device, layouts.groups[0], entries, "Mixed vertex textures / fragment SSBO");
  }
};

void copy_mirror(const wgpu::CommandEncoder& encoder, const wgpu::Buffer& source,
                 const wgpu::Texture& mirror, uint32_t count) {
  require(count == kOldWords || count == kNewWords, "Mixed mirror copy outside bounded sizes");
  wgpu::TexelCopyBufferInfo from{};
  from.buffer = source;
  from.layout.bytesPerRow = kFetchWidth * sizeof(uint32_t);
  from.layout.rowsPerImage = count / kFetchWidth;
  wgpu::TexelCopyTextureInfo to{};
  to.texture = mirror;
  const wgpu::Extent3D extent{kFetchWidth, count / kFetchWidth, 1};
  encoder.CopyBufferToTexture(&from, &to, &extent);
}

std::vector<uint8_t> mapped_bytes(const wgpu::Buffer& resource, size_t size,
                                 const std::function<void(wgpu::Future)>& wait) {
  struct Result { bool called = false; bool ok = false; std::string message; };
  auto result = std::make_shared<Result>();
  const auto future = resource.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
      [result](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        result->called = true;
        result->ok = status == wgpu::MapAsyncStatus::Success;
        result->message = std::string(std::string_view(message));
      });
  wait(future);
  require(result->called && result->ok, "Mixed map callback failed: " + result->message);
  const auto* bytes = static_cast<const uint8_t*>(resource.GetConstMappedRange(0, size));
  if (!bytes) {
    resource.Unmap();
    throw std::runtime_error("Mixed mapped bytes unavailable");
  }
  std::vector<uint8_t> copy(size);
  std::memcpy(copy.data(), bytes, size);
  resource.Unmap();
  return copy;
}

template <typename F>
void must_reject(F&& work) {
  bool rejected = false;
  try { work(); } catch (const std::runtime_error&) { rejected = true; }
  require(rejected, "Mixed CPU guard did not reject invalid data");
}

} // namespace

void validate_shaders(const wgpu::Device& device, const std::function<void(wgpu::Future)>& wait) {
  require(!device.HasFeature(wgpu::FeatureName::CoreFeaturesAndLimits),
          "Mixed shader regression requires Compatibility validation");
  for (bool shadow : {false, true}) {
    validated(device, wait, shadow ? "Mixed shadow shader validation" : "Mixed normal shader validation", [&] {
      Variant variant;
      variant.init_pipeline(device, shadow);
    });
  }
  std::cout << "PASS mixed shader-validation pipelines=2 (Dawn Null; no GPU execution)\n";
}

void self_test() {
  require(kDraws == 8 && uint64_t(kDraws) * kWidth * kHeight == 32768, "Mixed bounded coverage");
  require(expected_numerator(4094, 0) == 199 && expected_numerator(4095, 0) == 16 &&
              expected_numerator(4096, 0) == 33 && expected_numerator(8191, 0) == 48,
          "Mixed independent boundary golden values");
  require(expected_numerator(4094, 1) == 28 && expected_numerator(4095, 1) == 45 &&
              expected_numerator(4096, 1) == 62 && expected_numerator(8191, 2) == 77 &&
              expected_numerator(8192, 2) == 0 && expected_numerator(12288, 3) == 155,
          "Mixed update/preserve/growth golden values");
  require(expected_pixel(0, 0, 0, false) == Pixel{199, 10, 136, 255} &&
              expected_pixel(0, 0, 0, true) == Pixel{199, 61, 136, 255} &&
              expected_pixel(0, 0, 1, false) == Pixel{28, 35, 136, 255} &&
              expected_pixel(0, 0, 2, false) == Pixel{28, 144, 136, 255} &&
              expected_pixel(0, 0, 3, false) == Pixel{57, 162, 136, 255},
          "Mixed independent pixel golden values");
  auto words = upload_words(kOldWords, 0);
  verify_source(words, 0);
  words = upload_words(kOldWords, 1);
  verify_source(words, 1);
  words.resize(kNewWords, 0);
  verify_source(words, 2);
  words = upload_words(kNewWords, 2);
  verify_source(words, 3);
  words[12288] ^= 1;
  must_reject([&] { verify_source(words, 3); });
  words = upload_words(kOldWords, 1);
  words.resize(kNewWords, 0);
  words[8191] ^= 1;
  must_reject([&] { verify_source(words, 2); });
  for (uint32_t state = 0; state < kStates; ++state) {
    for (bool shadow : {false, true}) {
      const auto values = uniform_values(state, shadow);
      require(values[1] < word_count(state), "Mixed probe index in bounds");
      const auto tags = immediates(state * 2 + uint32_t(shadow));
      uint32_t sum = 0;
      for (auto tag : tags) sum += tag;
      require(sum == 440 + (state * 2 + uint32_t(shadow)) * 304, "Mixed all-64-byte immediate tag");
      std::vector<uint8_t> pixels(static_cast<size_t>(kReadbackBytes));
      for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
          const auto pixel = expected_pixel(x, y, state, shadow);
          std::memcpy(pixels.data() + y * kRowBytes + x * 4, pixel.data(), pixel.size());
        }
      }
      verify_pixels(pixels, state, shadow);
      for (uint32_t channel = 0; channel < 4; ++channel) {
        pixels[kReadbackBytes - 4 + channel] ^= 1;
        must_reject([&] { verify_pixels(pixels, state, shadow); });
        pixels[kReadbackBytes - 4 + channel] ^= 1;
      }
    }
  }
  must_reject([] { (void)expected_pixel(kWidth, 0, 0, false); });
  must_reject([] { (void)expected_numerator(kOldWords, 0); });
  must_reject([] { (void)word_count(kStates); });
  must_reject([] { (void)immediates(kDraws); });
  must_reject([] { (void)upload_words(kOldWords, 3); });
  std::cout << "PASS mixed CPU pattern/guards boundary=4095/4096 epochs=0/1/2 preservation/growth checked\n";
}

void run(const wgpu::Device& device, const wgpu::Queue& queue,
         const std::function<void(wgpu::Future)>& wait, const std::function<void()>& completed) {
  require(bool(device) && bool(queue) && bool(wait) && bool(completed), "Mixed run requires live device/queue/callbacks");
  wgpu::Limits limits{};
  require(device.GetLimits(&limits) == wgpu::Status::Success, "Mixed device limits query failed");
  const uint32_t alignment = limits.minUniformBufferOffsetAlignment;
  require(alignment != 0 && alignment != wgpu::kLimitU32Undefined && (alignment & (alignment - 1)) == 0,
          "Mixed uniform offset alignment unavailable");
  const uint64_t stride = ((sizeof(Uniform) + uint64_t(alignment) - 1) / alignment) * alignment;
  require(stride * kDraws <= std::numeric_limits<uint32_t>::max() &&
              stride * (kDraws + 1) <= limits.maxBufferSize,
          "Mixed dynamic uniform offsets exceed buffer/offset bounds");

  wgpu::Texture positions;
  wgpu::TextureView positions_view;
  wgpu::Texture output;
  wgpu::TextureView output_view;
  wgpu::Buffer uniforms;
  std::array<Variant, 2> variants{};
  validated(device, wait, "Mixed setup", [&] {
    positions = texture(device, wgpu::TextureFormat::R32Uint, wgpu::TextureDimension::e2D,
                        wgpu::TextureViewDimension::e2D, {kFetchWidth, 1, 1},
                        wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst, "Mixed vertex positions");
    positions_view = texture_view(positions, wgpu::TextureViewDimension::e2D);
    std::array<uint32_t, kFetchWidth> data{};
    constexpr std::array<float, 6> triangle{-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
    for (size_t i = 0; i < triangle.size(); ++i) data[i] = std::bit_cast<uint32_t>(triangle[i]);
    wgpu::TexelCopyTextureInfo to{};
    to.texture = positions;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = sizeof(data);
    layout.rowsPerImage = 1;
    const wgpu::Extent3D extent{kFetchWidth, 1, 1};
    queue.WriteTexture(&to, data.data(), sizeof(data), &layout, &extent);
    output = texture(device, wgpu::TextureFormat::RGBA8Unorm, wgpu::TextureDimension::e2D,
                     wgpu::TextureViewDimension::e2D, {kWidth, kHeight, 1},
                     wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc, "Mixed readback target");
    output_view = texture_view(output, wgpu::TextureViewDimension::e2D);
    // Slot zero stays zero: accidentally omitting the nonzero dynamic offset must fail.
    uniforms = buffer(device, stride * (kDraws + 1), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                      "Mixed aligned dynamic uniform slots");
    for (uint32_t variant = 0; variant < variants.size(); ++variant) {
      variants[variant].init(device, queue, uniforms, variant != 0);
    }
    completed();
  });

  wgpu::Buffer source;
  wgpu::Texture mirror;
  wgpu::TextureView mirror_view;
  uint32_t pixel_checks = 0;
  for (uint32_t state = 0; state < kStates; ++state) {
    const uint32_t count = word_count(state);
    const uint64_t bytes = uint64_t(count) * sizeof(uint32_t);
    validated(device, wait, kStateNames[state], [&] {
      auto encoder = device.CreateCommandEncoder();
      wgpu::Buffer old_source;
      wgpu::Buffer staging;
      if (state == 0 || state == 2) {
        old_source = source;
        source = buffer(device, bytes,
                        wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc,
                        "Mixed source SSBO / mirror copy source");
        mirror = texture(device, wgpu::TextureFormat::R32Uint, wgpu::TextureDimension::e2D,
                         wgpu::TextureViewDimension::e2D, {kFetchWidth, count / kFetchWidth, 1},
                         wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst, "Mixed vertex storage mirror");
        mirror_view = texture_view(mirror, wgpu::TextureViewDimension::e2D);
        for (auto& variant : variants) variant.bind_source(device, positions_view, mirror_view, source, bytes);
      }
      if (state == 2) {
        // This state has NO CPU source upload: retain epoch 1 through the GPU copy.
        encoder.CopyBufferToBuffer(old_source, 0, source, 0, uint64_t(kOldWords) * sizeof(uint32_t));
      } else {
        const uint32_t epoch = state == 3 ? 2 : state;
        const auto words = upload_words(count, epoch);
        staging = buffer(device, bytes, wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst,
                         "Mixed staging upload (one source, two shader consumers)");
        queue.WriteBuffer(staging, 0, words.data(), bytes);
        encoder.CopyBufferToBuffer(staging, 0, source, 0, bytes);
      }
      copy_mirror(encoder, source, mirror, count);
      auto command = encoder.Finish();
      queue.Submit(1, &command);
      // Local old_source/staging handles may drop here; encoded work retains them.
      completed();
    });

    for (uint32_t variant = 0; variant < variants.size(); ++variant) {
      const bool shadow = variant != 0;
      validated(device, wait, shadow ? "Mixed shadow draw/readback" : "Mixed normal draw/readback", [&] {
        const uint32_t draw = state * 2 + variant;
        const auto values = uniform_values(state, shadow);
        const auto tags = immediates(draw);
        const uint32_t offset = static_cast<uint32_t>(stride * (draw + 1));
        queue.WriteBuffer(uniforms, offset, values.data(), sizeof(values));
        auto readback = buffer(device, kReadbackBytes, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                               "Mixed RGBA8 readback");
        auto source_readback = buffer(device, bytes, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                      "Mixed source preservation readback");
        auto encoder = device.CreateCommandEncoder();
        wgpu::RenderPassColorAttachment color{};
        color.view = output_view;
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = {0, 0, 0, 0};
        wgpu::RenderPassDescriptor pass_desc{};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        auto pass = encoder.BeginRenderPass(&pass_desc);
        pass.SetPipeline(variants[variant].pipeline);
        pass.SetBindGroup(0, variants[variant].group0);
        pass.SetBindGroup(1, variants[variant].group1, 1, &offset);
        pass.SetBindGroup(2, variants[variant].group2);
        pass.SetImmediates(0, tags.data(), sizeof(tags));
        pass.Draw(3);
        pass.End();
        wgpu::TexelCopyTextureInfo from{};
        from.texture = output;
        wgpu::TexelCopyBufferInfo to{};
        to.buffer = readback;
        to.layout.bytesPerRow = kRowBytes;
        to.layout.rowsPerImage = kHeight;
        const wgpu::Extent3D extent{kWidth, kHeight, 1};
        encoder.CopyTextureToBuffer(&from, &to, &extent);
        encoder.CopyBufferToBuffer(source, 0, source_readback, 0, bytes);
        auto command = encoder.Finish();
        queue.Submit(1, &command);
        completed();
        const auto pixels = mapped_bytes(readback, static_cast<size_t>(kReadbackBytes), wait);
        verify_pixels(pixels, state, shadow);
        const auto source_bytes = mapped_bytes(source_readback, static_cast<size_t>(bytes), wait);
        std::vector<uint32_t> source_words(count);
        std::memcpy(source_words.data(), source_bytes.data(), static_cast<size_t>(bytes));
        verify_source(source_words, state);
      });
      pixel_checks += kWidth * kHeight;
      std::cout << "PASS mixed case=" << kStateNames[state] << " variant=" << (shadow ? "shadow" : "normal")
                << " pixels=" << kWidth * kHeight << " sourceWords=" << count << " probe=" << probe_index(state)
                << " source=" << (state == 2 ? "GPU-preserved-epoch1-no-upload" : state == 3 ? "epoch2" : state == 1 ? "epoch1" : "epoch0")
                << "\n";
    }
  }
  require(pixel_checks == 32768, "Mixed pixel coverage incomplete");
  std::cout << "PASS mixed workload draws=8 pixels=32768 updates/growth checked\n";
}

} // namespace h700_probe::mixed
