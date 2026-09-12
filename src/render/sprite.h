#pragma once

#include <vector>

#include "../types.h"
#include "frame.h"

namespace sdfhook {

struct ScreenVertex {
    f32 x, y;
    f32 u, v;
    Rgba color;
    f32 z = 0.0f;
};

// The GS magnifies this 2x vertically to the 512x448 it presents.
constexpr f32 kGuestWidth = 512.0f;
constexpr f32 kGuestHeight = 224.0f;

// The GS XYOFFSET the draw envs set, in whole pixels: the draw space centred on 2048.
constexpr f32 kOffsetX = 1792.0f;
constexpr f32 kOffsetY = 1936.0f;

// UVs come out normalised, which the guest's are not.
void triangulate(const DrawSprite& sprite, f32 texWidth, f32 texHeight, std::vector<ScreenVertex>& out);

}  // namespace sdfhook
