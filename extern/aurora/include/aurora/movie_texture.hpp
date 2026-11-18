#pragma once

#include <dolphin/gx.h>

namespace aurora::h700 {
// Annotates movie planes before GXLoadTexObj; client transport remains H700 opt-in.
void mark_movie_plane(GXTexObj* object) noexcept;
} // namespace aurora::h700
