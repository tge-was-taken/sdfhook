#include "texture.h"

#include "image.h"


#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>

#include "log.h"
#include "json.h"
#include "unsupported.h"

namespace sdfhook {

namespace {

LogChannel log("tex");

constexpr u32 kMaxDimension = 1024;



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


}  // namespace

// Keyed by handle because that is the pointer DrwSprite2D is later handed; texId is not unique.
void TextureCache::add(const Asset& aAsset, TexHandle handle, Frame* frame) {
    if (!handle.addr) return;
    if (mUploaded.count(handle)) return;

    const u32 width = aAsset.width;
    const u32 height = aAsset.height;
    if (!width || !height || width > kMaxDimension || height > kMaxDimension) {
        mFailed++;
        return;
    }

    const bool fourBit = aAsset.pixelFormat == sdfPsmt4;
    const bool paletted = fourBit || aAsset.pixelFormat == sdfPsmt8;
    if (!paletted && aAsset.pixelFormat != sdfPsmct32) {
        reportUnsupported("TMX pixelFormat", aAsset.pixelFormat);
        mFailed++;
        return;
    }
    const bool clut16 = aAsset.clutFormat == sdfPsmct16 || aAsset.clutFormat == sdfPsmct16s;
    const u32 clutEntries = fourBit ? 16 : 256;
    const u32 clutBytes = paletted ? aAsset.numOfCLUT * clutEntries * (clut16 ? 2 : 4) : 0;

    const u32 dataStart = aAsset.dataStart;
    const usize texels = usize(width) * height;
    const usize pixelBytes = fourBit ? texels / 2 : (paletted ? texels : texels * 4);

    const u8* pixels = u8Ptr_t{dataStart + clutBytes}.slice(u32(pixelBytes));
    const u8* clutSource =
        clutBytes ? u8Ptr_t{dataStart}.slice(clutBytes) : nullptr;
    if (!pixels || (paletted && !clutSource)) {
        mFailed++;
        return;
    }

    log.debug("tex {:08X} psm {:#x} clutFormat {:#x} numOfCLUT {} {}x{}", handle.addr,
              aAsset.pixelFormat, aAsset.clutFormat, aAsset.numOfCLUT, width, height);

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
    if (!mDumpDir.empty()) {
        const std::string name = std::format("{}/tex_{:08X}_{}x{}_psm{:02X}.png", mDumpDir,
                                             handle.addr, width, height, aAsset.pixelFormat);
        writePng(name.c_str(), width, height, upload.rgba.data(), true);
    }
    if (frame) frame->textureUploads.push_back(std::move(upload));
}

void TextureCache::onDuplicate(TexHandle source, TexHandle copy) {
    if (!source || !copy) return;
    const TexHandle resolved = getHandle(source, nullptr);
    if (resolved) mAlias[TexHandle{copy}] = resolved;
}


// A duplicate never reaches sdfTexSetEx, so an unknown handle is followed to its origin.
// A bit no EE pointer carries, so the sheet cannot collide with a real texture handle.
constexpr u32 kSheetHandleBit = 0x80000000;

void TextureCache::onFontClut(u32 dataAddr) {
    if (!mFontClut.empty() || !dataAddr) return;
    for (u32 i = 0; i < 16; i++) {
        mFontClut.push_back(Rgba::fromPacked(u32Ptr_t{dataAddr + i * 4}.valueOr()));
    }
}

// frSetFontClut builds six palettes of sixteen entries from this table, each a plain RGBA word.
void TextureCache::loadFontClut(u32 tableAddr) {
    if (!mFontClut.empty() || !tableAddr) return;
    for (u32 i = 0; i < 16; i++) {
        mFontClut.push_back(Rgba::fromPacked(u32Ptr_t{tableAddr + i * 4}.valueOr()));
    }
}

void TextureCache::onGlyph(u32 dstX, u32 dstY, u32 width, u32 height,
                           u32 dataAddr) {
    if (!width || !height) return;
    const usize bytes = usize(width) * height / 2;
    const u8* pixels = u8Ptr_t{dataAddr}.slice(u32(bytes));
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
        const Rgba color = mFontClut[sheet.index[i] & 0xF];
        upload.rgba[i * 4 + 0] = color.r;
        upload.rgba[i * 4 + 1] = color.g;
        upload.rgba[i * 4 + 2] = color.b;
        upload.rgba[i * 4 + 3] = color.a;
    }
    mUploaded.insert(handle);
    frame->textureUploads.push_back(std::move(upload));
    return handle;
}

void TextureCache::onFrameBuffer(sdfGsMemSlicePtr_t aSlice) {
    if (!aSlice) return;
    const GsRegion region{aSlice->addr, u32(aSlice->size)};
    for (const GsRegion& known : mFrameBuffers) {
        if (known.addr == region.addr && known.size == region.size) return;
    }
    log.debug("frame buffer at {:#x} size {:#x} ({}x{} psm {:#x})", region.addr, region.size,
              aSlice->w, aSlice->h, aSlice->pixelFormat);
    mFrameBuffers.push_back(region);
}

void TextureCache::onFree(TexHandle handle) {
    if (!handle.addr) return;

    const bool known = mUploaded.erase(handle) != 0;
    mWanted.erase(handle);
    // A copy outlives nothing: both the aliases of this handle and the ones pointing at it go.
    for (auto it = mAlias.begin(); it != mAlias.end();) {
        it = (it->first == handle || it->second == handle) ? mAlias.erase(it) : std::next(it);
    }
    if (known) mFreed++;
}

TexHandle TextureCache::getHandleForTex0(TexHandle key, u64 tex0) {
    if (!key.addr || !tex0 || !mConfig->recoverTexturesFromGs) return {};
    if (mUploaded.count(key)) return key;
    const auto alias = mAlias.find(key);
    if (alias != mAlias.end()) return alias->second;
    // The read is not safe from inside a hook, so it waits for the frame boundary as handles do.
    mWantedTex0.emplace(key, tex0);
    return {};
}

TexHandle TextureCache::getHandle(TexHandle wanted, Frame*) {
    if (!wanted.addr) return {};
    if (mUploaded.count(wanted)) return wanted;
    const auto alias = mAlias.find(wanted);
    if (alias != mAlias.end()) return alias->second;

    TexHandle origin = wanted;
    for (int depth = 0; depth < 4; depth++) {
        const TexHandle handle{origin.addr};
        if (!handle || !handle->origin) break;
        origin = handle->origin;
        if (mUploaded.count(origin)) {
            mAlias.emplace(wanted, origin);
            return origin;
        }
    }

    // Nothing registered it in this window, but the GS still holds the pixels. The read is not safe
    // from here, so the handle waits for the frame boundary and the draw goes untextured once.
    mWanted.insert(wanted);
    return {};
}

void TextureCache::wantFontSheet(u32 addr, u32 bufWidth, u32 width, u32 height) {
    if (!mFontSheet.index.empty() || !addr || !width || !height) return;
    mWantedSheet = {addr, bufWidth, width, height};
}

bool TextureCache::recoverFontSheet() {
    const FontSheetWanted want = mWantedSheet;
    mWantedSheet = {};
    mRecoveryAttempts++;

    std::vector<u32> texels(usize(want.width) * want.height);
    if (pcsx2hook_read_gs_pixels(mRuntime, want.addr, want.bufWidth, sdfPsmt4, want.width,
                                 want.height, texels.data(), texels.size()) != texels.size()) {
        return false;
    }

    mFontSheet.width = want.width;
    mFontSheet.height = want.height;
    mFontSheet.index.resize(texels.size());
    for (usize i = 0; i < texels.size(); i++) mFontSheet.index[i] = u8(texels[i] & 0xF);
    mFontSheet.dirty = true;
    mRecovered++;
    log.info("glyph sheet recovered from GS memory, {}x{}", want.width, want.height);
    return true;
}

void TextureCache::recoverQueued() {
    if (!mConfig->recoverTexturesFromGs || !mRuntime) return;
    if (mWantedSheet.addr && mFontSheet.index.empty()) recoverFontSheet();
    if (mWanted.empty()) return;

    // A material that named a TEX0 with no handle: the register is in hand, so no packet is read.
    for (auto it = mWantedTex0.begin(); it != mWantedTex0.end();) {
        if (recoverTex0(it->first, it->second)) {
            mUploaded.insert(it->first);
            mRecovered++;
        } else {
            mAlias.emplace(it->first, TexHandle{});
        }
        mRecoveryAttempts++;
        it = mWantedTex0.erase(it);
    }

    for (auto it = mWanted.begin(); it != mWanted.end();) {
        const TexHandle handle = *it;
        if (recoverFromGs(handle)) {
            mUploaded.insert(handle);
            mRecovered++;
        } else {
            mAlias.emplace(handle, TexHandle{});
        }
        mRecoveryAttempts++;
        it = mWanted.erase(it);
    }
}

void TextureCache::drainRecovered(Frame& frame) {
    for (TextureUpload& upload : mRecoveredUploads) {
        frame.textureUploads.push_back(std::move(upload));
    }
    mRecoveredUploads.clear();
}

bool TextureCache::recoverFromGs(TexHandle handle) {
    const TexHandle entry{handle.addr};
    if (!entry || !entry->packet) {
        if (mConfig->dumpRecover) {
            JsonObject row;
            row.addString("dump", "recover").addHexDigits("handle", handle.addr, 8);
            log.dump(row.addString("why", "no upload packet").take());
        }
        return false;
    }

    // sdfTexGetTex0Reg reads the register out of the packet the texture was uploaded with.
    return recoverTex0(handle, u64Ptr_t{entry->packet.addr + kTex0InPacket}.valueOr());
}

bool TextureCache::recoverTex0(TexHandle handle, u64 reg) {
    const auto note = [&](const char* why, u64 tex0) {
        if (!mConfig->dumpRecover) return;
        JsonObject row;
        row.addString("dump", "recover").addHexDigits("handle", handle.addr, 8).addString("why", why);
        if (tex0) row.addHexDigits("tex0", tex0, 16);
        log.dump(row.take());
    };

    if (!reg) {
        note("packet has no TEX0", 0);
        return false;
    }
    const GsTex0 tex0 = GsTex0::fromReg(reg);
    log.debug("recover {:08X} tbp0 {:#x} psm {:#x} cbp {:#x} cpsm {:#x} csa {}", handle.addr,
              tex0.tbp0, tex0.psm, tex0.cbp, tex0.cpsm, tex0.csa);
    const u32 width = 1u << tex0.tw;
    const u32 height = 1u << tex0.th;
    note("read", reg);

    std::vector<u32> texels(usize(width) * height);
    if (pcsx2hook_read_gs_pixels(mRuntime, tex0.tbp0, tex0.tbw, tex0.psm, width, height,
                                 texels.data(), texels.size()) != texels.size()) {
        note("no pixels", reg);
        return false;
    }

    // Paletted formats come back as indices, so the palette is read and unswizzled here rather than
    // through the renderer's own CLUT cache, which the emulated draws after this still need.
    u8 clut[256][4] = {};
    const u32 entries = tex0.isFourBit() ? 16u : 256u;
    if (tex0.isPaletted()) {
        std::vector<u32> palette(entries);
        const u32 clutWidth = tex0.isFourBit() ? 8 : 16;
        if (pcsx2hook_read_gs_pixels(mRuntime, tex0.cbp, 1, tex0.cpsm, clutWidth,
                                     entries / clutWidth, palette.data(),
                                     palette.size()) != palette.size()) {
            note("no clut", reg);
            return false;
        }
        readClut(reinterpret_cast<const u8*>(palette.data()), entries,
                 tex0.cpsm == sdfPsmct16 || tex0.cpsm == sdfPsmct16s, clut);
        if (!tex0.isFourBit()) unswizzleClut(clut);
    }

    TextureUpload upload;
    upload.handle = handle;
    upload.width = width;
    upload.height = height;
    upload.rgba.resize(texels.size() * 4);
    for (usize i = 0; i < texels.size(); i++) {
        u8* out = &upload.rgba[i * 4];
        if (tex0.isPaletted()) {
            std::memcpy(out, clut[texels[i] & (entries - 1)], 4);
            continue;
        }
        const u32 value = texels[i];
        out[0] = u8(value & 0xFF);
        out[1] = u8((value >> 8) & 0xFF);
        out[2] = u8((value >> 16) & 0xFF);
        out[3] = u8((value >> 24) & 0xFF);
    }

    note("done", reg);
    if (!mDumpDir.empty()) {
        const std::string name = std::format("{}/gs_{:08X}_{}x{}_psm{:02X}.png", mDumpDir,
                                             handle.addr, width, height, tex0.psm);
        writePng(name.c_str(), width, height, upload.rgba.data(), true);
    }
    mRecoveredUploads.push_back(std::move(upload));
    return true;
}

}  // namespace sdfhook
