#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pcsx2hook/pcsx2hook.h"
#include "config.h"
#include "guest_ptr.h"
#include "libsdf.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// Each texture is sent to the render thread once, the first frame it is drawn with.
class TextureCache {
public:
    void attach(pcsx2hook_ctx* runtime, const Config& config) {
        mRuntime = runtime;
        mConfig = &config;
    }

    /// The handle to draw a texture with, empty when nothing was registered for it. One uploaded
    /// before the capture window is queued for recovery out of GS memory, the only copy left by
    /// then, and resolves from the next frame on.
    TexHandle getHandle(TexHandle wanted, Frame* frame = nullptr);

    /// Reads the queued handles out of GS memory. Call only from the runner's frame callback:
    /// the read runs on the GS thread and blocks for it, which is not safe from inside a hook.
    void recoverQueued();

    /// Moves anything recoverQueued read into the frame being built.
    void drainRecovered(Frame& frame);

    /// Links a copy made by sdfTexDup to its source: the copy never reaches sdfTexSetEx, so
    /// nothing is uploaded for it.
    void onDuplicate(TexHandle source, TexHandle copy);

    /// Only callable during a load: afterwards the engine keeps the GS allocation alone.
    void add(TMX_HEADER_PTR_t aTex, TexHandle handle, Frame* frame);

    /// Glyphs are uploaded one at a time into a sheet shared in GS memory, so the sheet is
    /// assembled here from those same uploads and identified by the address it lives at.
    void onGlyph(u32 dstX, u32 dstY, u32 width, u32 height, u32 dataAddr);
    void onFontClut(u32 dataAddr);

    /// The palettes are static data, so they can be read whether or not frSetFontClut ran inside
    /// the capture window.
    void loadFontClut(u32 tableAddr);

    /// The handle to draw the glyph sheet with, sized as the engine sees it and re-uploaded when
    /// glyphs have changed. There is one sheet, so it needs no address to tell it apart.
    TexHandle getFontSheet(u32 sheetWidth, Frame* frame);

    void dumpTo(const std::string& directory) { mDumpDir = directory; }

    u64 getRecovered() const { return mRecovered; }
    u64 getRecoveryAttempts() const { return mRecoveryAttempts; }
    u64 getQueued() const { return mWanted.size(); }
    u64 getDecoded() const { return mDecoded; }
    u64 getFailed() const { return mFailed; }
    u64 getGlyphUploads() const { return mGlyphUploads; }
    bool hasFontClut() const { return !mFontClut.empty(); }

private:
    // A sheet many uploads share, kept as the indices the game wrote so a palette can change.
    struct Sheet {
        u32 width = 0;
        u32 height = 0;
        std::vector<u8> index;
        bool dirty = false;
    };

    Sheet mFontSheet;
    std::vector<Rgba> mFontClut;
    u64 mGlyphUploads = 0;
    std::unordered_set<TexHandle> mUploaded;
    // Duplicated handles share their origin's pixels, resolved once and remembered.
    std::unordered_map<TexHandle, TexHandle> mAlias;

    /// Reads the texture the handle names out of GS memory, through its own TEX0.
    bool recoverFromGs(TexHandle handle);

    /// Handles a draw asked for that nothing had registered, waiting for the frame boundary.
    std::unordered_set<TexHandle> mWanted;
    std::vector<TextureUpload> mRecoveredUploads;

    pcsx2hook_ctx* mRuntime = nullptr;
    u64 mRecovered = 0;
    u64 mRecoveryAttempts = 0;
    u64 mDecoded = 0;
    u64 mFailed = 0;
    const Config* mConfig = nullptr;
    std::string mDumpDir;

};

}  // namespace sdfhook
