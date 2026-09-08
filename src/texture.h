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

/// Sent to the render thread once, the first frame it is drawn with.
class TextureCache {
public:
    void attach(pcsx2hook_ctx* runtime, const Config& config) {
        mRuntime = runtime;
        mConfig = &config;
    }

    /// Empty when nothing was registered. Uploaded before the capture window means GS memory is
    /// the only copy left, so it is queued and resolves from the next frame.
    TexHandle getHandle(TexHandle wanted, Frame* frame = nullptr);

    /// For a material naming a TEX0 with no handle, which would otherwise draw untextured.
    /// `key` files the upload and is never dereferenced.
    TexHandle getHandleForTex0(TexHandle key, u64 tex0);

    /// Call only from the frame callback: the read blocks on the GS thread, unsafe from a hook.
    void recoverQueued();

    /// Moves anything recoverQueued read into the frame being built.
    void drainRecovered(Frame& frame);

    /// A texture's GS slot can since have gone to the display, and recovering it returns the
    /// screen instead.
    /// @fn sdfGsAllocFrameBuff
    /// @note SLUS-20911 0x002AA8D0
    void onFrameBuffer(sdfGsMemSlicePtr_t aSlice);

    /// Replaced at its next upload rather than dropped: a submitted frame still draws with it.
    /// @fn sdfTexFree
    /// @note SLUS-20911 0x002AB860
    void onFree(TexHandle handle);

    /// A copy never reaches sdfTexSetEx, so nothing is uploaded for it.
    void onDuplicate(TexHandle source, TexHandle copy);

    /// Only callable during a load: afterwards the engine keeps the GS allocation alone.
    void add(TMX_HEADER_PTR_t aTex, TexHandle handle, Frame* frame);

    /// Glyphs upload one at a time, so the sheet is assembled here and keyed by its address.
    void onGlyph(u32 dstX, u32 dstY, u32 width, u32 height, u32 dataAddr);
    void onFontClut(u32 dataAddr);

    /// A savestate begins past the uploads, so frTranslateUserData never runs and every string
    /// would draw as a blank bar.
    void wantFontSheet(u32 addr, u32 bufWidth, u32 width, u32 height);

    /// Static data, so readable whether or not frSetFontClut ran in the capture window.
    void loadFontClut(u32 tableAddr);

    /// Re-uploaded when glyphs change. One sheet, so no address is needed.
    TexHandle getFontSheet(u32 sheetWidth, Frame* frame);

    void dumpTo(const std::string& directory) { mDumpDir = directory; }

    u64 getRecovered() const { return mRecovered; }
    u64 getRecoveryAttempts() const { return mRecoveryAttempts; }
    u64 getQueued() const { return mWanted.size(); }
    u64 getDecoded() const { return mDecoded; }
    u64 getFailed() const { return mFailed; }
    u64 getGlyphUploads() const { return mGlyphUploads; }
    u64 getFreed() const { return mFreed; }
    bool hasFontClut() const { return !mFontClut.empty(); }

private:
    // Kept as the indices the game wrote, so a palette can change under it.
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
    // Duplicates share their origin's pixels; resolved once.
    std::unordered_map<TexHandle, TexHandle> mAlias;

    /// Reads the texture the handle names out of GS memory, through its own TEX0.
    bool recoverFromGs(TexHandle handle);

    /// The half needing only the register. `key` files the upload and is never dereferenced.
    bool recoverTex0(TexHandle key, u64 reg);

    bool recoverFontSheet();

    struct FontSheetWanted {
        u32 addr = 0;
        u32 bufWidth = 0;
        u32 width = 0;
        u32 height = 0;
    };

    FontSheetWanted mWantedSheet;

    struct GsRegion {
        u32 addr;
        u32 size;
    };

    std::vector<GsRegion> mFrameBuffers;

    /// Asked for by a draw, registered by nothing; drained at the frame boundary.
    std::unordered_set<TexHandle> mWanted;

    /// The same with no upload packet to read. Keyed by material packet address, unique per
    /// material.
    std::unordered_map<TexHandle, u64> mWantedTex0;
    std::vector<TextureUpload> mRecoveredUploads;

    pcsx2hook_ctx* mRuntime = nullptr;
    u64 mRecovered = 0;
    u64 mRecoveryAttempts = 0;
    u64 mDecoded = 0;
    u64 mFailed = 0;
    u64 mFreed = 0;
    const Config* mConfig = nullptr;
    std::string mDumpDir;

};

}  // namespace sdfhook
