#pragma once

#include <webgpu/webgpu_cpp.h>

#include <functional>

namespace h700_probe::mixed {

// CPU-only checks; does not initialize Dawn or execute a GPU workload.
void self_test();
void run(const wgpu::Device&, const wgpu::Queue&,
         const std::function<void(wgpu::Future)>& wait,
         const std::function<void()>& completed);

} // namespace h700_probe::mixed
