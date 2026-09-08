#pragma once

#include <map>
#include <string>

#include "render/frame.h"
#include "render_state.h"
#include "types.h"

namespace sdfhook {

class TextureCache;

/// Global because the guest's packet buffers and rset* globals are: a reimplementation reads and
/// writes them where the engine does, and takes no parameter for either.
struct DrawSink {
    Frame* frame = nullptr;
    TextureCache* textures = nullptr;

    /// Set by SinkScope for the call in progress.
    const char* producer = "";

    u64* apiDraws = nullptr;
    std::map<std::string, u64>* unresolvedTextures = nullptr;

    /// Appends the draw and stamps the producer's own tag with it. The producer registers that
    /// tag itself, as the guest does; the sink never registers a second one.
    void addSprite(struct xsdfDrawTag_t* aTag, DrawSprite& aSprite);
    void addMesh(DrawMesh&& aMesh);

    /// For the 3D walk, which holds its own frame rather than running inside a SinkScope. A
    /// producer that registers its own tag passes it, as the 2D ones do; the walk passes none and
    /// the sink registers one for it.
    static void addMeshTo(Frame* aFrame, DrawMesh&& aMesh, struct xsdfDrawTag_t* aTag = nullptr);

    /// The table the draw registers into, which the producers set from the engine's own index.
    u32 otIndex = 0;

    /// The handle the guest named, or the one sdf2DLinkTexPacket bound ahead of it.
    TexHandle resolveTexture(u32 aNamed);
};

extern DrawSink gSink;
extern RenderState gState;

}  // namespace sdfhook
