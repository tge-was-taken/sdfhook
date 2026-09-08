/* SDF's VU1 microcode, all 1884 instructions of 0x0000..0x075B, from docs/vu1/vu1_image.c
 * (SLUS-20911). Captured on smt3_field_unknown_realm; `vu2c.py --check` reports 0 mismatches.
 * Regenerate the listing with:
 *
 *     vu2c.py docs/vu1/vu1_images.jsonl --clean --flow --keep-nops --names docs/vu1/vu1_names.txt
 *
 * `--keep-nops` is not optional: without it 81 addresses vanish and every range below reads as
 * incomplete. The listing is the authority; this is for reading. `scripts/vucheck.py` keeps the
 * two in step, and this file compiles under `gcc -fsyntax-only` so nothing here is undeclared.
 *
 * Written by hand under .claude/skills/vu-translate. Delay slots are in program order, unrolls are
 * folded into the loop they are, values are named for what they hold rather than for the register
 * that held them, and a routine takes what it needs as arguments. Every line carries the addresses
 * it came from. What a caller must set up is the `subroutine(...)` banner in the listing; where
 * this file and that banner disagree, the banner is right.
 *
 * A swizzle is one float here, as in the listing: `a.xyz = b.xyz * c.x` is the whole vector
 * operation written as something a C parser accepts, not three multiplies.
 *
 * Ranges covered here, complete and with no gaps:
 *   0x0000..0x0038  dispatch             the MSCAL entries and the per-batch reset
 *   0x0039..0x004E  loadCamera           the screen matrices and the winding reference point
 *   0x004F..0x0080  composeMatrices      object x camera x light, into the drawing set
 *   0x0081..0x008E  copyBlock            a counted quadword copy into mem[936]
 *   0x008F..0x00A8  lightSetup           once per material, unpacks its colors
 *   0x00A9..0x00C1  unpackCounts         twelve quadwords into mem[896..901] and mem[974..979]
 *   0x00C2..0x00D2  unpackTexture        the texture parameters and the material-changed flag
 *   0x00D3..0x00D9  unpackTwo            two scalars into mem[912] and mem[913]
 *   0x00DA..0x00E5  readFlags            the draw flags, masked into what the callers test
 *   0x00E6..0x00FF  texcoordPlain        one multiply-add per vertex
 *   0x0100..0x0102  returnPlain          the trampoline that restores the link register
 *   0x0103..0x012E  texcoordReflect      a sphere map about the view vector
 *   0x012F..0x0131  returnReflect        the same trampoline
 *   0x0132..0x0172  lightVertex          the three-light sum, two vertices an iteration
 *   0x0173..0x0187  lightVertexColor    the same with the mesh's own vertex colors
 *   0x0188..0x0194  lightVertexFlat      the no-normals case
 *   0x0195..0x019C  fillFour             one color to four vertices at a time
 *   0x019D..0x01B0  lightFlatColor      flat, with vertex colors, two an iteration
 *   0x01B1..0x01B8  fillOne              one color to every vertex
 *   0x01B9..0x01BA  drawEntry            the MSCAL entry into the per-draw unpack
 *   0x01BB..0x01C5  lightRebase          once per material, rewrites the base and light 0
 *   0x01C6..0x01DE  drawBatch            the driver, and the two-sided second pass
 *   0x01DF..0x01F2  negateNormals        every normal flipped in place, two an iteration
 *   0x01F3..0x01FD  swapIndices          y and z of each index triple exchanged
 *   0x01FE..0x020F  texcoordMatrix       UVs through the material's 2x3 matrix
 *   0x0210..0x0219  drawFront            the first pass, picking a texcoord routine
 *   0x021A..0x021D  drawEmit             the PRIM packet, then emitTriangles
 *   0x021E..0x0223  texcoordJump         loads the matrix and jumps to transformVerticesUv
 *   0x0224..0x022E  drawSecond           the second pass's calls, and flag 0x1000
 *   0x022F..0x0234  drawReturn           the reflection pass, then the stacked return
 *   0x0235..0x0270  emitTriangles        one triangle an iteration: cull, clip, project, store
 *   0x0271..0x0279  drainClip            the untextured emitter's retry list
 *   0x027A..0x0289  emitClipped          its clipper output, two registers a vertex
 *   0x028A..0x02CE  emitTexTriangles     the textured emitter, three registers a vertex
 *   0x02CF..0x02D7  drainClipTex         the textured emitter's retry list
 *   0x02D8..0x02E9  emitClippedTex       its clipper output, three registers a vertex
 *   0x02EA..0x031B  texcoordSecond       the second UV set, with a six-register A+D block
 *   0x031C..0x0328  packetOne            a tag and one A+D write
 *   0x0329..0x033B  packetThree          a tag and three
 *   0x033C..0x034C  packetClipped        the header pair the clipped emitters draw under
 *   0x034D..0x035C  normalizeVectors     RSQRT over a run of vectors, in place
 *   0x035D..0x0381  unpackPointers       the batch header, and one cursor over the arrays
 *   0x0382..0x03A2  skinFirstBone        position and normal through one bone, times its weight
 *   0x03A3..0x03BD  skinAddBone          the same, accumulated
 *   0x03BE..0x03DF  skinLastBone         accumulated, then the normal renormalized
 *   0x03E0..0x03F6  morphFirst           shape 0 scaled by its weight
 *   0x03F7..0x0402  morphAdd             a further shape accumulated by its weight
 *   0x0403..0x0419  morphSum             the unweighted base added last
 *   0x041A..0x041E  loadObjectMatrix     the rigid case
 *   0x041F..0x0439  drawEntry2           the second MSCAL entry and its second pass
 *   0x043A..0x047A  lightRamp            one light, a color ramp and a per-vertex scalar
 *   0x047B..0x0484  drawFront2           the first pass
 *   0x0485..0x0488  drawEmit2            into emitTriangles
 *   0x0489..0x048C  texcoordJump2        the matrix path
 *   0x048D..0x049B  drawSecond2          the second pass's calls and the stacked return
 *   0x049C..0x04A3  kick                 XGKICK, and the output buffers swapped
 *   0x04A4..0x04A7  kickAndEnd           a kick, then the microprogram's end
 *   0x04A8..0x04B6  clipReject           the six trivial rejects, out of line
 *   0x04B7..0x04D0  packetSix            a tag and six A+D writes from a caller's block
 *   0x04D1..0x04F5  transformVertices    position, reciprocal and fog
 *   0x04F6..0x0524  transformVerticesUv  the same, with the texture matrix
 *   0x0525..0x052E  loadClipTriangle     three vertices of attributes for the clipper
 *   0x052F..0x056D  clipTriangle         seed the buffer, run the six planes
 *   0x056E..0x05C9  clipPlane            one plane, in and out of two scratch buffers
 *   0x05CA..0x05F0  reflectPass          the sphere-map pass
 *   0x05F1..0x05FA  spriteDispatch       the five ITOP entries
 *   0x05FB..0x0608  matrixScale          the billboard axes over the camera's column lengths
 *   0x0609..0x0631  spriteOne            one quad a sprite, two registers a vertex
 *   0x0632..0x0666  spriteTwo            the same, drawn twice
 *   0x0667..0x0681  spriteProject        the corner projection, divide last
 *   0x0682..0x06AC  spriteThree          one quad a sprite, three registers a vertex
 *   0x06AD..0x06EA  spriteFour           the same, drawn twice
 *   0x06EB..0x0707  spriteProjectQ       the corner projection, divide first
 *   0x0708..0x0711  copyHeader           the EE's own quadwords, straight through
 *   0x0712..0x071A  directSetup          fog, the first divide, and the layout branch
 *   0x071B..0x073E  directTextured       three registers a vertex
 *   0x073F..0x075B  directUntextured     two
 *
 * Three things this transcription settled that nothing else in the tree states:
 *
 *   mem[800 + v] is the vertex color and mem[848 + v].xy the texture coordinate. The lighting
 *   routines clamp the first to 255 and the transform divides the second by w; emitTexTriangles
 *   emits them as [ST, RGBA, XYZ], which is what pins the pair.
 *
 *   Fog is computed once a vertex in the transform, `fog.z + fog.w / w` clamped to [fog.x, fog.y],
 *   and carried in mem[848 + v].w.
 *
 *   mem[907] is the GS PRIM word's flag bits, `attrBits & 0x78`. Every packet builder adds a
 *   primitive type to it, and the +512 the second passes add is PRIM's CTXT bit, so those passes
 *   draw through GS context 2.
 */

typedef union {
    struct { float x, y, z, w; };
    float xy, xz, xw, yz, yw, zw, xyz, xyw, xzw, yzw, xyzw;
} vec4;

typedef union {
    struct { int x, y, z, w; };
    int xy, xz, xw, yz, yw, zw, xyz, xyw, xzw, yzw, xyzw;
} ivec4;

union { vec4 f[1024]; ivec4 w[1024]; } vu_mem;
#define mem   vu_mem.f
#define mem_w vu_mem.w

/* The fixed addresses, as vu1_names.txt gives them to the listing. */
#define position(v)     mem[(v) + 704]   /* view space, .w the clip w */
#define vertexColor(v) mem[(v) + 800]   /* float until emit's FTOI0 */
#define texcoord(v)     mem[(v) + 848]   /* .xy the UV, .z the reciprocal, .w the fog */
#define faceCount       mem_w[896].w
#define vertexCount     mem_w[897].w
#define indexList       mem_w[898].w
#define srcPositions    mem_w[899].w
#define srcNormals      mem_w[900].w
#define srcTexcoords    mem_w[901].w
#define srcColors      mem_w[902].w
#define drawFlags       mem_w[903].w     /* packetBits */
#define drawMode        mem_w[904].w     /* attrBits masked by the material */
#define shapeIndex      mem_w[911].w
#define morphWeight(i)  mem[(i) + 912]
#define texcoordMode    mem_w[914].w
#define materialMask    mem_w[915].w
#define materialChanged mem_w[917].w
#define fogParams       mem[919]         /* .x min, .y max, .z offset, .w scale */
#define lightDir        mem[920]         /* three rows, the light vectors as columns */
#define colorBase       mem[923]
#define lightColor(i)  mem[(i) + 924]
#define flatten         mem[910].w
#define primFlags       mem_w[907].w     /* attrBits & 0x78 */
#define lastPrim        mem_w[908].w
#define screenScaleMem  mem[934]
#define boneBlock(i)    mem[(i) + 936]   /* seven quadwords a bone */
#define uvMatrix(i)     mem[(i) + 974]
#define uvMatrixB(i)    mem[(i) + 977]
#define adTag           mem[980]         /* GIF tag templates, .yz only */
#define vertexTag2      mem[981]
#define vertexTag3      mem[982]
#define objectCamera(i) mem[(i) + 964]
#define lightNormal(i)  mem[(i) + 968]
#define tintScale       mem[971]
#define reflectColor   mem[973]
#define normalMatrix(i) mem[(i) + 988]   /* mem[988..990], and the object's own at mem[999..1001] */
#define cameraMatrix(i) mem[(i) + 992]
#define objectNormal(i) mem[(i) + 996]
#define matDiffuse      mem[1002]
#define lightBase       mem[1003]
#define matAmbient      mem[1004]
#define materialColor  mem[1005]
#define drawTint        mem[1007]     /* a color: every use multiplies a color by it */
#define drawTint2       mem[1006]
#define windingRef      mem[1011]
#define spriteTop       mem_w[1020].y
#define retryHead       mem_w[1021].w
#define clipBufA        mem_w[1021].y
#define clipBufB        mem_w[1021].z
#define cursors         mem_w[1023]      /* .y the emit cursor, .z and .w the kick pair */
#define reflectAdBlock  909              /* the five A+D quadwords the reflection pass draws under */

/* What the hardware gives us. */
float I, Q, P;
unsigned mac, clip;
int VIF1_TOP, VIF1_ITOP;
float max(float a, float b), min(float a, float b), fabsf(float v), sqrtf(float v);
float ftoi0(float v), ftoi4(float v), itof0(float v);
float opmul(vec4 a, vec4 b), cross(vec4 a, vec4 b);
float mul3x3(vec4 m, float v), mul4x4(vec4 m, float v);
vec4 mr32(vec4 v);
unsigned clip_bits(float v, float w);
void xgkick(int addr), goto_micro(int addr);

/* State the routines share through registers rather than through memory. */
vec4 modelView;     /* vf26..vf29, object x camera, or camera alone once a skin has run */
vec4 screenScale;   /* vf30, also at mem[934] */
vec4 screenOffset;  /* vf31 */
vec4 one;           /* vf01, (1, 1, 1, 0), built at 0x001E */
vec4 drawColor;    /* vf15, materialColor: drawTint times the material's diffuse */
int  stream;        /* vi02 at entry, the VIF's TOP */
int  outCursor;     /* vi11, where an emitter is writing */
int  emittedCount;  /* vi10, the vertices an emitter has produced */
int  clipEmitter;   /* vi13, where the clipper hands its vertices back */
int  kickTop;       /* vi14, the return stack, growing down from 1024 */
int  link;          /* vi15 */

/* Micro addresses the code carries as numbers, so a jump names its target. A name ending in a role
 * rather than a routine points into the middle of one. */
enum {
    atReturnPlain     = 256,                         /* 0x0100 */
    atReturnReflect   = 303,                         /* 0x012F */
    atLightVertex     = 306,                         /* 0x0132 */
    atFillFourTail    = 410,                         /* 0x019A, the last store of fillFour's group of four */
    atFillOne         = 433,                         /* 0x01B1 */
    atDrawSecond      = 548,                         /* 0x0224 */
    atDrawReturn      = 559,                         /* 0x022F */
    atDrawReturnPop   = 562,                         /* 0x0232, drawReturn past the reflection pass */
    atDrainClip       = 625,                         /* 0x0271 */
    atEmitClipped     = 634,                         /* 0x027A */
    atEmitTexTriangles = 650,                        /* 0x028A */
    atDrainClipTex    = 719,                         /* 0x02CF */
    atEmitClippedTex  = 728,                         /* 0x02D8 */
    atLightRamp       = 1082,                        /* 0x043A */
    atDrawSecond2Pop  = 1177,                        /* 0x0499, drawSecond2's stacked return */
    atKickAndEnd      = 1188,                        /* 0x04A4 */
    atSharedReturn    = 1190,                        /* 0x04A6, kickAndEnd past the kick */
    atPacketSix       = 1207,                        /* 0x04B7 */
    atPacketSixBody   = 1209,                        /* 0x04B9, packetSix without the two loads */
    atTransformUv     = 1270,                        /* 0x04F6 */
    atReflectPass     = 1482,                        /* 0x05CA */
    atSpriteDispatch  = 1521,                        /* 0x05F1 */
    atCopyHeader      = 1800,                        /* 0x0708 */
};

static void loadCamera(void), composeMatrices(void), copyBlock(void), lightSetup(void);
/* What readFlags hands back: drawMode bit 0, drawMode bit 10, drawFlags bit 11. */
typedef struct { int corrected, reflect, objectNormals; } DrawSelect;
vec4 uvScale;       /* vf23, mem[932] or mem[933] */
static void unpackCounts(void), unpackTexture(void), unpackTwo(void);
static DrawSelect readFlags(void);
static void texcoordPlain(void), texcoordReflect(void), resetBatch(void);
static void lightVertex(vec4 color), lightRebase(void);
static void lightVertexColor(vec4 lightRow, vec4 base, int count, int normals);
static void lightVertexFlat(void), fillFour(vec4 color, int count, int out);
static void lightFlatColor(vec4 flat, int count), fillOne(vec4 color);
static void drawEntry(void), drawBatch(void), drawEntry2(void);
static void negateNormals(void), swapIndices(void), texcoordMatrix(void);
static void drawFront(void), drawEmit(void), drawSecond(void), drawReturn(void);
static void drawFront2(void), drawEmit2(void), drawSecond2(void);
static void emitTriangles(void), drainClip(void), emitClipped(void);
static void emitTexTriangles(void), drainClipTex(void), emitClippedTex(void);
static void texcoordSecond(int lightRoutine, int emitter);
static void packetOne(void), packetThree(void);
static void packetClipped(vec4 vertexTag, int count), normalizeVectors(int count);
static void packetSix(int block), unpackPointers(int packetBits);
static void skinFirstBone(int packetBits), morphFirst(int packetBits);
static void skinAddBone(int packetBits, vec4 pos0, vec4 nrm0, vec4 p, vec4 n, int src, int nsrc);
static void skinLastBone(vec4 pos0, vec4 nrm0, vec4 p, vec4 n, int src, int nsrc);
static void morphAdd(vec4 weight, vec4 p, vec4 n, int src, int nsrc, int v);
static void morphSum(int shape, int src, int nsrc, int v), loadObjectMatrix(void);
static void transformVertices(void), transformVerticesUv(void);
static void loadClipTriangle(int a, int b, int c);
static void clipTriangle(int triangle, int emitter);
static void clipPlane(vec4 plane, int here, int next, int axis);
static void clipSeed(vec4 pa, vec4 ca, vec4 sta, vec4 pb, vec4 cb, vec4 stb,
                     vec4 pc, vec4 cc, vec4 stc);
static void reflectPass(void), lightRamp(vec4 color), kick(void), kickAndEnd(void);
static void copyHeader(void), directSetup(int layout, int out, vec4 header);
static void directTextured(int count, int out, vec4 p, vec4 fog, vec4 st, vec4 stOdd);
static void directUntextured(int count, int out, vec4 p, vec4 fog);
static int  clipReject(void);
vec4 axisU, axisV, centreBias;   /* vf23, vf24, vf25, the billboard's frame */
vec4 spriteCentre;               /* vf15 or vf13, the centre in view space */
int  cornerClip;                 /* vi02, the AND of the four corners' clip bits */
static void spriteDispatch(void), matrixScale(void);
static void spriteOne(void), spriteTwo(void), spriteThree(void), spriteFour(void);
static void spriteProject(int tagWord, int out, vec4 attr, vec4 c0, vec4 c1, vec4 c2, vec4 c3,
                          vec4 k0, vec4 k1, vec4 k2, vec4 k3);
static void spriteProjectQ(int tagWord, int out, vec4 c0, vec4 c1, vec4 c2, vec4 c3,
                           vec4 k0, vec4 k1, vec4 k2, vec4 k3,
                           vec4 t0, vec4 t1, vec4 t2, vec4 t3);

/* ============================================================ dispatch */

/* Every address the EE can MSCAL is two or three instructions that pick a body and jump to it,
 * which is why vu1_programs.jsonl shows only stubs and the image is the unit worth reading. An
 * ITOP entry adds a fixed base to the number the EE left in the VIF's ITOP register, so the EE
 * picks the body by writing a number rather than by picking an entry. */

static void entryItop30(void) {
    stream = VIF1_TOP;                               /* 0x0003 XTOP, the JR's delay slot */
    goto_micro(VIF1_ITOP + 30);                      /* 0x0000 XITOP, 0x0001 IADDIU, 0x0002 JR */
}

static void entry1800(void) {
    stream = VIF1_TOP;                               /* 0x0006 XTOP */
    goto_micro(atCopyHeader);                        /* 0x0004 IADDIU, 0x0005 JR, into copyHeader */
}

static void entryItop1521(void) {                    /* 0x0007 nop */
    stream = VIF1_TOP;                               /* 0x000B XTOP */
    goto_micro(VIF1_ITOP + atSpriteDispatch);        /* 0x0008 XITOP, 0x0009 IADDIU, 0x000A JR */
}

static void entryLighting(void) {
    stream = VIF1_TOP;                               /* 0x000D XTOP */
    drawEntry();                                     /* 0x000C BAL */
    return;                                              /* [E] at 0x000E; 0x000F nop */
}

static void entryRamp(void) {
    stream = VIF1_TOP;                               /* 0x0011 XTOP */
    drawEntry2();                                    /* 0x0010 BAL */
    return;                                              /* [E] at 0x0012; 0x0013 nop */
}

/* Three quadwords the EE just unpacked. The first is the fog parameters, which nothing but the
 * transform reads; the other two go to the output cursor. */
static void storeFog(void) {
    stream = VIF1_TOP;                               /* 0x0014 XTOP */
    int out = cursors.y;                             /* 0x0015 ILW */
    fogParams = mem[stream];                         /* 0x0017 LQ, 0x001A SQ */
    mem[out] = mem[stream + 1];                      /* 0x0018 LQ, 0x001B SQ */
    mem[out + 1] = mem[stream + 2];                  /* 0x0019 LQ, 0x001D SQ; 0x001C nop */
    goto_micro(atKickAndEnd);                        /* 0x0016 IADDIU, 0x001C JR, kickAndEnd */
}

/* The rest of the table. Each entry sets the link register to 1190, the shared return at 0x04A6,
 * and branches. 0x001E also builds `one`, which the whole program uses as its one and zero. */
static void entryTable(void) {
    one.w = 0.0f;                                    /* 0x001E SUBw */
    one.xyz = 1.0f;                                  /* 0x001F ADDw */
    link = atSharedReturn;
    resetBatch();                                    /* 0x001E B, 0x001F IADDIU */
    link = atSharedReturn; loadCamera();             /* 0x0020 B, 0x0021 IADDIU */
    link = atSharedReturn; unpackTexture();          /* 0x0022 B, 0x0023 IADDIU */
    return;                                              /* [E] at 0x0024; 0x0025 nop */
    return;                                              /* [E] at 0x0026; 0x0027 nop */
    link = atSharedReturn; unpackTwo();              /* 0x0028 B, 0x0029 IADDIU */
    link = atSharedReturn; lightSetup();             /* 0x002A B, 0x002B IADDIU */
    link = atSharedReturn; copyBlock();              /* 0x002C B, 0x002D IADDIU */
    link = atSharedReturn; composeMatrices();        /* 0x002E B, 0x002F IADDIU */
}

/* Points the kick stack at the top of memory and the three output cursors at their buffers, so a
 * batch always starts writing where the last one began rather than where it ended. */
static void resetBatch(void) {                       /* 0x0030, from 0x001E and 0x0193 */
    kickTop = 1024;                                  /* 0x0030 IADDIU */
    cursors.z = 384;                                 /* 0x0031 IADDIU, 0x0032 IADD, 0x0033 ISW */
    cursors.y = 384;                                 /* 0x0034 ISW, the emit output cursor */
    cursors.w = 544;                                 /* 0x0035 IADDIU, 0x0036 ISW */
    mem_w[909].w = 0;                                /* 0x0037 ISW */
    return;                                              /* [E] at 0x0037; 0x0038 nop */
}

/* ============================================================ setup */

/* Ten quadwords of camera state, filed where the rest of the program reads them. Two matter
 * downstream: `screenScale` at mem[934], and the reference point at mem[1011] that emitTriangles
 * takes its winding test against, put through the camera matrix here. */
static void loadCamera(void) {                       /* 0x0039, from 0x0020 */
    vec4 refPoint = mem[stream + 10];                /* 0x0039 LQ */
    vec4 n0 = mem[stream], n1 = mem[stream + 1];     /* 0x003A, 0x003B LQ */
    vec4 n2 = mem[stream + 2], n3 = mem[stream + 3]; /* 0x003C, 0x003D LQ */
    modelView = mem[stream + 4];                     /* 0x003E, 0x003F, 0x0040, 0x0041 LQ */
    screenScale = mem[stream + 8];                   /* 0x0042 LQ */
    screenOffset = mem[stream + 9];                  /* 0x0043 LQ */

    normalMatrix(0) = n0;                            /* 0x0044 SQ */
    normalMatrix(1) = n1;                            /* 0x0045 SQ */
    normalMatrix(2) = n2;                            /* 0x0046 SQ */
    refPoint.xyzw = mul4x4(modelView, refPoint.xyzw); /* 0x0047, folds 0x0044..0x0047 */
    mem[991] = n3;                                   /* 0x0047 SQ */
    cameraMatrix(0) = modelView;                     /* 0x0048, 0x0049, 0x004A, 0x004B SQ */
    screenScaleMem = screenScale;                    /* 0x004C SQ */
    windingRef = refPoint;                           /* 0x004D SQ */
    return;                                              /* [E] at 0x004D; 0x004E nop */
}

/* Concatenates the object's transform with the camera matrix and with the light matrix, writing
 * every product the per-vertex routines read. Each MADDw line folds the four instructions the
 * listing names beside it. */
static void composeMatrices(void) {                  /* 0x004F, from 0x002E */
    vec4 r0 = mem[stream], r1 = mem[stream + 1];     /* 0x004F, 0x0054 LQ, the object's rows */
    vec4 r2 = mem[stream + 2], r3 = mem[stream + 3]; /* 0x0055, 0x0056 LQ */
    vec4 cam = cameraMatrix(0);                      /* 0x0050, 0x0051, 0x0052, 0x0053 LQ */

    modelView.xyzw = mul4x4(cam, r0.xyzw);           /* 0x0057, folds 0x0054..0x0057 */
    vec4 n0 = mem[stream + 4], n1 = mem[stream + 5]; /* 0x0057, 0x0058 LQ, the normal rows */
    vec4 n2 = mem[stream + 6];                       /* 0x0059 LQ */
    vec4 scale = mem[932];                           /* 0x005A LQ */
    objectCamera(0) = modelView;                     /* 0x005B SQ */
    objectCamera(1).xyzw = mul4x4(cam, r1.xyzw);     /* 0x005B, folds 0x0058..0x005B; 0x005F SQ */

    vec4 lit = mem[920];                             /* 0x005C..0x005E LQ, mem[920..922] */
    objectCamera(2).xyzw = mul4x4(cam, r2.xyzw);     /* 0x005F, folds 0x005C..0x005F; 0x0063 SQ */

    vec4 nrm = normalMatrix(0);                      /* 0x0060, 0x0061, 0x0062 LQ */
    objectCamera(3).xyzw = mul4x4(cam, r3.xyzw);     /* 0x0063, folds 0x0060..0x0063; 0x0067 SQ */

    mem[999] = n0;                                   /* 0x0064 SQ, the object's own normal rows */
    mem[1000] = n1;                                  /* 0x0065 SQ */
    mem[1001] = n2;                                  /* 0x0066 SQ */

    /* The normal rows through each matrix, three components only: a direction has no translation. */
    objectNormal(0).xyz = mul3x3(nrm, n0.xyz);       /* 0x0066, folds 0x0064..0x0066; 0x006E SQ */
    objectNormal(1).xyz = mul3x3(nrm, n1.xyz);       /* 0x0069, folds 0x0067..0x0069; 0x006F SQ */
    objectNormal(2).xyz = mul3x3(nrm, n2.xyz);       /* 0x006C, folds 0x006A..0x006C; 0x0070 SQ */
    lightNormal(0).xyz = mul3x3(lit, n0.xyz);        /* 0x006F, folds 0x006D..0x006F; 0x0073 SQ */
    lightNormal(1).xyz = mul3x3(lit, n1.xyz);        /* 0x0072, folds 0x0070..0x0072; 0x0076 SQ */
    lightNormal(2).xyz = mul3x3(lit, n2.xyz);        /* 0x0075, folds 0x0073..0x0075; 0x0079 SQ */
    /* 0x0068, 0x006B, 0x006D, 0x0071, 0x0074 nop */

    /* The row sums after mem[932] scales them: the length correction a non-uniform object scale
     * needs before a normal is dotted against anything. */
    n0.xyz = n0.xyz * scale.xyz;                     /* 0x0076 MUL */
    n1.xyz = n1.xyz * scale.xyz;                     /* 0x0077 MUL */
    n2.xyz = n2.xyz * scale.xyz;                     /* 0x0078 MUL */
    vec4 sum;
    sum.x = n0.x + n0.y + n0.z;                      /* 0x0079 ADDy, 0x007C ADDz */
    sum.y = n1.y + n1.x + n1.z;                      /* 0x007A ADDx, 0x007D ADDz */
    sum.z = n2.z + n2.x + n2.y;                      /* 0x007B ADDx, 0x007E ADDy */
    mem[933] = sum;                                  /* 0x007F SQ */
    return;                                              /* [E] at 0x007F; 0x0080 nop */
}

/* A counted copy: the first word at the stream is n, and 7n quadwords after it go to mem[936].
 * Seven is one bone's transform, four rows for the position and three for the normal. The multiply
 * is three doublings and a subtract, since the VU has no integer multiply. */
static void copyBlock(void) {                        /* 0x0081, from 0x002C */
    int n = mem_w[stream].x;                         /* 0x0081 ILW */
    int count = n + n + n + n + n + n + n + n - n;   /* 0x0082..0x0085, n * 8 - n */
    int src = stream + 1, dst = 936;                 /* 0x0086, 0x0088 IADDIU */
    goto body;                                       /* 0x0087 B */
body:
    do {                                             /* 0x0089, from 0x0087 and 0x008B */
        count = count - 1;                           /* 0x0089 ISUBIU */
        mem[dst++] = mem[src++];                     /* 0x008A LQI, 0x008C SQI */
    } while (count != 0);                            /* 0x008B IBNE */
    goto_micro(link);                                /* 0x008D JR; 0x008E nop */
}

/* ============================================================ per-material state */

/* The material packet, a VIF stream of packed byte colors expanded to a quadword each. `drawTint`
 * is the per-draw color the EE left at mem[1007]: this multiplies it by the material's diffuse and
 * files the product as `materialColor`, so the diffuse scales every term of the sum later. */
static void lightSetup(void) {                       /* 0x008F, from 0x002A */
    vec4 tint = drawTint;                            /* 0x008F LQ */
    vec4 ambient = mem[stream++], diffuse = mem[stream++]; /* 0x0090, 0x0091 LQI */
    vec4 c5 = mem[stream++], c6 = mem[stream++];     /* 0x0092, 0x0093 LQI */
    vec4 c7 = mem[stream++], c8 = mem[stream++];     /* 0x0094, 0x0096 LQI */
    vec4 flags = mem[stream++], mode = mem[stream++]; /* 0x0097, 0x0098 LQI */
    vec4 c12 = mem[stream++], c13 = mem[stream++];   /* 0x0099, 0x009A LQI */

    I = 0.0078125f;                                  /* 0x0095 LOI, 1/128 */
    ambient.xyzw = itof0(ambient.xyzw) * I;          /* 0x0094 ITOF0, 0x0099 MULi */
    diffuse.xyzw = itof0(diffuse.xyzw) * I;          /* 0x0095 ITOF0, 0x009A MULi */
    c5.xyzw = itof0(c5.xyzw) * I;                    /* 0x0096 ITOF0, 0x009B MULi */
    c6.xyzw = itof0(c6.xyzw) * I;                    /* 0x0097 ITOF0, 0x009C MULi */
    c7.xyzw = itof0(c7.xyzw) * I;                    /* 0x0098 ITOF0, 0x009D MULi */
    tint.xyzw = tint.xyzw * diffuse.xyzw;            /* 0x009E MUL */

    mem[903].x = flags.x;                            /* 0x009B SQ, packetBits */
    mem[914].w = mode.w;                             /* 0x009C SQ */
    matAmbient.xyz = ambient.xyz;                    /* 0x009D SQ */
    matDiffuse = diffuse;                            /* 0x009E SQ */
    drawTint2 = c5;                                  /* 0x009F SQ */
    tintScale.xyz = c6.xyz;                          /* 0x00A0 SQ */
    mem[972].w = c6.w;                               /* 0x00A1 SQ */
    reflectColor = c7;                              /* 0x00A2 SQ */
    flatten = c8.w;                                  /* 0x00A3 SQ */
    vertexTag2.w = c12.w;                            /* 0x00A4 SQ */
    adTag.w = c13.w;                                 /* 0x00A5 SQ */
    materialColor = tint;                           /* 0x00A6 SQ */
    materialChanged = 1;                             /* 0x00A7 IADDIU, 0x00A8 ISW */
    /* falls through into unpackCounts */
}

/* Twelve quadwords in two groups of six, `.xy` of each kept. These are not vectors: the counts and
 * pointers live in `.w` and `.z` carries a GS register address, so one quadword serves both as a
 * scalar the microcode reads and as a GIF A+D entry the packet builders copy out whole. */
static void unpackCounts(void) {
    mem[896].xy = mem[stream++].xy;                  /* 0x00A9 LQI, 0x00AF SQ, the face count */
    mem[897].xy = mem[stream++].xy;                  /* 0x00AA LQI, 0x00B0 SQ, the vertex count */
    mem[898].xy = mem[stream++].xy;                  /* 0x00AB LQI, 0x00B1 SQ, the index list */
    mem[899].xy = mem[stream++].xy;                  /* 0x00AC LQI, 0x00B2 SQ */
    mem[900].xy = mem[stream++].xy;                  /* 0x00AD LQI, 0x00B3 SQ */
    mem[901].xy = mem[stream++].xy;                  /* 0x00AE LQI, 0x00B4 SQ */
    uvMatrix(0).xy = mem[stream++].xy;               /* 0x00B5 LQI, 0x00BB SQ */
    uvMatrix(1).xy = mem[stream++].xy;               /* 0x00B6 LQI, 0x00BC SQ */
    uvMatrix(2).xy = mem[stream++].xy;               /* 0x00B7 LQI, 0x00BD SQ */
    uvMatrixB(0).xy = mem[stream++].xy;              /* 0x00B8 LQI, 0x00BE SQ */
    uvMatrixB(1).xy = mem[stream++].xy;              /* 0x00B9 LQI, 0x00BF SQ */
    uvMatrixB(2).xy = mem[stream++].xy;              /* 0x00BA LQI, 0x00C0 SQ */
    return;                                              /* [E] at 0x00C0; 0x00C1 nop */
}

/* The texture state. The third quadword is packed bytes converted with ITOF0, which is the same
 * color idiom lightSetup uses, and its product with the material's diffuse is `materialColor`.
 * Setting `materialChanged` is what makes lightRebase run once for the material. */
static void unpackTexture(void) {                    /* 0x00C2, from 0x0022 */
    vec4 tint = mem[stream + 2];                     /* 0x00C2 LQ */
    int a = mem_w[stream].x, b = mem_w[stream + 1].x; /* 0x00C3, 0x00C4 ILW */
    vec4 diffuse = matDiffuse;                       /* 0x00C5 LQ */
    tint.xyzw = itof0(tint.xyzw);                    /* 0x00C6 ITOF0 */
    vec4 bias = mem[stream + 3];                     /* 0x00C6 LQ */
    screenScale = screenScaleMem;                    /* 0x00C7 LQ */
    vec4 e = mem[stream + 4], f = mem[stream + 5];   /* 0x00C8, 0x00C9 LQ */

    diffuse.xyzw = diffuse.xyzw * tint.xyzw;         /* 0x00CB MUL */
    materialMask = a;                                /* 0x00CB ISW, mem[915] */
    screenScale.z = screenScale.z + bias.x;          /* 0x00CC ADDx, the depth bias */
    mem_w[916].w = b;                                /* 0x00CC ISW */
    drawTint = tint;                                 /* 0x00CD SQ */
    materialChanged = 1;                             /* 0x00CA IADDIU, 0x00CE ISW */
    mem[1014].x = e.x;                               /* 0x00CF SQ */
    mem[1015].x = f.x;                               /* 0x00D0 SQ */
    materialColor = diffuse;                        /* 0x00D1 SQ */
    return;                                              /* [E] at 0x00D1; 0x00D2 nop */
}

/* Two quadwords in, their .x kept as the .w of two others: the blend-shape weights morphFirst
 * indexes. The MULx by one lane is how the VU moves a value across lanes, since it has no move
 * that crosses them. */
static void unpackTwo(void) {                        /* 0x00D3, from 0x0028 */
    vec4 a = mem[stream], b = mem[stream + 1];       /* 0x00D3, 0x00D4 LQ */
    shapeIndex = 0;                                  /* 0x00D5 ISW */
    morphWeight(0).w = a.x;                          /* 0x00D5 MULx, 0x00D7 SQ */
    morphWeight(1).w = b.x;                          /* 0x00D6 MULx, 0x00D8 SQ */
    return;                                              /* [E] at 0x00D8; 0x00D9 nop */
}

/* Returns the three bits its callers branch on, and leaves the UV scale in vf23 when bit 0 is
 * clear. Both IBNEs test the same condition and both target 0x00E5, the JR's delay slot, which is
 * how the load at 0x00E3 ends up conditional. */
static DrawSelect readFlags(void) {                  /* 0x00DA, from 0x0233 and 0x049A */
    DrawSelect s;
    int mode = drawMode;                             /* 0x00DA ILW */
    s.corrected = 1 & mode;                          /* 0x00DB IADDIU, 0x00DE IAND */
    s.reflect = 1024 & mode;                         /* 0x00DC IADDIU, 0x00DF IAND */
    int flags = drawFlags;                           /* 0x00DD ILW */
    s.objectNormals = 2048;                          /* 0x00E1 IADDIU */
    if (s.corrected == 0) {                          /* 0x00E0, 0x00E2 IBNE */
        uvScale = mem[932];                          /* 0x00E3 LQ */
    }
    s.objectNormals = flags & s.objectNormals;       /* 0x00E5 IAND, the JR's delay slot */
    goto_micro(link);                                /* 0x00E4 JR */
    return s;
}

/* ============================================================ texture coordinates */

/* Two loops that write the texture coordinate to texcoord(v).xy and a flat tint to
 * vertexColor(v). Neither computes a light term, so the tint is one quadword for every vertex;
 * the reflection loop is the exception and zeroes it for a back-facing vertex. readFlags picks
 * between them: the plain one when bit 10 is clear.
 *
 * Both read texcoord(v).z, the reciprocal the transform left there, and neither writes it.
 * Both tails stash the caller's link, jump to 1207 or 1209, and come back through a two-instruction
 * trampoline that restores it; the two trampolines are identical. */
static void texcoordPlain(void) {
    DrawSelect sel = readFlags();
    uvScale = mem[932];                              /* 0x00E7 LQ, the JR's delay slot */
    if (sel.reflect) { texcoordReflect(); return; }  /* 0x00E6 IBNE */
    if (sel.corrected == 0) {                        /* 0x00E8 IBNE; 0x00E9 nop */
        uvScale = mem[933];                          /* 0x00EA LQ, the corrected scale */
    }
    /* .y and .z spread into two more registers so the three can be one MADDz. */
    vec4 rowY, rowZ;
    rowY.x = uvScale.y;                              /* 0x00EB ADDy */
    rowZ.x = uvScale.z;                              /* 0x00EC ADDz */

    vec4 tint;
    tint.w = 1.0f;                                   /* 0x00ED MULw */
    int src = srcNormals;                            /* 0x00ED ILW */
    vec4 offset = mem[972];                          /* 0x00EE LQ */
    tint.xyz = tintScale.xyz;                        /* 0x00EF LQ */
    vec4 color = drawTint;                          /* 0x00F0 LQ */
    int count = vertexCount;                         /* 0x00F1 ILW */
    vec4 n = mem[src++];                             /* 0x00F2 LQI */
    offset.xy = offset.w;                            /* 0x00F3 ADDw */
    int uv = 848;                                    /* 0x00F3 IADDIU */
    tint.xyzw = tint.xyzw * color.xyzw;             /* 0x00F4 MUL */
    int out = 800;                                   /* 0x00F4 IADDIU */

    do {
        vec4 recip;
        recip.z = mem[uv].z;                         /* 0x00F5 LQ */
        count = count - 1;                           /* 0x00F6 ISUBIU */
        offset.x = mul3x3(uvScale, n.xyz);           /* 0x00F7, folds 0x00F5..0x00F7 */
        mem[out++] = tint;                           /* 0x00F7 SQI */
        vec4 st;
        st.xy = offset.xy * recip.z;                 /* 0x00F8 MULz, divided through by w */
        n = mem[src++];                              /* 0x00F8 LQI */
        mem[uv++].xy = st.xy;                        /* 0x00FA SQI; 0x00F9 nop */
    } while (count != 0);                            /* 0x00F9 IBNE */

    int saved = link;                                /* 0x00FB IADD */
    link = atReturnPlain;                            /* 0x00FF IADDIU */
    goto_micro(atPacketSix);                         /* 0x00FC, 0x00FD IADDIU, 0x00FE JR */
    link = saved;                                    /* 0x0102 IADD; 0x0101 nop */
    goto_micro(atEmitTexTriangles);                  /* 0x0100 IADDIU, 0x0101 JR */
}

/* The reflection case. It builds an identity in three registers and replaces it with the object's
 * normal rows when drawFlags bit 11 is clear, then per vertex reflects the transformed normal about
 * the view direction and maps the result into the unit square. That is a sphere map: the texture
 * coordinate is the reflected normal, not anything the mesh carries.
 *
 * The light matrix rows are scaled by sqrt(|mem[972].w|); the SQRT at 0x010E starts it and the
 * MULq at 0x0115 waits for it. */
static void texcoordReflect(void) {                  /* 0x0103, from 0x00E6 */
    vec4 r0, r1, r2;
    r0.yz = 0.0f; r1.xz = 0.0f; r2.xy = 0.0f;        /* 0x0103, 0x0104, 0x0105 MULx */
    r0.x = 1.0f;  r1.y = 1.0f;  r2.z = 1.0f;         /* 0x0106, 0x0107, 0x0108 ADDw */
    int flags = drawFlags;                           /* 0x0103 ILW */
    int src = srcNormals;                            /* 0x0104 ILW */
    vec4 tint;
    tint.xyz = tintScale.xyz;                        /* 0x0105 LQ */
    int useIdentity = flags & 2048;                  /* 0x0106 IADDIU, 0x0107 IAND */
    vec4 color = drawTint;                          /* 0x0108 LQ */
    vec4 zero;
    zero.xyzw = 0.0f;                                /* 0x0109 SUB */
    tint.w = 1.0f;                                   /* 0x010A MULw */
    vec4 q = mem[972];                               /* 0x010A LQ */
    if (useIdentity == 0) {                          /* 0x010A IBNE */
        r0 = mem[999]; r1 = mem[1000]; r2 = mem[1001]; /* 0x010B, 0x010C, 0x010D LQ */
    }
    Q = sqrtf(fabsf(q.w));                           /* 0x010E SQRT */
    I = 0.5f;                                        /* 0x010F LOI */
    vec4 half, view;
    half.z = 0.0f;                                   /* 0x0110 MULx */
    vec4 l0 = mem[929], l1 = mem[930], l2 = mem[931]; /* 0x0110, 0x0111, 0x0112 LQ */
    view.xy = 0.0f;                                  /* 0x0111 MULx */
    view.z = 1.0f;                                   /* 0x0112 ADDw, straight down the axis */
    tint.xyzw = tint.xyzw * color.xyzw;             /* 0x0113 MUL */
    vec4 n = mem[src++];                             /* 0x0113 LQI */
    half.xy = I;                                     /* 0x0114 ADDi */
    int count = vertexCount;                         /* 0x0114 ILW */
    l0.xy = l0.xy * Q;                               /* 0x0115 MULq, waits on the SQRT */
    l1.xy = l1.xy * Q;                               /* 0x0116 MULq */
    int uv = 848;                                    /* 0x0116 IADDIU */
    l2.xy = l2.xy * Q;                               /* 0x0117 MULq */
    int out = 800;                                   /* 0x0117 IADDIU */

    do {                                             /* 0x0118, from 0x0126 */
        vec4 recip;
        recip.z = mem[uv].z;                         /* 0x0118 LQ */
        count = count - 1;                           /* 0x0119 ISUBIU */
        vec4 t;
        t.xyz = mul3x3(r0, n.x);                     /* 0x011A, folds 0x0118..0x011A */
        n = mem[src++];                              /* 0x011C LQI */
        /* view - 2 (t.z) t, spelled as two subtracts of the same product. */
        t.xyz = view.xyz - t.xyz * t.z - t.xyz * t.z; /* 0x011D, folds 0x011B..0x011D */
        t.xyz = half.xyz + mul3x3(l0, t.x);          /* 0x0121, folds 0x011E..0x0121 */
        int frontFacing = 32;                        /* 0x0121 IADDIU */
        t.xy = t.xy * recip.z;                       /* 0x0122 MULz, divided through by w */
        frontFacing = frontFacing & (mac & 0xffff);  /* 0x0122 FMAND, the sign of t.z */
        mem[out++] = tint;                           /* 0x0124 SQI; 0x0123 nop */
        if (frontFacing == 0) {                      /* 0x0123 IBNE */
            mem[out - 1] = zero;                     /* 0x0125 SQ, a back-facing vertex gets none */
        }
        mem[uv++].xy = t.xy;                         /* 0x0127 SQI; 0x0126 nop */
    } while (count != 0);                            /* 0x0126 IBNE */

    vec4 discarded = mem[1008];                      /* 0x0128 LQ, read and not used */
    discarded = mem[1009];                           /* 0x0129 LQ */
    int saved = link;                                /* 0x012A IADD */
    link = atReturnReflect;                          /* 0x012E IADDIU */
    goto_micro(atPacketSixBody);                     /* 0x012B, 0x012C IADDIU, 0x012D JR */
    link = saved;                                    /* 0x0131 IADD; 0x0130 nop */
    goto_micro(atEmitTexTriangles);                  /* 0x012F IADDIU, 0x0130 JR */
}

/* ============================================================ lighting */

/* The sum, three parallel directions plus a base, each dot clamped at zero and the whole clamped
 * to 255 per vertex before interpolation. `flatten` (mem[910].w) lerps the base toward 1.0 and
 * scales the three light colors by 1 - t; it is 0 in every scene measured.
 *
 * The per-draw color multiplies the light colors before the sum, so it is inside the clamp.
 * Two vertices an iteration, with a tail for an odd count. */
static void lightVertex(vec4 color) {               /* 0x0132, from 0x01C6 and 0x01D2 */
    int flags = drawFlags;                           /* 0x0132 ILW */
    vec4 t;
    t.w = flatten;                                   /* 0x0134 LQ */
    int count = vertexCount;                         /* 0x0135 ILW */
    int mode = drawMode;                             /* 0x0137 ILW */
    int lit = flags & 128;                           /* 0x0133 IADDIU, 0x0136 IAND */
    vec4 keep;
    keep.w = 1.0f - t.w;                             /* 0x0139 SUB */
    if (lit == 0) { lightVertexFlat(); return; }     /* 0x0138 IBEQ */

    vec4 base = lightBase;                           /* 0x013C LQ */
    vec4 c0 = lightColor(0);                        /* 0x013E LQ */
    int packetMatrix = 2048 & flags;                 /* 0x013A IADDIU, 0x013B IAND */
    /* mul3x3 below reads three consecutive registers, so naming the first names the matrix. */
    vec4 row0;
    if (packetMatrix == 0) {                         /* 0x013D IBNE */
        row0 = lightNormal(0);                       /* 0x013F, 0x0140, 0x0142 LQ; 0x0141 nop */
    } else {
        row0 = mem[920];                             /* 0x0143, 0x0144, 0x0145 LQ */
    }
    vec4 c1 = lightColor(1), c2 = lightColor(2);   /* 0x0146, 0x0147 LQ */
    base.xyz = t.w + base.xyz * keep.w;              /* 0x0146 ADDAw, 0x0148 MADDw */
    c0.xyz = c0.xyz * keep.w;                        /* 0x0149 MULw */
    c1.xyz = c1.xyz * keep.w;                        /* 0x014A MULw */
    c2.xyz = c2.xyz * keep.w;                        /* 0x014B MULw */
    int normals = srcNormals;                        /* 0x014B ILW */
    int vertexColors = mode & 2048;                 /* 0x0148 IAND */
    if (vertexColors != 0) {                        /* 0x014A IBNE */
        lightVertexColor(row0, base, count, normals);   /* into 0x0173 */
        return;
    }

    vec4 l0, l1, l2, l3;
    l0.xyzw = c0.xyzw * color.xyzw;                 /* 0x014C MUL */
    l1.xyzw = c1.xyzw * color.xyzw;                 /* 0x014D MUL */
    l2.xyzw = c2.xyzw * color.xyzw;                 /* 0x014E MUL */
    l3.xyzw = base.xyzw * color.xyzw;               /* 0x014F MUL */
    vec4 n0 = mem[normals++], n1 = mem[normals++];   /* 0x014C, 0x014D LQI */
    int odd = count & 1;                             /* 0x014E IADDIU, 0x014F IAND */
    count = count - odd;                             /* 0x0150 ISUB */
    int out = 800;                                   /* 0x0151 IADDIU */
    I = 255.0f;                                      /* 0x0152 LOI */

    vec4 d0, d1;
    d0.xyzw = mul3x3(row0, n0.xyz);                  /* 0x0150, 0x0151, 0x0152 */
    d1.xyzw = mul3x3(row0, n1.xyz);                  /* 0x0155, folds 0x0153..0x0155 */
    d0.xyzw = max(d0.xyzw, 0.0f);                    /* 0x0156 MAXx */

    do {                                             /* 0x0157, from 0x0167 */
        d1.xyzw = max(d1.xyzw, 0.0f);                /* 0x0157 MAXx */
        vec4 lit0, lit1;
        lit0.xyzw = l3.xyzw + l0.xyzw * d0.x + l1.xyzw * d0.y + l2.xyzw * d0.z; /* 0x015B, folds 0x0158..0x015B */
        lit1.xyzw = l3.xyzw + l0.xyzw * d1.x + l1.xyzw * d1.y + l2.xyzw * d1.z; /* 0x015C, 0x015E, 0x0160 */
        n0 = mem[normals++];                         /* 0x015D LQI */
        lit0.xyzw = min(lit0.xyzw, 255.0f);          /* 0x015F MINIi, clamped per vertex */
        n1 = mem[normals++];                         /* 0x0161 LQI */
        d0.xyzw = mul3x3(row0, n0.xyz);              /* 0x0163, folds 0x0161..0x0163 */
        mem[out++] = lit0;                           /* 0x0163 SQI */
        lit1.xyzw = min(lit1.xyzw, 255.0f);          /* 0x0164 MINIi */
        d1.xyzw = mul3x3(row0, n1.xyz);              /* 0x0165, 0x0166, 0x0167 */
        count = count - 2;                           /* 0x0165 ISUBIU, two a turn */
        d0.xyzw = max(d0.xyzw, 0.0f);                /* 0x0168 MAXx */
        mem[out++] = lit1;                           /* 0x0168 SQI */
    } while (count != 0);                            /* 0x0167 IBNE */

    if (odd != 0) {                                  /* 0x0169 IBEQ; 0x016A nop */
        vec4 lit0;
        lit0.xyzw = l0.xyzw * d0.x + l1.xyzw * d0.y + l2.xyzw * d0.z + l3.xyzw; /* 0x016E, folds 0x016B..0x016E */
        lit0.xyzw = min(lit0.xyzw, 255.0f);          /* 0x016F MINIi */
        mem[out++] = lit0;                           /* 0x0170 SQI */
    }
    goto_micro(link);                                /* 0x0171 JR; 0x0172 nop */
}

/* The lit case with the mesh's own vertex colors: the draw color is divided by 128 and the vertex
 * color takes its place, so the product is the same 128-is-one convention the GS modulate uses.
 * Plain max(dot, 0) per light, no ramp, one vertex an iteration. */
static void lightVertexColor(vec4 lightRow, vec4 base, int count, int normals) { /* 0x0173 */
    I = 0.0078125f;                                  /* 0x0173 LOI, 1/128 */
    drawColor.xyzw = drawColor.xyzw * I;           /* 0x0174 MULi */
    int colors = srcColors;                        /* 0x0174 ILW */
    int out = 800;                                   /* 0x0175 IADDIU */

    do {
        vec4 n = mem[normals++];                     /* 0x0176 LQI */
        vec4 packed = mem[colors++];                /* 0x0177 LQI */
        n.xyzw = mul3x3(lightRow, n.xyz);            /* 0x017A, folds 0x0178..0x017A */
        packed.xyzw = itof0(packed.xyzw);            /* 0x017B ITOF0 */
        n.xyzw = max(n.xyzw, 0.0f);                  /* 0x017C MAXx */
        vec4 tint;
        tint.xyzw = packed.xyzw * drawColor.xyzw;   /* 0x017D MUL */
        vec4 sum;
        sum.xyzw = base.xyzw + lightColor(0).xyzw * n.x
                             + lightColor(1).xyzw * n.y
                             + lightColor(2).xyzw * n.z; /* 0x017E, 0x017F, 0x0180, 0x0181 */
        I = 255.0f;                                  /* 0x0180 LOI */
        count = count - 1;                           /* 0x0181 ISUBIU */
        sum.xyzw = sum.xyzw * tint.xyzw;             /* 0x0182 MUL */
        sum.xyzw = min(sum.xyzw, 255.0f);            /* 0x0183 MINIi */
        mem[out++] = sum;                            /* 0x0185 SQI; 0x0184 nop */
    } while (count != 0);                            /* 0x0184 IBNE */
    goto_micro(link);                                /* 0x0186 JR; 0x0187 nop */
}

/* The no-normals case. No sum runs: every vertex takes color[4], which the engine stores as
 * colorBase plus color[3]. Giving it either half alone is wrong in both directions, color[3]
 * being four times too dark and colorBase close enough to pass while hiding the formula. */
static void lightVertexFlat(void) {                  /* 0x0188, from 0x0138 */
    vec4 t, keep;
    t.w = flatten;                                   /* 0x0134 LQ */
    keep.w = 1.0f - t.w;                             /* 0x0139 SUB */
    vec4 full = lightColor(4);                      /* 0x0188 LQ, mem[928] */
    vec4 lit;
    lit.xyz = t.w + full.xyz * keep.w;               /* 0x0188 ADDAw, 0x0189 MULAw, 0x018C MADDw */
    I = 255.0f;                                      /* 0x018A LOI */
    int vertexColors = drawMode & 128;              /* 0x0189 IAND */
    if (vertexColors != 0) {                        /* 0x018B IBNE */
        lightFlatColor(lit, vertexCount);               /* into 0x019D */
        return;
    }
    lit.xyzw = min(lit.xyzw * drawColor.xyzw, 255.0f); /* 0x0190 MUL, 0x0194 MINIi */

    int count = vertexCount, out = 800;              /* 0x0135 ILW, 0x018C IADDIU */
    int tail = count & 3;                            /* 0x018D IADDIU, 0x018E IAND */
    if (tail != 0) {                                 /* 0x0190 IBEQ */
        count = count - tail;                        /* 0x0192 ISUB */
        goto_micro(atFillFourTail - tail);           /* 0x018F, 0x0191 ISUB, 0x0193 JR */
    }
    fillFour(lit, count, out);
}

/* Four vertices an iteration, so the caller has already rounded the count down to a multiple of
 * four and jumped into the middle of this run for the remainder. */
static void fillFour(vec4 color, int count, int out) { /* 0x0195, from 0x0199 */
    do {
        count = count - 4;                           /* 0x0195 ISUBIU */
        mem[out++] = color;                         /* 0x0196 SQI */
        mem[out++] = color;                         /* 0x0197 SQI */
        mem[out++] = color;                         /* 0x0198 SQI */
        mem[out++] = color;                         /* 0x019A SQI; 0x0199 nop */
    } while (count != 0);                            /* 0x0199 IBNE */
    goto_micro(link);                                /* 0x019B JR; 0x019C nop */
}

/* Flat with vertex colors: color[4] times the draw color over 128, times each vertex's own.
 * Two vertices an iteration with the loads and conversions staggered a step ahead of the stores;
 * 0x01A0..0x01A5 are that pipeline's prologue. The loop exits on IBGTZ, so an odd count leaves the
 * counter at -1 and the tail at 0x01AE runs. */
static void lightFlatColor(vec4 flat, int count) {  /* 0x019D, from 0x018B */
    int colors = srcColors;                        /* 0x019D ILW */
    I = 0.0078125f;                                  /* 0x019E LOI */
    drawColor.xyzw = drawColor.xyzw * I;           /* 0x019F MULi */
    int out = 800;                                   /* 0x019F IADDIU */

    vec4 packed0 = mem[colors++];                   /* 0x01A0 LQI */
    flat.xyzw = flat.xyzw * drawColor.xyzw;         /* 0x01A1 MUL */
    vec4 packed1 = mem[colors++];                   /* 0x01A1 LQI */
    vec4 c0, c1, out0, out1;
    c0.xyzw = itof0(packed0.xyzw);                   /* 0x01A2 ITOF0 */
    c1.xyzw = itof0(packed1.xyzw);                   /* 0x01A3 ITOF0 */
    packed0 = mem[colors++];                        /* 0x01A3 LQI */
    out0.xyzw = c0.xyzw * flat.xyzw;                 /* 0x01A4 MUL */
    packed1 = mem[colors++];                        /* 0x01A4 LQI */
    c0.xyzw = itof0(packed0.xyzw);                   /* 0x01A5 ITOF0 */

    do {
        out1.xyzw = c1.xyzw * flat.xyzw;             /* 0x01A6 MUL */
        packed0 = mem[colors++];                    /* 0x01A6 LQI */
        c1.xyzw = itof0(packed1.xyzw);               /* 0x01A7 ITOF0 */
        mem[out++] = out0;                           /* 0x01A7 SQI */
        out0.xyzw = c0.xyzw * flat.xyzw;             /* 0x01A8 MUL */
        count = count - 2;                           /* 0x01A8 ISUBIU */
        packed1 = mem[colors++];                    /* 0x01A9 LQI */
        c0.xyzw = itof0(packed0.xyzw);               /* 0x01AA ITOF0 */
        mem[out++] = out1;                           /* 0x01AB SQI */
    } while (count > 0);                             /* 0x01AA IBGTZ */

    if (count != 0) {                                /* 0x01AC IBEQ; 0x01AD nop */
        mem[out++] = c0;                             /* 0x01AE SQI, the odd vertex */
    }
    goto_micro(link);                                /* 0x01AF JR; 0x01B0 nop */
}

/* The simplest case: every vertex takes one color. Reached from texcoordSecond and drawFront. */
static void fillOne(vec4 color) {                   /* 0x01B1, from 0x02F1 and 0x05C6 */
    int count = vertexCount;                         /* 0x01B1 ILW */
    int out = 800;                                   /* 0x01B2 IADDIU */
    do {
        count = count - 1;                           /* 0x01B3 ISUBIU */
        mem[out++] = color;                         /* 0x01B6 SQI; 0x01B4, 0x01B5 nop */
    } while (count != 0);                            /* 0x01B5 IBNE */
    goto_micro(link);                                /* 0x01B7 JR; 0x01B8 nop */
}

/* ============================================================ the draw driver */

/* Two return conventions run side by side. A BAL sets the link register to the address after it, so
 * a callee's JR comes back inline; the pipeline's own return goes on the stack at vi14 instead,
 * because the callees in between overwrite the link for their own calls.
 *
 * drawMode bits: 0x4000 draw back faces too, 0x0010 the texture-matrix path, 0x1000 a further pass.
 * drawFlags bit 0x0080 says the batch carries normals at all. */

static void drawEntry(void) {                        /* 0x01B9, from 0x000C */
    unpackPointers(mem_w[stream].w);                 /* 0x01BA ILW, 0x01B9 BAL */
    lightRebase();                                       /* falls through into 0x01BB */
}

/* Runs once per material and clears the flag that made it run. Light 0's color is a *difference*,
 * not a color, and the sum's base is color[3] rather than colorBase: a vertex facing light 0
 * reaches diffuse * colorBase and one facing away falls to ambient * color[3]. The packet's own
 * color[0] is scratch and never survives this.
 *
 * Confirmed live with PCSX2HOOK_VU_WATCH on 923/924/927/1002/1003/1004: base(1003) is
 * (0.2969, 0.1953, 0.1953) = ambient * color3, and color0(924) is (0.7031, 0.5, 0.5) =
 * diffuse * colorBase - base, exactly, with both material terms 1.0. */
static void lightRebase(void) {                      /* 0x01BB */
    int changed = materialChanged;                   /* 0x01BB ILW */
    vec4 diffuse = matDiffuse;                       /* 0x01BC LQ */
    vec4 cb = colorBase;                             /* 0x01BD LQ */
    vec4 ambient = matAmbient;                       /* 0x01BE LQ */
    vec4 base;
    base.w = 1.0f;                                   /* 0x01C0 MULw */
    vec4 c3 = lightColor(3);                        /* 0x01C0 LQ, mem[927] */
    if (changed != 0) {                              /* 0x01BF IBEQ */
        base.xyz = diffuse.xyz * cb.xyz;             /* 0x01C1 MULA */
        materialChanged = 0;                         /* 0x01C1 ISW */
        vec4 amb;
        amb.xyz = ambient.xyz * c3.xyz;              /* 0x01C2 MUL */
        vec4 diff;
        diff.xyz = base.xyz - ambient.xyz * c3.xyz;  /* 0x01C3 MSUB */
        lightBase = amb;                             /* 0x01C4 SQ, mem[1003] */
        lightColor(0).xyz = diff.xyz;               /* 0x01C5 SQ, mem[924] */
    }
    drawBatch();
}

/* Lights the batch, transforms its texture coordinates, emits its triangles, and, when the draw
 * asks for both faces, negates the normals and does the whole thing again with the winding
 * reversed. */
static void drawBatch(void) {                        /* 0x01C6 */
    drawColor = materialColor;                     /* 0x01C7 LQ */
    lightVertex(drawColor);                         /* 0x01C6 BAL */

    int mode = drawMode;                             /* 0x01C8 ILW */
    int bothFaces = mode & 16384;                    /* 0x01C9 IADDIU, 0x01CA IAND */
    link = atSharedReturn;                           /* 0x01CD IADDIU; 0x01CB, 0x01CC nop */
    if (bothFaces == 0) { drawFront(); return; }     /* 0x01CC IBEQ */

    drawFront();                                     /* 0x01CE BAL; 0x01CF nop */
    negateNormals();                                 /* 0x01D0 BAL; 0x01D1 nop */
    drawColor = materialColor;                     /* 0x01D3 LQ */
    lightVertex(drawColor);                         /* 0x01D2 BAL, the normals just changed sign */

    mode = drawMode;                                 /* 0x01D4 ILW */
    int useMatrix = mode & 16;                       /* 0x01D5 IADDIU, 0x01D7 IAND */
    link = atSharedReturn;                           /* 0x01D6 IADDIU */
    mem_w[--kickTop].x = link;                       /* 0x01D8 ISUBIU, 0x01DA ISW; 0x01D9 nop */
    if (useMatrix == 0) { drawEmit(); return; }      /* 0x01D9 IBEQ */
    texcoordMatrix();                                /* 0x01DB BAL; 0x01DC nop */
    drawSecond();                                    /* 0x01DD B; 0x01DE nop */
}

/* Negates every normal in place, two an iteration with an odd tail, then reverses the winding.
 * drawFlags bit 7 says the batch has normals; without it only the winding swap runs. The negation
 * is a SUB against the zero register, since the VU has no unary minus. */
static void negateNormals(void) {                    /* 0x01DF, from 0x01D0 and 0x042B */
    int flags = drawFlags;                           /* 0x01DF ILW */
    int hasNormals = 128 & flags;                    /* 0x01E0 IADDIU, 0x01E2 IAND */
    int src = srcNormals;                            /* 0x01E1 ILW */
    int count = vertexCount;                         /* 0x01E3 ILW */
    int odd = 1;                                     /* 0x01E5 IADDIU; 0x01E4 nop */
    if (hasNormals == 0) { swapIndices(); return; }  /* 0x01E4 IBEQ */

    vec4 a, b, na, nb;
    a.xyz = mem[src++].xyz;                          /* 0x01E6 LQI */
    b.xyz = mem[src++].xyz;                          /* 0x01E7 LQI */
    odd = odd & count;                               /* 0x01E8 IAND */
    na.xyz = -a.xyz;                                 /* 0x01E9 SUB */
    count = count - odd;                             /* 0x01E9 ISUB, rounded down to even */

    do {
        nb.xyz = -b.xyz;                             /* 0x01EA SUB */
        a.xyz = mem[src++].xyz;                      /* 0x01EA LQI */
        b.xyz = mem[src++].xyz;                      /* 0x01EB LQI */
        count = count - 2;                           /* 0x01EC ISUBIU */
        mem[src - 4].xyz = na.xyz;                   /* 0x01ED SQ, two vertices back */
        na.xyz = -a.xyz;                             /* 0x01EE SUB */
        mem[src - 3].xyz = nb.xyz;                   /* 0x01EF SQ */
    } while (count != 0);                            /* 0x01EE IBNE */

    if (odd != 0) {                                  /* 0x01F0 IBEQ; 0x01F1 nop */
        mem[src - 2].xyz = na.xyz;                   /* 0x01F2 SQ */
    }
    swapIndices();
}

/* Exchanges .y and .z of every index quadword, so emitTriangles' signed-area test keeps the face
 * it just rejected. */
static void swapIndices(void) {                      /* 0x01F3, from 0x01E4 */
    int idx = indexList;                             /* 0x01F3 ILW */
    int count = faceCount;                           /* 0x01F4 ILW */
    do {
        int b = mem_w[idx].y;                        /* 0x01F5 ILW */
        int c = mem_w[idx].z;                        /* 0x01F6 ILW */
        count = count - 1;                           /* 0x01F7 ISUBIU */
        idx = idx + 1;                               /* 0x01F8 IADDIU */
        mem_w[idx - 1].z = b;                        /* 0x01F9 ISW */
        mem_w[idx - 1].y = c;                        /* 0x01FB ISW; 0x01FA nop */
    } while (count != 0);                            /* 0x01FA IBNE */
    goto_micro(link);                                /* 0x01FC JR; 0x01FD nop */
}

/* Each source UV through the 2x3 affine at mem[974..976], divided by w, into texcoord(v).xy. This
 * is the runtime side of `sdfMatSetUvwParm`'s six floats.
 *
 * Pipelined a vertex deep: the store writes the previous vertex's result and 0x020F writes the last
 * one after the loop, which is why the count is compared against the cursor rather than counted
 * down. The reciprocal quadword is reloaded each turn only for its .z. */
static void texcoordMatrix(void) {                   /* 0x01FE, from 0x01DB and 0x0436 */
    int src = srcTexcoords;                          /* 0x01FE ILW */
    vec4 m0, m1, m2;
    m0.xy = uvMatrix(0).xy;                          /* 0x01FF LQ */
    m1.xy = uvMatrix(1).xy;                          /* 0x0200 LQ */
    m2.xy = uvMatrix(2).xy;                          /* 0x0201 LQ, the offset */
    vec4 uv = mem[src++];                            /* 0x0202 LQI */
    vec4 recip = mem[848];                           /* 0x0203 LQ, vertex 0's */
    int v = 1;                                       /* 0x0204 IADDIU */
    vec4 st;
    st.xy = m0.xy * uv.x + m1.xy * uv.y + m2.xy;     /* 0x0205, folds 0x0203..0x0205 */
    vec4 next = texcoord(v);                         /* 0x0205 LQ */
    uv = mem[src++];                                 /* 0x0206 LQI */
    st.xy = st.xy * recip.z;                         /* 0x0207 MULz, divided through by w */
    int count = vertexCount;                         /* 0x0207 ILW */

    do {
        vec4 acc;
        acc.xy = m0.xy * uv.x;                       /* 0x0208 MULAx */
        v = v + 1;                                   /* 0x0208 IADDIU */
        acc.xy = acc.xy + m1.xy * uv.y;              /* 0x0209 MADDAy */
        texcoord(v - 2).xy = st.xy;                  /* 0x0209 SQ, the previous vertex */
        next.xy = acc.xy + m2.xy;                    /* 0x020A MADDw */
        uv = mem[src++];                             /* 0x020A LQI */
        st.z = next.z;                               /* 0x020B MOVE, keep the reciprocal */
        st.xy = next.xy * next.z;                    /* 0x020C MULz */
        next = texcoord(v);                          /* 0x020D LQ */
    } while (count != v);                            /* 0x020C IBNE */

    texcoord(v - 1).xy = st.xy;                      /* 0x020F SQ, the last vertex; 0x020E JR */
    goto_micro(link);                                /* 0x020E JR */
}

/* One pass: pick the texture-coordinate routine by drawMode bit 4, then fall into the emit. The
 * push at 0x0212 writes below the stack pointer that 0x0213 then decrements, so the two are one
 * push. 0x0213 is also a jump target from texcoordSecond and reflectPass. */
static void drawFront(void) {                        /* 0x0210, from 0x01CC and 0x01CE */
    int mode = drawMode;                             /* 0x0210 ILW */
    int useMatrix = 16;                              /* 0x0211 IADDIU */
    mem_w[kickTop - 1].x = link;                     /* 0x0212 ISW */
    kickTop = kickTop - 1;                           /* 0x0213 ISUBIU */
    useMatrix = mode & useMatrix;                    /* 0x0214 IAND */
    if (useMatrix != 0) {                            /* 0x0216 IBNE; 0x0215, 0x0217 nop */
        vec4 m0;
        m0.xy = uvMatrix(0).xy;                      /* 0x021E LQ */
        m0.xy = uvMatrix(1).xy;                      /* 0x021F LQ */
        m0.xy = uvMatrix(2).xy;                      /* 0x0220 LQ, also reached from 0x0193 */
        link = atDrawSecond;                         /* 0x0223 IADDIU */
        goto_micro(atTransformUv);                   /* 0x0221 IADDIU, 0x0222 JR */
    }
    transformVertices();                             /* 0x0218 BAL; 0x0219 nop */
    drawEmit();
}

/* 559 is drawReturn's own entry, so emitTriangles comes back there rather than here. */
static void drawEmit(void) {                         /* 0x021A, from 0x01D9 */
    packetOne();                                     /* 0x021A BAL; 0x021B nop */
    link = atDrawReturn;                             /* 0x021D IADDIU */
    emitTriangles();                                 /* 0x021C B */
}

/* The second pass. drawMode bit 12 picks a further one at texcoordSecond, which relights through
 * whichever routine the caller named: 306 here is lightVertex. */
static void drawSecond(void) {                       /* 0x0224, from 0x01DD */
    packetThree();                                   /* 0x0224 BAL; 0x0225 nop */
    emitTexTriangles();                              /* 0x0226 BAL; 0x0227 nop */
    int mode = drawMode;                             /* 0x0228 ILW */
    int further = mode & 4096;                       /* 0x0229 IADDIU, 0x022A IAND */
    int lightRoutine = atLightVertex;                /* 0x022B IADDIU */
    int emitter = atEmitTexTriangles;                /* 0x022C IADDIU, emitTexTriangles */
    link = atDrawReturn;                             /* 0x022E IADDIU; 0x022D nop */
    if (further != 0) {                              /* 0x022D IBNE */
        texcoordSecond(lightRoutine, emitter);
        return;
    }
    drawReturn();
}

/* The reflection pass, then the return pushed at 0x01DA or 0x0212, reached through readFlags: that
 * routine ends in a JR through the link register, so branching into it returns to the caller and
 * refreshes the flag registers on the way. */
static void drawReturn(void) {                       /* 0x022F */
    link = atDrawReturnPop;                          /* 0x0231 IADDIU; 0x0230 nop */
    goto_micro(atReflectPass);                       /* 0x022F IADDIU, 0x0230 JR */
    link = mem_w[kickTop].x;                         /* 0x0232 ILW */
    kickTop = kickTop + 1;                           /* 0x0234 IADDIU; 0x0233 nop */
    readFlags();                                     /* 0x0233 B */
}

/* ============================================================ the emitters */

/* Two emitters, one untextured and one textured, and the two routines that drain the retry list
 * they build. emitTriangles writes two registers a vertex, color and position; emitTexTriangles
 * writes three, the texture coordinate as well. That is what settles the per-vertex arrays:
 * texcoord(v).xy is the ST, vertexColor(v) the RGBA and position(v) the XYZ.
 *
 * A triangle that crosses the frustum is threaded onto the retry list at mem[1021].w rather than
 * clipped inline, and only after six trivial rejects fail to throw it away. Each test asks whether
 * all three vertices are outside one plane; the masks are the complements of 0x001041, 0x002082,
 * 0x004104, 0x008208, 0x010410 and 0x020820, which are bits 0/6/12 through 5/11/17. */

static void emitTriangles(void) {                    /* 0x0235, from 0x021C and 0x0487 */
    int out = cursors.y;                             /* 0x0235 ILW */
    vec4 tag = vertexTag2;                           /* 0x0236 LQ, .yz only */
    mem_w[--kickTop].x = link;                       /* 0x0237 ISW, 0x0238 ISUBIU */
    int emitted = 1;                                 /* 0x0239 IADDIU */
    int idx = indexList;                             /* 0x023A ILW */
    mem[out++].yz = tag.yz;                          /* 0x023B SQI, the GIF tag */
    int faces = faceCount;                           /* 0x023C ILW */
    int chain = 1021;                                /* 0x023D IADDIU, the retry list head */
    int a = mem_w[idx].x, b = mem_w[idx].y, c = mem_w[idx].z; /* 0x023E, 0x023F, 0x0240 ILW */
    vec4 refPoint = windingRef;                      /* 0x0241 LQ */
    vec4 pa = position(a), pb = position(b), pc = position(c); /* 0x0242, 0x0243, 0x0244 LQ */

    do {                                             /* 0x0245, from 0x026A */
        vec4 e0, e1, toRef;
        e0.xyz = pa.xyz - pb.xyz;                    /* 0x0245 SUB */
        vec4 ra = texcoord(a);                       /* 0x0245 LQ */
        e1.xyz = pc.xyz - pb.xyz;                    /* 0x0246 SUB */
        vec4 rb = texcoord(b);                       /* 0x0246 LQ */
        toRef.xyz = refPoint.xyz - pb.xyz;           /* 0x0247 SUB */
        vec4 rc = texcoord(c);                       /* 0x0247 LQ */

        /* CLIP shifts six bits in per call, so after three the low 18 hold the whole triangle. */
        clip = (clip << 6) | clip_bits(pa.xyz, pa.w); /* 0x0248 CLIP */
        vec4 ca = vertexColor(a);                   /* 0x0248 LQ */
        clip = (clip << 6) | clip_bits(pb.xyz, pb.w); /* 0x0249 CLIP */
        vec4 cb = vertexColor(b);                   /* 0x0249 LQ */
        vec4 area;
        area.xyz = cross(e0, e1);                    /* 0x024A OPMULA, 0x024B OPMSUB */
        vec4 cc = vertexColor(c);                   /* 0x024A LQ */
        a = mem_w[idx + 1].x;                        /* 0x024B ILW, the next triangle */
        clip = (clip << 6) | clip_bits(pc.xyz, pc.w); /* 0x024C CLIP */
        b = mem_w[idx + 1].y;                        /* 0x024C ILW */

        vec4 sa, sb, sc;
        sa.xyz = pa.xyz * ra.z;                      /* 0x024D MULz, times the reciprocal */
        c = mem_w[idx + 1].z;                        /* 0x024D ILW */
        sb.xyz = pb.xyz * rb.z;                      /* 0x024E MULz */
        faces = faces - 1;                           /* 0x024E ISUBIU */

        /* The winding test: the cross product projected onto the vector to the reference point. */
        area.xyz = area.xyz * toRef.xyz;             /* 0x024F MUL */
        sa.w = ra.w;                                 /* 0x024F MOVE, the fog */
        sc.xyz = pc.xyz * rc.z;                      /* 0x0250 MULz */
        int crosses = (clip & 0x03ffff) != 0;        /* 0x0250 FCAND */

        ca.xyzw = ftoi0(ca.xyzw);                    /* 0x0251 FTOI0, colors to bytes */
        sb.w = rb.w;                                 /* 0x0251 MOVE */
        cb.xyzw = ftoi0(cb.xyzw);                    /* 0x0252 FTOI0 */
        sc.w = rc.w;                                 /* 0x0252 MOVE */

        area.x = area.x + area.y + one.x * area.z;   /* 0x0254 MADDz, folds 0x0253 */
        pa = position(a);                            /* 0x0253 LQ, the next triangle */
        pb = position(b);                            /* 0x0254 LQ */
        pc = position(c);                            /* 0x0255 LQ */

        cc.xyzw = ftoi0(cc.xyzw);                    /* 0x0256 FTOI0 */
        mem[out] = ca;                               /* 0x0256 SQ, the first color */

        vec4 acc;
        acc.xyzw = screenOffset.xyzw;                /* 0x0257 MULAw */
        mem[out + 2] = cb;                           /* 0x0257 SQ */
        sa.xyz = acc.xyz + sa.xyz * screenScale.xyz; /* 0x0258 MADD, into screen space */
        int backfacing = 128;                        /* 0x0258 IADDIU */
        sb.xyz = acc.xyz + sb.xyz * screenScale.xyz; /* 0x0259 MADD */
        backfacing = backfacing & (mac & 0xffff);    /* 0x0259 FMAND, the area's sign bit */
        sc.xyz = acc.xyz + sc.xyz * screenScale.xyz; /* 0x025A MADD */
        mem[out + 4] = cc;                           /* 0x025B SQ */
        if (backfacing != 0) goto next;              /* 0x025A, 0x025B IBNE */

        sa.xyzw = ftoi4(sa.xyzw);                    /* 0x025C FTOI4, 12.4 fixed point */
        sb.xyzw = ftoi4(sb.xyzw);                    /* 0x025D FTOI4 */
        if (crosses == 0) goto keep;                 /* 0x025C, 0x025D IBEQ */

        crosses = clipReject();                      /* 0x025E BAL; 0x025F, 0x0261 nop */
        if (crosses != 0) goto next;                 /* 0x0260 IBNE, thrown away outright */

        mem_w[chain].w = idx;                        /* 0x0262 ISW, onto the retry list */
        chain = idx;                                 /* 0x0264 IADD */
        goto next;                                   /* 0x0263 B */

    keep:                                            /* 0x0265, from 0x025C */
        sc.xyzw = ftoi4(sc.xyzw);                    /* 0x0265 FTOI4 */
        out = out + 6;                               /* 0x0265 IADDIU, three color/XYZ pairs */
        emitted = emitted + 3;                       /* 0x0266 IADDIU */
        mem[out - 5] = sa;                           /* 0x0267 SQ, beside its color */
        mem[out - 3] = sb;                           /* 0x0268 SQ */
        mem[out - 1] = sc;                           /* 0x0269 SQ */

    next:                                            /* 0x026A, from 0x025A, 0x0260, 0x0263 */
        idx = idx + 1;                               /* 0x026B IADDIU */
    } while (faces != 0);                            /* 0x026A IBNE */

    mem_w[chain].w = 0;                              /* 0x026C ISW, terminate the retry list */
    int tagAt = cursors.y;                           /* 0x026D ILW */
    emitted = emitted + 32767;                       /* 0x026E IADDIU, wraps to emitted - 1 */
    mem_w[tagAt].x = emitted;                        /* 0x0270 ISW, the tag's NLOOP */
    kick();                                          /* 0x026F BAL */
    drainClip();
}

/* Walks the retry list one triangle a pass. The clipper returns to `emitter` for the vertices it
 * produced or to this routine's own head for the next entry, so those two constants are the loop.
 * An empty list pops the return pushed at 0x0237 and leaves. */
static void drainClip(void) {                        /* 0x0271 */
    int triangle = retryHead;                        /* 0x0271 ILW */
    int emitter = atEmitClipped;                     /* 0x0272 IADDIU */
    link = atDrainClip;                              /* 0x0273 IADDIU, this routine's head */
    int next = mem_w[triangle].w;                    /* 0x0274 ILW */
    retryHead = next;                                /* 0x0276 ISW; 0x0275 nop */
    if (triangle != 0) { clipTriangle(triangle, emitter); return; } /* 0x0275 IBNE */

    link = mem_w[kickTop].x;                         /* 0x0277 ILW */
    kickTop = kickTop + 1;                           /* 0x0279 IADDIU; 0x0278 nop */
    goto_micro(link);                                /* 0x0278 JR */
}

/* The clipper leaves a flat vertex list, three quadwords a vertex in the order position, color,
 * texture coordinate. This projects and writes it out two registers a vertex.
 *
 * The SUB at 0x0287 reads a register nothing in this loop writes and its result is never stored:
 * dead here, and only meaningful to whichever caller shares the register. */
static void emitClipped(void) {                      /* 0x027A, from 0x031A, 0x034B, 0x056C */
    int src = clipBufA;                              /* 0x027A ILW */
    vec4 tag;
    tag.yz = vertexTag2.yz;                          /* 0x027C LQ */
    packetClipped(tag, emittedCount);                /* 0x027B BAL; the GIF tag */

    do {
        vec4 p = mem[src++];                         /* 0x027D LQI, the position */
        vec4 color = mem[src++];                    /* 0x027E LQI */
        vec4 extra = mem[src++];                     /* 0x027F LQI, only .w is read */
        Q = 1.0f / p.w;                              /* 0x0280 DIV */
        color.xyzw = ftoi0(color.xyzw);            /* 0x0281 FTOI0 */
        emittedCount = emittedCount - 1;             /* 0x0281 ISUBIU */
        vec4 acc;
        acc.xyz = screenOffset.xyz;                  /* 0x0282 MULAw */
        outCursor = outCursor + 2;                   /* 0x0282 IADDIU, two registers a vertex */
        vec4 s;
        s.w = extra.w;                               /* 0x0283 MOVE */
        vec4 ndc;
        ndc.xyz = p.xyz * Q;                         /* 0x0284 MULq, waits on the DIV */
        s.xyz = acc.xyz + ndc.xyz * screenScale.xyz; /* 0x0285 MADD */
        mem[outCursor - 2] = color;                 /* 0x0285 SQ */
        s.xyzw = ftoi4(s.xyzw);                      /* 0x0286 FTOI4 */
        vec4 dead;
        dead.xyz = ndc.xyz - dead.xyz;               /* 0x0287 SUB, dead */
        mem[outCursor - 1] = s;                      /* 0x0287 SQ */
    } while (emittedCount != 0);                     /* 0x0286 IBNE */
    kick();                                          /* 0x0288 B; 0x0289 nop */
}

/* One triangle an iteration: cull by signed area, trivially reject or defer what crosses the
 * frustum, and otherwise write three vertices of [ST, RGBA, XYZ]. The loads run a triangle ahead
 * throughout, which is why the index triple and the positions are fetched mid-body. */
static void emitTexTriangles(void) {                 /* 0x028A, from 0x0226 and 0x048F */
    int idx = indexList;                             /* 0x028A ILW */
    outCursor = cursors.y;                           /* 0x028B ILW */
    vec4 tag = vertexTag3;                           /* 0x028C LQ */
    emittedCount = 1;                                /* 0x028D IADDIU */
    int a = mem_w[idx].x, b = mem_w[idx].y, c = mem_w[idx].z; /* 0x028E, 0x028F, 0x0290 ILW */
    mem[outCursor++] = tag;                          /* 0x0291 SQI */
    vec4 pa = position(a), pb = position(b), pc = position(c); /* 0x0292, 0x0293, 0x0294 LQ */
    vec4 refPoint = windingRef;                      /* 0x0295 LQ */
    vec4 e0, e1, toRef;
    e0.xyz = pa.xyz - pb.xyz;                        /* 0x0296 SUB */
    int chain = 1021;                                /* 0x0296 IADDIU */
    e1.xyz = pc.xyz - pb.xyz;                        /* 0x0297 SUB */
    int faces = faceCount;                           /* 0x0297 ILW */
    toRef.xyz = refPoint.xyz - pb.xyz;               /* 0x0298 SUB */
    mem_w[kickTop - 1].x = link;                     /* 0x0298 ISW */
    kickTop = kickTop - 1;                           /* 0x0299 ISUBIU */

    do {                                             /* 0x029A, from 0x02C8 */
        vec4 area;
        area.xyz = cross(e0, e1);                    /* 0x029A OPMULA, 0x029B OPMSUB */
        vec4 sta = texcoord(a), stb = texcoord(b), stc = texcoord(c); /* 0x029A, 0x029B, 0x029C LQ */
        clip = (clip << 6) | clip_bits(pa.xyz, pa.w); /* 0x029C CLIP */
        clip = (clip << 6) | clip_bits(pb.xyz, pb.w); /* 0x029D CLIP */
        vec4 ca = vertexColor(a), cb = vertexColor(b); /* 0x029D, 0x029E LQ */
        clip = (clip << 6) | clip_bits(pc.xyz, pc.w); /* 0x029E CLIP */
        area.xyz = area.xyz * toRef.xyz;             /* 0x029F MUL */
        vec4 cc = vertexColor(c);                   /* 0x029F LQ */

        vec4 sa, sb, sc;
        sa.xyz = pa.xyz * sta.z;                     /* 0x02A0 MULz */
        b = mem_w[idx + 1].y;                        /* 0x02A0 ILW, the next triangle */
        sb.xyz = pb.xyz * stb.z;                     /* 0x02A1 MULz */
        a = mem_w[idx + 1].x;                        /* 0x02A1 ILW */
        sc.xyz = pc.xyz * stc.z;                     /* 0x02A2 MULz */
        c = mem_w[idx + 1].z;                        /* 0x02A2 ILW */
        faces = faces - 1;                           /* 0x02A3 ISUBIU */
        area.x = area.x + area.y + one.x * area.z;   /* 0x02A4, folds 0x02A3..0x02A4 */
        pb = position(b);                            /* 0x02A4 LQ */
        ca.xyzw = ftoi0(ca.xyzw);                    /* 0x02A5 FTOI0 */
        pa = position(a);                            /* 0x02A5 LQ */
        cb.xyzw = ftoi0(cb.xyzw);                    /* 0x02A6 FTOI0 */
        pc = position(c);                            /* 0x02A6 LQ */
        sa.w = sta.w;                                /* 0x02A7 MULw, the fog */
        int backfacing = 128;                        /* 0x02A7 IADDIU */

        /* The next triangle's edges, so the top of the loop has them. */
        toRef.xyz = refPoint.xyz - pb.xyz;           /* 0x02A8 SUB */
        backfacing = backfacing & (mac & 0xffff);    /* 0x02A8 FMAND */
        e0.xyz = pa.xyz - pb.xyz;                    /* 0x02A9 SUB */
        e1.xyz = pc.xyz - pb.xyz;                    /* 0x02AA SUB */
        int crosses = (clip & 0x03ffff) != 0;        /* 0x02AA FCAND */
        if (backfacing != 0) goto next;              /* 0x02A9 IBNE */

        sb.w = stb.w;                                /* 0x02AB MULw */
        vec4 acc;
        acc.xyzw = screenOffset.xyzw;                /* 0x02AC MULAw */
        sc.w = stc.w;                                /* 0x02AC MOVE */
        if (crosses != 0) {                          /* 0x02AB IBEQ */
            /* Six trivial rejects, one clip plane each. */
            if ((clip | 0xfdf7df) == 0xffffff) goto next; /* 0x02AD FCOR, 0x02AE IBNE, z < -w */
            if ((clip | 0xfefbef) == 0xffffff) goto next; /* 0x02AF FCOR, 0x02B0 IBNE, z > w */
            if ((clip | 0xff7df7) == 0xffffff) goto next; /* 0x02B1 FCOR, 0x02B2 IBNE, y < -w */
            if ((clip | 0xffbefb) == 0xffffff) goto next; /* 0x02B3 FCOR, 0x02B4 IBNE, y > w */
            if ((clip | 0xffdf7d) == 0xffffff) goto next; /* 0x02B5 FCOR, 0x02B6 IBNE, x < -w */
            if ((clip | 0xffefbe) == 0xffffff) goto next; /* 0x02B7 FCOR, 0x02B8 IBNE, x > w */
            mem_w[chain].w = idx;                    /* 0x02BA ISW, onto the retry list */
            chain = idx;                             /* 0x02BC IADD; 0x02B9, 0x02BB nop */
            goto next;                               /* 0x02BB B */
        }

        sa.xyz = acc.xyz + sa.xyz * screenScale.xyz; /* 0x02BD MADD, into screen space */
        mem[outCursor] = sta;                        /* 0x02BD SQ, the first ST */
        sb.xyz = acc.xyz + sb.xyz * screenScale.xyz; /* 0x02BE MADD */
        mem[outCursor + 3] = stb;                    /* 0x02BE SQ */
        sc.xyz = acc.xyz + sc.xyz * screenScale.xyz; /* 0x02BF MADD */
        mem[outCursor + 6] = stc;                    /* 0x02BF SQ */
        cc.xyzw = ftoi0(cc.xyzw);                    /* 0x02C0 FTOI0 */
        mem[outCursor + 1] = ca;                     /* 0x02C0 SQ, the first color */
        sa.xyzw = ftoi4(sa.xyzw);                    /* 0x02C1 FTOI4 */
        mem[outCursor + 4] = cb;                     /* 0x02C1 SQ */
        sb.xyzw = ftoi4(sb.xyzw);                    /* 0x02C2 FTOI4 */
        outCursor = outCursor + 9;                   /* 0x02C2 IADDIU, three vertices */
        sc.xyzw = ftoi4(sc.xyzw);                    /* 0x02C3 FTOI4 */
        emittedCount = emittedCount + 3;             /* 0x02C3 IADDIU */
        mem[outCursor - 2] = cc;                     /* 0x02C4 SQ */
        mem[outCursor - 7] = sa;                     /* 0x02C5 SQ, the first position */
        mem[outCursor - 4] = sb;                     /* 0x02C6 SQ */
        mem[outCursor - 1] = sc;                     /* 0x02C7 SQ */

    next:                                            /* 0x02C8, from 0x02A9 and 0x02AE..0x02BB */
        idx = idx + 1;                               /* 0x02C9 IADDIU */
    } while (faces != 0);                            /* 0x02C8 IBNE */

    mem_w[chain].w = 0;                              /* 0x02CA ISW, terminate the retry list */
    int tagAt = cursors.y;                           /* 0x02CB ILW */
    emittedCount = emittedCount + 32767;             /* 0x02CC IADDIU, wraps to one less */
    mem_w[tagAt].x = emittedCount;                   /* 0x02CE ISW; 0x02CD nop */
    kick();                                          /* 0x02CD BAL */
    drainClipTex();
}

/* The same drain with the textured emitter's addresses. */
static void drainClipTex(void) {                     /* 0x02CF */
    int triangle = retryHead;                        /* 0x02CF ILW */
    int emitter = atEmitClippedTex;                  /* 0x02D0 IADDIU */
    link = atDrainClipTex;                           /* 0x02D1 IADDIU, this routine's head */
    retryHead = mem_w[triangle].w;                   /* 0x02D2 ILW, 0x02D4 ISW; 0x02D3 nop */
    if (triangle != 0) { clipTriangle(triangle, emitter); return; } /* 0x02D3 IBNE */
    link = mem_w[kickTop].x;                         /* 0x02D5 ILW */
    kickTop = kickTop + 1;                           /* 0x02D7 IADDIU; 0x02D6 nop */
    goto_micro(link);                                /* 0x02D6 JR */
}

/* Three quadwords in and three out. The texture coordinate keeps its own divide: its .z is forced
 * to 1.0 and the whole vector multiplied by Q, so the emitted ST carries s/w, t/w and 1/w in the
 * GS's own Q slot. */
static void emitClippedTex(void) {                   /* 0x02D8, from 0x031A, 0x034B, 0x056C */
    int src = clipBufA;                              /* 0x02D8 ILW */
    vec4 tag;
    tag.yz = vertexTag3.yz;                          /* 0x02DA LQ */
    packetClipped(tag, emittedCount);                /* 0x02D9 BAL */

    do {
        vec4 ndc;
        ndc.w = 0.0f;                                /* 0x02DB MULx */
        vec4 p = mem[src++];                         /* 0x02DB LQI, the position */
        vec4 color = mem[src++];                    /* 0x02DC LQI */
        vec4 st = mem[src++];                        /* 0x02DD LQI */
        Q = 1.0f / p.w;                              /* 0x02DE DIV */
        color.xyzw = ftoi0(color.xyzw);            /* 0x02DF FTOI0 */
        st.z = one.z;                                /* 0x02DF MOVE, 1.0 into the Q slot */
        vec4 acc;
        acc.xyz = screenOffset.xyz;                  /* 0x02E0 MULAw */
        emittedCount = emittedCount - 1;             /* 0x02E0 ISUBIU */
        vec4 s;
        s.w = st.w;                                  /* 0x02E1 MOVE */
        ndc.xyz = p.xyz * Q;                         /* 0x02E2 MULq, waits on the DIV */
        st.xyz = st.xyz * Q;                         /* 0x02E3 MULq, s/w, t/w and 1/w */
        outCursor = outCursor + 3;                   /* 0x02E3 IADDIU */
        s.xyz = acc.xyz + ndc.xyz * screenScale.xyz; /* 0x02E4 MADD */
        mem[outCursor - 2] = color;                 /* 0x02E4 SQ */
        s.xyzw = ftoi4(s.xyzw);                      /* 0x02E5 FTOI4 */
        mem[outCursor - 3] = st;                     /* 0x02E5 SQ */
        mem[outCursor - 1] = s;                      /* 0x02E7 SQ; 0x02E6 nop */
    } while (emittedCount != 0);                     /* 0x02E6 IBNE */
    kick();                                          /* 0x02E8 B; 0x02E9 nop */
}

/* ============================================================ GIF packet builders */

/* mem[980..982] hold GIF tag templates, `.yz` only, so a builder supplies the NLOOP itself.
 * mem[907] is the PRIM word's flag bits and each builder adds a primitive type: 3 for a triangle
 * list, +512 for GS context 2, and packetClipped's 5 for a strip. The value is written as A+D
 * register 0, which is PRIM, and kept at mem[908] for the next builder. */

/* The pass drawSecond reaches on drawMode bit 12. It relights through the routine its caller named
 * unless mem[914] is 3, in which case fillOne gives every vertex one color instead. The UVs come
 * out of the source quadword's .zw rather than its .xy, which is what makes this the second
 * texture coordinate set: one quadword carries both. */
static void texcoordSecond(int lightRoutine, int emitter) { /* 0x02EA, from 0x022D and 0x0497 */
    int mode = texcoordMode;                         /* 0x02EA ILW */
    mem_w[kickTop - 1].x = link;                     /* 0x02EC ISW */
    vec4 tint2 = drawTint2;                          /* 0x02ED LQ */
    vec4 tint = drawTint;                            /* 0x02EF LQ; 0x02EE nop */
    if (mode == 3) {                                 /* 0x02EB IADDIU, 0x02EE IBNE */
        lightRoutine = atFillOne;                    /* 0x02F0 IADDIU */
    }
    tint.xyzw = tint.xyzw * tint2.xyzw;              /* 0x02F1 MUL */
    kickTop = kickTop - 1;                           /* 0x02F2 ISUBIU */
    goto_micro(lightRoutine);                        /* 0x02F1 JALR, back at 0x02F3 */

    int src = srcTexcoords;                          /* 0x02F3 ILW */
    int count = vertexCount;                         /* 0x02F4 ILW */
    int v = 1;                                       /* 0x02F5 IADDIU */
    int prim = primFlags;                            /* 0x02F6 ILW */
    int out = cursors.y;                             /* 0x02F7 ILW */
    vec4 uv = mem[src++];                            /* 0x02F8 LQI */
    vec4 recip = mem[848];                           /* 0x02F9 LQ */
    vec4 m0, m1, m2;
    m0 = uvMatrixB(0); m1 = uvMatrixB(1); m2 = uvMatrixB(2); /* 0x02FA, 0x02FB, 0x02FC LQ */
    vec4 tagTemplate = adTag;                        /* 0x02FD LQ */
    vec4 ad0 = mem[899], ad1 = mem[900];             /* 0x02FE, 0x02FF LQ, five A+D writes */
    vec4 st;
    st.xy = m0.xy * uv.z + m1.xy * uv.w + m2.xy;     /* 0x0300, folds 0x02FE..0x0300 */
    vec4 ad2 = mem[901], ad3 = mem[902], ad4 = mem[903]; /* 0x0300, 0x0301, 0x0302 LQ */
    prim = prim + 515;                               /* 0x0303 IADDIU, triangles in context 2 */
    st.xy = st.xy * recip.z;                         /* 0x0304 MULz, divided through by w */
    mem_w[out + 6].z = 0;                            /* 0x0304 ISW, A+D register 0, PRIM */
    mem_w[out + 6].x = prim;                         /* 0x0305 ISW */
    lastPrim = prim;                                 /* 0x0306 ISW */
    mem_w[out].x = 6;                                /* 0x0307 IADDIU, 0x0308 ISW, NLOOP 6 */
    mem[out].yz = tagTemplate.yz;                    /* 0x0309 SQ */
    mem[out + 1].xyz = ad0.xyz;                      /* 0x030A SQ */
    mem[out + 2].xyz = ad1.xyz;                      /* 0x030B SQ */
    mem[out + 3].xyz = ad2.xyz;                      /* 0x030C SQ */
    mem[out + 4].xyz = ad3.xyz;                      /* 0x030D SQ */
    uv = mem[src++];                                 /* 0x030E LQI */
    vec4 next = texcoord(v);                         /* 0x030F LQ */
    mem[out + 5].xyz = ad4.xyz;                      /* 0x0310 SQ */
    out = out + 7;                                   /* 0x0311 IADDIU, the tag plus six */

    do {                                                 /* the same vertex-deep pipeline */
        vec4 acc;
        acc.xy = m0.xy * uv.z;                       /* 0x0312 MULAz */
        v = v + 1;                                   /* 0x0312 IADDIU */
        acc.xy = acc.xy + m1.xy * uv.w;              /* 0x0313 MADDAw */
        texcoord(v - 2).xy = st.xy;                  /* 0x0313 SQ, the previous vertex */
        st.xy = acc.xy + m2.xy;                      /* 0x0314 MADDw */
        uv = mem[src++];                             /* 0x0314 LQI */
        st.xy = st.xy * next.z;                      /* 0x0315 MULz */
        next = texcoord(v);                          /* 0x0316 LQ */
    } while (count != v);                            /* 0x0315 IBNE */

    texcoord(v - 1).xy = st.xy;                      /* 0x0317 SQ */
    link = mem_w[kickTop].x;                         /* 0x0318 ILW */
    kickTop = kickTop + 1;                           /* 0x0319 IADDIU */
    cursors.y = out;                                 /* 0x031B ISW; 0x031A nop */
    goto_micro(emitter);                             /* 0x031A JR, not the link: the emitter */
}

static void packetOne(void) {                        /* 0x031C, from 0x021A and 0x0485 */
    int prim = primFlags;                            /* 0x031C ILW */
    int out = cursors.y;                             /* 0x031D ILW */
    vec4 tagTemplate = adTag;                        /* 0x031E LQ */
    prim = prim + 3;                                 /* 0x031F IADDIU, a triangle list */
    mem_w[out + 1].z = 0;                            /* 0x0320 ISW, A+D register 0, PRIM */
    mem_w[out + 1].x = prim;                         /* 0x0321 ISW */
    lastPrim = prim;                                 /* 0x0322 ISW */
    mem_w[out].x = 1;                                /* 0x0323 IADDIU, 0x0324 ISW, NLOOP 1 */
    mem[out].yz = tagTemplate.yz;                    /* 0x0325 SQ */
    out = out + 2;                                   /* 0x0326 IADDIU */
    cursors.y = out;                                 /* 0x0328 ISW; 0x0327 nop */
    goto_micro(link);                                /* 0x0327 JR */
}

/* The same with NLOOP 4 and three A+D quadwords out of the counts region. */
static void packetThree(void) {                      /* 0x0329, from 0x0224 and 0x048D */
    int prim = primFlags;                            /* 0x0329 ILW */
    int out = cursors.y;                             /* 0x032A ILW */
    vec4 tagTemplate = adTag;                        /* 0x032B LQ */
    vec4 ad0 = mem[896], ad1 = mem[897], ad2 = mem[898]; /* 0x032C, 0x032D, 0x032E LQ */
    prim = prim + 3;                                 /* 0x032F IADDIU */
    mem_w[out + 4].z = 0;                            /* 0x0330 ISW */
    mem_w[out + 4].x = prim;                         /* 0x0331 ISW */
    lastPrim = prim;                                 /* 0x0332 ISW */
    mem_w[out].x = 4;                                /* 0x0333 IADDIU, 0x0334 ISW, NLOOP 4 */
    mem[out].yz = tagTemplate.yz;                    /* 0x0335 SQ */
    mem[out + 1].xyz = ad0.xyz;                      /* 0x0336 SQ */
    mem[out + 2].xyz = ad1.xyz;                      /* 0x0337 SQ */
    mem[out + 3].xyz = ad2.xyz;                      /* 0x0338 SQ */
    out = out + 5;                                   /* 0x0339 IADDIU */
    cursors.y = out;                                 /* 0x033B ISW; 0x033A nop */
    goto_micro(link);                                /* 0x033A JR */
}

/* Three quadwords: the A+D tag, its one PRIM write turned into a triangle strip, and the caller's
 * vertex tag with the vertex count as its NLOOP. It returns through vi13, because the emitters call
 * it with a BAL that sets that rather than the link register. */
static void packetClipped(vec4 vertexTag, int count) { /* 0x033C, from 0x027B and 0x02D9 */
    int out = cursors.y;                             /* 0x033C ILW */
    vec4 tagTemplate = adTag;                        /* 0x033D LQ */
    int prim = lastPrim;                             /* 0x033E ILW */
    mem_w[out].x = 1;                                /* 0x033F IADDIU, 0x0340 ISW, NLOOP 1 */
    mem[out].yz = tagTemplate.yz;                    /* 0x0341 SQ */
    prim = prim & 32760;                             /* 0x0342 IADDIU, 0x0343 IAND, ~7 */
    prim = prim + 5;                                 /* 0x0344 IADDIU, a triangle strip */
    mem_w[out + 1].x = prim;                         /* 0x0345 ISW */
    mem_w[out + 1].z = 0;                            /* 0x0346 ISW */
    mem[out + 2].yz = vertexTag.yz;                  /* 0x0347 SQ */
    mem_w[out + 2].x = count + 1 + 32767;            /* 0x0348, 0x0349 IADDIU, 0x034A ISW */
    outCursor = out + 3;                             /* 0x034C IADDIU; 0x034B nop */
    goto_micro(clipEmitter);                         /* 0x034B JR */
}

/* Normalizes a run of vectors at mem[752] in place. The RSQRT for one vector starts an iteration
 * before its WAITQ, so two are in flight and the last store happens after the loop. */
static void normalizeVectors(int count) {            /* 0x034D, from 0x0418 */
    vec4 v = mem[752];                               /* 0x034D LQ */
    vec4 sq;
    sq.xyz = v.xyz * v.xyz;                          /* 0x034E MUL */
    int i = 1;                                       /* 0x034E IADDIU */
    vec4 next = mem[753];                            /* 0x034F LQ */
    sq.x = sq.x + sq.y + one.x * sq.z;               /* 0x0350, folds 0x034F..0x0350 */
    vec4 sqNext;
    sqNext.xyz = next.xyz * next.xyz;                /* 0x0351 MUL */
    Q = 1.0f / sqrtf(fabsf(sq.x));                   /* 0x0351 RSQRT */
    sqNext.x = sqNext.x + sqNext.y + one.x * sqNext.z; /* 0x0353, folds 0x0352..0x0353 */
    vec4 unit;
    unit.xyz = v.xyz * Q;                            /* 0x0354 MULq, waits on the RSQRT */

    do {
        v.xyz = next.xyz;                            /* 0x0355 MULw */
        Q = 1.0f / sqrtf(fabsf(sqNext.x));           /* 0x0355 RSQRT, for the next one */
        next = mem[i + 753];                         /* 0x0356 LQ */
        sqNext.xyz = next.xyz * next.xyz;            /* 0x0357 MUL */
        i = i + 1;                                   /* 0x0357 IADDIU */
        vec4 acc;
        acc.x = sqNext.x + sqNext.y;                 /* 0x0358 ADDAy */
        mem[i + 750] = unit;                         /* 0x0358 SQ, the previous vector */
        sqNext.x = acc.x + one.x * sqNext.z;         /* 0x0359 MADDz */
        unit.xyz = v.xyz * Q;                        /* 0x035A MULq */
    } while (count != i);                            /* 0x0359 IBNE */

    mem[i + 751] = unit;                             /* 0x035C SQ; 0x035B nop */
    goto_micro(link);                                /* 0x035B JR */
}

/* ============================================================ geometry */

/* What happens to a batch's positions and normals before anything is lit or projected. The header
 * quadword is [faceCount, pointCount, attrBits, packetBits], the same record the DDS3 mesh batches
 * carry, and the arrays follow it in a fixed order, each present only if packetBits says so. Three
 * paths can run, and all three end with the results at mem[704] and mem[752] and the pointers
 * repointed there, so everything downstream reads one layout.
 *
 * packetBits: 0x0020 a face list, 0x0080 normals, 0x0100 texture coordinates, 0x0200 vertex
 * colors, 0x0400 the first bone of a skin, 0x0800 skinned at all, 0x1000 a blend shape in a
 * series, 0x2000 a middle bone or a weighted shape. None of 0x3C00 means a plain rigid batch.
 *
 * A bone's transform is seven quadwords in the block copyBlock filled, four for the position and
 * three for the normal, and `packetBits & 0x1F` is the offset: the low five bits hold the slot
 * already multiplied by seven. Skinning loads the camera matrix afterwards and the other two paths
 * load object-times-camera, which is the whole difference: a bone matrix already has the object
 * transform folded in and a morph target does not. */
static void unpackPointers(int packetBits) {         /* 0x035D, from 0x01B9 and 0x041F */
    int faces = mem_w[stream].x;                     /* 0x035D ILW */
    int points = mem_w[stream].y;                    /* 0x035E ILW */
    int cursor = stream + 1;                         /* 0x0361 IADDIU */
    int attrBits = mem_w[stream].z;                  /* 0x0363 ILW; 0x0362 nop */
    if ((packetBits & 32) != 0) {                    /* 0x035F IADDIU, 0x0360 IAND, 0x0362 IBEQ */
        indexList = cursor;                          /* 0x0364 ISW */
        cursor = cursor + faces;                     /* 0x0365 IADD */
    }
    srcPositions = cursor;                           /* 0x0368 ISW, always present */
    cursor = cursor + points;                        /* 0x036A IADD; 0x0369 IBEQ */
    if ((packetBits & 128) != 0) {                   /* 0x0366 IADDIU, 0x0367 IAND */
        srcNormals = cursor;                         /* 0x036B ISW */
        cursor = cursor + points;                    /* 0x036C IADD */
    }
    int mask = materialMask;                         /* 0x036F ILW */
    attrBits = attrBits & mask;                      /* 0x0371 IAND; 0x0370 IBEQ */
    if ((packetBits & 256) != 0) {                   /* 0x036D IADDIU, 0x036E IAND */
        srcTexcoords = cursor;                       /* 0x0372 ISW */
        cursor = cursor + points;                    /* 0x0373 IADD */
    }
    drawMode = attrBits;                             /* 0x0376 ISW */
    faceCount = faces;                               /* 0x0378 ISW; 0x0377 nop */
    if ((packetBits & 512) != 0) {                   /* 0x0374 IADDIU, 0x0375 IAND, 0x0377 IBEQ */
        srcColors = cursor;                         /* 0x0379 ISW */
    }
    vertexCount = points;                            /* 0x037A ISW */
    primFlags = attrBits & 120;                      /* 0x037B IADDIU, 0x037C IAND, 0x0381 ISW */
    drawFlags = packetBits;                          /* 0x037F ISW */
    if ((packetBits & 15360) == 0) {                 /* 0x037D, 0x037E, 0x0380 IBEQ, 0x3C00 */
        loadObjectMatrix();
        return;
    }
    skinFirstBone(packetBits);
}

/* Position through the bone's four rows and normal through its three, both scaled by the vertex's
 * weight in the position quadword's .w, written straight out. One vertex deep, so the prologue at
 * 0x0395..0x0398 exists to fill the pipe. */
static void skinFirstBone(int packetBits) {          /* 0x0382 */
    int skinned = packetBits & 2048;                 /* 0x0382 IADDIU, 0x0383 IAND */
    int bone = 31 & packetBits;                      /* 0x0384 IADDIU, 0x0386 IAND; 0x0385 nop */
    if (skinned == 0) { morphFirst(packetBits); return; } /* 0x0385 IBEQ */

    int src = srcPositions;                          /* 0x0387 ILW */
    int nsrc = srcNormals;                           /* 0x0388 ILW */
    int first = packetBits & 1024;                   /* 0x0389 IADDIU, 0x038A IAND */
    vec4 p = mem[src++];                             /* 0x038B LQI */
    vec4 n = mem[nsrc++];                            /* 0x038C LQI */
    vec4 pos0 = boneBlock(bone);                     /* 0x038D, 0x038E, 0x038F, 0x0390 LQ */
    vec4 nrm0 = boneBlock(bone + 4);                 /* 0x0391, 0x0392, 0x0394 LQ */
    vec4 acc;
    acc.xyz = pos0.xyz * p.x;                        /* 0x0392 MULAx */
    acc.xyz = acc.xyz + pos0.xyz * p.y;              /* 0x0393 MADDAy */
    if (first == 0) { skinAddBone(packetBits, pos0, nrm0, p, n, src, nsrc); return; } /* 0x0394 IBEQ */

    vec4 outPos, outNrm;
    outPos.xyz = mul4x4(pos0, p.xyz);                /* 0x0395, folds 0x0394..0x0395 */
    vec4 weight;
    weight.w = p.w;                                  /* 0x0395 MOVE */
    p = mem[src++];                                  /* 0x0396 LQI */
    int v = 0;                                       /* 0x0397 IADDIU */
    outNrm.xyz = mul3x3(nrm0, n.xyz);                /* 0x0398, folds 0x0396..0x0398 */
    int count = vertexCount;

    do {
        outPos.xyz = outPos.xyz * weight.w;          /* 0x0399 MULw */
        n = mem[nsrc++];                             /* 0x0399 LQI */
        acc.xyz = pos0.xyz * p.x;                    /* 0x039A MULAx */
        acc.xyz = acc.xyz + pos0.xyz * p.y;          /* 0x039B MADDAy */
        vec4 scaledNrm;
        scaledNrm.xyz = outNrm.xyz * weight.w;       /* 0x039C MULw */
        v = v + 1;                                   /* 0x039C IADDIU */
        position(v - 1) = outPos;                    /* 0x039D SQ, the previous vertex */
        outPos.xyz = mul4x4(pos0, p.xyz);            /* 0x039E, folds 0x039D..0x039E */
        weight.w = p.w;                              /* 0x039E MOVE */
        acc.xyz = nrm0.xyz * n.x;                    /* 0x039F MULAx */
        p = mem[src++];                              /* 0x039F LQI */
        acc.xyz = acc.xyz + nrm0.xyz * n.y;          /* 0x03A0 MADDAy */
        outNrm.xyz = acc.xyz + nrm0.xyz * n.z;       /* 0x03A1 MADDz */
        mem[v + 751] = scaledNrm;                    /* 0x03A1 SQ */
    } while (count != v);                            /* 0x03A0 IBNE */

    /* 0x03A2's branch has 0x03A3 as its delay slot, so falling out of the loop runs the IADDIU that
     * skinAddBone begins with and then leaves, while 0x0394's IBEQ lands on 0x03A3 and carries on. */
    kickAndEnd();                                    /* 0x03A2 B */
}

/* Adds this bone's weighted contribution to what is already at mem[704] and mem[752]. It ends the
 * microprogram outright rather than returning, because a middle bone is always followed by another
 * MSCAL. */
static void skinAddBone(int packetBits, vec4 pos0, vec4 nrm0, vec4 p, vec4 n, int src, int nsrc) {
    int middle = packetBits & 8192;                  /* 0x03A3 IADDIU, 0x03A4 IAND, 0x2000 */
    if (middle == 0) {                               /* 0x03A6 IBEQ; 0x03A5, 0x03A7 nop */
        skinLastBone(pos0, nrm0, p, n, src, nsrc);
        return;
    }
    vec4 acc, moved, rotated;
    moved.xyz = 0.0f + pos0.xyz;                     /* 0x03A8 MADDw */
    vec4 base = position(0);                         /* 0x03A8 LQ, what is already there */
    vec4 weight;
    weight.w = p.w;                                  /* 0x03A9 MOVE */
    vec4 baseNrm = mem[752];                         /* 0x03AA LQ */
    rotated.xyz = mul3x3(nrm0, n.xyz);               /* 0x03AB, folds 0x03A9..0x03AB */
    int v = 1;                                       /* 0x03AB IADDIU */
    p = mem[src++];                                  /* 0x03AC LQI */
    base.xyz = base.xyz + moved.xyz * weight.w;      /* 0x03AD, folds 0x03AC..0x03AD */
    n = mem[nsrc++];                                 /* 0x03AD LQI */
    baseNrm.xyz = baseNrm.xyz + rotated.xyz * weight.w; /* 0x03AF, folds 0x03AE..0x03AF */
    int count = vertexCount;

    do {
        acc.xyz = pos0.xyz * p.x;                    /* 0x03B0 MULAx */
        weight.w = p.w;                              /* 0x03B0 MOVE */
        acc.xyz = acc.xyz + pos0.xyz * p.y;          /* 0x03B1 MADDAy */
        position(v - 1) = base;                      /* 0x03B1 SQ */
        acc.xyz = acc.xyz + pos0.xyz * p.z;          /* 0x03B2 MADDAz */
        base = position(v);                          /* 0x03B2 LQ */
        moved.xyz = acc.xyz + pos0.xyz;              /* 0x03B3 MADDw */
        mem[v + 751] = baseNrm;                      /* 0x03B3 SQ */
        p = mem[src++];                              /* 0x03B4 LQI */
        baseNrm = mem[v + 752];                      /* 0x03B5 LQ */
        rotated.xyz = mul3x3(nrm0, n.xyz);           /* 0x03B6, folds 0x03B4..0x03B6 */
        v = v + 1;                                   /* 0x03B6 IADDIU */
        n = mem[nsrc++];                             /* 0x03B7 LQI */
        base.xyz = base.xyz + moved.xyz * weight.w;  /* 0x03B8, folds 0x03B7..0x03B8 */
        acc.xyz = baseNrm.xyz;                       /* 0x03B9 MULAw */
        baseNrm.xyz = acc.xyz + rotated.xyz * weight.w; /* 0x03BA MADDw */
    } while (count != v);                            /* 0x03B9 IBNE */

    position(v - 1) = base;                          /* 0x03BB SQ, the last vertex */
    mem[v + 751] = baseNrm;                          /* 0x03BC SQ */
    return;                                              /* [E] at 0x03BC; 0x03BD nop */
}

/* The last bone accumulates like skinAddBone and then renormalizes the summed normal, which is the
 * point of separating this case: summing normals in each bone's own space leaves a vector shorter
 * than one wherever the influences disagree. The RSQRT runs an iteration ahead of its WAITQ, so a
 * normal is stored a turn late. */
static void skinLastBone(vec4 pos0, vec4 nrm0, vec4 p, vec4 n, int src, int nsrc) { /* 0x03BE */
    int v = 0;                                       /* 0x03BE IADDIU */
    vec4 baseNrm = mem[v + 752];                     /* 0x03BF LQ */
    vec4 rotated;
    rotated.xyz = mul3x3(nrm0, n.xyz);               /* 0x03C0, folds 0x03BE..0x03C0 */
    rotated.xyz = baseNrm.xyz + rotated.xyz * p.w;   /* 0x03C2, folds 0x03C1..0x03C2 */
    vec4 sq;
    sq.xyz = rotated.xyz * rotated.xyz;              /* 0x03C3 MUL */
    n = mem[nsrc++];                                 /* 0x03C3 LQI */
    sq.x = sq.x + sq.y + one.x * sq.z;               /* 0x03C5, folds 0x03C4..0x03C5 */
    vec4 nextP = mem[src++];                         /* 0x03C5 LQI */
    int count = vertexCount;

    do {
        n.xyz = mul3x3(nrm0, n.xyz);                 /* 0x03C8, folds 0x03C6..0x03C8 */
        Q = 1.0f / sqrtf(fabsf(sq.x));               /* 0x03C8 RSQRT */
        baseNrm = mem[v + 753];                      /* 0x03CA LQ */
        vec4 moved;
        moved.xyz = mul4x4(pos0, p.xyz);             /* 0x03CC, folds 0x03C9..0x03CC */
        vec4 base = position(v);                     /* 0x03CC LQ */
        vec4 acc;
        acc.xyz = n.xyz * nextP.w;                   /* 0x03CD MULAw, the next weight */
        vec4 unit = rotated;                         /* 0x03CD MOVE, the one to normalize */
        rotated.xyz = acc.xyz + baseNrm.xyz;         /* 0x03CE MADDw */
        n = mem[nsrc++];                             /* 0x03CE LQI */
        v = v + 1;                                   /* 0x03CF IADDIU */
        moved.xyz = base.xyz + moved.xyz * p.w;      /* 0x03D0, folds 0x03CF..0x03D0 */
        sq.xyz = rotated.xyz * rotated.xyz;          /* 0x03D1 MUL */
        p = nextP;                                   /* 0x03D1 MOVE */
        unit.xyz = unit.xyz * Q;                     /* 0x03D2 MULq, waits on the RSQRT */
        nextP = mem[src++];                          /* 0x03D3 LQI */
        sq.x = sq.x + sq.y + one.x * sq.z;           /* 0x03D4, folds 0x03D3..0x03D4 */
        position(v - 1) = moved;                     /* 0x03D4 SQ */
        acc.xyz = nrm0.xyz * n.x;                    /* 0x03D5 MULAx */
        acc.xyz = acc.xyz + nrm0.xyz * n.y;          /* 0x03D6 MADDAy */
        mem[v + 751] = unit;                         /* 0x03D6 SQ, the normalized normal */
    } while (count != v);                            /* 0x03D5 IBNE */

    srcPositions = 704;                              /* 0x03D7 IADDIU, 0x03D8 ISW */
    srcNormals = 752;                                /* 0x03D9 IADDIU, 0x03DA ISW */
    modelView = cameraMatrix(0);                     /* 0x03DB, 0x03DC, 0x03DD, 0x03DF LQ */
    goto_micro(link);                                /* 0x03DE JR */
}

/* The blend-shape path. mem[911] counts the shapes seen and indexes the two weights unpackTwo
 * filed, so shape 0 is scaled by its weight and stored and each later one is accumulated. */
static void morphFirst(int packetBits) {             /* 0x03E0, from 0x0385 */
    int v = 0;                                       /* 0x03E0 IADDIU */
    int shape = shapeIndex;                          /* 0x03E1 ILW */
    int weighted = packetBits & 8192;                /* 0x03E2 IADDIU, 0x03E3 IAND, 0x2000 */
    int src = srcPositions;                          /* 0x03E4 ILW */
    vec4 weight;
    weight.w = morphWeight(shape).w;                 /* 0x03E5 LQ */
    int nsrc = srcNormals;                           /* 0x03E7 ILW; 0x03E6 nop */
    if (weighted == 0) { morphSum(shape, src, nsrc, v); return; } /* 0x03E6 IBEQ */

    vec4 p = mem[src++];                             /* 0x03E8 LQI */
    vec4 n = mem[nsrc++];                            /* 0x03E9 LQI */
    shapeIndex = shape + 1;                          /* 0x03EA IADDIU, 0x03EC ISW; 0x03EB nop */
    if (shape != 0) { morphAdd(weight, p, n, src, nsrc, v); return; } /* 0x03EB IBNE */

    vec4 outPos, outNrm;
    outPos.xyz = p.xyz * weight.w;                   /* 0x03ED MULw */
    outNrm.xyz = n.xyz * weight.w;                   /* 0x03EE MULw */
    p = mem[src++];                                  /* 0x03EE LQI */
    int count = vertexCount;

    do {
        n = mem[nsrc++];                             /* 0x03EF LQI */
        v = v + 1;                                   /* 0x03F0 IADDIU */
        position(v - 1) = outPos;                    /* 0x03F1 SQ */
        outPos.xyz = p.xyz * weight.w;               /* 0x03F2 MULw */
        mem[v + 751] = outNrm;                       /* 0x03F2 SQ */
        outNrm.xyz = n.xyz * weight.w;               /* 0x03F3 MULw */
        p = mem[src++];                              /* 0x03F4 LQI */
    } while (count != v);                            /* 0x03F3 IBNE */
    kickAndEnd();                                    /* 0x03F5 B; 0x03F6 nop */
}

static void morphAdd(vec4 weight, vec4 p, vec4 n, int src, int nsrc, int v) { /* 0x03F7 */
    vec4 base = position(v);                         /* 0x03F7 LQ */
    vec4 baseNrm = mem[v + 752];                     /* 0x03F8 LQ */
    int count = vertexCount;

    do {
        vec4 acc, outPos, outNrm;
        acc.xyz = p.xyz * weight.w;                  /* 0x03F9 MULAw */
        v = v + 1;                                   /* 0x03F9 IADDIU */
        outPos.xyz = acc.xyz + base.xyz;             /* 0x03FA MADDw */
        p = mem[src++];                              /* 0x03FA LQI */
        acc.xyz = n.xyz * weight.w;                  /* 0x03FB MULAw */
        base = position(v);                          /* 0x03FB LQ */
        outNrm.xyz = acc.xyz + baseNrm.xyz;          /* 0x03FC MADDw */
        n = mem[nsrc++];                             /* 0x03FC LQI */
        baseNrm = mem[v + 752];                      /* 0x03FD LQ */
        position(v - 1) = outPos;                    /* 0x03FE SQ */
        mem[v + 751] = outNrm;                       /* 0x0400 SQ; 0x03FF nop */
    } while (count != v);                            /* 0x03FF IBNE */
    kickAndEnd();                                    /* 0x0401 B; 0x0402 nop */
}

/* The last array in the series is added with no weight, which is what makes it the base the deltas
 * were relative to. A batch that gets here with the counter already zero had no shapes at all. */
static void morphSum(int shape, int src, int nsrc, int v) { /* 0x0403, from 0x03E6 */
    shapeIndex = 0;                                  /* 0x0404 ISW; 0x0403 nop */
    if (shape == 0) { loadObjectMatrix(); return; }  /* 0x0403 IBEQ */

    vec4 p = mem[src++];                             /* 0x0405 LQI */
    vec4 base = position(v);                         /* 0x0406 LQ */
    vec4 n = mem[nsrc++];                            /* 0x0407 LQI */
    vec4 baseNrm = mem[v + 752];                     /* 0x0408 LQ */
    int count = vertexCount;

    do {
        vec4 outPos, outNrm;
        outPos.xyz = p.xyz + base.xyz;               /* 0x0409 ADD */
        v = v + 1;                                   /* 0x0409 IADDIU */
        p = mem[src++];                              /* 0x040A LQI */
        outNrm.xyz = n.xyz + baseNrm.xyz;            /* 0x040B ADD */
        base = position(v);                          /* 0x040B LQ */
        n = mem[nsrc++];                             /* 0x040C LQI */
        baseNrm = mem[v + 752];                      /* 0x040D LQ */
        position(v - 1) = outPos;                    /* 0x040E SQ */
        mem[v + 751] = outNrm;                       /* 0x0410 SQ; 0x040F nop */
    } while (count != v);                            /* 0x040F IBNE */

    srcPositions = 704;                              /* 0x0411 IADDIU, 0x0412 ISW */
    srcNormals = 752;                                /* 0x0413 IADDIU, 0x0414 ISW */
    modelView = objectCamera(0);                     /* 0x0415, 0x0416, 0x0417, 0x0419 LQ */
    normalizeVectors(vertexCount);                   /* 0x0418 B */
}

/* The rigid case: the arrays are already where they arrived and the pointers still name them. */
static void loadObjectMatrix(void) {                 /* 0x041A, from 0x0380 and 0x0403 */
    modelView = objectCamera(0);                     /* 0x041A, 0x041B, 0x041C, 0x041E LQ */
    goto_micro(link);                                /* 0x041D JR */
}

/* ============================================================ the second driver */

/* The same shape as drawBatch, flag for flag and call for call. The one difference is what it
 * lights with: drawBatch calls lightVertex, which sums three directions, and this calls lightRamp,
 * which interpolates one. */
static void drawEntry2(void) {                       /* 0x041F, from 0x0010 */
    unpackPointers(mem_w[stream].w);                 /* 0x0420 ILW, 0x041F BAL */
    drawColor = materialColor;                     /* 0x0422 LQ */
    lightRamp(drawColor);                           /* 0x0421 BAL */

    int mode = drawMode;                             /* 0x0423 ILW */
    int bothFaces = mode & 16384;                    /* 0x0424 IADDIU, 0x0425 IAND */
    link = atSharedReturn;                           /* 0x0428 IADDIU; 0x0426, 0x0427 nop */
    if (bothFaces == 0) { drawFront2(); return; }    /* 0x0427 IBEQ */

    drawFront2();                                    /* 0x0429 BAL; 0x042A nop */
    negateNormals();                                 /* 0x042B BAL; 0x042C nop */
    drawColor = materialColor;                     /* 0x042E LQ */
    lightRamp(drawColor);                           /* 0x042D BAL */

    mode = drawMode;                                 /* 0x042F ILW */
    int useMatrix = mode & 16;                       /* 0x0430 IADDIU, 0x0432 IAND */
    link = atSharedReturn;                           /* 0x0431 IADDIU */
    kickTop = kickTop - 1;                           /* 0x0433 ISUBIU */
    mem_w[kickTop].x = link;                         /* 0x0435 ISW; 0x0434 nop */
    if (useMatrix == 0) { drawEmit2(); return; }     /* 0x0434 IBEQ */
    texcoordMatrix();                                /* 0x0436 BAL; 0x0437 nop */
    drawSecond2();                                   /* 0x0438 B; 0x0439 nop */
}

/* One light, taken as a ramp rather than a sum. The two ends are built first: the dark end is the
 * packet's ambient times the material's ambient times drawTint, and the lit end is colorBase times
 * the draw color, both clamped to 255 before the ramp runs, so a vertex can never interpolate past
 * what the clamp allows.
 *
 * `t` is the light direction dotted against the normal, with an offset from mem[981].w plus
 * mem[1015].x and a gain from mem[980].w plus mem[1014].x floored at 1.0. The direction is the .x
 * of each of the three matrix rows, one column, so this reads one light out of the matrix.
 *
 * Each vertex also gets `2t - 1` written unclamped into the normal array's .w, which nothing in
 * this range reads back. Two vertices an iteration with an odd tail. */
static void lightRamp(vec4 color) {                 /* 0x043A, from 0x0421 and 0x042D */
    I = 255.0f;                                      /* 0x043A LOI */
    vec4 matAmb = matAmbient;                        /* 0x043B LQ */
    vec4 dark, bright;
    dark.w = 0.0f;                                   /* 0x043C MULx */
    dark.xyz = lightColor(3).xyz;                   /* 0x043C LQ, mem[927] */
    bright.xyz = colorBase.xyz;                      /* 0x043D LQ */
    vec4 tint = drawTint;                            /* 0x043E LQ */
    dark.w = color.w;                               /* 0x043F MULw */
    vec4 fade;
    fade.w = flatten;                                /* 0x043F LQ */
    dark.xyz = dark.xyz * matAmb.xyz;                /* 0x0440 MUL */
    vec4 gainBias = mem[1014];                       /* 0x0440 LQ */
    bright.xyz = bright.xyz * color.xyz;            /* 0x0441 MUL */
    vec4 gain;
    gain.w = adTag.w;                                /* 0x0441 LQ, mem[980].w */
    vec4 offsetBias = mem[1015];                     /* 0x0442 LQ */
    vec4 offset;
    offset.w = vertexTag2.w;                         /* 0x0443 LQ, mem[981].w */
    int flags = drawFlags;                           /* 0x0444 ILW */
    dark.xyz = dark.xyz * tint.xyz;                  /* 0x0445 MUL */
    int packetMatrix = 2048 & flags;                 /* 0x0445 IADDIU, 0x0446 IAND */
    gain.w = gain.w + gainBias.x;                    /* 0x0446 ADDx */
    offset.w = offset.w + offsetBias.x;              /* 0x0447 ADDx */
    int out = 800;                                   /* 0x0447 IADDIU */
    vec4 acc;
    acc.xyz = bright.xyz * fade.w;                   /* 0x0448 MULAw */
    acc.xyz = acc.xyz + dark.xyz;                    /* 0x0449 MADDAw */
    int normals = srcNormals;                        /* 0x0449 ILW */
    vec4 row0;
    if (packetMatrix != 0) {                         /* 0x0448 IBEQ */
        gain.w = max(1.0f, gain.w);                  /* 0x044A MAXw */
        row0 = mem[920];                             /* 0x044A, 0x044B, 0x044D LQ */
        dark.xyz = acc.xyz - dark.xyz * fade.w;      /* 0x044B MSUBw */
        bright.xyzw = min(bright.xyzw, 255.0f);      /* 0x044C MINIi */
    } else {
        gain.w = max(1.0f, gain.w);                  /* 0x044E MAXw */
        row0 = lightNormal(0);                       /* 0x044E, 0x044F, 0x0450 LQ */
        dark.xyz = acc.xyz - dark.xyz * fade.w;      /* 0x044F MSUBw */
        bright.xyzw = min(bright.xyzw, 255.0f);      /* 0x0450 MINIi */
    }

    vec4 lightX, lightY, lightZ;
    lightX.w = gain.w * row0.x;                      /* 0x0451 MULx, one column of the matrix */
    vec4 n = mem[normals++];                         /* 0x0451 LQI */
    offset.w = offset.w * gain.w;                    /* 0x0452 MUL */
    dark.xyzw = min(dark.xyzw, 255.0f);              /* 0x0453 MINIi */
    lightY.w = gain.w * row0.x;                      /* 0x0454 MULx */
    int count = vertexCount;                         /* 0x0454 ILW */
    lightZ.w = gain.w * row0.x;                      /* 0x0455 MULx */
    int scalarOut = 752;                             /* 0x0455 IADDIU, into the normal array */
    vec4 span;
    span.xyz = bright.xyz - dark.xyz;                /* 0x0456 SUB, the ramp's span */
    vec4 n1 = mem[normals++];                        /* 0x0456 LQI */
    int odd = 1;                                     /* 0x0457 IADDIU */
    odd = odd & count;                               /* 0x0458 IAND */
    count = count - odd;                             /* 0x0459 ISUB */
    vec4 t0, t1, lit0, lit1;
    t0.w = offset.w + lightX.w * n.x + lightY.w * n.y + lightZ.w * n.z; /* 0x045A, folds 0x0457..0x045A */

    do {
        n = mem[normals++];                          /* 0x045C LQI; 0x045B, 0x045D nop */
        t1.w = offset.w + lightX.w * n1.x + lightY.w * n1.y + lightZ.w * n1.z; /* 0x045E, folds 0x045B..0x045E */
        vec4 k0, k1;
        k0.w = max(t0.w, 0.0f);                      /* 0x045F MAXx */
        n1 = mem[normals++];                         /* 0x045F LQI */
        t0.w = t0.w + t0.w - 1.0f;                   /* 0x0461, folds 0x0460..0x0461, 2t - 1 */
        k1.w = max(t1.w, 0.0f);                      /* 0x0462 MAXx */
        t1.w = t1.w + t1.w - 1.0f;                   /* 0x0464, folds 0x0463..0x0464 */
        k0.w = min(k0.w, 1.0f);                      /* 0x0465 MINIw */
        mem[scalarOut++].w = t0.w;                   /* 0x0465 SQI, the scalar */
        k1.w = min(k1.w, 1.0f);                      /* 0x0466 MINIw */
        lit0.xyzw = dark.xyzw + span.xyzw * k0.w;    /* 0x0468, folds 0x0467..0x0468, the ramp */
        mem[scalarOut++].w = t1.w;                   /* 0x0468 SQI */
        lit1.xyzw = acc.xyzw + span.xyzw * k1.w;     /* 0x0469 MADDw */
        acc.w = offset.w;                            /* 0x046A MULAw */
        acc.w = acc.w + lightX.w * n.x;              /* 0x046B MADDAx */
        count = count - 2;                           /* 0x046B ISUBIU, two a turn */
        acc.w = acc.w + lightY.w * n.y;              /* 0x046C MADDAy */
        mem[out++] = lit0;                           /* 0x046C SQI, the color */
        t0.w = acc.w + lightZ.w * n.z;               /* 0x046D MADDz */
        acc.w = offset.w;                            /* 0x046E MULAw */
        mem[out++] = lit1;                           /* 0x046E SQI */
    } while (count != 0);                            /* 0x046D IBNE */

    if (odd != 0) {                                  /* 0x046F IBEQ; 0x0470 nop */
        vec4 k0;
        k0.w = max(t0.w, 0.0f);                      /* 0x0471 MAXx */
        t0.w = t0.w + t0.w - 1.0f;                   /* 0x0473, folds 0x0472..0x0473 */
        k0.w = min(k0.w, 1.0f);                      /* 0x0474 MINIw */
        lit0.xyzw = dark.xyzw + span.xyzw * k0.w;    /* 0x0476, folds 0x0475..0x0476 */
        mem[scalarOut++].w = t0.w;                   /* 0x0476 SQI */
        mem[out++] = lit0;                           /* 0x0478 SQI; 0x0477 nop */
        goto_micro(link);                            /* 0x0477 JR */
    }
    goto_micro(link);                                /* 0x0479 JR; 0x047A nop */
}

static void drawFront2(void) {                       /* 0x047B, from 0x0427 and 0x0429 */
    int mode = drawMode;                             /* 0x047B ILW */
    int useMatrix = 16;                              /* 0x047C IADDIU */
    mem_w[kickTop - 1].x = link;                     /* 0x047D ISW */
    kickTop = kickTop - 1;                           /* 0x047E ISUBIU */
    useMatrix = mode & useMatrix;                    /* 0x047F IAND */
    if (useMatrix != 0) {                            /* 0x0481 IBNE; 0x0480, 0x0482 nop */
        vec4 m0;
        m0.xy = uvMatrix(0).xy;                      /* 0x0489 LQ */
        m0.xy = uvMatrix(1).xy;                      /* 0x048A LQ */
        m0.xy = uvMatrix(2).xy;                      /* 0x048C LQ; 0x048B BAL */
        transformVerticesUv();                       /* 0x048B BAL */
        drawSecond2();
        return;
    }
    transformVertices();                             /* 0x0483 BAL; 0x0484 nop */
    drawEmit2();
}

/* 1177 is drawSecond2's return at 0x0499. */
static void drawEmit2(void) {                        /* 0x0485, from 0x0434 */
    packetOne();                                     /* 0x0485 BAL; 0x0486 nop */
    link = atDrawSecond2Pop;                         /* 0x0488 IADDIU; 0x0487 B */
    emitTriangles();                                 /* 0x0487 B */
}

/* 1082 is lightRamp, where drawSecond passes lightVertex: texcoordSecond relights through whichever
 * routine its caller named. */
static void drawSecond2(void) {                      /* 0x048D, from 0x0438 */
    packetThree();                                   /* 0x048D BAL; 0x048E nop */
    emitTexTriangles();                              /* 0x048F BAL; 0x0490 nop */
    int mode = drawMode;                             /* 0x0491 ILW */
    int further = 4096;                              /* 0x0492 IADDIU */
    drawColor = materialColor;                     /* 0x0493 LQ */
    further = mode & further;                        /* 0x0494 IAND */
    int lightRoutine = atLightRamp;                  /* 0x0495 IADDIU, lightRamp */
    int emitter = atEmitTexTriangles;                /* 0x0496 IADDIU, emitTexTriangles */
    link = atDrawSecond2Pop;                         /* 0x0498 IADDIU; 0x0497 nop */
    if (further != 0) { texcoordSecond(lightRoutine, emitter); return; } /* 0x0497 IBNE */
    link = mem_w[kickTop].x;                         /* 0x0499 ILW */
    kickTop = kickTop + 1;                           /* 0x049B IADDIU; 0x049A nop */
    readFlags();                                     /* 0x049A B, which JRs to the link */
}

/* XGKICK sends what was built and the three cursors rotate, so building and kicking never touch the
 * same buffer. 0x04A2 is a jump target from the three transform tails. */
static void kick(void) {                             /* 0x049C, from 0x026F, 0x02CD, 0x04A4 */
    int sending = cursors.z;                         /* 0x049C ILW */
    int spare = cursors.w;                           /* 0x049D ILW */
    xgkick(sending);                                 /* 0x049E XGKICK */
    cursors.w = sending;                             /* 0x049F ISW */
    cursors.z = spare;                               /* 0x04A0 ISW */
    cursors.y = spare;                               /* 0x04A1 ISW */
    goto_micro(link);                                /* 0x04A2 JR; 0x04A3 nop */
}

/* Where every path that finished a batch lands. 0x04A6 resets the return stack before the [E], so a
 * batch that left entries on it cannot corrupt the next one. */
static void kickAndEnd(void) {                       /* 0x04A4, from eight sprite and skin tails */
    link = atSharedReturn;                           /* 0x04A4 BAL; 0x04A5 nop */
    kick();                                          /* 0x04A4 BAL */
    kickTop = 1024;                                  /* 0x04A6 IADDIU */
    return;                                              /* [E] at 0x04A6; 0x04A7 nop */
}

/* The six trivial rejects emitTexTriangles writes inline, out of line for emitTriangles. Nonzero
 * means every vertex is outside one plane and the triangle can be thrown away. Falling off the end
 * returns the last test's result; any test that fires jumps to 0x04B5, which returns a hard 1. */
static int clipReject(void) {                        /* 0x04A8, from 0x025E */
    if ((clip | 0xfdf7df) == 0xffffff) goto reject;  /* 0x04A8 FCOR, 0x04A9 IBNE, z < -w */
    if ((clip | 0xfefbef) == 0xffffff) goto reject;  /* 0x04AA FCOR, 0x04AB IBNE, z > w */
    if ((clip | 0xff7df7) == 0xffffff) goto reject;  /* 0x04AC FCOR, 0x04AD IBNE, y < -w */
    if ((clip | 0xffbefb) == 0xffffff) goto reject;  /* 0x04AE FCOR, 0x04AF IBNE, y > w */
    if ((clip | 0xffdf7d) == 0xffffff) goto reject;  /* 0x04B0 FCOR, 0x04B1 IBNE, x < -w */
    goto_micro(link);                                /* 0x04B3 JR; 0x04B2 nop */
    return (clip | 0xffefbe) == 0xffffff;            /* 0x04B4 FCOR, x > w, the JR's delay slot */
reject:
    goto_micro(link);                                /* 0x04B5 JR */
    return 1;                                        /* 0x04B6 IADDIU */
}

/* packetThree's shape with six A+D writes taken from five quadwords at the caller's block plus the
 * PRIM. The PRIM is built differently from every other builder: `(primFlags | 0x213) & ~0x21` forces
 * TME and CTXT on and FGE off, and leaves the primitive type at 2, a line, where the others write 3. */
static void packetSix(int block) {                   /* 0x04B7, from 0x05EC */
    vec4 ad0 = mem[block], ad1 = mem[block + 1];     /* 0x04B7, 0x04B8 LQ */
    int prim = primFlags;                            /* 0x04B9 ILW */
    int out = cursors.y;                             /* 0x04BA ILW */
    vec4 tagTemplate = adTag;                        /* 0x04BB LQ */
    vec4 ad2 = mem[block + 2], ad3 = mem[block + 3]; /* 0x04BC, 0x04BD LQ */
    vec4 ad4 = mem[block + 4];                       /* 0x04BE LQ */
    prim = prim | 531;                               /* 0x04BF IADDIU, 0x04C0 IOR, 0x213 */
    prim = prim & -33;                               /* 0x04C1 ISUBIU, 0x04C2 IAND, ~0x21 */
    mem_w[out + 6].z = 0;                            /* 0x04C3 ISW, A+D register 0, PRIM */
    mem_w[out + 6].x = prim;                         /* 0x04C4 ISW */
    lastPrim = prim;                                 /* 0x04C5 ISW */
    mem_w[out].x = 6;                                /* 0x04C6 IADDIU, 0x04C7 ISW, NLOOP 6 */
    mem[out].yz = tagTemplate.yz;                    /* 0x04C8 SQ */
    mem[out + 1].xyz = ad0.xyz;                      /* 0x04C9 SQ */
    mem[out + 2].xyz = ad1.xyz;                      /* 0x04CA SQ */
    mem[out + 3].xyz = ad2.xyz;                      /* 0x04CB SQ */
    mem[out + 4].xyz = ad3.xyz;                      /* 0x04CC SQ */
    mem[out + 5].xyz = ad4.xyz;                      /* 0x04CD SQ */
    out = out + 7;                                   /* 0x04CE IADDIU */
    cursors.y = out;                                 /* 0x04D0 ISW; 0x04CF nop */
    goto_micro(link);                                /* 0x04CF JR */
}

/* ============================================================ the per-vertex transform */

/* Two variants that differ only in whether they also run the texture matrix. Both write position(v)
 * and texcoord(v), and texcoord(v) is where the rest of the program's per-vertex scalars come from:
 *
 *   .xy  the texture coordinate divided by w, or Q in both lanes when there is no texture matrix
 *   .z   Q, the reciprocal the emitters multiply a position by
 *   .w   the fog, `fogParams.z + fogParams.w * Q` clamped to [fogParams.x, fogParams.y]
 *
 * So fog is an offset plus a reciprocal, clamped, computed once a vertex and carried in the same
 * quadword as the reciprocal. Both loops do two vertices an iteration with an odd tail, pipelined a
 * vertex deep around the divide: one vertex's reciprocal starts while the previous one's WAITQ is
 * still pending, which is why the stores lag the multiplies by a full step. */
static void transformVertices(void) {                /* 0x04D1, from 0x0218 and 0x0483 */
    vec4 unit = one;                                 /* 0x04D1 MULw, (1, 1, 1, 0) */
    int src = srcPositions;                          /* 0x04D1 ILW */
    vec4 unitOdd = one;                              /* 0x04D2 MULw, the odd vertex's copy */
    vec4 p = mem[src++];                             /* 0x04D2 LQI */
    vec4 acc;
    acc.xyzw = modelView.xyzw * p.x;                 /* 0x04D3 MULAx */
    vec4 pNext = mem[src++];                         /* 0x04D3 LQI */
    acc.xyzw = acc.xyzw + modelView.xyzw * p.y;      /* 0x04D4 MADDAy */
    vec4 fog = fogParams;                            /* 0x04D4 LQ */
    acc.xyzw = acc.xyzw + modelView.xyzw * p.z;      /* 0x04D5 MADDAz */
    int count = vertexCount;                         /* 0x04D5 ILW */
    vec4 view = acc;
    view.xyzw = acc.xyzw + modelView.xyzw;           /* 0x04D6 MADDw, the first position */
    p = mem[src++];                                  /* 0x04D6 LQI */
    int out = 704;                                   /* 0x04D7 IADDIU */
    unit.w = fog.w;                                  /* 0x04D8 MOVE, the fog scale */
    unitOdd.w = fog.w;                               /* 0x04D9 MOVE */
    vec4 viewNext;
    viewNext.xyzw = mul4x4(modelView, pNext.xyzw);   /* 0x04DA, folds 0x04D7..0x04DA */
    Q = 1.0f / view.w;                               /* 0x04DA DIV */
    int uv = 848;                                    /* 0x04DB IADDIU */
    vec4 st;
    st.xyzw = 0.0f * fog.z + unit.xyzw * Q;          /* 0x04DC, folds 0x04DB..0x04DC */
    acc.xyzw = modelView.xyzw * p.x;                 /* 0x04DD MULAx */
    Q = 1.0f / viewNext.w;                           /* 0x04DD DIV, the next reciprocal */
    acc.xyzw = acc.xyzw + modelView.xyzw * p.y;      /* 0x04DE MADDAy */
    int odd = 1;                                     /* 0x04DE IADDIU */
    st.w = max(st.w, fog.x);                         /* 0x04DF MAXx, the fog minimum */
    odd = count & odd;                               /* 0x04DF IAND */
    acc.xyzw = acc.xyzw + modelView.xyzw * p.z;      /* 0x04E0 MADDAz */
    count = count - odd;                             /* 0x04E0 ISUB */

    do {
        vec4 view2;
        view2.xyzw = modelView.xyzw * p.z + modelView.xyzw; /* 0x04E1, folds 0x04E0..0x04E1 */
        pNext = mem[src++];                          /* 0x04E1 LQI */
        acc.xyzw = 0.0f * fog.z;                     /* 0x04E2 MULAz, the fog offset */
        mem[out++] = view;                           /* 0x04E2 SQI */
        st.w = min(st.w, fog.y);                     /* 0x04E3 MINIy, the fog maximum */
        mem[out++] = viewNext;                       /* 0x04E3 SQI */
        vec4 stNext;
        stNext.xyzw = acc.xyzw + unitOdd.xyzw * Q;   /* 0x04E4 MADDq, waits on the DIV */
        acc.xyzw = modelView.xyzw * pNext.x;         /* 0x04E5 MULAx */
        Q = 1.0f / view2.w;                          /* 0x04E5 DIV */
        acc.xyzw = acc.xyzw + modelView.xyzw * pNext.y; /* 0x04E6 MADDAy */
        view = view2;                                /* 0x04E6 MOVE */
        acc.xyzw = acc.xyzw + modelView.xyzw * pNext.z; /* 0x04E7 MADDAz */
        mem[uv++] = st;                              /* 0x04E7 SQI, reciprocal and fog */
        stNext.w = max(stNext.w, fog.x);             /* 0x04E8 MAXx */
        viewNext.xyzw = acc.xyzw + modelView.xyzw;   /* 0x04E9 MADDw */
        p = mem[src++];                              /* 0x04EA LQI */
        st.xyzw = 0.0f * fog.z + unit.xyzw * Q;      /* 0x04EB, folds 0x04EA..0x04EB */
        stNext.w = min(stNext.w, fog.y);             /* 0x04EC MINIy */
        count = count - 2;                           /* 0x04EC ISUBIU, two a turn */
        acc.xyzw = modelView.xyzw * p.x;             /* 0x04ED MULAx */
        Q = 1.0f / viewNext.w;                       /* 0x04ED DIV */
        acc.xyzw = acc.xyzw + modelView.xyzw * p.y;  /* 0x04EE MADDAy */
        st.w = max(st.w, fog.x);                     /* 0x04EF MAXx */
        acc.xyzw = acc.xyzw + modelView.xyzw * p.z;  /* 0x04F0 MADDAz */
        mem[uv++] = stNext;                          /* 0x04F0 SQI */
    } while (count != 0);                            /* 0x04EF IBNE */

    if (odd == 0) { goto_micro(link); return; }      /* 0x04F1 IBEQ; 0x04F2 nop */
    st.w = min(st.w, fog.y);                         /* 0x04F3 MINIy, the odd vertex */
    mem[out++] = view;                               /* 0x04F3 SQI */
    mem[uv++] = st;                                  /* 0x04F5 SQI; 0x04F4 nop */
    goto_micro(link);                                /* 0x04F4 JR */
}

/* The same loop with the texture matrix applied to each source UV before the divide, so texcoord(v)
 * comes out as (u/w, v/w, 1/w, fog): the third lane is left at 1.0, so it is the bare reciprocal
 * either way. */
static void transformVerticesUv(void) {              /* 0x04F6, from 0x048B */
    int src = srcPositions;                          /* 0x04F6 ILW */
    vec4 uvIn = one;                                 /* 0x04F7 MULw */
    vec4 p = mem[src++];                             /* 0x04F7 LQI */
    vec4 uvOdd = one;                                /* 0x04F8 MULw */
    int tsrc = srcTexcoords;                         /* 0x04F8 ILW */
    uvIn.xy = mem[tsrc++].xy;                        /* 0x04F9 LQI */
    vec4 fog = fogParams;                            /* 0x04FA LQ */
    int count = vertexCount;                         /* 0x04FB ILW */
    vec4 view;
    view.xyzw = mul4x4(modelView, p.xyzw);           /* 0x04FC, folds 0x04F9..0x04FC */
    vec4 pNext = mem[src++];                         /* 0x04FC LQI */
    Q = 1.0f / view.w;                               /* 0x04FE DIV; 0x04FD nop */
    vec4 m0, m1, m2;
    m0.xy = uvMatrix(0).xy; m1.xy = uvMatrix(1).xy; m2.xy = uvMatrix(2).xy;
    uvIn.xy = m0.xy * uvIn.x + m1.xy * uvIn.y + m2.xy; /* 0x04FF, folds 0x04FD..0x04FF */
    int out = 704;                                   /* 0x04FF IADDIU */
    int uv = 848;                                    /* 0x0500 IADDIU */
    uvIn.w = fog.w;                                  /* 0x0501 MOVE */
    uvOdd.xy = mem[tsrc++].xy;                       /* 0x0502 LQI */
    vec4 viewNext;
    viewNext.xyzw = mul4x4(modelView, pNext.xyzw);   /* 0x0503, folds 0x0500..0x0503 */
    uvOdd.w = fog.w;                                 /* 0x0503 MOVE */
    p = mem[src++];                                  /* 0x0504 LQI */
    vec4 st;
    st.xyzw = 0.0f * fog.z + uvIn.xyzw * Q;          /* 0x0505, folds 0x0504..0x0505 */
    int odd = 1;                                     /* 0x0506 IADDIU */
    Q = 1.0f / viewNext.w;                           /* 0x0507 DIV */
    uvOdd.xy = m0.xy * uvOdd.x + m1.xy * uvOdd.y + m2.xy; /* 0x0508, folds 0x0506..0x0508 */
    odd = count & odd;                               /* 0x0508 IAND */
    vec4 acc;
    acc.xyzw = modelView.xyzw * p.x;                 /* 0x0509 MULAx */
    count = count - odd;                             /* 0x0509 ISUB */

    do {
        acc.xyzw = acc.xyzw + modelView.xyzw * p.y;  /* 0x050A MADDAy */
        mem[out++] = view;                           /* 0x050A SQI */
        acc.xyzw = acc.xyzw + modelView.xyzw * p.z;  /* 0x050B MADDAz */
        mem[out++] = viewNext;                       /* 0x050B SQI */
        view.xyzw = mul4x4(modelView, p.xyzw);       /* 0x050C, folds 0x0509..0x050C */
        uvIn.xy = mem[tsrc++].xy;                    /* 0x050C LQI */
        vec4 stNext;
        stNext.xyzw = 0.0f * fog.z + uvOdd.xyzw * Q; /* 0x050E, folds 0x050D..0x050E */
        st.w = max(st.w, fog.x);                     /* 0x050F MAXx */
        pNext = mem[src++];                          /* 0x050F LQI */
        Q = 1.0f / view.w;                           /* 0x0510 DIV */
        uvIn.xy = m0.xy * uvIn.x + m1.xy * uvIn.y + m2.xy; /* 0x0512, folds 0x0510..0x0512 */
        st.w = min(st.w, fog.y);                     /* 0x0513 MINIy */
        acc.xyzw = modelView.xyzw * pNext.x;         /* 0x0514 MULAx */
        acc.xyzw = acc.xyzw + modelView.xyzw * pNext.y; /* 0x0515 MADDAy */
        acc.xyzw = acc.xyzw + modelView.xyzw * pNext.z; /* 0x0516 MADDAz */
        count = count - 2;                           /* 0x0516 ISUBIU */
        stNext.w = max(stNext.w, fog.x);             /* 0x0517 MAXx */
        mem[uv++] = st;                              /* 0x0517 SQI, UV, reciprocal and fog */
        viewNext.xyzw = acc.xyzw + modelView.xyzw;   /* 0x0518 MADDw */
        uvOdd.xy = mem[tsrc++].xy;                   /* 0x0518 LQI */
        st.xyzw = 0.0f * fog.z + uvIn.xyzw * Q;      /* 0x051A, folds 0x0519..0x051A */
        stNext.w = min(stNext.w, fog.y);             /* 0x051B MINIy */
        p = mem[src++];                              /* 0x051B LQI */
        acc.xy = m0.xy * uvOdd.x;                    /* 0x051C MULAx */
        acc.xy = acc.xy + m1.xy * uvOdd.y;           /* 0x051D MADDAy */
        Q = 1.0f / viewNext.w;                       /* 0x051D DIV */
        uvOdd.xy = acc.xy + m2.xy;                   /* 0x051E MADDw */
        acc.xyzw = modelView.xyzw * p.x;             /* 0x051F MULAx */
        mem[uv++] = stNext;                          /* 0x051F SQI */
    } while (count != 0);                            /* 0x051E IBNE */

    st.w = max(st.w, fog.x);                         /* 0x0520 MAXx */
    if (odd == 0) { goto_micro(link); return; }      /* 0x0520 IBEQ; 0x0521 nop */
    st.w = min(st.w, fog.y);                         /* 0x0522 MINIy */
    mem[out++] = view;                               /* 0x0522 SQI */
    mem[uv++] = st;                                  /* 0x0524 SQI; 0x0523 nop */
    goto_micro(link);                                /* 0x0523 JR */
}

/* ============================================================ the clipper */

/* Three vertices of attributes, in the order the clipper's own code expects. */
static void loadClipTriangle(int a, int b, int c) {  /* 0x0525, reached from clipTriangle */
    vec4 pa = position(a), sta = texcoord(a), ca = vertexColor(a); /* 0x0525, 0x0526, 0x0527 LQ */
    vec4 pb = position(b), stb = texcoord(b), cb = vertexColor(b); /* 0x0528, 0x0529, 0x052A LQ */
    vec4 pc = position(c), stc = texcoord(c), cc = vertexColor(c); /* 0x052B, 0x052C, 0x052E LQ */
    clipSeed(pa, ca, sta, pb, cb, stb, pc, cc, stc); /* 0x052D B */
}

/* Sutherland-Hodgman against the six frustum planes, once per triangle on the retry list. It runs
 * one plane a pass over two scratch buffers, swapping them each time, and produces a fan the
 * clipped emitters then project.
 *
 * A vertex is three quadwords throughout, position, color, texture coordinate, which is the order
 * emitClipped reads back. The texture coordinate is multiplied by w on the way in because
 * everything downstream of the transform holds it divided by w and the interpolation has to happen
 * before that divide.
 *
 * A vertex past the far plane gets its fog forced to the maximum: the three tests at 0x0540,
 * 0x0544 and 0x0548 each check bit 4, z > w, of one vertex. */
static void clipTriangle(int triangle, int emitter) { /* 0x052F, from 0x0275 and 0x02D3 */
    clipEmitter = emitter;
    int a = mem_w[triangle].x, b = mem_w[triangle].y, c = mem_w[triangle].z; /* 0x052F..0x0531 ILW */
    vec4 pa = position(a), sta = texcoord(a);        /* 0x0532, 0x0533 LQ */
    vec4 pb = position(b), stb = texcoord(b);        /* 0x0534, 0x0535 LQ */
    vec4 pc = position(c);                           /* 0x0536 LQ */
    sta.xy = sta.xy * pa.w;                          /* 0x0537 MULw, the UV back to clip space */
    vec4 stc = texcoord(c);                          /* 0x0537 LQ */
    vec4 ca = vertexColor(a);                       /* 0x0538 LQ */
    stb.xy = stb.xy * pb.w;                          /* 0x0539 MULw */
    vec4 cb = vertexColor(b);                       /* 0x0539 LQ */
    stc.xy = stc.xy * pc.w;                          /* 0x053A MULw */
    vec4 cc = vertexColor(c);                       /* 0x053A LQ */
    clipSeed(pa, ca, sta, pb, cb, stb, pc, cc, stc);
}

/* The two buffers are 72 quadwords apart, which is 24 vertices: a triangle clipped by six planes
 * cannot produce more. */
static void clipSeed(vec4 pa, vec4 ca, vec4 sta, vec4 pb, vec4 cb, vec4 stb,
                     vec4 pc, vec4 cc, vec4 stc) {   /* 0x053B, from 0x052D */
    clip = (clip << 6) | clip_bits(pa.xyz, pa.w);    /* 0x053B CLIP */
    int in = cursors.z;                              /* 0x053B ILW */
    vec4 fog = fogParams;                            /* 0x053C LQ */
    in = in + 16;                                    /* 0x053D IADDIU, past the kicked buffer */
    int out = in + 72;                               /* 0x053E IADDIU */
    clip = (clip << 6) | clip_bits(pb.xyz, pb.w);    /* 0x053F CLIP */
    clipBufB = in;                                   /* 0x053F ISW */
    int farA = (clip & 0x000010) != 0;               /* 0x0540 FCAND */
    clip = (clip << 6) | clip_bits(pc.xyz, pc.w);    /* 0x0542 CLIP; 0x0541 nop */
    clipBufA = out;                                  /* 0x0542 ISW */
    if (farA != 0) sta.w = fog.y;                    /* 0x0541 IBEQ, 0x0543 MULy, full fog */
    int farB = (clip & 0x000010) != 0;               /* 0x0544 FCAND */
    mem[out] = pa;                                   /* 0x0546 SQ; 0x0545 nop */
    if (farB != 0) stb.w = fog.y;                    /* 0x0545 IBEQ, 0x0547 MULy */
    int farC = (clip & 0x000010) != 0;               /* 0x0548 FCAND */
    mem[out + 1] = ca;                               /* 0x054A SQ; 0x0549 nop */
    if (farC != 0) stc.w = fog.y;                    /* 0x0549 IBEQ, 0x054B MULy */
    mem[out + 2] = sta;                              /* 0x054C SQ */
    mem[out + 3] = pb;                               /* 0x054D SQ */
    mem[out + 4] = cb;                               /* 0x054E SQ */
    mem[out + 5] = stb;                              /* 0x054F SQ */
    mem[out + 6] = pc;                               /* 0x0550 SQ */
    mem[out + 7] = cc;                               /* 0x0551 SQ */
    mem[out + 8] = stc;                              /* 0x0552 SQ */
    mem[out + 9] = pa;                               /* 0x0553 SQ, vertex 0 again, closing it */
    mem[out + 10] = ca;                              /* 0x0554 SQ */
    mem[out + 11] = sta;                             /* 0x0555 SQ */
    emittedCount = 3;                                /* 0x0556 IADDIU */

    /* Six planes, each described by a plane vector, the clip bit of the vertex being emitted, the
     * same bit for the next vertex, and the MAC sign-flag bit for the plane's axis. */
    vec4 plane;
    plane.xyz = 1.0f;  plane.w = 1.0f;               /* 0x0558 ADDw, 0x0559 ADDx */
    clipPlane(plane, 16, 1024, 32);                  /* 0x0557, 0x055A, 0x0559 BAL, z > w */
    plane.xyz = -1.0f; plane.w = 1.0f;               /* 0x055B SUBw, 0x055C SUBx */
    clipPlane(plane, 32, 2048, 32);                  /* 0x055D, 0x055C BAL, z < -w */
    plane.xyz = 1.0f;  plane.w = 1.0f;               /* 0x055F ADDw, 0x0560 ADDx */
    clipPlane(plane, 1, 64, 128);                    /* 0x055E, 0x0561, 0x0560 BAL, x > w */
    plane.xyz = -1.0f; plane.w = 1.0f;               /* 0x0562 SUBw, 0x0563 SUBx */
    clipPlane(plane, 2, 128, 128);                   /* 0x0564, 0x0563 BAL, x < -w */
    plane.xyz = 1.0f;  plane.w = 1.0f;               /* 0x0566 ADDw, 0x0567 ADDx */
    clipPlane(plane, 4, 256, 64);                    /* 0x0565, 0x0568, 0x0567 BAL, y > w */
    plane.xyz = -1.0f; plane.w = 1.0f;               /* 0x0569 SUBw, 0x056A SUBx */
    clipPlane(plane, 8, 512, 64);                    /* 0x056B, 0x056A BAL, y < -w */
    goto_micro(clipEmitter);                         /* 0x056C JR; 0x056D nop */
}

/* One plane. The buffers change roles every call, so a pass reads what the previous one wrote. A
 * pass that keeps nothing returns straight to the drain's own head, which skips the remaining
 * planes and the emit for a triangle that has been clipped away entirely. */
static void clipPlane(vec4 plane, int here, int next, int axis) { /* 0x056E, from six calls */
    int out = clipBufB;                              /* 0x056E ILW */
    int in = clipBufA;                               /* 0x056F ILW */
    int count = emittedCount;                        /* 0x0570 IADD */
    emittedCount = 0;                                /* 0x0571 IADD */
    clipBufA = out;                                  /* 0x0572 ISW, swapped for the next pass */
    clipBufB = in;                                   /* 0x0573 ISW */
    vec4 p = mem[in++], color = mem[in++], st = mem[in++]; /* 0x0574, 0x0575, 0x0576 LQI */

    do {                                             /* 0x0577, from 0x05BD */
        clip = (clip << 6) | clip_bits(p.xyz, p.w);  /* 0x0577 CLIP */
        vec4 hereP = p;                              /* 0x0577 MOVE */
        p = mem[in++];                               /* 0x0578 LQI, the next vertex */
        clip = (clip << 6) | clip_bits(p.xyz, p.w);  /* 0x0579 CLIP */
        vec4 hereC = color;                         /* 0x0579 MOVE */
        vec4 hereS = st;                             /* 0x057A MOVE */
        color = mem[in++];                          /* 0x057B LQI */
        st = mem[in++];                              /* 0x057C LQI */
        int bits = clip & 0xfff;                     /* 0x057D FCGET */

        /* The result goes to the zero register, so this subtract exists only for the MAC flags. */
        vec4 delta;
        delta.xyzw = p.xyzw - hereP.xyzw;            /* 0x057E SUB */
        int nextOut = bits & next;                   /* 0x057E IAND */
        int hereOut = bits & here;                   /* 0x057F IAND */
        count = count - 1;                           /* 0x0581 ISUBIU; 0x0580 nop */
        if (nextOut == 0) {                          /* 0x0580 IBNE */
            mem[out++] = hereP;                      /* 0x0582 SQI, keep this vertex */
            mem[out++] = hereC;                      /* 0x0583 SQI */
            mem[out++] = hereS;                      /* 0x0584 SQI */
            emittedCount = emittedCount + 1;         /* 0x0586 IADDI; 0x0585 nop */
            if (hereOut == 0) continue;              /* 0x0585 IBEQ, both inside */
            goto crossing;                           /* 0x0587 B; 0x0588 nop */
        }
        if (hereOut != 0) continue;                  /* 0x0589 IBNE, both outside; 0x058A nop */

    crossing:                                        /* 0x058B, from 0x0587 */
        ;
        int reversed = axis & (mac & 0xffff);        /* 0x058B FMAND, the delta's sign */
        vec4 from, to, fromC, toC, fromS, toS;
        if (reversed != 0) {                         /* 0x058C IBNE; 0x058D nop */
            from = p;  to = hereP;                   /* 0x05A4..0x05AB, the mirrored form */
            fromC = color; toC = hereC;
            fromS = st;     toS = hereS;
        } else {
            from = hereP; to = p;                    /* 0x058E..0x0595 */
            fromC = hereC; toC = color;
            fromS = hereS; toS = st;
        }
        /* How far `from` is past the plane over how much the pair crosses it. */
        vec4 past, span;
        past.xyzw = plane.xyzw * from.w;             /* 0x058E / 0x05A4 MULw */
        span.xyzw = plane.xyzw * to.w;               /* 0x058F / 0x05A5 MULw */
        vec4 dP, dC, dS;
        dP.xyzw = to.xyzw - from.xyzw;               /* 0x0590 / 0x05A6 SUB */
        dC.xyzw = toC.xyzw - fromC.xyzw;             /* 0x0591 / 0x05A7 SUB */
        past.xyzw = from.xyzw - past.xyzw;           /* 0x0592 / 0x05A8 SUB */
        span.xyzw = to.xyzw - span.xyzw;             /* 0x0593 / 0x05A9 SUB */
        dS.xyzw = toS.xyzw - fromS.xyzw;             /* 0x0594 / 0x05AA SUB */
        span.xyzw = span.xyzw - past.xyzw;           /* 0x0595 / 0x05AB SUB */
        int zPlane = 48 & here;                      /* 0x058E, 0x058F / 0x05A4, 0x05A5 */
        int yPlane = 12 & here;                      /* 0x0590, 0x0591 / 0x05A6, 0x05A7 */
        if (zPlane != 0) {                           /* 0x0594 / 0x05AA IBNE */
            Q = past.z / span.z;                     /* 0x059C / 0x05B2 DIV */
        } else if (yPlane == 0) {                    /* 0x0596 / 0x05AC IBNE; 0x0597, 0x05AD nop */
            Q = past.x / span.x;                     /* 0x0599 / 0x05AF DIV; 0x0598, 0x05AE B */
        } else {
            Q = past.y / span.y;                     /* 0x059B / 0x05B1 DIV; 0x059A, 0x05B0 B */
        }
        vec4 t;
        t.x = Q;                                     /* 0x059D / 0x05B3 ADDq, waits on the DIV */
        t.x = fabsf(t.x);                            /* 0x059E / 0x05B4 ABS */
        vec4 cutP, cutC, cutS, acc;
        cutP.xyzw = from.xyzw + dP.xyzw * t.x;       /* 0x05A0 folds 0x059F; 0x05B6 folds 0x05B5 */
        acc.xyzw = fromC.xyzw;                       /* 0x05A1 MULAw / 0x05B8 */
        cutC.xyzw = acc.xyzw + dC.xyzw * t.x;        /* 0x05A2 MADDx; 0x05B8 folds 0x05B7 */
        acc.xyzw = fromS.xyzw;                       /* 0x05A3 MULAw / 0x05B9 MULAw */
        emittedCount = emittedCount + 1;             /* 0x05A3 / 0x05B9 IADDI */
        cutS.xyzw = acc.xyzw + dS.xyzw * t.x;        /* 0x05BA MADDx, from 0x05A2's B */
        mem[out++] = cutP;                           /* 0x05BA SQI */
        mem[out++] = cutC;                           /* 0x05BB SQI */
        mem[out++] = cutS;                           /* 0x05BC SQI */
    } while (count != 0);                            /* 0x05BD IBNE; 0x05BE nop */

    in = clipBufA;                                   /* 0x05C0 ILW; 0x05BF nop */
    if (emittedCount != 0) {                         /* 0x05BF IBEQ */
        /* Close the output polygon by repeating its first vertex. */
        p = mem[in++]; color = mem[in++]; st = mem[in++]; /* 0x05C1, 0x05C2, 0x05C3 LQI */
        mem[out++] = p;                              /* 0x05C4 SQI */
        mem[out++] = color;                         /* 0x05C5 SQI */
        mem[out++] = st;                             /* 0x05C7 SQI; 0x05C6 nop */
        goto_micro(clipEmitter);                     /* 0x05C6 JR, the next plane */
    }
    goto_micro(link);                                /* 0x05C8 JR; 0x05C9 nop */
}

/* ============================================================ the reflection pass */

/* Runs only when drawMode bit 1 is set, and it is the microcode behind
 * `corePktEmitTexTrisCtx2(cb, 0, sdf3DRefMapParam, ...)`: a sphere map indexed by the vertex
 * normal, drawn as its own textured pass through GS context 2.
 *
 * The texture coordinate is `0.5 + 0.5 * (M * normal)` divided by w, and the color is flat. So
 * this pass replaces both per-vertex arrays and leaves the positions alone, which is why it can run
 * after the ordinary draw without transforming anything again. drawFlags bit 11 picks the matrix,
 * the same pair lightVertex chooses between. */
static void reflectPass(void) {                      /* 0x05CA, from 0x022F and drawSecond2 */
    int mode = drawMode;                             /* 0x05CA ILW */
    int wanted = mode & 2;                           /* 0x05CB IADDIU, 0x05CC IAND */
    if (wanted == 0) { goto_micro(link); return; }   /* 0x05CE IBEQ; 0x05CD, 0x05CF nop */

    int flags = drawFlags;                           /* 0x05D0 ILW */
    int src = srcNormals;                            /* 0x05D1 ILW */
    I = 0.5f;                                        /* 0x05D2 LOI */
    vec4 half;
    half.xyz = I;                                    /* 0x05D3 ADDi, the map's centre */
    int objectMatrix = flags & 2048;                 /* 0x05D3 IADDIU, 0x05D4 IAND */
    vec4 flat = reflectColor;                       /* 0x05D5 LQ */
    vec4 tint = drawTint;                            /* 0x05D7 LQ; 0x05D6 nop */
    vec4 m0;
    if (objectMatrix == 0) {                         /* 0x05D6 IBNE */
        m0 = objectNormal(0);                        /* 0x05D8, 0x05D9, 0x05DB LQ; 0x05DA B */
    } else {
        m0 = normalMatrix(0);                        /* 0x05DC, 0x05DD, 0x05DE LQ */
    }
    vec4 m1, m2;
    m0.xy = m0.xy * half.x;                          /* 0x05DF MULx, halved */
    vec4 n = mem[src++];                             /* 0x05DF LQI */
    m1.xy = m0.xy * half.x;                          /* 0x05E0 MULx */
    int count = vertexCount;                         /* 0x05E0 ILW */
    m2.xy = m0.xy * half.x;                          /* 0x05E1 MULx */
    int uv = 848;                                    /* 0x05E1 IADDIU */
    flat.xyzw = flat.xyzw * tint.xyzw;               /* 0x05E2 MUL */
    int out = 800;                                   /* 0x05E2 IADDIU */
    vec4 acc;
    acc.xy = half.xy;                                /* 0x05E3 MULAw, the 0.5 offset */

    do {
        acc.xy = acc.xy + m0.xy * n.x;               /* 0x05E4 MADDAx */
        vec4 recip;
        recip.z = mem[uv].z;                         /* 0x05E4 LQ */
        acc.xy = acc.xy + m1.xy * n.y;               /* 0x05E5 MADDAy */
        count = count - 1;                           /* 0x05E5 ISUBIU */
        vec4 st;
        st.xy = acc.xy + m2.xy * n.z;                /* 0x05E6 MADDz */
        I = 1.0f;                                    /* 0x05E6 LOI, loaded and not used */
        mem[out++] = flat;                           /* 0x05E7 SQI, the flat color */
        st.xy = st.xy * recip.z;                     /* 0x05E8 MULz, divided through by w */
        n = mem[src++];                              /* 0x05E8 LQI */
        acc.xy = half.xy;                            /* 0x05EA MULAw; 0x05E9 nop */
        mem[uv++].xy = st.xy;                        /* 0x05EA SQI, the sphere-map UV */
    } while (count != 0);                            /* 0x05E9 IBNE */

    int saved = link;                                /* 0x05EB IADD */
    packetSix(reflectAdBlock);                       /* 0x05ED IADDIU, 0x05EC BAL */
    link = saved;                                    /* 0x05F0 IADD; 0x05EF nop */
    goto_micro(atEmitTexTriangles);                  /* 0x05EE IADDIU, 0x05EF JR, emitTexTriangles */
}

/* ============================================================ the billboard programs */

/* entryItop1521 adds the VIF's ITOP to 1521, so the EE picks one of these five by writing 0, 2, 4,
 * 6 or 8. Entry 0 is setup and draws nothing; the other four share a shape, a record count and a
 * GIF tag followed by parallel arrays of centres, corner offsets, colors and texture coordinates,
 * one sprite an iteration. They differ in how many registers a vertex takes and whether a second
 * pass follows.
 *
 * The two projections differ in where the divide lands. spriteProject builds the corner in view
 * space and divides at the end, so a sprite's size is in world units; spriteProjectQ divides the
 * axes first, so its size is in screen units and does not shrink with distance. Both accumulate the
 * AND of all four corners' clip bits, and the caller emits the sprite only when that comes out
 * zero, so a sprite is kept or dropped whole and never clipped.
 *
 * `axisU`, `axisV` and `centreBias` are the billboard's two axes and its offset. matrixScale writes
 * the axes; nothing in this image writes the offset, so it arrives from the EE. */

static void spriteDispatch(void) {
    spriteTop = stream; matrixScale();               /* 0x05F1 B, 0x05F2 ISW, entry 0 */
    spriteTop = stream; spriteOne();                 /* 0x05F3 B, 0x05F4 ISW, entry 2 */
    spriteTop = stream; spriteTwo();                 /* 0x05F5 B, 0x05F6 ISW, entry 4 */
    spriteTop = stream; spriteThree();               /* 0x05F7 B, 0x05F8 ISW, entry 6 */
    spriteTop = stream; spriteFour();                /* 0x05F9 B, 0x05FA ISW, entry 8 */
}

/* Divides the two billboard axes by the lengths of the camera matrix's first two columns, so a
 * sprite keeps its size whatever scale the matrix carries. The two square roots run on different
 * units, SQRT for x and ESQRT for y, so both are in flight at once. It ends the microprogram rather
 * than returning, since setting the axes is the whole job. */
static void matrixScale(void) {                      /* 0x05FB, from 0x05F1 */
    modelView = cameraMatrix(0);                     /* 0x05FB, 0x05FC, 0x05FD, 0x05FE LQ */
    screenScale = screenScaleMem;                    /* 0x05FF LQ */
    axisU = mem[stream];                             /* 0x0600 LQ */
    vec4 len;
    len.xy = modelView.xy * modelView.xy
           + modelView.xy * modelView.xy
           + modelView.xy * modelView.xy;            /* 0x0601, folds 0x05FF..0x0601 */
    axisV = mem[stream + 1];                         /* 0x0601 LQ */
    centreBias = mem[stream + 2];                    /* 0x0602 LQ */
    P = sqrtf(len.y);                                /* 0x0603 ESQRT, on the EFU */
    Q = sqrtf(fabsf(len.x));                         /* 0x0604 SQRT */
    len.x = Q;                                       /* 0x0605 ADDq, waits on the SQRT */
    len.y = P;                                       /* 0x0606 MFP, and on the EFU */
    axisU.xy = axisU.xy * len.xy;                    /* 0x0607 MUL */
    axisV.xy = axisV.xy * len.xy;                    /* 0x0608 MUL */
    return;                                             /* [E] at 0x0607 */
}

/* Ten quadwords a sprite: a tag word, one attribute, and four color/position pairs. The record
 * layout, all counted by the sprite count at mem[stream].x:
 *   mem[stream]      the GIF tag, copied out first
 *   mem[stream + 1]  .x a per-sprite tag word written into every sprite's first quadword
 *   centres, one quadword a sprite; one further attribute a sprite; four colors; four corners
 * The GIF loop counter is fixed up by 32767 at the end, which wraps to minus one and undoes the
 * extra it started at. */
static void spriteOne(void) {                        /* 0x0609, from 0x05F3 */
    int count = mem_w[stream].x;                     /* 0x0609 ILW */
    vec4 tag = mem[stream];                          /* 0x060A LQ */
    int out = cursors.y;                             /* 0x060B ILW */
    int tagWord = mem_w[stream + 1].x;               /* 0x060C ILW */
    axisU.zw = 0.0f; axisV.zw = 0.0f; centreBias.zw = 0.0f; /* 0x060B, 0x060C, 0x060D MULx */
    vec4 attr, corner;
    attr.xyzw = 0.0f; corner.xyzw = 0.0f;            /* 0x0609, 0x060A MULx */
    int centres = stream + 2;                        /* 0x060D IADDIU */
    int attrs = centres + count;                     /* 0x060E IADD */
    mem[out++] = tag;                                /* 0x060F SQI */
    int colors = attrs + count;                     /* 0x0610 IADD */
    vec4 centre = mem[centres++];                    /* 0x0611 LQI */
    int four = count + count + count + count;        /* 0x0612, 0x0613 IADD */
    int corners = colors + four;                    /* 0x0614 IADD */
    vec4 acc;
    acc.xyzw = modelView.xyzw * centre.x;            /* 0x0615 MULAx */
    int loops = 1;                                   /* 0x0615 IADDIU */

    do {                                             /* 0x0616, from 0x062D */
        acc.xyzw = acc.xyzw + modelView.xyzw * centre.y; /* 0x0616 MADDAy */
        attr = mem[attrs++];                         /* 0x0616 LQI */
        acc.xyzw = acc.xyzw + modelView.xyzw * centre.z; /* 0x0617 MADDAz */
        vec4 c0, c1, c2, c3;
        c0.xy = mem[corners++].xy;                   /* 0x0617 LQI */
        spriteCentre.xyzw = acc.xyzw + modelView.xyzw; /* 0x0618 MADDw, the centre in view space */
        c1.xy = mem[corners++].xy;                   /* 0x0618 LQI */
        c2.xy = mem[corners++].xy;                   /* 0x0619 LQI */
        c3.xy = mem[corners++].xy;                   /* 0x061A LQI */
        count = count - 1;                           /* 0x061B ISUBIU */
        clip = (clip << 6) | clip_bits(spriteCentre.xyz, spriteCentre.w); /* 0x061C CLIP */
        Q = 1.0f / spriteCentre.w;                   /* 0x061C DIV */
        c0.zw = 0.0f;                                /* 0x061D MULx */
        vec4 k0 = mem[colors++];                    /* 0x061D LQI */
        c1.zw = 0.0f;                                /* 0x061E MULx */
        vec4 k1 = mem[colors++];                    /* 0x061E LQI */
        c2.zw = 0.0f;                                /* 0x061F MULx */
        vec4 k2 = mem[colors++];                    /* 0x061F LQI */
        c3.zw = 0.0f;                                /* 0x0620 MULx */
        int behind = (clip & 0x000030) != 0;         /* 0x0620 FCAND, the centre against z */
        vec4 k3 = mem[colors++];                    /* 0x0621 LQI */
        centre = mem[centres++];                     /* 0x0623 LQI; 0x0622 nop */
        if (behind == 0) {                           /* 0x0622 IBNE */
            spriteProject(tagWord, out, attr, c0, c1, c2, c3, k0, k1, k2, k3); /* 0x0624 BAL */
            mem[out + 3] = c0;                       /* 0x0627 SQ; 0x0625, 0x0626 nop */
            if (cornerClip == 0) {                   /* 0x0626 IBNE, all four on screen */
                mem[out + 5] = c1;                   /* 0x0628 SQ */
                mem[out + 7] = c2;                   /* 0x0629 SQ */
                mem[out + 9] = c3;                   /* 0x062A SQ */
                out = out + 10;                      /* 0x062B IADDIU */
                loops = loops + 1;                   /* 0x062C IADDIU */
            }
        }
        acc.xyzw = modelView.xyzw * centre.x;        /* 0x062E MULAx; 0x062D nop */
    } while (count != 0);                            /* 0x062D IBNE */

    int tagAt = cursors.y;                           /* 0x062E ILW */
    loops = loops + 32767;                           /* 0x062F IADDIU, wraps to one less */
    mem_w[tagAt].x = loops;                          /* 0x0631 ISW; 0x0630 nop */
    kickAndEnd();                                    /* 0x0630 B */
}

/* spriteOne's arrays with twice as many attributes, and each sprite written out twice: once under
 * the tag word and once under that plus 512, PRIM's CTXT bit, so the second pass draws from GS
 * context 2. The positions are shared between the passes. */
static void spriteTwo(void) {                        /* 0x0632, from 0x05F5 */
    int count = mem_w[stream].x;                     /* 0x0632 ILW */
    vec4 tag = mem[stream];                          /* 0x0633 LQ */
    int out = cursors.y;                             /* 0x0634 ILW */
    int tagWord = mem_w[stream + 1].x;               /* 0x0635 ILW */
    axisU.zw = 0.0f; axisV.zw = 0.0f; centreBias.zw = 0.0f; /* 0x0634, 0x0635, 0x0636 MULx */
    vec4 attr, attr2;
    attr.xyzw = 0.0f; attr2.xyzw = 0.0f;             /* 0x0632, 0x0633 MULx */
    int centres = stream + 2;                        /* 0x0636 IADDIU */
    int attrs = centres + count;                     /* 0x0637 IADD */
    mem[out++] = tag;                                /* 0x0638 SQI */
    int two = count + count;                         /* 0x0639 IADD */
    int colors = attrs + two;                       /* 0x063A IADD, two attributes a sprite */
    vec4 centre = mem[centres++];                    /* 0x063B LQI */
    int four = two + two;                            /* 0x063C IADD */
    int corners = colors + four;                    /* 0x063D IADD */
    int loops = 1;                                   /* 0x063E IADDIU */
    int tagWord2 = tagWord + 512;                    /* 0x063F IADDIU, the same tag in context 2 */

    do {                                             /* 0x0640, from 0x0662 */
        attr = mem[attrs++];                         /* 0x0640 LQI */
        vec4 c0, c1, c2, c3;
        c0.xy = mem[corners++].xy;                   /* 0x0641 LQI */
        c1.xy = mem[corners++].xy;                   /* 0x0642 LQI */
        spriteCentre.xyzw = mul4x4(modelView, centre.xyzw); /* 0x0643, folds 0x0640..0x0643 */
        c2.xy = mem[corners++].xy;                   /* 0x0643 LQI */
        c3.xy = mem[corners++].xy;                   /* 0x0644 LQI */
        count = count - 1;                           /* 0x0645 ISUBIU */
        attr2 = mem[attrs++];                        /* 0x0646 LQI, the second pass's */
        clip = (clip << 6) | clip_bits(spriteCentre.xyz, spriteCentre.w); /* 0x0647 CLIP */
        Q = 1.0f / spriteCentre.w;                   /* 0x0647 DIV */
        c0.zw = 0.0f;                                /* 0x0648 MULx */
        vec4 k0 = mem[colors++];                    /* 0x0648 LQI */
        c1.zw = 0.0f;                                /* 0x0649 MULx */
        vec4 k1 = mem[colors++];                    /* 0x0649 LQI */
        c2.zw = 0.0f;                                /* 0x064A MULx */
        vec4 k2 = mem[colors++];                    /* 0x064A LQI */
        c3.zw = 0.0f;                                /* 0x064B MULx */
        int behind = (clip & 0x000030) != 0;         /* 0x064B FCAND */
        vec4 k3 = mem[colors++];                    /* 0x064C LQI */
        centre = mem[centres++];                     /* 0x064E LQI; 0x064D nop */
        if (behind == 0) {                           /* 0x064D IBNE */
            spriteProject(tagWord, out, attr, c0, c1, c2, c3, k0, k1, k2, k3); /* 0x064F BAL */
            mem[out + 3] = c0;                       /* 0x0652 SQ; 0x0650, 0x0651 nop */
            if (cornerClip == 0) {                   /* 0x0651 IBNE */
                mem[out + 5] = c1;                   /* 0x0653 SQ */
                mem[out + 7] = c2;                   /* 0x0654 SQ */
                mem[out + 9] = c3;                   /* 0x0655 SQ */
                mem_w[out + 10].x = tagWord2;        /* 0x0656 ISW, the second pass */
                mem[out + 11] = attr2;               /* 0x0657 SQ */
                mem[out + 12] = k0;                  /* 0x0658 SQ */
                mem[out + 13] = c0;                  /* 0x0659 SQ, the same positions */
                mem[out + 14] = k1;                  /* 0x065A SQ */
                mem[out + 15] = c1;                  /* 0x065B SQ */
                mem[out + 16] = k2;                  /* 0x065C SQ */
                mem[out + 17] = c2;                  /* 0x065D SQ */
                mem[out + 18] = k3;                  /* 0x065E SQ */
                mem[out + 19] = c3;                  /* 0x065F SQ */
                out = out + 20;                      /* 0x0660 IADDIU */
                loops = loops + 2;                   /* 0x0661 IADDIU, two GIF loops */
            }
        }
        cursors.y = cursors.y;                       /* 0x0663 ILW; 0x0662 nop */
    } while (count != 0);                            /* 0x0662 IBNE */

    int tagAt = cursors.y;                           /* 0x0663 ILW */
    loops = loops + 32767;                           /* 0x0664 IADDIU */
    mem_w[tagAt].x = loops;                          /* 0x0666 ISW; 0x0665 nop */
    kickAndEnd();                                    /* 0x0665 B */
}

/* Four corners around one centre, then projected. The corner is `axisU * u + axisV * v +
 * (centre + centreBias)` in view space, so the axes are world sized, and the divide is folded into
 * the screen scale over w and applied at the end. Each corner is clipped and the four results
 * ANDed: 15 is the four x and y bits, so a sprite survives only when no single side rejects all
 * four.
 *
 * The MR32s rotate each color quadword by one lane, twice, which puts .z into .x: the colors
 * arrive with the byte the GS wants in a different lane from the one it is emitted in. */
static void spriteProject(int tagWord, int out, vec4 attr, vec4 c0, vec4 c1, vec4 c2, vec4 c3,
                          vec4 k0, vec4 k1, vec4 k2, vec4 k3) { /* 0x0667, from 0x0624 and 0x064F */
    vec4 origin, scaleQ;
    origin.xyz = spriteCentre.xyz + centreBias.xyz;  /* 0x0667 ADD */
    scaleQ.xyz = screenScale.xyz * Q;                /* 0x0668 MULq, the scale over w */
    vec4 acc;
    acc.xyz = axisU.xyz * c0.x;                      /* 0x0669, folded into 0x066B */
    mem_w[out].x = tagWord;                          /* 0x0669 ISW */
    acc.xyz = acc.xyz + axisV.xyz * c0.y;            /* 0x066A, folded into 0x066B */
    mem[out + 1] = attr;                             /* 0x066A SQ */
    c0.xyz = axisU.xyz * c0.x + axisV.xyz * c0.y + origin.xyz; /* 0x066B, folds 0x0669..0x066B */
    mem[out + 2] = k0;                               /* 0x066B SQ, color 0 */
    acc.xyz = axisU.xyz * c1.x;                      /* 0x066C, folded into 0x066E */
    mem[out + 4] = k1;                               /* 0x066C SQ */
    acc.xyz = acc.xyz + axisV.xyz * c1.y;            /* 0x066D, folded into 0x066E */
    mem[out + 6] = k2;                               /* 0x066D SQ */
    c1.xyz = axisU.xyz * c1.x + axisV.xyz * c1.y + origin.xyz; /* 0x066E, folds 0x066C..0x066E */
    mem[out + 8] = k3;                               /* 0x066E SQ */
    clip = (clip << 6) | clip_bits(c0.xyz, spriteCentre.w); /* 0x066F CLIP */
    k0 = mr32(k0);                                   /* 0x066F MR32 */
    k1 = mr32(k1);                                   /* 0x0670 MR32 */
    k2 = mr32(k2);                                   /* 0x0671 MR32 */
    c2.xyz = axisU.xyz * c2.x + axisV.xyz * c2.y + origin.xyz; /* 0x0672, folds 0x0670..0x0672 */
    cornerClip = 15;                                 /* 0x0672 IADDIU, the four x and y bits */
    int bits = clip & 0xfff;                         /* 0x0673 FCGET */
    clip = (clip << 6) | clip_bits(c1.xyz, spriteCentre.w); /* 0x0673 CLIP */
    cornerClip = cornerClip & bits;                  /* 0x0674 IAND */
    k3 = mr32(k3);                                   /* 0x0675 MR32 */
    c3.xyz = axisU.xyz * c3.x + axisV.xyz * c3.y + origin.xyz; /* 0x0676, folds 0x0674..0x0676 */
    k0 = mr32(k0);                                   /* 0x0676 MR32, the second rotation */
    bits = clip & 0xfff;                             /* 0x0677 FCGET */
    clip = (clip << 6) | clip_bits(c2.xyz, spriteCentre.w); /* 0x0677 CLIP */
    acc.xyz = screenOffset.xyz;                      /* 0x0678, folded into 0x0679 */
    cornerClip = cornerClip & bits;                  /* 0x0678 IAND */
    c0.xyz = screenOffset.xyz + c0.xyz * scaleQ.xyz; /* 0x0679, folds 0x0678..0x0679 */
    k1 = mr32(k1);                                   /* 0x0679 MR32 */
    clip = (clip << 6) | clip_bits(c3.xyz, spriteCentre.w); /* 0x067A CLIP */
    k2 = mr32(k2);                                   /* 0x067A MR32 */
    c1.xyz = acc.xyz + c1.xyz * scaleQ.xyz;          /* 0x067B MADD */
    bits = clip & 0xfff;                             /* 0x067B FCGET */
    c2.xyz = acc.xyz + c2.xyz * scaleQ.xyz;          /* 0x067C MADD */
    cornerClip = cornerClip & bits;                  /* 0x067C IAND */
    c3.xyz = acc.xyz + c3.xyz * scaleQ.xyz;          /* 0x067D MADD */
    k3 = mr32(k3);                                   /* 0x067D MR32 */
    c0.xyz = ftoi4(c0.xyz);                          /* 0x067E FTOI4, 12.4 fixed point */
    bits = clip & 0xfff;                             /* 0x067E FCGET */
    c1.xyz = ftoi4(c1.xyz);                          /* 0x067F FTOI4 */
    cornerClip = cornerClip & bits;                  /* 0x067F IAND */
    c2.xyz = ftoi4(c2.xyz);                          /* 0x0680 FTOI4 */
    c3.xyz = ftoi4(c3.xyz);                          /* 0x0681 FTOI4 */
    goto_micro(link);                                /* 0x0680 JR */
}

/* Three registers a vertex, so the arrays carry four texture coordinates a sprite as well and a
 * sprite is thirteen quadwords out. It projects through spriteProjectQ. */
static void spriteThree(void) {                      /* 0x0682, from 0x05F7 */
    int count = mem_w[stream].x;                     /* 0x0682 ILW */
    vec4 tag = mem[stream];                          /* 0x0683 LQ */
    int out = cursors.y;                             /* 0x0684 ILW */
    int tagWord = mem_w[stream + 1].x;               /* 0x0685 ILW */
    axisU.zw = 0.0f; axisV.zw = 0.0f; centreBias.zw = 0.0f; /* 0x0684, 0x0685, 0x0686 MULx */
    spriteCentre.xyzw = 0.0f;                        /* 0x0682, 0x0683 MULx */
    int centres = stream + 2;                        /* 0x0686 IADDIU */
    int texcoords = centres + count;                 /* 0x0687 IADD */
    mem[out++] = tag;                                /* 0x0688 SQI */
    int four = count + count + count + count;        /* 0x0689, 0x068A IADD */
    int colors = texcoords + four;                  /* 0x068B IADD */
    int corners = colors + four;                    /* 0x068C IADD */
    int loops = 1;                                   /* 0x068D IADDIU */

    do {                                             /* 0x068E, from 0x06A8 */
        vec4 centre = mem[centres++];                /* 0x068E LQI */
        vec4 t0 = mem[texcoords++], t1 = mem[texcoords++]; /* 0x068F, 0x0690 LQI */
        vec4 t2 = mem[texcoords++], t3 = mem[texcoords++]; /* 0x0691, 0x0692 LQI */
        vec4 c0, c1, c2, c3;
        c0.xy = mem[corners++].xy;                   /* 0x0693 LQI */
        c1.xy = mem[corners++].xy;                   /* 0x0694 LQI */
        spriteCentre.xyzw = mul4x4(modelView, centre.xyzw); /* 0x0695, folds 0x0692..0x0695 */
        c2.xy = mem[corners++].xy;                   /* 0x0695 LQI */
        c3.xy = mem[corners++].xy;                   /* 0x0696 LQI */
        count = count - 1;                           /* 0x0697 ISUBIU */
        clip = (clip << 6) | clip_bits(spriteCentre.xyz, spriteCentre.w); /* 0x0698 CLIP */
        Q = 1.0f / spriteCentre.w;                   /* 0x0698 DIV */
        c0.zw = 0.0f;                                /* 0x0699 MULx */
        vec4 k0 = mem[colors++];                    /* 0x0699 LQI */
        c1.zw = 0.0f;                                /* 0x069A MULx */
        vec4 k1 = mem[colors++];                    /* 0x069A LQI */
        c2.zw = 0.0f;                                /* 0x069B MULx */
        vec4 k2 = mem[colors++];                    /* 0x069B LQI */
        c3.zw = 0.0f;                                /* 0x069C MULx */
        int behind = (clip & 0x000030) != 0;         /* 0x069C FCAND */
        vec4 k3 = mem[colors++];                    /* 0x069E LQI; 0x069D nop */
        if (behind == 0) {                           /* 0x069D IBNE */
            spriteProjectQ(tagWord, out, c0, c1, c2, c3, k0, k1, k2, k3, t0, t1, t2, t3); /* 0x069F BAL */
            mem[out + 3] = c0;                       /* 0x06A2 SQ; 0x06A0, 0x06A1 nop */
            if (cornerClip == 0) {                   /* 0x06A1 IBNE */
                mem[out + 6] = c1;                   /* 0x06A3 SQ */
                mem[out + 9] = c2;                   /* 0x06A4 SQ */
                mem[out + 12] = c3;                  /* 0x06A5 SQ */
                out = out + 13;                      /* 0x06A6 IADDIU */
                loops = loops + 1;                   /* 0x06A7 IADDIU */
            }
        }
    } while (count != 0);                            /* 0x06A8 IBNE; 0x06A9 ILW */

    int tagAt = cursors.y;                           /* 0x06A9 ILW */
    loops = loops + 32767;                           /* 0x06AA IADDIU */
    mem_w[tagAt].x = loops;                          /* 0x06AC ISW; 0x06AB nop */
    kickAndEnd();                                    /* 0x06AB B */
}

/* spriteThree drawn twice, with a second set of four texture coordinates and the second pass again
 * under the tag word plus 512. Twenty-six quadwords a sprite. */
static void spriteFour(void) {                       /* 0x06AD, from 0x05F9 */
    int count = mem_w[stream].x;                     /* 0x06AD ILW */
    vec4 tag = mem[stream];                          /* 0x06AE LQ */
    int out = cursors.y;                             /* 0x06AF ILW */
    int tagWord = mem_w[stream + 1].x;               /* 0x06B0 ILW */
    axisU.zw = 0.0f; axisV.zw = 0.0f; centreBias.zw = 0.0f; /* 0x06AF, 0x06B0, 0x06B1 MULx */
    spriteCentre.xyzw = 0.0f;                        /* 0x06AD, 0x06AE MULx */
    int centres = stream + 2;                        /* 0x06B1 IADDIU */
    int texcoords = centres + count;                 /* 0x06B2 IADD */
    mem[out++] = tag;                                /* 0x06B3 SQI */
    int four = count + count + count + count;        /* 0x06B4, 0x06B5 IADD */
    int colors = texcoords + four + four;           /* 0x06B6, 0x06B7 IADD, eight texcoords */
    int corners = colors + four;                    /* 0x06B8 IADD */
    int loops = 1;                                   /* 0x06B9 IADDIU */
    int tagWord2 = tagWord + 512;                    /* 0x06BA IADDIU, context 2 */

    do {                                             /* 0x06BB, from 0x06E6 */
        vec4 centre = mem[centres++];                /* 0x06BB LQI */
        vec4 t0 = mem[texcoords++], t1 = mem[texcoords++]; /* 0x06BC, 0x06BD LQI */
        vec4 t2 = mem[texcoords++], t3 = mem[texcoords++]; /* 0x06BE, 0x06BF LQI */
        vec4 c0, c1, c2, c3;
        c0.xy = mem[corners++].xy;                   /* 0x06C0 LQI */
        c1.xy = mem[corners++].xy;                   /* 0x06C1 LQI */
        spriteCentre.xyzw = mul4x4(modelView, centre.xyzw); /* 0x06C2, folds 0x06BF..0x06C2 */
        c2.xy = mem[corners++].xy;                   /* 0x06C2 LQI */
        c3.xy = mem[corners++].xy;                   /* 0x06C3 LQI */
        count = count - 1;                           /* 0x06C4 ISUBIU */
        vec4 k0 = mem[colors++];                    /* 0x06C5 LQI */
        clip = (clip << 6) | clip_bits(spriteCentre.xyz, spriteCentre.w); /* 0x06C6 CLIP */
        Q = 1.0f / spriteCentre.w;                   /* 0x06C6 DIV */
        c0.zw = 0.0f;                                /* 0x06C7 MULx */
        vec4 k1 = mem[colors++];                    /* 0x06C7 LQI */
        c1.zw = 0.0f;                                /* 0x06C8 MULx */
        vec4 k2 = mem[colors++];                    /* 0x06C8 LQI */
        c2.zw = 0.0f;                                /* 0x06C9 MULx */
        vec4 k3 = mem[colors++];                    /* 0x06C9 LQI */
        c3.zw = 0.0f;                                /* 0x06CA MULx */
        int behind = (clip & 0x000030) != 0;         /* 0x06CA FCAND */
        vec4 u0 = mem[texcoords++], u1 = mem[texcoords++]; /* 0x06CB, 0x06CC LQI, the second pass */
        vec4 u2 = mem[texcoords++], u3 = mem[texcoords++]; /* 0x06CD, 0x06CF LQI; 0x06CE nop */
        if (behind == 0) {                           /* 0x06CE IBNE */
            spriteProjectQ(tagWord, out, c0, c1, c2, c3, k0, k1, k2, k3, t0, t1, t2, t3); /* 0x06D0 BAL */
            mem[out + 3] = c0;                       /* 0x06D3 SQ; 0x06D1, 0x06D2 nop */
            if (cornerClip == 0) {                   /* 0x06D2 IBNE */
                mem[out + 6] = c1;                   /* 0x06D4 SQ */
                mem[out + 9] = c2;                   /* 0x06D5 SQ */
                mem[out + 12] = c3;                  /* 0x06D6 SQ */
                mem_w[out + 13].x = tagWord2;        /* 0x06D7 ISW */
                mem[out + 14] = k0;                  /* 0x06D8 SQ */
                mem[out + 15] = u0;                  /* 0x06D9 SQ */
                mem[out + 16] = c0;                  /* 0x06DA SQ, the same positions */
                mem[out + 17] = k1;                  /* 0x06DB SQ */
                mem[out + 18] = u1;                  /* 0x06DC SQ */
                mem[out + 19] = c1;                  /* 0x06DD SQ */
                mem[out + 20] = k2;                  /* 0x06DE SQ */
                mem[out + 21] = u2;                  /* 0x06DF SQ */
                mem[out + 22] = c2;                  /* 0x06E0 SQ */
                mem[out + 23] = k3;                  /* 0x06E1 SQ */
                mem[out + 24] = u3;                  /* 0x06E2 SQ */
                mem[out + 25] = c3;                  /* 0x06E3 SQ */
                out = out + 26;                      /* 0x06E4 IADDIU */
                loops = loops + 2;                   /* 0x06E5 IADDIU */
            }
        }
    } while (count != 0);                            /* 0x06E6 IBNE; 0x06E7 ILW */

    int tagAt = cursors.y;                           /* 0x06E7 ILW */
    loops = loops + 32767;                           /* 0x06E8 IADDIU */
    mem_w[tagAt].x = loops;                          /* 0x06EA ISW; 0x06E9 nop */
    kickAndEnd();                                    /* 0x06E9 B */
}

/* spriteProject with the divide moved to the front: both axes and the centre are multiplied by Q
 * before the corner is built, so it comes out in normalized device coordinates and the clip is
 * taken against a hard 1.0 rather than against the centre's w. A sprite drawn this way keeps its
 * pixel size at any distance. */
static void spriteProjectQ(int tagWord, int out, vec4 c0, vec4 c1, vec4 c2, vec4 c3,
                           vec4 k0, vec4 k1, vec4 k2, vec4 k3,
                           vec4 t0, vec4 t1, vec4 t2, vec4 t3) { /* 0x06EB, from 0x069F, 0x06D0 */
    vec4 u, v, origin;
    u.xyz = axisU.xyz * Q;                           /* 0x06EB MULq, the first axis over w */
    v.xyz = axisV.xyz * Q;                           /* 0x06EC MULq */
    mem_w[out].x = tagWord;                          /* 0x06EC ISW */
    vec4 acc;
    acc.xyz = spriteCentre.xyz * Q;                  /* 0x06ED MULAq */
    mem[out + 1] = k0;                               /* 0x06ED SQ, color 0 */
    origin.xyz = spriteCentre.xyz * Q + centreBias.xyz * Q; /* 0x06EE, folds 0x06ED..0x06EE */
    mem[out + 2] = t0;                               /* 0x06EE SQ, texture coordinate 0 */
    acc.xyz = u.xyz * c0.x;                          /* 0x06EF, folded into 0x06F1 */
    mem[out + 4] = k1;                               /* 0x06EF SQ */
    acc.xyz = acc.xyz + v.xyz * c0.y;                /* 0x06F0, folded into 0x06F1 */
    mem[out + 5] = t1;                               /* 0x06F0 SQ */
    c0.xyz = u.xyz * c0.x + v.xyz * c0.y + origin.xyz; /* 0x06F1, folds 0x06EF..0x06F1 */
    mem[out + 7] = k2;                               /* 0x06F1 SQ */
    acc.xyz = u.xyz * c1.x;                          /* 0x06F2, folded into 0x06F4 */
    mem[out + 8] = t2;                               /* 0x06F2 SQ */
    acc.xyz = acc.xyz + v.xyz * c1.y;                /* 0x06F3, folded into 0x06F4 */
    mem[out + 10] = k3;                              /* 0x06F3 SQ */
    c1.xyz = u.xyz * c1.x + v.xyz * c1.y + origin.xyz; /* 0x06F4, folds 0x06F2..0x06F4 */
    mem[out + 11] = t3;                              /* 0x06F4 SQ */
    clip = (clip << 6) | clip_bits(c0.xyz, 1.0f);    /* 0x06F5 CLIP, against 1.0 */
    k0 = mr32(k0);                                   /* 0x06F5 MR32 */
    k1 = mr32(k1);                                   /* 0x06F6 MR32 */
    k2 = mr32(k2);                                   /* 0x06F7 MR32 */
    c2.xyz = u.xyz * c2.x + v.xyz * c2.y + origin.xyz; /* 0x06F8, folds 0x06F6..0x06F8 */
    cornerClip = 15;                                 /* 0x06F8 IADDIU */
    int bits = clip & 0xfff;                         /* 0x06F9 FCGET */
    clip = (clip << 6) | clip_bits(c1.xyz, 1.0f);    /* 0x06F9 CLIP */
    cornerClip = cornerClip & bits;                  /* 0x06FA IAND */
    k3 = mr32(k3);                                   /* 0x06FB MR32 */
    c3.xyz = u.xyz * c3.x + v.xyz * c3.y + origin.xyz; /* 0x06FC, folds 0x06FA..0x06FC */
    k0 = mr32(k0);                                   /* 0x06FC MR32 */
    bits = clip & 0xfff;                             /* 0x06FD FCGET */
    clip = (clip << 6) | clip_bits(c2.xyz, 1.0f);    /* 0x06FD CLIP */
    acc.xyz = screenOffset.xyz;                      /* 0x06FE, folded into 0x06FF */
    cornerClip = cornerClip & bits;                  /* 0x06FE IAND */
    c0.xyz = screenOffset.xyz + c0.xyz * screenScale.xyz; /* 0x06FF, folds 0x06FE..0x06FF */
    k1 = mr32(k1);                                   /* 0x06FF MR32 */
    clip = (clip << 6) | clip_bits(c3.xyz, 1.0f);    /* 0x0700 CLIP */
    k2 = mr32(k2);                                   /* 0x0700 MR32 */
    c1.xyz = acc.xyz + c1.xyz * screenScale.xyz;     /* 0x0701 MADD */
    bits = clip & 0xfff;                             /* 0x0701 FCGET */
    c2.xyz = acc.xyz + c2.xyz * screenScale.xyz;     /* 0x0702 MADD */
    cornerClip = cornerClip & bits;                  /* 0x0702 IAND */
    c3.xyz = acc.xyz + c3.xyz * screenScale.xyz;     /* 0x0703 MADD */
    k3 = mr32(k3);                                   /* 0x0703 MR32 */
    c0.xyz = ftoi4(c0.xyz);                          /* 0x0704 FTOI4 */
    bits = clip & 0xfff;                             /* 0x0704 FCGET */
    c1.xyz = ftoi4(c1.xyz);                          /* 0x0705 FTOI4 */
    cornerClip = cornerClip & bits;                  /* 0x0705 IAND */
    c2.xyz = ftoi4(c2.xyz);                          /* 0x0706 FTOI4 */
    c3.xyz = ftoi4(c3.xyz);                          /* 0x0707 FTOI4 */
    goto_micro(link);                                /* 0x0706 JR */
}

/* ============================================================ the direct vertex list */

/* What entry1800 reaches, and the simplest path in the image: a header the EE built is copied out
 * untouched, then a flat vertex list is projected and emitted, with no matrix, no lighting, no
 * clipping and no index list. The positions arrive already in view space, so all that happens is
 * the divide, the screen scale and offset, and the fog.
 *
 * mem[stream].y picks between two layouts and is the only branch in the program: zero means three
 * quadwords a vertex, position then texture coordinate then color, and non-zero means two.
 *
 * The fog is the same formula the transform computes, spelled with the offset in the screen
 * offset's .w and the scale in the screen scale's .w, so the MADDq that projects a position
 * computes the fog in its fourth lane at the same time. */
static void copyHeader(void) {                       /* 0x0708, from the stub at 0x0004 */
    int count = mem_w[stream].x;                     /* 0x0708 ILW */
    int layout = mem_w[stream].y;                    /* 0x0709 ILW */
    int out = cursors.y;                             /* 0x070A ILW */
    stream = stream + 1;                             /* 0x070B IADDIU */
    vec4 q = mem[stream++];                          /* 0x070D LQI; 0x070C nop */
    if (count != 0) {                                /* 0x070C IBEQ */
        do {
            count = count - 1;                       /* 0x070E ISUBIU */
            mem[out++] = q;                          /* 0x070F SQI */
            q = mem[stream++];                       /* 0x0711 LQI; 0x0710 nop */
        } while (count != 0);                        /* 0x0710 IBNE */
    }
    directSetup(layout, out, q);
}

/* The texture coordinate's .z is forced to 1.0 so it comes out as Q when the whole vector is
 * multiplied by it, which is the GS's own Q slot. */
static void directSetup(int layout, int out, vec4 header) { /* 0x0712, from 0x070C */
    vec4 p = mem[stream++];                          /* 0x0712 LQI, the first position */
    vec4 fog = fogParams;                            /* 0x0713 LQ */
    screenScale.w = fogParams.w;                     /* 0x0714 LQ, the fog scale */
    vec4 st, stOdd;
    st.z = 1.0f;                                     /* 0x0715 ADDw */
    int count = mem_w[stream - 2].x;                 /* 0x0715 ILW */
    stOdd.z = 1.0f;                                  /* 0x0716 ADDw */
    Q = 1.0f / p.w;                                  /* 0x0716 DIV */
    screenOffset.w = fog.z;                          /* 0x0717 MULz, the fog offset */
    count = count & 32767;                           /* 0x0717 IADDIU, 0x071A IAND */
    p.w = fog.w;                                     /* 0x0718 MULw */
    mem[out++] = header;                             /* 0x0718 SQI, the header's last quadword */
    p.xyz = p.xyz * screenScale.xyz;                 /* 0x0719 MUL */
    if (layout != 0) { directUntextured(count, out, p, fog); return; } /* 0x0719 IBNE */
    directTextured(count, out, p, fog, st, stOdd);
}

/* Three quadwords in a vertex, position then texture coordinate then color, and three out, texture
 * coordinate then color then position. Two vertices an iteration with an odd tail, one divide in
 * flight while the previous vertex is stored. */
static void directTextured(int count, int out, vec4 p, vec4 fog, vec4 st, vec4 stOdd) { /* 0x071B */
    st.xy = mem[stream].xy;                          /* 0x071B LQ, texture coordinate 0 */
    vec4 pNext = mem[stream + 2];                    /* 0x071C LQ, position 1 */
    p.xyzw = screenOffset.xyzw + p.xyzw * Q;         /* 0x071D, folds 0x071C..0x071D, and the fog */
    vec4 color = mem[stream + 1];                   /* 0x071E LQ */
    vec4 outSt;
    outSt.xyz = st.xyz * Q;                          /* 0x071F MULq, the UV over w, Q in .z */
    stOdd.xy = mem[stream + 3].xy;                   /* 0x071F LQ */
    vec4 pScaled;
    pScaled.w = fog.w;                               /* 0x0720 MULw */
    Q = 1.0f / pNext.w;                              /* 0x0720 DIV */
    p.w = min(p.w, fog.y);                           /* 0x0721 MINIy, the fog maximum */
    vec4 colorNext = mem[stream + 4];               /* 0x0721 LQ */
    pScaled.xyz = pNext.xyz * screenScale.xyz;       /* 0x0722 MUL */
    int odd = 1;                                     /* 0x0722 IADDIU */
    color.xyzw = ftoi0(color.xyzw);                /* 0x0723 FTOI0 */
    stream = stream + 5;                             /* 0x0723 IADDIU */
    odd = count & odd;                               /* 0x0724 IAND */
    count = count - odd;                             /* 0x0725 ISUB */

    do {
        vec4 acc;
        acc.xyzw = screenOffset.xyzw;                /* 0x0726 MULAw */
        p.w = max(p.w, fog.x);                       /* 0x0727 MAXx, the fog minimum */
        vec4 p2 = mem[stream++];                     /* 0x0727 LQI */
        vec4 pOut;
        pOut.xyzw = acc.xyzw + pScaled.xyzw * Q;     /* 0x0728 MADDq, waits on the DIV */
        vec4 outStOdd;
        outStOdd.xyz = stOdd.xyz * Q;                /* 0x0729 MULq */
        st.xy = mem[stream++].xy;                    /* 0x0729 LQI */
        vec4 colorOdd;
        colorOdd.xyzw = ftoi0(colorNext.xyzw);     /* 0x072A FTOI0 */
        vec4 c2 = mem[stream++];                     /* 0x072A LQI */
        p.xyzw = ftoi4(p.xyzw);                      /* 0x072B FTOI4 */
        Q = 1.0f / p2.w;                             /* 0x072B DIV */
        pOut.w = min(pOut.w, fog.y);                 /* 0x072C MINIy */
        vec4 p3 = mem[stream++];                     /* 0x072C LQI */
        p2.w = fog.w;                                /* 0x072D MULw */
        mem[out++] = outSt;                          /* 0x072D SQI, the texture coordinate */
        p2.xyz = p2.xyz * screenScale.xyz;           /* 0x072E MUL */
        mem[out++] = color;                         /* 0x072E SQI, the color */
        color.xyzw = ftoi0(c2.xyzw);                /* 0x072F FTOI0 */
        mem[out++] = p;                              /* 0x072F SQI, the position */
        pOut.w = max(pOut.w, fog.x);                 /* 0x0730 MAXx */
        stOdd.xy = mem[stream++].xy;                 /* 0x0730 LQI */
        colorNext = mem[stream++];                  /* 0x0731 LQI */
        p.xyzw = screenOffset.xyzw + p2.xyzw * Q;    /* 0x0732, folds 0x0731..0x0732 */
        outSt.xyz = st.xyz * Q;                      /* 0x0733 MULq */
        Q = 1.0f / p3.w;                             /* 0x0733 DIV */
        pOut.xyzw = ftoi4(pOut.xyzw);                /* 0x0734 FTOI4 */
        count = count - 2;                           /* 0x0734 ISUBIU, two a turn */
        pScaled.w = fog.w;                           /* 0x0735 MULw */
        mem[out++] = outStOdd;                       /* 0x0735 SQI */
        p.w = min(p.w, fog.y);                       /* 0x0736 MINIy */
        mem[out++] = colorOdd;                      /* 0x0736 SQI */
        pScaled.xyz = p3.xyz * screenScale.xyz;      /* 0x0737 MUL */
        mem[out++] = pOut;                           /* 0x0738 SQI */
    } while (count != 0);                            /* 0x0737 IBNE */

    p.w = max(p.w, fog.x);                           /* 0x073A MAXx; 0x0739 nop */
    if (odd == 0) { kickAndEnd(); return; }          /* 0x0739 IBEQ */
    p.xyzw = ftoi4(p.xyzw);                          /* 0x073B FTOI4, the odd vertex */
    mem[out++] = outSt;                              /* 0x073B SQI */
    mem[out++] = color;                             /* 0x073C SQI */
    mem[out++] = p;                                  /* 0x073E SQI; 0x073D nop */
    kickAndEnd();                                    /* 0x073D B */
}

/* Two quadwords a vertex, position then color, and two out, color then position, which is the
 * order emitTriangles writes as well. */
static void directUntextured(int count, int out, vec4 p, vec4 fog) { /* 0x073F, from 0x0719 */
    vec4 color = mem[stream++];                     /* 0x073F LQI */
    vec4 pNext = mem[stream++];                      /* 0x0740 LQI, position 1 */
    p.xyzw = screenOffset.xyzw + p.xyzw * Q;         /* 0x0741, folds 0x0740..0x0741 */
    Q = 1.0f / pNext.w;                              /* 0x0742 DIV */
    vec4 colorNext = mem[stream++];                 /* 0x0743 LQI */
    vec4 pScaled;
    pScaled.w = fog.w;                               /* 0x0744 MULw */
    int odd = 1;                                     /* 0x0744 IADDIU */
    pScaled.xyz = pNext.xyz * screenScale.xyz;       /* 0x0745 MUL */
    odd = count & odd;                               /* 0x0745 IAND */
    p.w = min(p.w, fog.y);                           /* 0x0746 MINIy */
    count = count - odd;                             /* 0x0746 ISUB */

    do {
        vec4 acc;
        acc.xyzw = screenOffset.xyzw;                /* 0x0747, folded into 0x0748 */
        vec4 p2 = mem[stream++];                     /* 0x0747 LQI */
        vec4 pOut;
        pOut.xyzw = acc.xyzw + pScaled.xyzw * Q;     /* 0x0748 MADDq, waits on the DIV */
        p.w = max(p.w, fog.x);                       /* 0x0749 MAXx */
        vec4 outColor;
        outColor.xyzw = ftoi0(color.xyzw);         /* 0x074A FTOI0 */
        count = count - 2;                           /* 0x074A ISUBIU */
        p2.xyz = p2.xyz * screenScale.xyz;           /* 0x074B MUL */
        Q = 1.0f / p2.w;                             /* 0x074B DIV */
        pOut.w = min(pOut.w, fog.y);                 /* 0x074C MINIy */
        color = mem[stream++];                      /* 0x074C LQI */
        p.xyzw = ftoi4(p.xyzw);                      /* 0x074D FTOI4 */
        p2.w = fog.w;                                /* 0x074E MULw */
        vec4 p3 = mem[stream++];                     /* 0x074E LQI */
        pOut.w = max(pOut.w, fog.x);                 /* 0x074F MAXx */
        mem[out++] = outColor;                      /* 0x074F SQI, the color */
        vec4 outColorOdd;
        outColorOdd.xyzw = ftoi0(colorNext.xyzw);  /* 0x0750 FTOI0 */
        mem[out++] = p;                              /* 0x0750 SQI, the position */
        p.xyzw = acc.xyzw + p2.xyzw * Q;             /* 0x0751 MADDq */
        p3.xyz = p3.xyz * screenScale.xyz;           /* 0x0752 MUL */
        Q = 1.0f / p3.w;                             /* 0x0752 DIV */
        pOut.xyzw = ftoi4(pOut.xyzw);                /* 0x0753 FTOI4 */
        colorNext = mem[stream++];                  /* 0x0753 LQI */
        pScaled = p3;                                /* 0x0754 MULw */
        mem[out++] = outColorOdd;                   /* 0x0754 SQI */
        p.w = min(p.w, fog.y);                       /* 0x0755 MINIy */
        mem[out++] = pOut;                           /* 0x0756 SQI */
    } while (count != 0);                            /* 0x0755 IBNE */

    vec4 outColor;
    outColor.xyzw = ftoi0(color.xyzw);             /* 0x0757 FTOI0 */
    p.w = max(p.w, fog.x);                           /* 0x0758 MAXx */
    if (odd == 0) { kickAndEnd(); return; }          /* 0x0757 IBEQ */
    p.xyzw = ftoi4(p.xyzw);                          /* 0x0759 FTOI4, the odd vertex */
    mem[out++] = outColor;                          /* 0x0759 SQI */
    mem[out++] = p;                                  /* 0x075B SQI; 0x075A nop */
    kickAndEnd();                                    /* 0x075A B */
}
