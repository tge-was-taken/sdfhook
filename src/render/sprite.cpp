#include "sprite.h"

namespace sdfhook {

void triangulate(const DrawSprite& sprite, f32 texWidth, f32 texHeight, std::vector<ScreenVertex>& out) {
    ScreenVertex v[4];
    for (int i = 0; i < 4; i++) {
        const SpriteCorner& c = sprite.corner[i];
        v[i] = ScreenVertex{c.x, c.y, c.u / texWidth, c.v / texHeight, c.color, c.z, c.fog};
    }
    out.insert(out.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
}

}  // namespace sdfhook
