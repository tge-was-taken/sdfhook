// Compiled twice, as GLSL and as C++ by shading_cpu.h, so a CPU check cannot drift from the shader.
// GS units: 1.0 is 128. Addresses are docs/vu1/vu1_annotated.c.
vec3 sdfMeshLit(vec3 n, bool lit, vec3 ambient, vec3 diffuse, vec3 draw, vec3 lightAmbient,
                vec3 lightBase, vec3 lightFull, vec3 dir0, vec3 dir1, vec3 dir2, vec3 color1,
                vec3 color2, float flatten, float soften, float bias, bool streamColors) {
    vec3 base = ambient * lightAmbient;  // lightRebase 0x01C2, 0x01C4: color 3, not colorBase
    float keep = 1.0 - flatten;          // 0x0139

    if (!lit) {
        vec3 full = vec3(flatten, flatten, flatten) + lightFull * keep;  // 0x0188..0x0194
        if (streamColors) return full * diffuse * draw;  // 0x019D..0x01B0 has no MINIi
        return min(full * diffuse * draw, vec3(2.0, 2.0, 2.0));
    }

    // 0x01C1..0x01C5. A difference, so facing away falls back to base rather than to black.
    vec3 light0 = (diffuse * lightBase - base) * keep;  // 0x0149

    // The light 0 ramp is not VU1's (0x0156 clamps at zero and no more); it stands in for
    // corePktApplyFaceDivColor, on the EE, over the points face subdivision adds.
    // @todo keep reaches only light 0. See TODO.md V23.
    vec3 sum = base
             + clamp(soften * (bias + dot(n, dir0)), 0.0, 1.0) * light0
             + max(dot(n, dir1), 0.0) * color1
             + max(dot(n, dir2), 0.0) * color2;

    // 0x0158..0x015B: base plus three lights, no specular; reflectPass 0x05CA is a separate draw.
    // draw is already drawTint * matDiffuse (0x009E), so diffuse must not be applied twice.
    return min(sum * draw, vec3(2.0, 2.0, 2.0));
}

// Per fragment, not per vertex: the engine subdivides the face, so interpolating smears the step.
// cppcheck-suppress unusedFunction  // called from the fragment shader, which cppcheck cannot parse
vec3 sdfMeshRamp(vec3 n, vec3 rampLo, vec3 rampDelta, bool rampKnown, vec3 mtexDiffuse,
                 vec3 lightBase, vec3 ambient, vec3 lightAmbient, vec3 draw, vec3 dir0,
                 float constant, float soften, float bias) {
    float t = clamp(soften * (bias + dot(n, dir0)), 0.0, 1.0);  // lightRamp 0x045A..0x0465
    if (rampKnown) return rampLo + rampDelta * t;
    vec3 lit = mtexDiffuse * lightBase;
    vec3 lo = mix(ambient * lightAmbient, lit, constant) * draw;
    vec3 hi = min(lit * draw, vec3(2.0, 2.0, 2.0));
    return lo + (hi - lo) * t;
}
