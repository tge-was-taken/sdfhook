#include "texture.h"

#include "unsupported.h"


#include <png.h>

#include <cstdio>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

namespace sdfhook {
namespace {

constexpr u32 kMaxDimension = 1024;

void writePng(const char* path, u32 width, u32 height, const u8* rgba) {
    if (!width || !height) return;
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
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<png_bytep> rows(height);
    for (u32 y = 0; y < height; y++) {
        rows[y] = const_cast<png_bytep>(rgba + usize(y) * width * 4);
    }
    png_write_image(png, rows.data());
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
}


// Alpha stays in PS2 range (0..0x80); the shader scales it.
void readClut(const u8* source, u32 count, bool clut16, u8 (*out)[4]) {
    for (u32 i = 0; i < count; i++) {
        if (clut16) {
            u16 entry = 0;
            std::memcpy(&entry, source + i * 2, sizeof(entry));
            out[i][0] = u8((entry & 0x1F) << 3);
            out[i][1] = u8(((entry >> 5) & 0x1F) << 3);
            out[i][2] = u8(((entry >> 10) & 0x1F) << 3);
            out[i][3] = (entry & 0x8000) ? 0x80 : 0x00;
            continue;
        }
        u32 entry = 0;
        std::memcpy(&entry, source + i * 4, sizeof(entry));
        out[i][0] = u8(entry & 0xFF);
        out[i][1] = u8((entry >> 8) & 0xFF);
        out[i][2] = u8((entry >> 16) & 0xFF);
        out[i][3] = u8((entry >> 24) & 0xFF);
    }
}

void unswizzleClut(u8 (*clut)[4]) {
    for (u32 i = 0; i < 256; i++) {
        if ((i & 0x18) != 0x10) continue;
        for (u32 c = 0; c < 4; c++) std::swap(clut[i][c], clut[i ^ 0x18][c]);
    }
}

u32 swizzledIndex8(u32 x, u32 y, u32 width) {
    const u32 block = (y & ~0xFu) * width + (x & ~0xFu) * 2;
    const u32 swap = (((y + 2) >> 2) & 1) * 4;
    const u32 row = ((((y & ~3u) >> 1) + (y & 1)) & 7);
    const u32 column = row * width * 2 + ((x + swap) & 7) * 4;
    return block + column + (((y >> 1) & 1) + ((x >> 2) & 2));
}

}  // namespace

// Keyed by handle because that is the pointer DrwSprite2D is later handed; texId is not unique.
void TextureCache::add(const Guest& guest, u32 texFileAddr, u32 handleAddr, Frame* frame) {
    TMX_HEADER_t tmx{};
    const TexHandle handle{handleAddr};
    if (!texFileAddr || !handle || !guest.read(texFileAddr, tmx)) return;
    if (mUploaded.count(handle)) return;

    const u32 width = u32(tmx.width);
    const u32 height = u32(tmx.height);
    if (!width || !height || width > kMaxDimension || height > kMaxDimension) {
        mFailed++;
        return;
    }

    const bool fourBit = tmx.pixelFormat == sdfPsmt4;
    const bool paletted = fourBit || tmx.pixelFormat == sdfPsmt8;
    if (!paletted && tmx.pixelFormat != sdfPsmct32) {
        reportUnsupported("TMX pixelFormat", tmx.pixelFormat);
        mFailed++;
        return;
    }
    const bool clut16 = tmx.CLUTFormat == sdfPsmct16 || tmx.CLUTFormat == sdfPsmct16s;
    const u32 clutEntries = fourBit ? 16 : 256;
    const u32 clutBytes = paletted ? tmx.numOfCLUT * clutEntries * (clut16 ? 2 : 4) : 0;

    const u32 dataStart = texFileAddr + (tmx.compression & 0xF0) + sdfTmxDataGap;
    const usize texels = usize(width) * height;
    const usize pixelBytes = fourBit ? texels / 2 : (paletted ? texels : texels * 4);

    const u8* pixels = static_cast<const u8*>(guest.map(dataStart + clutBytes, u32(pixelBytes)));
    const u8* clutSource =
        clutBytes ? static_cast<const u8*>(guest.map(dataStart, clutBytes)) : nullptr;
    if (!pixels || (paletted && !clutSource)) {
        mFailed++;
        return;
    }

    // Rows, not tiles: the GS swizzles as the transfer lands.
    u8 clut[256][4] = {};
    if (paletted) {
        readClut(clutSource, clutEntries, clut16, clut);
        if (!fourBit) unswizzleClut(clut);
    }

    TextureUpload upload;
    upload.handle = handle;
    upload.width = width;
    upload.height = height;
    upload.rgba.resize(texels * 4);
    for (usize texel = 0; texel < texels; texel++) {
        u8* out = &upload.rgba[texel * 4];
        if (!paletted) {
            std::memcpy(out, pixels + texel * 4, 4);
            continue;
        }
        u32 entry = 0;
        if (fourBit) {
            const u8 pair = pixels[texel / 2];
            entry = (texel & 1) ? (pair >> 4) : (pair & 0xF);
        } else {
            entry = pixels[texel];
        }
        for (u32 c = 0; c < 4; c++) out[c] = clut[entry][c];
    }

    mDecoded++;
    mUploaded.insert(handle);
    if (mDumpDir) {
        char name[80];
        std::snprintf(name, sizeof(name), "/tex_%08X_%ux%u_psm%02X.png", handle.addr, width, height,
                      tmx.pixelFormat);
        writePng((std::string(mDumpDir) + name).c_str(), width, height, upload.rgba.data());
    }
    if (frame) frame->textureUploads.push_back(std::move(upload));
}

void TextureCache::onDuplicate(const Guest& guest, u32 sourceHandle, u32 copyHandle) {
    if (!sourceHandle || !copyHandle) return;
    const TexHandle resolved = getHandle(guest, sourceHandle);
    if (resolved) mAlias[TexHandle{copyHandle}] = resolved;
}


// A duplicate never reaches sdfTexSetEx, so an unknown handle is followed to its origin.
// A bit no EE pointer carries, so the sheet cannot collide with a real texture handle.
constexpr u32 kSheetHandleBit = 0x80000000;

void TextureCache::onFontClut(const Guest& guest, u32 dataAddr) {
    if (!mFontClut.empty() || !dataAddr) return;
    for (u32 i = 0; i < 16; i++) {
        mFontClut.push_back(Rgba::fromPacked(guest.readOr<u32>(dataAddr + i * 4)));
    }
}

// frSetFontClut builds six palettes of sixteen entries from this table, each a plain RGBA word.
void TextureCache::loadFontClut(const Guest& guest, u32 tableAddr) {
    if (!mFontClut.empty() || !tableAddr) return;
    for (u32 i = 0; i < 16; i++) {
        mFontClut.push_back(Rgba::fromPacked(guest.readOr<u32>(tableAddr + i * 4)));
    }
}

void TextureCache::onGlyph(const Guest& guest, u32 dstX, u32 dstY, u32 width, u32 height,
                           u32 dataAddr) {
    if (!width || !height) return;
    const usize bytes = usize(width) * height / 2;
    const u8* pixels = static_cast<const u8*>(guest.map(dataAddr, u32(bytes)));
    if (!pixels) {
        mFailed++;
        return;
    }

    Sheet& sheet = mFontSheet;
    const u32 right = dstX + width;
    const u32 bottom = dstY + height;
    if (right > sheet.width || bottom > sheet.height) {
        Sheet grown;
        grown.width = right > sheet.width ? right : sheet.width;
        grown.height = bottom > sheet.height ? bottom : sheet.height;
        grown.index.assign(usize(grown.width) * grown.height, 0);
        for (u32 y = 0; y < sheet.height; y++) {
            std::copy_n(sheet.index.begin() + std::ptrdiff_t(y) * sheet.width, sheet.width,
                        grown.index.begin() + std::ptrdiff_t(y) * grown.width);
        }
        sheet = std::move(grown);
    }

    for (u32 y = 0; y < height; y++) {
        for (u32 x = 0; x < width; x++) {
            const usize texel = usize(y) * width + x;
            const u8 pair = pixels[texel / 2];
            sheet.index[usize(dstY + y) * sheet.width + dstX + x] =
                (texel & 1) ? (pair >> 4) : (pair & 0xF);
        }
    }
    sheet.dirty = true;
    mGlyphUploads++;
}

TexHandle TextureCache::getFontSheet(u32 sheetWidth, Frame* frame) {
    Sheet& sheet = mFontSheet;
    if (sheet.index.empty()) return {};

    // The engine's own width, so the glyph UVs land where it put them.
    if (sheetWidth > sheet.width) {
        Sheet grown;
        grown.width = sheetWidth;
        grown.height = sheet.height;
        grown.index.assign(usize(grown.width) * grown.height, 0);
        for (u32 y = 0; y < sheet.height; y++) {
            std::copy_n(sheet.index.begin() + std::ptrdiff_t(y) * sheet.width, sheet.width,
                        grown.index.begin() + std::ptrdiff_t(y) * grown.width);
        }
        sheet = std::move(grown);
        sheet.dirty = true;
    }

    const TexHandle handle{kSheetHandleBit};
    if (!sheet.dirty) return handle;
    sheet.dirty = false;
    if (!frame) return handle;

    if (mFontClut.empty()) {
        reportUnsupported("font CLUT unavailable", 0);
        return {};
    }

    TextureUpload upload;
    upload.handle = handle;
    upload.width = sheet.width;
    upload.height = sheet.height;
    upload.rgba.resize(sheet.index.size() * 4);
    for (usize i = 0; i < sheet.index.size(); i++) {
        const Rgba colour = mFontClut[sheet.index[i] & 0xF];
        upload.rgba[i * 4 + 0] = colour.r;
        upload.rgba[i * 4 + 1] = colour.g;
        upload.rgba[i * 4 + 2] = colour.b;
        upload.rgba[i * 4 + 3] = colour.a;
    }
    mUploaded.insert(handle);
    frame->textureUploads.push_back(std::move(upload));
    return handle;
}

TexHandle TextureCache::getHandle(const Guest& guest, u32 handleAddr) {
    const TexHandle wanted{handleAddr};
    if (!wanted) return {};
    if (mUploaded.count(wanted)) return wanted;
    const auto alias = mAlias.find(wanted);
    if (alias != mAlias.end()) return alias->second;

    TexHandle origin = wanted;
    for (int depth = 0; depth < 4; depth++) {
        sdfTexHandle_t handle{};
        if (!guest.read(origin.addr, handle) || !handle.origin) break;
        origin = handle.origin;
        if (mUploaded.count(origin)) {
            mAlias.emplace(wanted, origin);
            return origin;
        }
    }
    mAlias.emplace(wanted, TexHandle{});
    return {};
}

}  // namespace sdfhook
