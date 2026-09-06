#pragma once

#include "types.h"

namespace sdfhook {

/// Writes an uncompressed PNG. Uncompressed because these are dump files a person opens once and a
/// script reads: the encode is on the path of a render, and no viewer cares.
/// @param pixels Rows of 8-bit RGB or RGBA, tightly packed.
void writePng(const char* path, u32 width, u32 height, const u8* pixels, bool hasAlpha);

}  // namespace sdfhook
