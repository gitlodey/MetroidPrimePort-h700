#pragma once

#include <webgpu/webgpu_cpp.h>

#include <functional>

namespace h700_probe::mixed {

// CPU-only checks; does not initialize Dawn or execute a GPU workload.
void self_test();
// The existing Null self-test validates the actual pipelines without draw execution.
void validate_shaders(const wgpu::Device&, const std::function<void(wgpu::Future)>& wait);
void run(const wgpu::Device&, const wgpu::Queue&,
         const std::function<void(wgpu::Future)>& wait,
         const std::function<void()>& completed);

} // namespace h700_probe::mixed
