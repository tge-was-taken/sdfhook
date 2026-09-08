#pragma once

#include <algorithm>

#include "../types.h"

/// Compiles shading.inc as C++ by giving it the GLSL names it uses, so a CPU check and the GL
/// shader are the same text.
namespace sdfhook::shading {

struct vec3 {
    f32 x = 0.0f, y = 0.0f, z = 0.0f;

    constexpr vec3() = default;
    constexpr vec3(f32 ax, f32 ay, f32 az) : x(ax), y(ay), z(az) {}
    explicit constexpr vec3(const f32 v[3]) : x(v[0]), y(v[1]), z(v[2]) {}

    f32 operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};

inline vec3 operator+(vec3 a, vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline vec3 operator-(vec3 a, vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline vec3 operator*(vec3 a, vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline vec3 operator*(f32 s, vec3 a) { return {s * a.x, s * a.y, s * a.z}; }
inline vec3 operator*(vec3 a, f32 s) { return s * a; }

inline f32 dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline f32 clamp(f32 v, f32 lo, f32 hi) { return std::clamp(v, lo, hi); }
inline f32 max(f32 a, f32 b) { return std::max(a, b); }
inline vec3 mix(vec3 a, vec3 b, f32 t) { return a + (b - a) * t; }
inline vec3 min(vec3 a, vec3 b) {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

#include "shading.inc"

}  // namespace sdfhook::shading
