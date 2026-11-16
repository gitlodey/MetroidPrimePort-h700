#pragma once

#include <cstddef>
#include <webgpu/webgpu_cpp.h>

// Render-thread hooks of aurora/vis_query.hpp, beside overdraw's.
namespace aurora::gfx::vis_query::detail {
// The query set for a pass's descriptor (null before the first query, without a free slot, or
// when overdraw claimed the pass).
wgpu::QuerySet pass_begin(bool claimed);
void pass_end();
void frame_end(const wgpu::CommandEncoder& encoder);
void after_submit();
void poll_maps(); // owner thread, no new maps/Submit
size_t pending_maps() noexcept; // owner thread, or after producer/worker quiescence
bool shutdown(); // after producer/worker quiescence; refuses unsettled callbacks
} // namespace aurora::gfx::vis_query::detail
