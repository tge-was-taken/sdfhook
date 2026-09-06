#include "image.h"

#include <png.h>

#include <csetjmp>
#include <cstdio>
#include <vector>

namespace sdfhook {

void writePng(const char* path, u32 width, u32 height, const u8* pixels, bool hasAlpha) {
    if (!width || !height || !pixels) return;
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(png ? &png : nullptr, info ? &info : nullptr);
        std::fclose(file);
        return;
    }

    png_init_io(png, file);
    png_set_compression_level(png, 0);
    png_set_filter(png, 0, PNG_FILTER_NONE);
    png_set_IHDR(png, info, width, height, 8,
                 hasAlpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    const usize stride = usize(width) * (hasAlpha ? 4u : 3u);
    std::vector<png_bytep> rows(height);
    for (u32 y = 0; y < height; y++) {
        rows[y] = const_cast<png_bytep>(pixels + usize(y) * stride);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}

}  // namespace sdfhook
