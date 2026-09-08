#include "sink.h"

#include "libsdf_impl.h"
#include "texture.h"

namespace sdfhook {

DrawSink gSink;
RenderState gState;

// Every draw is registered into the shadow queue as the guest registers its tag, and the frame's
// order is what xsdfGrOtClose concatenates out of the 85 tables.
void DrawSink::addSprite(xsdfDrawTag_t* aTag, DrawSprite& aSprite) {
    aSprite.producer = producer;
    if (apiDraws) (*apiDraws)++;
    if (!frame) return;
    if (aTag) {
        aTag->kind = xsdfDrawTag_t::Kind::Sprite;
        aTag->draw = u32(frame->sprites.size());
        aTag->hasDraw = true;
    }
    frame->addSprite(aSprite);
}

void DrawSink::addMesh(DrawMesh&& aMesh) {
    addMeshTo(frame, std::move(aMesh));
}

void DrawSink::addMeshTo(Frame* aFrame, DrawMesh&& aMesh, xsdfDrawTag_t* aTag) {
    Frame* const frame = aFrame;
    if (!frame) return;
    xsdfDrawTag_t* tag = aTag ? aTag : xsdfAllocDrawTag();
    tag->head = true;
    tag->kind = xsdfDrawTag_t::Kind::Mesh;
    tag->draw = u32(frame->meshes.size());
    tag->hasDraw = true;
    const u32 table = aMesh.otIndex;
    frame->addMesh(std::move(aMesh));
    if (!aTag) xsdfGrOtRegister(table, tag);
}

TexHandle DrawSink::resolveTexture(u32 aNamed) {
    const TexHandle named{aNamed ? aNamed : gState.texture};
    const TexHandle handle = textures ? textures->getHandle(named, frame) : TexHandle{};
    // A named texture we never decoded draws flat. Counted by producer, so the gap names it.
    if (named.addr && !handle.addr && unresolvedTextures) (*unresolvedTextures)[producer]++;
    return handle;
}

}  // namespace sdfhook
