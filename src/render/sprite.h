#pragma once

#include <vector>

#include "../types.h"
#include "frame.h"

namespace sdfhook {

// A vertex ready for the GPU, in the guest's draw space.
struct ScreenVertex {
    f32 x, y;
    f32 u, v;
    Rgba colour;
};

// The guest's draw space, which the GS magnifies 2x vertically to the 512x448 it presents.
constexpr f32 kGuestWidth = 512.0f;
constexpr f32 kGuestHeight = 224.0f;

// The GS XYOFFSET the draw envs set, in whole pixels: the draw space centred on 2048.
constexpr f32 kOffsetX = 1792.0f;
constexpr f32 kOffsetY = 1936.0f;

// Two triangles per sprite, with UVs normalised against the texture it draws with.
void toTriangles(const DrawSprite& sprite, f32 texWidth, f32 texHeight, std::vector<ScreenVertex>& out);

}  // namespace sdfhook
