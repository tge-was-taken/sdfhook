#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "guest.h"
#include "libsdf.h"
#include "render/frame.h"
#include "types.h"

namespace sdfhook {

/// Each texture is sent to the render thread once, the first frame it is drawn with.
class TextureCache {
public:
    /// The handle to draw a texture with, empty when nothing was registered for it.
    TexHandle getHandle(const Guest& guest, u32 handleAddr);

    /// Links a copy made by sdfTexDup to its source: the copy never reaches sdfTexSetEx, so
    /// nothing is uploaded for it.
    void onDuplicate(const Guest& guest, u32 sourceHandle, u32 copyHandle);

    /// Only callable during a load: afterwards the engine keeps the GS allocation alone.
    void add(const Guest& guest, u32 texFileAddr, u32 handleAddr, Frame* frame);

    /// Glyphs are uploaded one at a time into a sheet shared in GS memory, so the sheet is
    /// assembled here from those same uploads and identified by the address it lives at.
    void onGlyph(const Guest& guest, u32 dstX, u32 dstY, u32 width, u32 height, u32 dataAddr);
    void onFontClut(const Guest& guest, u32 dataAddr);

    /// The palettes are static data, so they can be read whether or not frSetFontClut ran inside
    /// the capture window.
    void loadFontClut(const Guest& guest, u32 tableAddr);

    /// The handle to draw the glyph sheet with, sized as the engine sees it and re-uploaded when
    /// glyphs have changed. There is one sheet, so it needs no address to tell it apart.
    TexHandle getFontSheet(u32 sheetWidth, Frame* frame);

    void dumpTo(const char* directory) { mDumpDir = directory; }

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
    u64 mDecoded = 0;
    u64 mFailed = 0;
    const char* mDumpDir = nullptr;

};

}  // namespace sdfhook
