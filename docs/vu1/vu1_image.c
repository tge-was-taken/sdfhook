/* scripts/vu2c.py 1.1, from docs/vu1/vu1_images.jsonl, options: --clean --flow --keep-nops --names docs/vu1/vu1_names.txt.
 * 1 program, 1884 instructions.  No timestamp: one input and one set of options
 * always give the same file. */
/* Pseudo-C for VU1 microcode, one statement per instruction.  Nothing here links or runs; the
 * declarations are what lets an editor parse it, and a swizzle is one float rather than the lanes
 * it names, which is all a parser needs.
 *
 *   vfNN four float lanes .x .y .z .w   viNN 16-bit integer, writes truncate   ACC accumulator
 *   I Q P R  immediate, divide, EFU, random      status mac clip  flag registers
 *   mem[a]  data memory, quadword a              mem_w[a].x  the same word as an integer
 *
 *   as_float(v)  a bit copy, not a conversion (MFIR, RGET).  A lane out of memory prints this way
 *                when it cannot be a number, so it is a packed field: a GIF tag among them
 *   lo16(f)      low 16 bits of the bit pattern (MTIR)      mr32(v)  (v.y, v.z, v.w, v.x)
 *   ftoiN/itofN  convert with N fraction bits; the result stays raw bits, so only the scale shows
 *   opmul(a, b)  (a.y*b.z, a.z*b.x, a.x*b.y), the halves an OPMULA/OPMSUB pair crosses
 *   cross(a, b)  the cross product those halves come to    clamp(v, lo, hi)  min(max(v, lo), hi)
 *   mul3x3(m, v) m[0]*v.x + m[1]*v.y + m[2]*v.z, m the registers from m[0] up; mul4x4 adds m[3]*v.w
 *   clip_bits(v, w)  x > w, x < -w, y > w, y < -w, z > w, z < -w, with w = |ft.w|
 *   xgkick(a)    start the GS transfer at mem[a]           goto_micro(a)  jump away for good
 *   subroutine(f, ...)  a routine entry: f, then what f reads before writing it, which is what
 *                a caller has to set.  A BAL to f prints as f().  The map above says where f ends.
 *
 * A statement reads all of its right side before writing any lane, so a register on both sides is
 * the value the instruction started with.  Both halves of a pair read the register file as it
 * stood before the pair, so where the lower half reads what the upper writes, the lower prints
 * first.  A branch takes after its delay slot, so the condition is captured at the branch and the
 * goto printed after the delay slot.
 */
#include <stdbool.h>

typedef union {
    struct { float x, y, z, w; };
    float xy, xz, xw, yz, yw, zw, xyz, xyw, xzw, yzw, xyzw;
} vec4;

typedef union {
    struct { int x, y, z, w; };
    int xy, xz, xw, yz, yw, zw, xyz, xyw, xzw, yzw, xyzw;
} ivec4;

vec4 vf00, vf01, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf10, vf11, vf12, vf13, vf14, vf15;
vec4 vf16, vf17, vf18, vf19, vf20, vf21, vf22, vf23, vf24, vf25, vf26, vf27, vf28, vf29, vf30, vf31;
vec4 ACC;
int vi00, vi01, vi02, vi03, vi04, vi05, vi06, vi07, vi08, vi09, vi10, vi11, vi12, vi13, vi14, vi15;
float I, Q, P;
unsigned R, status, mac, clip;
int VIF1_TOP, VIF1_ITOP;
union { vec4 f[1024]; ivec4 w[1024]; } vu_mem;

#define mem vu_mem.f
#define mem_w vu_mem.w

float max(float a, float b), min(float a, float b), fabsf(float v), sqrtf(float v);
float ftoi0(float v), ftoi4(float v), ftoi12(float v), ftoi15(float v);
float itof0(float v), itof4(float v), itof12(float v), itof15(float v);
float as_float(int raw), opmul(vec4 a, vec4 b);
vec4 mr32(vec4 v);
int lo16(float v);
unsigned bits(float v), clip_bits(float v, float w), lfsr_next(unsigned r);
float esin(float v), eatan(float v), eexp(float v), eatan2(float a, float b);
float vec2f(float x, float y), vec3f(float x, float y, float z);
float vec4f(float x, float y, float z, float w);
float clamp(float v, float lo, float hi), cross(vec4 a, vec4 b);
float mul3x3(vec4 m, float v), mul4x4(vec4 m, float v);
void xgkick(int addr), goto_micro(int addr);

/* This declares f and drops the rest, so `subroutine(f, vf13, vi06)` parses and f() below it
 * resolves; the registers are a macro argument nothing expands, read rather than resolved. */
#define subroutine(f, ...)  void f(void)

/* simplified, and nothing is folded that was not asked for:
 *   const   vf00 is (0, 0, 0, 1) and vi00 is 0; I comes from the nearest LOI and a viNN from the
 *           IADDIU that set it, dropped at every label and after every jump
 *   join    a constant crosses a label when every path into it carries the same value
 *   alias   a register loaded from a named location prints under that name until it is loaded
 *           from another, kept through a step and through an update from itself
 *   acc     a run of ACC writes ending in one vf write prints as one expression
 *   mat     an ACC chain over consecutive registers broadcasting x, y, z (, w) prints as
 *           mul3x3/mul4x4
 *   cross   OPMULA(a, b) then OPMSUB(b, a) prints as cross(a, b)
 *   ptr     LQI/SQI/LQD/SQD print as mem[viNN++] or mem[--viNN]
 *   clamp   an adjacent MAX/MINI pair against known constants prints as clamp(v, lo, hi)
 *   flow    a branch whose shape is unambiguous prints as do/while, if or break
 *   else    an if whose body ends in a branch past the next block prints as if/else
 *   call    a BAL prints as a call, the routine map above says what each entry reaches, and a JR
 *           on a register only ever written as a link prints as return
 *   loops   a comment over each loop names its counter and step
 *   branch  a branch condition is inlined where the delay slot cannot change it */

/* 6A2E83C2520CA542: micro 0x0000..0x075B, 1884 instructions, uploaded to 0x0000 0 times */
/* reads before writing (linear scan): mac mem */
/* writes: ACC GS I P Q clip mem vf01 vf02 vf03 vf04 vf05 vf06 vf07 vf08 vf09 vf10 vf11 vf12 vf13 vf14 vf15 vf16 vf17 vf18 vf19 vf20 vf21 vf22 vf23 vf24 vf25 vf26 vf27 vf28 vf29 vf30 vf31 vi01 vi02 vi03 vi04 vi05 vi06 vi07 vi08 vi09 vi10 vi11 vi12 vi13 vi14 vi15 */
/* routines: entry, name, size, returns, callers, callees, other entries, what a call
 * writes, and whether one entry and a return are all there is to it. */
/*   0000 dispatch               4, ret 0002, +2 entries, writes 2 registers, not a function: it is also entered at 0001 0002 0003 */
/*   0003 sub_0003               4, ret 0005, +1 entries, writes 2 registers, not a function: it is also entered at 0004 0006 */
/*   0006 sub_0006               6, ret 000A, +1 entries, writes 2 registers, function */
/*   000E sub_000E               16, ret 001C, calls drawEntry2, +2 entries, writes unknown, not a function: it is also entered at 0010 */
/*   0020 sub_0020               2, leaves 0020, writes unknown, not a function: it leaves without coming back */
/*   0030 sub_0030               89, leaves 0087, from 001E, +4 entries, writes unknown, not a function: it leaves without coming back */
/*   0039 loadCamera             80, leaves 0087, from 0020, writes unknown, not a function: it leaves without coming back */
/*   004F composeMatrices        58, leaves 0087, from 002E, writes unknown, not a function: it leaves without coming back */
/*   0081 copyBlock              8, leaves 0087, from 002C, writes unknown, not a function: it leaves without coming back */
/*   0089 sub_0089               6, ret 008D, from 0087, writes 4 registers, function */
/*   008F lightSetup             87, ret 00E4, from 002A, +2 entries, writes 21 registers, not a function: it is also entered at 00C2 00D3 00DA */
/*   00C2 unpackTexture          36, ret 00E4, from 0022, writes 13 registers, not a function: it is also entered at 00D3 00DA */
/*   00D3 unpackTwo              19, ret 00E4, from 0028, writes 10 registers, not a function: it is also entered at 00DA */
/*   0100 returnPlain            3, ret 0101, writes 2 registers, function */
/*   012F returnReflect          3, ret 0130, writes 2 registers, function */
/*   0132 lightVertex            127, ret 0171 0186 0193 019B 01AF, from 01C6 01D2, +2 entries, writes 28 registers, not a function: it is also entered at 0180 019A */
/*   01B1 fillOne                8, ret 01B7, writes 2 registers, function */
/*   01B9 drawEntry              81, in 01B9..02F2, leaves 0230, leaves 02F1, leaves 021C, leaves 0222, from 000C, calls drawFront emitTexTriangles lightVertex negateNormals packetOne packetThree texcoordMatrix transformVertices unpackPointers, +16 entries, writes unknown, not a function: its body is not one run */
/*   01DF negateNormals          31, ret 01FC, from 01D0 042B, writes 9 registers, function */
/*   01FE texcoordMatrix         18, ret 020E, from 01DB 0436, +1 entries, writes 11 registers, not a function: it is also entered at 0200 */
/*   0210 drawFront              20, leaves 021C, leaves 0222, from 01CE, calls packetOne transformVertices, writes unknown, not a function: it leaves without coming back */
/*   0232 sub_0232               15, in 00DA..0234, ret 00E4, writes 8 registers, not a function: its body is not one run */
/*   0235 emitTriangles          132, in 0235..056D, ret 0278 056C, from 021C 0487, calls clipPlane clipReject kick, +2 entries, writes unknown, not a function: its body is not one run */
/*   027A emitClipped            24, in 027A..04A3, ret 04A2, calls packetClipped, +1 entries, writes 16 registers, not a function: its body is not one run */
/*   028A emitTexTriangles       141, in 028A..056D, ret 02D6 056C, from 0226 048F, calls clipPlane kick, +8 entries, writes 35 registers, not a function: its body is not one run */
/*   02D8 emitClippedTex         26, in 02D8..04A3, ret 04A2, calls packetClipped, +2 entries, writes 16 registers, not a function: its body is not one run */
/*   02F3 sub_02F3               41, ret 031A, writes 19 registers, not a function: the code above falls into it */
/*   031C packetOne              13, ret 0327, from 021A 0485, +1 entries, writes 3 registers, not a function: it is also entered at 0320 */
/*   0329 packetThree            19, ret 033A, from 0224 048D, writes 6 registers, function */
/*   033C packetClipped          17, ret 034B, from 027B 02D9, writes 4 registers, function */
/*   035D unpackPointers         167, in 034D..04B6, ret 04B3 035B 041D, leaves 04B5, from 01B9 041F, +5 entries, writes unknown, not a function: its body is not one run */
/*   03A8 sub_03A8               56, ret 03DE, writes 19 registers, not a function: it is also entered at 03BE */
/*   041F drawEntry2             81, in 00DA..049B, ret 00E4, leaves 02F1, leaves 0487, from 0010, calls drawFront2 emitTexTriangles lightRamp negateNormals packetOne packetThree texcoordMatrix transformVertices transformVerticesUv unpackPointers, +13 entries, writes unknown, not a function: its body is not one run */
/*   043A lightRamp              65, ret 0477 0479, from 0421 042D, writes 24 registers, function */
/*   047B drawFront2             54, in 00DA..049B, ret 00E4, leaves 0487, leaves 02F1, from 0429, calls emitTexTriangles packetOne packetThree transformVertices transformVerticesUv, writes unknown, not a function: its body is not one run */
/*   049C kick                   8, ret 04A2, from 026F 02CD 04A4, writes 2 registers, not a function: it is also entered at 04A2 */
/*   04A8 clipReject             15, ret 04B3, leaves 04B5, from 025E, writes unknown, not a function: it leaves without coming back */
/*   04B7 packetSix              26, ret 04CF, from 05EC, +1 entries, writes 9 registers, not a function: it is also entered at 04B9 */
/*   04D1 transformVertices      39, in 04A2..04F5, ret 04F4 04A2, from 0218 0483, writes 17 registers, not a function: its body is not one run */
/*   04F6 transformVerticesUv    49, in 04A2..0524, ret 0523 04A2, from 048B, writes 17 registers, not a function: its body is not one run */
/*   056E clipPlane              92, ret 05C6 05C8, from 0559 055C 0560 0563 0567 056A, writes 21 registers, function */
/*   05CA reflectPass            41, in 04A2..05F0, ret 05EF 04A2, calls packetSix, +1 entries, writes 22 registers, not a function: its body is not one run */
/*   0626 sub_0626               47, in 04A4..0631, ret 04B3, leaves 04B5, calls kick spriteProject, writes unknown, not a function: its body is not one run */
/*   0651 sub_0651               58, in 04A4..0666, ret 04B3, leaves 04B5, calls kick spriteProject, writes unknown, not a function: its body is not one run */
/*   0667 spriteProject          27, ret 0680, from 0624 064F, writes 14 registers, function */
/*   06A1 sub_06A1               50, in 04A4..06AC, ret 04B3, leaves 04B5, calls kick spriteProjectQ, writes unknown, not a function: its body is not one run */
/*   06D2 sub_06D2               67, in 04A4..06EA, ret 04B3, leaves 04B5, calls kick spriteProjectQ, writes unknown, not a function: its body is not one run */
/*   06EB spriteProjectQ         29, ret 0706, from 069F 06D0, writes 15 registers, function */
/*   0708 copyHeader             103, in 04A4..075B, ret 04B3, leaves 04B5, calls kick, +1 entries, writes unknown, not a function: its body is not one run */
/*   179 addresses no entry reaches: 000C 000D 001E 001F 0022 0023 0024 0025 0026 0027 0028 0029 002A 002B 002C 002D ... */
/* vi01 at 0002 0005 000A 001C 00FE 0101 012D 0130 0222 0230 05EF: one of 0001 0002 0003 0004 0006 0010 0020 0080 0100 0200 028A 02F0 0400 04A4 04B7 04B9 04F6 05CA 0708, or a value loaded at 01BB 02EA 02F6 031C 0329 04B9 */
/*   0005: 0708 here */
/*   001C: 04A4 here */
/*   00FE: 04B7 here */
/*   0101: 028A here */
/*   012D: 04B9 here */
/*   0130: 028A here */
/*   0222: 04F6 here */
/*   0230: 05CA here */
/*   05EF: 028A here */
/* vi15 at 008D 00E4 0171 0186 019B 01AF 01B7 01FC 020E 0278 02D6 0327 033A 035B 03DE 041D 0477 0479 04A2 04B3 04B5 04CF 04F4 0523 05C8 0680 0706: one of 0100 012F 0224 022F 0232 0271 02CF 0499 04A6, or a value loaded at 0232 0277 02D5 0318 0499 */
/* vi04 at 0193: one of 0030 0180 019A 0220 02C0 02F0 0320 0388 038D, or a value loaded at 00C3 0103 014B 023E 024B 028E 02A1 033E 035D 03E4 049C 04F6 052F 05D0 062E 0663 06A9 06E7 0708 0715 */
/* vi03 at 02F1 05C6: one of 0001 0080 0132 01B1 0213 0320 03A8 043A, or a value loaded at 0081 00DD 00ED 0104 0132 01BA 01DF 01F6 0420 0444 05D1 0609 0632 0682 06AD 070A */
/* vi13 at 031A 034B 056C: one of 027A 028A 02D8 */
/* fixed addresses with no name in docs/vu1/vu1_names.txt, by uses:
 *   mem[972] (3) mem[932] (3) mem[911] (3) mem[933] (2) mem[6] (2) mem[1] (2) mem[0] (2)
 *   mem[1009] (1) mem[1008] (1) mem[931] (1) mem[930] (1) mem[929] (1) mem[913] (1)
 *   mem[912] (1) mem[909] (1) mem[4] (1)
 */
/* names from docs/vu1/vu1_names.txt.  Each is the subscript it stands for, so the listing is still the
 * same C it was without them. */
/* mem[914].w is read as a float and as an integer, so it keeps its subscript */
#define position         (mem + 704)
#define normal           (mem + 752)
#define vertexColor      (mem + 800)
#define texcoord         (mem + 848)
#define faceCount        mem_w[896].w
#define vertexCount      mem_w[897].w
#define indexList        mem_w[898].w
#define outPosition      mem_w[899].w
#define srcNormals       mem_w[900].w
#define srcTexcoords     mem_w[901].w
#define srcColors        mem_w[902].w
#define drawFlags        mem_w[903].w
#define drawMode         mem_w[904].w
#define primFlags        mem_w[907].w
#define lastPrim         mem_w[908].w
#define flatten          mem[910].w
#define savedPointer     mem_w[915].w
#define materialChanged  mem_w[917].w
#define fogParams        mem[919]
#define lightDir0        mem[920]
#define lightDir1        mem[921]
#define lightDir2        mem[922]
#define colorBase        mem[923]
#define color0           mem[924]
#define color1           mem[925]
#define color2           mem[926]
#define color3           mem[927]
#define color4           mem[928]
#define screenScale      mem[934]
#define objectCamera0    mem[964]
#define objectCamera1    mem[965]
#define objectCamera2    mem[966]
#define objectCamera3    mem[967]
#define lightNormal0     mem[968]
#define lightNormal1     mem[969]
#define lightNormal2     mem[970]
#define tintScale        mem[971]
#define reflectColor     mem[973]
#define uvMatrix0        mem[974]
#define uvMatrix1        mem[975]
#define uvMatrix2        mem[976]
#define uvMatrixB0       mem[977]
#define uvMatrixB1       mem[978]
#define uvMatrixB2       mem[979]
#define rampGain         mem[980].w
#define rampBias         mem[981].w
#define tagTriangles     mem[981]
#define tagTexTriangles  mem[982]
#define cameraNormal0    mem[988]
#define cameraNormal1    mem[989]
#define cameraNormal2    mem[990]
#define cameraNormal3    mem[991]
#define camera0          mem[992]
#define camera1          mem[993]
#define camera2          mem[994]
#define camera3          mem[995]
#define viewNormal0      mem[996]
#define viewNormal1      mem[997]
#define viewNormal2      mem[998]
#define objectNormal0    mem[999]
#define objectNormal1    mem[1000]
#define objectNormal2    mem[1001]
#define matDiffuse       mem[1002]
#define lightBase        mem[1003]
#define matAmbient       mem[1004]
#define drawColor        mem[1005]
#define drawTint2        mem[1006]
#define drawTint         mem[1007]
#define windingRef       mem[1011]
#define rampGainAdd      mem[1014].x
#define rampBiasAdd      mem[1015].x
#define spriteTop        mem_w[1020].y
#define retryHead        mem_w[1021].w
#define clipBufA         mem_w[1021].y
#define clipBufB         mem_w[1021].z
#define cursors          mem_w[1023]
/* the routines the calls below name, and what a caller has to set: */
subroutine(lightVertex, vf15);                                           /* 0132 */
subroutine(drawEntry, clip, mac, vf01, vf15, vf26, vf27, vf28, vf29, vf30, vf31, vi02, vi14); /* 01B9 */
subroutine(negateNormals, nothing);                                      /* 01DF */
subroutine(texcoordMatrix, nothing);                                     /* 01FE */
subroutine(drawFront, vf01, vf26, vf27, vf28, vf29, vi14);               /* 0210 */
subroutine(emitTexTriangles, mac, vf01, vf30, vf31, vi14);               /* 028A */
subroutine(packetOne, nothing);                                          /* 031C */
subroutine(packetThree, nothing);                                        /* 0329 */
subroutine(packetClipped, vf02, vi10);                                   /* 033C */
subroutine(unpackPointers, clip, vf01, vi02);                            /* 035D */
subroutine(drawEntry2, clip, mac, vf01, vf15, vf18, vf26, vf27, vf28, vf29, vf30, vf31, vi02, vi14); /* 041F */
subroutine(lightRamp, vf15);                                             /* 043A */
subroutine(drawFront2, mac, vf01, vf18, vf26, vf27, vf28, vf29, vf30, vf31, vi14); /* 047B */
subroutine(kick, nothing);                                               /* 049C */
subroutine(clipReject, clip);                                            /* 04A8 */
subroutine(packetSix, nothing);                                          /* 04B7 */
subroutine(transformVertices, vf01, vf26, vf27, vf28, vf29);             /* 04D1 */
subroutine(transformVerticesUv, vf01, vf16, vf17, vf18, vf26, vf27, vf28, vf29); /* 04F6 */
subroutine(clipPlane, mac, vf13, vi06, vi10, vi11);                      /* 056E */
subroutine(spriteProject, Q, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf13, vf15, vf23, vf24, vf25, vf30, vf31, vi06, vi12); /* 0667 */
subroutine(spriteProjectQ, Q, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf13, vf14, vf15, vf16, vf17, vf23, vf24, vf25, vf30, vf31, vi06, vi12); /* 06EB */
/* a register a routine is handed a record through: */
#define matPacket                (mem + vi02)
#define objectMatrix             (mem + vi02)
#define twoPacket                (mem + vi02)
/* while a register holds one of those, it prints under its name: */
#define cameraNormal0_vf02       vf02
#define flatten_vf02             vf02
#define rampGainAdd_vf02         vf02
#define tagTexTriangles_vf02     vf02
#define tagTriangles_vf02        vf02
#define cameraNormal1_vf03       vf03
#define drawTint_vf03            vf03
#define matAmbient_vf03          vf03
#define windingRef_vf03          vf03
#define cameraNormal2_vf04       vf04
#define drawTint_vf04            vf04
#define matDiffuse_vf04          vf04
#define position_vf04            vf04
#define normal_vf05              vf05
#define position_vf05            vf05
#define rampBias_vf05            vf05
#define texcoord_vf05            vf05
#define camera0_vf06             vf06
#define position_vf06            vf06
#define vertexColor_vf06         vf06
#define camera1_vf07             vf07
#define colorBase_vf07           vf07
#define position_vf07            vf07
#define vertexColor_vf07         vf07
#define camera2_vf08             vf08
#define flatten_vf08             vf08
#define texcoord_vf08            vf08
#define vertexColor_vf08         vf08
#define camera3_vf09             vf09
#define vertexColor_vf09         vf09
#define position_vf10            vf10
#define rampGain_vf10            vf10
#define texcoord_vf10            vf10
#define rampBiasAdd_vf11         vf11
#define texcoord_vf11            vf11
#define tintScale_vf11           vf11
#define texcoord_vf12            vf12
#define vertexColor_vf12         vf12
#define lightDir0_vf14           vf14
#define reflectColor_vf14        vf14
#define drawColor_vf15           vf15
#define drawTint_vf15            vf15
#define fogParams_vf15           vf15
#define lightDir1_vf15           vf15
#define matDiffuse_vf15          vf15
#define color3_vf16              vf16
#define lightDir2_vf16           vf16
#define uvMatrixB0_vf16          vf16
#define colorBase_vf17           vf17
#define uvMatrix1_vf17           vf17
#define uvMatrixB1_vf17          vf17
#define uvMatrix2_vf18           vf18
#define uvMatrixB2_vf18          vf18
#define color0_vf19              vf19
#define color1_vf20              vf20
#define color2_vf21              vf21
#define color4_vf22              vf22
#define lightBase_vf22           vf22
#define camera0_vf26             vf26
#define camera1_vf27             vf27
#define camera2_vf28             vf28
#define fogParams_vf30           vf30
#define screenScale_vf30         vf30
#define materialChanged_vi01     vi01
#define primFlags_vi01           vi01
#define cursors_vi02             vi02
#define srcNormals_vi02          vi02
#define vertexCount_vi02         vi02
#define cursors_vi03             vi03
#define drawFlags_vi03           vi03
#define srcNormals_vi03          vi03
#define cursors_vi04             vi04
#define drawFlags_vi04           vi04
#define lastPrim_vi04            vi04
#define outPosition_vi04         vi04
#define srcNormals_vi04          vi04
#define cursors_vi05             vi05
#define faceCount_vi05           vi05
#define outPosition_vi05         vi05
#define vertexCount_vi05         vi05
#define drawMode_vi06            vi06
#define srcColors_vi06           vi06
#define srcNormals_vi06          vi06
#define srcTexcoords_vi06        vi06
#define srcTexcoords_vi07        vi07
#define clipBufA_vi08            vi08
#define cursors_vi08             vi08
#define indexList_vi08           vi08
#define savedPointer_vi08        vi08
#define srcNormals_vi08          vi08
#define clipBufB_vi09            vi09
#define cursors_vi09             vi09
#define cursors_vi11             vi11
#define srcNormals_vi11          vi11
#define cursors_vi12             vi12
#define indexList_vi12           vi12
#define retryHead_vi12           vi12

void vu1_6a2e83c2(void)
{
  bool take = false;
/* 0000 000002FF 800106BD XITOP       */   vi01 = VIF1_ITOP;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 7 more can reach here */
/* 0001 000002FF 1001081E IADDIU      */   vi01 = vi01 + 30;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
/* 0003 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;                    // delay slot of 0002
/* 0002 000002FF 48000800 JR          */   goto_micro(vi01);
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
/* 0004 000002FF 10010708 IADDIU      */   vi01 = 1800;
/* 0006 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;                    // delay slot of 0005
/* 0005 000002FF 48000800 JR          */   goto L_0708;                        // vi01 holds 0708 here
/* 0007 000002FF 8000033C             */   /* nop */
/* 0008 000002FF 800106BD XITOP       */   vi01 = VIF1_ITOP;
/* 0009 000002FF 10010DF1 IADDIU      */   vi01 = vi01 + 1521;
/* 000B 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;                    // delay slot of 000A
/* 000A 000002FF 48000800 JR          */   goto_micro(vi01);
/* 000D 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;                    // delay slot of 000C
/* 000C 000002FF 420F01AC BAL         */   drawEntry();
/* 000E 400002FF 8000033C             */   /* nop */
/* 000F 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 000E */
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
/* 0011 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;                    // delay slot of 0010
/* 0010 000002FF 420F040E BAL         */   drawEntry2();
/* 0012 400002FF 8000033C             */   /* nop */
/* 0013 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 0012 */
/* 0014 000002FF 800206BC XTOP        */   vi02 = VIF1_TOP;
/* 0015 000002FF 088B03FF ILW.y       */   cursors_vi11 = cursors.y;
/* 0016 000002FF 100104A4 IADDIU      */   vi01 = 1188;
/* 0017 000002FF 01E21000 LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 0018 000002FF 01E31001 LQ.xyzw     */   vf03.xyzw = mem[vi02 + 1].xyzw;
/* 0019 000002FF 01E41002 LQ.xyzw     */   vf04.xyzw = mem[vi02 + 2].xyzw;
/* 001A 000002FF 03E01397 SQ.xyzw     */   fogParams.xyzw = vf02.xyzw;
/* 001B 000002FF 03EB1800 SQ.xyzw     */   mem[cursors_vi11].xyzw = vf03.xyzw;
/* 001D 000002FF 03EB2001 SQ.xyzw     */   mem[cursors_vi11 + 1].xyzw = vf04.xyzw; // delay slot of 001C
/* 001C 000002FF 48000800 JR          */   goto L_04A4;                        // vi01 holds 04A4 here
/* 001E 00200047 40000011 SUBw.w      */   vf01.w = 0.0f;
/* 001F 01C00043 100F04A6 ADDw.xyz    */   vf01.xyz = 1.0f;                    // delay slot of 001E
/*                        IADDIU      */   vi15 = 1190;
/*                        B           */   goto L_0030;                        // call sub_0030 through vi15, returns to 04A6
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
/* 0021 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 0020
/* 0020 000002FF 40000018 B           */   goto L_0039;                        // call loadCamera through vi15, returns to 04A6
/* 0023 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 0022
/* 0022 000002FF 4000009F B           */   goto L_00C2;                        // call unpackTexture through vi15, returns to 04A6
/* 0024 400002FF 8000033C             */   /* nop */
/* 0025 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 0024 */
/* 0026 400002FF 8000033C             */   /* nop */
/* 0027 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 0026 */
/* 0029 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 0028
/* 0028 000002FF 400000AA B           */   goto L_00D3;                        // call unpackTwo through vi15, returns to 04A6
/* 002B 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 002A
/* 002A 000002FF 40000064 B           */   goto L_008F;                        // call lightSetup through vi15, returns to 04A6
/* 002D 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 002C
/* 002C 000002FF 40000054 B           */   goto L_0081;                        // call copyBlock through vi15, returns to 04A6
/* 002F 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 002E
/* 002E 000002FF 40000020 B           */   goto L_004F;                        // call composeMatrices through vi15, returns to 04A6
subroutine(sub_0030, vi02);                                            /* 0030, from 001E */
/* a jump at 0193 can reach here */
L_0030:  /* from 001E */
/* 0030 000002FF 100E0400 IADDIU      */   vi14 = 1024;
/* 0031 000002FF 10040180 IADDIU      */   vi04 = 384;
/* 0032 000002FF 80002330 IADD        */   vi12 = 384;
/* 0033 000002FF 0A4403FF ISW.z       */   cursors.z = 384;
/* 0034 000002FF 0A8403FF ISW.y       */   cursors.y = 384;
/* 0035 000002FF 10040220 IADDIU      */   vi04 = 544;
/* 0036 000002FF 0A2403FF ISW.w       */   cursors.w = 544;
/* 0037 400002FF 0A20038D ISW.w       */   mem_w[909].w = 0;
/* 0038 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 0037 */
subroutine(loadCamera, vi02);                                          /* 0039, from 0020 */
L_0039:  /* from 0020 */
/* 0039 000002FF 01E6100A LQ.xyzw     */   vf06.xyzw = mem[vi02 + 10].xyzw;
/* 003A 000002FF 01E21000 LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 003B 000002FF 01E31001 LQ.xyzw     */   vf03.xyzw = mem[vi02 + 1].xyzw;
/* 003C 000002FF 01E41002 LQ.xyzw     */   vf04.xyzw = mem[vi02 + 2].xyzw;
/* 003D 000002FF 01E51003 LQ.xyzw     */   vf05.xyzw = mem[vi02 + 3].xyzw;
/* 003E 000002FF 01FA1004 LQ.xyzw     */   vf26.xyzw = mem[vi02 + 4].xyzw;
/* 003F 000002FF 01FB1005 LQ.xyzw     */   vf27.xyzw = mem[vi02 + 5].xyzw;
/* 0040 000002FF 01FC1006 LQ.xyzw     */   vf28.xyzw = mem[vi02 + 6].xyzw;
/* 0041 000002FF 01FD1007 LQ.xyzw     */   vf29.xyzw = mem[vi02 + 7].xyzw;
/* 0042 000002FF 01FE1008 LQ.xyzw     */   vf30.xyzw = mem[vi02 + 8].xyzw;
/* 0043 000002FF 01FF1009 LQ.xyzw     */   vf31.xyzw = mem[vi02 + 9].xyzw;
/* 0044 01E6D1BC 03E013DC SQ.xyzw     */   cameraNormal0.xyzw = vf02.xyzw;     // MULAx.xyzw folded into 0047
/* 0045 01E6D8BD 03E01BDD SQ.xyzw     */   cameraNormal1.xyzw = vf03.xyzw;     // MADDAy.xyzw folded into 0047
/* 0046 01E6E0BE 03E023DE SQ.xyzw     */   cameraNormal2.xyzw = vf04.xyzw;     // MADDAz.xyzw folded into 0047
/* 0047 01E0E98B 03E02BDF MADDw.xyzw  */   vf06.xyzw = vf26.xyzw * vf06.x + vf27.xyzw * vf06.y + vf28.xyzw * vf06.z + vf29.xyzw; // folds 0044..0047
/*                        SQ.xyzw     */   cameraNormal3.xyzw = vf05.xyzw;
/* 0048 000002FF 03E0D3E0 SQ.xyzw     */   camera0.xyzw = vf26.xyzw;
/* 0049 000002FF 03E0DBE1 SQ.xyzw     */   camera1.xyzw = vf27.xyzw;
/* 004A 000002FF 03E0E3E2 SQ.xyzw     */   camera2.xyzw = vf28.xyzw;
/* 004B 000002FF 03E0EBE3 SQ.xyzw     */   camera3.xyzw = vf29.xyzw;
/* 004C 000002FF 03E0F3A6 SQ.xyzw     */   screenScale.xyzw = vf30.xyzw;
/* 004D 400002FF 03E033F3 SQ.xyzw     */   windingRef.xyzw = vf06.xyzw;
/* 004E 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 004D */
subroutine(composeMatrices, vi02);                                     /* 004F, from 002E */
L_004F:  /* from 002E */
/* 004F 000002FF 01E21000 LQ.xyzw     */   vf02.xyzw = objectMatrix[0].xyzw;
/* 0050 000002FF 01E603E0 LQ.xyzw     */   camera0_vf06.xyzw = camera0.xyzw;
/* 0051 000002FF 01E703E1 LQ.xyzw     */   camera1_vf07.xyzw = camera1.xyzw;
/* 0052 000002FF 01E803E2 LQ.xyzw     */   camera2_vf08.xyzw = camera2.xyzw;
/* 0053 000002FF 01E903E3 LQ.xyzw     */   camera3_vf09.xyzw = camera3.xyzw;
/* 0054 01E231BC 01E31001 LQ.xyzw     */   vf03.xyzw = objectMatrix[1].xyzw;   // MULAx.xyzw folded into 0057
/* 0055 01E238BD 01E41002 LQ.xyzw     */   vf04.xyzw = objectMatrix[2].xyzw;   // MADDAy.xyzw folded into 0057
/* 0056 01E240BE 01E51003 LQ.xyzw     */   vf05.xyzw = objectMatrix[3].xyzw;   // MADDAz.xyzw folded into 0057
/* 0057 01E24E8B 01EA1004 MADDw.xyzw  */   vf26.xyzw = mul4x4(camera0_vf06, vf02.xyzw); // folds 0054..0057
/*                        LQ.xyzw     */   vf10.xyzw = objectMatrix[4].xyzw;
/* 0058 01E331BC 01EB1005 LQ.xyzw     */   vf11.xyzw = objectMatrix[5].xyzw;   // MULAx.xyzw folded into 005B
/* 0059 01E338BD 01EC1006 LQ.xyzw     */   vf12.xyzw = objectMatrix[6].xyzw;   // MADDAy.xyzw folded into 005B
/* 005A 01E340BE 01ED03A4 LQ.xyzw     */   vf13.xyzw = mem[932].xyzw;          // MADDAz.xyzw folded into 005B
/* 005B 01E34ECB 03E0D3C4 MADDw.xyzw  */   vf27.xyzw = mul4x4(camera0_vf06, vf03.xyzw); // folds 0058..005B
/*                        SQ.xyzw     */   objectCamera0.xyzw = vf26.xyzw;
/* 005C 01E431BC 01EE0398 LQ.xyzw     */   lightDir0_vf14.xyzw = lightDir0.xyzw; // MULAx.xyzw folded into 005F
/* 005D 01E438BD 01EF0399 LQ.xyzw     */   lightDir1_vf15.xyzw = lightDir1.xyzw; // MADDAy.xyzw folded into 005F
/* 005E 01E440BE 01F0039A LQ.xyzw     */   lightDir2_vf16.xyzw = lightDir2.xyzw; // MADDAz.xyzw folded into 005F
/* 005F 01E44F0B 03E0DBC5 MADDw.xyzw  */   vf28.xyzw = mul4x4(camera0_vf06, vf04.xyzw); // folds 005C..005F
/*                        SQ.xyzw     */   objectCamera1.xyzw = vf27.xyzw;
/* 0060 01E531BC 01E203DC LQ.xyzw     */   cameraNormal0_vf02.xyzw = cameraNormal0.xyzw; // MULAx.xyzw folded into 0063
/* 0061 01E538BD 01E303DD LQ.xyzw     */   cameraNormal1_vf03.xyzw = cameraNormal1.xyzw; // MADDAy.xyzw folded into 0063
/* 0062 01E540BE 01E403DE LQ.xyzw     */   cameraNormal2_vf04.xyzw = cameraNormal2.xyzw; // MADDAz.xyzw folded into 0063
/* 0063 01E54F4B 03E0E3C6 MADDw.xyzw  */   vf29.xyzw = mul4x4(camera0_vf06, vf05.xyzw); // folds 0060..0063
/*                        SQ.xyzw     */   objectCamera2.xyzw = vf28.xyzw;
/* 0064 01EA11BC 03E053E7 SQ.xyzw     */   objectNormal0.xyzw = vf10.xyzw;     // MULAx.xyzw folded into 0066
/* 0065 01EA18BD 03E05BE8 SQ.xyzw     */   objectNormal1.xyzw = vf11.xyzw;     // MADDAy.xyzw folded into 0066
/* 0066 01EA218A 03E063E9 MADDz.xyzw  */   vf06.xyzw = mul3x3(cameraNormal0_vf02, vf10.xyz); // folds 0064..0066
/*                        SQ.xyzw     */   objectNormal2.xyzw = vf12.xyzw;
/* 0067 01EB11BC 03E0EBC7 SQ.xyzw     */   objectCamera3.xyzw = vf29.xyzw;     // MULAx.xyzw folded into 0069
/* 0068 01EB18BD 8000033C             */   /* MADDAy.xyzw folded into 0069 */
/* 0069 01EB21CA 8000033C MADDz.xyzw  */   vf07.xyzw = mul3x3(cameraNormal0_vf02, vf11.xyz); // folds 0067..0069
/* 006A 01EC11BC 8000033C             */   /* MULAx.xyzw folded into 006C */
/* 006B 01EC18BD 8000033C             */   /* MADDAy.xyzw folded into 006C */
/* 006C 01EC220A 8000033C MADDz.xyzw  */   vf08.xyzw = mul3x3(cameraNormal0_vf02, vf12.xyz); // folds 006A..006C
/* 006D 01EA71BC 8000033C             */   /* MULAx.xyzw folded into 006F */
/* 006E 01EA78BD 03E033E4 SQ.xyzw     */   viewNormal0.xyzw = vf06.xyzw;       // MADDAy.xyzw folded into 006F
/* 006F 01EA85CA 03E03BE5 MADDz.xyzw  */   vf23.xyzw = mul3x3(lightDir0_vf14, vf10.xyz); // folds 006D..006F
/*                        SQ.xyzw     */   viewNormal1.xyzw = vf07.xyzw;
/* 0070 01EB71BC 03E043E6 SQ.xyzw     */   viewNormal2.xyzw = vf08.xyzw;       // MULAx.xyzw folded into 0072
/* 0071 01EB78BD 8000033C             */   /* MADDAy.xyzw folded into 0072 */
/* 0072 01EB860A 8000033C MADDz.xyzw  */   vf24.xyzw = mul3x3(lightDir0_vf14, vf11.xyz); // folds 0070..0072
/* 0073 01EC71BC 03E0BBC8 SQ.xyzw     */   lightNormal0.xyzw = vf23.xyzw;      // MULAx.xyzw folded into 0075
/* 0074 01EC78BD 8000033C             */   /* MADDAy.xyzw folded into 0075 */
/* 0075 01EC864A 8000033C MADDz.xyzw  */   vf25.xyzw = mul3x3(lightDir0_vf14, vf12.xyz); // folds 0073..0075
/* 0076 01CD52AA 03E0C3C9 MUL.xyz     */   vf10.xyz = vf10.xyz * vf13.xyz;
/*                        SQ.xyzw     */   lightNormal1.xyzw = vf24.xyzw;
/* 0077 01CD5AEA 8000033C MUL.xyz     */   vf11.xyz = vf11.xyz * vf13.xyz;
/* 0078 01CD632A 8000033C MUL.xyz     */   vf12.xyz = vf12.xyz * vf13.xyz;
/* 0079 010A5081 03E0CBCA ADDy.x      */   vf02.x = vf10.x + vf10.y;
/*                        SQ.xyzw     */   lightNormal2.xyzw = vf25.xyzw;
/* 007A 008B5880 8000033C ADDx.y      */   vf02.y = vf11.y + vf11.x;
/* 007B 004C6080 8000033C ADDx.z      */   vf02.z = vf12.z + vf12.x;
/* 007C 010A1082 8000033C ADDz.x      */   vf02.x = vf02.x + vf10.z;
/* 007D 008B1082 8000033C ADDz.y      */   vf02.y = vf02.y + vf11.z;
/* 007E 004C1081 8000033C ADDy.z      */   vf02.z = vf02.z + vf12.y;
/* 007F 400002FF 03E013A5 SQ.xyzw     */   mem[933].xyzw = vf02.xyzw;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 7 more can reach here */
/* 0080 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 007F */
subroutine(copyBlock, vi02);                                           /* 0081, from 002C */
L_0081:  /* from 002C */
/* 0081 000002FF 09031000 ILW.x       */   vi03 = objectMatrix[0].x;
/* 0082 000002FF 80031970 IADD        */   vi05 = vi03 + vi03;
/* 0083 000002FF 80052970 IADD        */   vi05 = vi05 + vi05;
/* 0084 000002FF 80052970 IADD        */   vi05 = vi05 + vi05;
/* 0085 000002FF 80032971 ISUB        */   vi05 = vi05 - vi03;
/* 0086 000002FF 10041001 IADDIU      */   vi04 = vi02 + 1;
/* 0088 000002FF 100303A8 IADDIU      */   vi03 = 936;                         // delay slot of 0087
/* 0087 000002FF 40000001 B           */   goto L_0089;                        // call sub_0089 through vi03, returns to 03A8
subroutine(sub_0089, vi05);                                            /* 0089, from 0087 */
L_0089:  /* from 0087, 008B */
/* 0089 000002FF 12052801 ISUBIU      */   vi05 = vi05 - 1;
/* 008A 000002FF 81E2237C LQI.xyzw    */   vf02.xyzw = mem[vi04++].xyzw;
/* 008C 000002FF 81E3137D SQI.xyzw    */   mem[vi03++].xyzw = vf02.xyzw;       // delay slot of 008B
/* 008B 000002FF 52002FFD IBNE        */   if (vi05 != 0) goto L_0089;
/* 008E 000002FF 8000033C             */   /* nop */                           // delay slot of 008D
/* 008D 000002FF 48007800 JR          */   goto_micro(vi15);
subroutine(lightSetup, vi02);                                          /* 008F, from 002A */
L_008F:  /* from 002A */
/* 008F 000002FF 01EF03EF LQ.xyzw     */   drawTint_vf15.xyzw = drawTint.xyzw;
/* 0090 000002FF 81E3137C LQI.xyzw    */   vf03.xyzw = mem[vi02++].xyzw;
/* 0091 000002FF 81E4137C LQI.xyzw    */   vf04.xyzw = mem[vi02++].xyzw;
/* 0092 000002FF 81E5137C LQI.xyzw    */   vf05.xyzw = mem[vi02++].xyzw;
/* 0093 000002FF 81E6137C LQI.xyzw    */   vf06.xyzw = mem[vi02++].xyzw;
/* 0094 01E3193C 81E7137C ITOF0.xyzw  */   vf03.xyzw = itof0(vf03.xyzw);
/*                        LQI.xyzw    */   vf07.xyzw = mem[vi02++].xyzw;
/* 0095 81E4213C 3C000000 ITOF0.xyzw  */   vf04.xyzw = itof0(vf04.xyzw);
/*                        LOI         */   I = 0.0078125f;
/* 0096 01E5293C 81E8137C ITOF0.xyzw  */   vf05.xyzw = itof0(vf05.xyzw);
/*                        LQI.xyzw    */   vf08.xyzw = mem[vi02++].xyzw;
/* 0097 01E6313C 81E9137C ITOF0.xyzw  */   vf06.xyzw = itof0(vf06.xyzw);
/*                        LQI.xyzw    */   vf09.xyzw = mem[vi02++].xyzw;
/* 0098 01E7393C 81EA137C ITOF0.xyzw  */   vf07.xyzw = itof0(vf07.xyzw);
/*                        LQI.xyzw    */   vf10.xyzw = mem[vi02++].xyzw;
/* 0099 01E018DE 81EC137C MULi.xyzw   */   vf03.xyzw = vf03.xyzw * 0.0078125f;
/*                        LQI.xyzw    */   vf12.xyzw = mem[vi02++].xyzw;
/* 009A 01E0211E 81ED137C MULi.xyzw   */   vf04.xyzw = vf04.xyzw * 0.0078125f;
/*                        LQI.xyzw    */   vf13.xyzw = mem[vi02++].xyzw;
/* 009B 01E0295E 03004B87 MULi.xyzw   */   vf05.xyzw = vf05.xyzw * 0.0078125f;
/*                        SQ.x        */   mem[903].x = vf09.x;
/* 009C 01E0319E 02205392 MULi.xyzw   */   vf06.xyzw = vf06.xyzw * 0.0078125f;
/*                        SQ.w        */   mem[914].w = vf10.w;
/* 009D 01E039DE 03C01BEC MULi.xyzw   */   vf07.xyzw = vf07.xyzw * 0.0078125f;
/*                        SQ.xyz      */   matAmbient.xyz = vf03.xyz;
/* 009E 01E47BEA 03E023EA MUL.xyzw    */   drawTint_vf15.xyzw = drawTint_vf15.xyzw * vf04.xyzw;
/*                        SQ.xyzw     */   matDiffuse.xyzw = vf04.xyzw;
/* 009F 000002FF 03E02BEE SQ.xyzw     */   drawTint2.xyzw = vf05.xyzw;
/* 00A0 000002FF 03C033CB SQ.xyz      */   tintScale.xyz = vf06.xyz;
/* 00A1 000002FF 022033CC SQ.w        */   mem[972].w = vf06.w;
/* 00A2 000002FF 03E03BCD SQ.xyzw     */   reflectColor.xyzw = vf07.xyzw;
/* 00A3 000002FF 0220438E SQ.w        */   flatten = vf08.w;
/* 00A4 000002FF 022063D5 SQ.w        */   rampBias = vf12.w;
/* 00A5 000002FF 02206BD4 SQ.w        */   rampGain = vf13.w;
/* 00A6 000002FF 03E07BED SQ.xyzw     */   drawColor.xyzw = drawTint_vf15.xyzw;
/* 00A7 000002FF 10010001 IADDIU      */   vi01 = 1;
/* 00A8 000002FF 0A210395 ISW.w       */   materialChanged = 1;
/* 00A9 000002FF 81E3137C LQI.xyzw    */   vf03.xyzw = mem[vi02++].xyzw;
/* 00AA 000002FF 81E4137C LQI.xyzw    */   vf04.xyzw = mem[vi02++].xyzw;
/* 00AB 000002FF 81E5137C LQI.xyzw    */   vf05.xyzw = mem[vi02++].xyzw;
/* 00AC 000002FF 81E6137C LQI.xyzw    */   vf06.xyzw = mem[vi02++].xyzw;
/* 00AD 000002FF 81E7137C LQI.xyzw    */   vf07.xyzw = mem[vi02++].xyzw;
/* 00AE 000002FF 81E8137C LQI.xyzw    */   vf08.xyzw = mem[vi02++].xyzw;
/* 00AF 000002FF 03801B80 SQ.xy       */   mem[896].xy = vf03.xy;
/* 00B0 000002FF 03802381 SQ.xy       */   mem[897].xy = vf04.xy;
/* 00B1 000002FF 03802B82 SQ.xy       */   mem[898].xy = vf05.xy;
/* 00B2 000002FF 03803383 SQ.xy       */   mem[899].xy = vf06.xy;
/* 00B3 000002FF 03803B84 SQ.xy       */   mem[900].xy = vf07.xy;
/* 00B4 000002FF 03804385 SQ.xy       */   mem[901].xy = vf08.xy;
/* 00B5 000002FF 81E3137C LQI.xyzw    */   vf03.xyzw = mem[vi02++].xyzw;
/* 00B6 000002FF 81E4137C LQI.xyzw    */   vf04.xyzw = mem[vi02++].xyzw;
/* 00B7 000002FF 81E5137C LQI.xyzw    */   vf05.xyzw = mem[vi02++].xyzw;
/* 00B8 000002FF 81E6137C LQI.xyzw    */   vf06.xyzw = mem[vi02++].xyzw;
/* 00B9 000002FF 81E7137C LQI.xyzw    */   vf07.xyzw = mem[vi02++].xyzw;
/* 00BA 000002FF 81E8137C LQI.xyzw    */   vf08.xyzw = mem[vi02++].xyzw;
/* 00BB 000002FF 03801BCE SQ.xy       */   uvMatrix0.xy = vf03.xy;
/* 00BC 000002FF 038023CF SQ.xy       */   uvMatrix1.xy = vf04.xy;
/* 00BD 000002FF 03802BD0 SQ.xy       */   uvMatrix2.xy = vf05.xy;
/* 00BE 000002FF 038033D1 SQ.xy       */   uvMatrixB0.xy = vf06.xy;
/* 00BF 000002FF 03803BD2 SQ.xy       */   uvMatrixB1.xy = vf07.xy;
/* 00C0 400002FF 038043D3 SQ.xy       */   uvMatrixB2.xy = vf08.xy;
/* 00C1 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 00C0 */
subroutine(unpackTexture, vi02);                                       /* 00C2, from 0022 */
L_00C2:  /* from 0022 */
/* 00C2 000002FF 01E31002 LQ.xyzw     */   vf03.xyzw = matPacket[2].xyzw;      // tint
/* 00C3 000002FF 09041000 ILW.x       */   vi04 = matPacket[0].x;
/* 00C4 000002FF 09051001 ILW.x       */   vi05 = matPacket[1].x;
/* 00C5 000002FF 01EF03EA LQ.xyzw     */   matDiffuse_vf15.xyzw = matDiffuse.xyzw;
/* 00C6 01E3193C 01E41003 ITOF0.xyzw  */   vf03.xyzw = itof0(vf03.xyzw);
/*                        LQ.xyzw     */   vf04.xyzw = matPacket[3].xyzw;      // screenZOffset
/* 00C7 000002FF 01FE03A6 LQ.xyzw     */   screenScale_vf30.xyzw = screenScale.xyzw;
/* 00C8 000002FF 01E51004 LQ.xyzw     */   vf05.xyzw = matPacket[4].xyzw;      // rampGainAdd
/* 00C9 000002FF 01E61005 LQ.xyzw     */   vf06.xyzw = matPacket[5].xyzw;      // rampBiasAdd
/* 00CA 000002FF 10010001 IADDIU      */   vi01 = 1;
/* 00CB 01E37BEA 0A240393 MUL.xyzw    */   matDiffuse_vf15.xyzw = matDiffuse_vf15.xyzw * vf03.xyzw;
/*                        ISW.w       */   savedPointer = vi04;
/* 00CC 0044F780 0A250394 ADDx.z      */   screenScale_vf30.z = screenScale_vf30.z + vf04.x;
/*                        ISW.w       */   mem_w[916].w = vi05;
/* 00CD 000002FF 03E01BEF SQ.xyzw     */   drawTint.xyzw = vf03.xyzw;
/* 00CE 000002FF 0A210395 ISW.w       */   materialChanged = 1;
/* 00CF 000002FF 03002BF6 SQ.x        */   rampGainAdd = vf05.x;
/* 00D0 000002FF 030033F7 SQ.x        */   rampBiasAdd = vf06.x;
/* 00D1 400002FF 03E07BED SQ.xyzw     */   drawColor.xyzw = matDiffuse_vf15.xyzw;
/* 00D2 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 00D1 */
subroutine(unpackTwo, vi02);                                           /* 00D3, from 0028 */
L_00D3:  /* from 0028 */
/* 00D3 000002FF 01E21000 LQ.xyzw     */   vf02.xyzw = twoPacket[0].xyzw;      // first
/* 00D4 000002FF 01E31001 LQ.xyzw     */   vf03.xyzw = twoPacket[1].xyzw;      // second
/* 00D5 00220118 0A20038F MULx.w      */   vf04.w = vf02.x;
/*                        ISW.w       */   mem_w[911].w = 0;
/* 00D6 00230158 8000033C MULx.w      */   vf05.w = vf03.x;
/* 00D7 000002FF 02202390 SQ.w        */   mem[912].w = vf04.w;
/* 00D8 400002FF 02202B91 SQ.w        */   mem[913].w = vf05.w;
/* 00D9 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 00D8 */
L_00DA:  /* from 0233, 049A */
/* 00DA 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 00DB 000002FF 10010001 IADDIU      */   vi01 = 1;
/* 00DC 000002FF 10050400 IADDIU      */   vi05 = 1024;
/* 00DD 000002FF 08230387 ILW.w       */   drawFlags_vi03 = drawFlags;
/* 00DE 000002FF 80060874 IAND        */   vi01 = 1 & drawMode_vi06;
/* 00DF 000002FF 80062974 IAND        */   vi05 = 1024 & drawMode_vi06;
/* 00E1 000002FF 10240000 IADDIU      */   vi04 = 2048;                        // delay slot of 00E0
/* 00E0 000002FF 52000804 IBNE        */   if (vi01 != 0) goto L_00E5;
/* 00E3 000002FF 01F703A4 LQ.xyzw     */   vf23.xyzw = mem[932].xyzw;          // delay slot of 00E2
/* 00E2 000002FF 52000802 IBNE        */   if (vi01 != 0) goto L_00E5;
L_00E5:  /* delay slot of 00E4, reached with take clear */
/* 00E5 000002FF 80041934 IAND        */   vi04 = drawFlags_vi03 & 2048;       // delay slot of 00E4
/* 00E4 000002FF 48007800 JR          */   return;
/* 00E7 000002FF 01F703A4 LQ.xyzw     */   vf23.xyzw = mem[932].xyzw;          // delay slot of 00E6
/* 00E6 000002FF 5200281C IBNE        */   if (vi05 != 0) goto L_0103;
/* 00E9 000002FF 8000033C             */   /* nop */                           // delay slot of 00E8
/* 00E8 000002FF 52002002 IBNE        */   if (vi04 == 0) {
/* 00EA 000002FF 01F703A5 LQ.xyzw     */     vf23.xyzw = mem[933].xyzw;
/*                                    */   }
/* 00EB 01170601 8000033C ADDy.x      */   vf24.x = vf23.y;
/* 00EC 01170642 8000033C ADDz.x      */   vf25.x = vf23.z;
/* 00ED 002002DB 08230384 MULw.w      */   vf11.w = 1.0f;
/*                        ILW.w       */   srcNormals_vi03 = srcNormals;
/* 00EE 000002FF 01E503CC LQ.xyzw     */   vf05.xyzw = mem[972].xyzw;
/* 00EF 000002FF 01CB03CB LQ.xyz      */   tintScale_vf11.xyz = tintScale.xyz;
/* 00F0 000002FF 01E403EF LQ.xyzw     */   drawTint_vf04.xyzw = drawTint.xyzw;
/* 00F1 000002FF 08220381 ILW.w       */   vertexCount_vi02 = vertexCount;
/* 00F2 000002FF 81E21B7C LQI.xyzw    */   vf02.xyzw = mem[srcNormals_vi03++].xyzw;
/* 00F3 01850143 10050350 ADDw.xy     */   vf05.xy = vf05.w;
/*                        IADDIU      */   vi05 = 848;                         // texcoord, the base
/* 00F4 01E45AEA 10040320 MUL.xyzw    */   tintScale_vf11.xyzw = tintScale_vf11.xyzw * drawTint_vf04.xyzw;
/*                        IADDIU      */   vi04 = 800;                         // vertexColor, the base
  /* loop 00F5..00FA: vi02 -= 1 per iteration (00F6) */
/*                                    */   do {
/* 00F5 0102B9BC 00442800 LQ.z        */     vf04.z = mem[vi05].z;             // MULAx.x folded into 00F7
/* 00F6 0102C0BD 12021001 ISUBIU      */     vertexCount_vi02 = vertexCount_vi02 - 1; // MADDAy.x folded into 00F7
/* 00F7 0102C94A 81E45B7D MADDz.x     */     vf05.x = mul3x3(vf23, vf02.xyz);  // folds 00F5..00F7
/*                        SQI.xyzw    */     mem[vi04++].xyzw = tintScale_vf11.xyzw;
/* 00F8 0184299A 81E21B7C MULz.xy     */     vf06.xy = vf05.xy * vf04.z;
/*                        LQI.xyzw    */     vf02.xyzw = mem[srcNormals_vi03++].xyzw;
/* 00FA 000002FF 8185337D SQI.xy      */     mem[vi05++].xy = vf06.xy;         // delay slot of 00F9
/* 00F9 000002FF 520017FB IBNE        */   } while (vi02 != 0);
/* 00FB 000002FF 80007A30 IADD        */   vi08 = vi15;
/* 00FC 000002FF 100104B7 IADDIU      */   vi01 = 1207;
/* 00FD 000002FF 10040388 IADDIU      */   vi04 = 904;
/* 00FF 000002FF 100F0100 IADDIU      */   vi15 = 256;                         // delay slot of 00FE
/* 00FE 000002FF 48000800 JR          */   goto L_04B7;                        // call away through vi15, returns to 0100
/* a jump at 0002 0005 000A 001C 008D 00E4 and 32 more can reach here */
/* 0100 000002FF 1001028A IADDIU      */   vi01 = 650;
/* 0102 000002FF 800043F0 IADD        */   vi15 = vi08;                        // delay slot of 0101
/* 0101 000002FF 48000800 JR          */   goto L_028A;                        // vi01 holds 028A here
L_0103:  /* from 00E6 */
/* 0103 00C00318 08240387 MULx.yz     */   vf12.yz = 0.0f;
/*                        ILW.w       */   drawFlags_vi04 = drawFlags;
/* 0104 01400358 08230384 MULx.xz     */   vf13.xz = 0.0f;
/*                        ILW.w       */   srcNormals_vi03 = srcNormals;
/* 0105 01800398 01CB03CB MULx.xy     */   vf14.xy = 0.0f;
/*                        LQ.xyz      */   tintScale_vf11.xyz = tintScale.xyz;
/* 0106 01000303 10210000 ADDw.x      */   vf12.x = 1.0f;
/*                        IADDIU      */   vi01 = 2048;
/* 0107 00800343 80012074 ADDw.y      */   vf13.y = 1.0f;
/*                        IAND        */   vi01 = drawFlags_vi04 & 2048;
/* 0108 00400383 01E303EF ADDw.z      */   vf14.z = 1.0f;
/*                        LQ.xyzw     */   drawTint_vf03.xyzw = drawTint.xyzw;
/* 0109 01E0026C 52000804 SUB.xyzw    */   vf09.xyzw = vf00.xyzw - vf00.xyzw;
/* 010A 002002DB 01E403CC MULw.w      */   vf11.w = 1.0f;                      // delay slot of 0109
/*                        LQ.xyzw     */   vf04.xyzw = mem[972].xyzw;
/*                        IBNE        */   if (vi01 == 0) {
/* 010B 000002FF 01EC03E7 LQ.xyzw     */     vf12.xyzw = objectNormal0.xyzw;
/* 010C 000002FF 01ED03E8 LQ.xyzw     */     vf13.xyzw = objectNormal1.xyzw;
/* 010D 000002FF 01EE03E9 LQ.xyzw     */     vf14.xyzw = objectNormal2.xyzw;
/*                                    */   }
/* 010E 000002FF 818403BD SQRT        */   Q = sqrtf(fabsf(vf04.w));
/* 010F 800002FF 3F000000 LOI         */   I = 0.5f;
/* 0110 004003D8 01F003A1 MULx.z      */   vf15.z = 0.0f;
/*                        LQ.xyzw     */   vf16.xyzw = mem[929].xyzw;
/* 0111 01800298 01F103A2 MULx.xy     */   vf10.xy = 0.0f;
/*                        LQ.xyzw     */   vf17.xyzw = mem[930].xyzw;
/* 0112 00400283 01F203A3 ADDw.z      */   vf10.z = 1.0f;
/*                        LQ.xyzw     */   vf18.xyzw = mem[931].xyzw;
/* 0113 01E35AEA 81E21B7C MUL.xyzw    */   vf11.xyzw = vf11.xyzw * drawTint_vf03.xyzw;
/*                        LQI.xyzw    */   vf02.xyzw = mem[srcNormals_vi03++].xyzw;
/* 0114 018003E2 08220381 ADDi.xy     */   vf15.xy = 0.5f;
/*                        ILW.w       */   vertexCount_vi02 = vertexCount;
/* 0115 0180841C 800003BF MULq.xy     */   vf16.xy = vf16.xy * Q;
/*                        WAITQ       */   /* WAITQ */
/* 0116 01808C5C 10050350 MULq.xy     */   vf17.xy = vf17.xy * Q;
/*                        IADDIU      */   vi05 = 848;                         // texcoord, the base
/* 0117 0180949C 10040320 MULq.xy     */   vf18.xy = vf18.xy * Q;
/*                        IADDIU      */   vi04 = 800;                         // vertexColor, the base
L_0118:  /* from 0126 */
/* 0118 01C261BC 00442800 LQ.z        */   vf04.z = mem[vi05].z;               // MULAx.xyz folded into 011A
/* 0119 01C268BD 12021001 ISUBIU      */   vertexCount_vi02 = vertexCount_vi02 - 1; // MADDAy.xyz folded into 011A
/* 011A 01C270CA 8000033C MADDz.xyz   */   vf03.xyz = mul3x3(vf12, vf02.xyz);  // folds 0118..011A
/* 011B 01C051BF 8000033C             */   /* MULAw.xyz folded into 011D */
/* 011C 01C318FE 81E21B7C LQI.xyzw    */   vf02.xyzw = mem[srcNormals_vi03++].xyzw; // MSUBAz.xyz folded into 011D
/* 011D 01C318CE 8000033C MSUBz.xyz   */   vf03.xyz = vf10.xyz - vf03.xyz * vf03.z - vf03.xyz * vf03.z; // folds 011B..011D
/* 011E 01C079BF 8000033C             */   /* MULAw.xyz folded into 0121 */
/* 011F 01C380BC 8000033C             */   /* MADDAx.xyz folded into 0121 */
/* 0120 01C388BD 8000033C             */   /* MADDAy.xyz folded into 0121 */
/* 0121 01C390CA 10010020 MADDz.xyz   */   vf03.xyz = vf15.xyz + vf16.xyz * vf03.x + vf17.xyz * vf03.y + vf18.xyz * vf03.z; // folds 011E..0121
/*                        IADDIU      */   vi01 = 32;
/* 0122 018418DA 34010800 MULz.xy     */   vf03.xy = vf03.xy * vf04.z;
/*                        FMAND       */   vi01 = 32 & (mac & 0xffff);
/* 0124 000002FF 81E45B7D SQI.xyzw    */   mem[vi04++].xyzw = vf11.xyzw;       // delay slot of 0123
/* 0123 000002FF 52000802 IBNE        */   if (vi01 == 0) {
/* 0125 000002FF 03E44FFF SQ.xyzw     */     mem[vi04 - 1].xyzw = vf09.xyzw;
/*                                    */   }
/* 0127 000002FF 81851B7D SQI.xy      */   mem[vi05++].xy = vf03.xy;           // delay slot of 0126
/* 0126 000002FF 520017F1 IBNE        */   if (vi02 != 0) goto L_0118;
/* 0128 000002FF 01C303F0 LQ.xyz      */   vf03.xyz = mem[1008].xyz;
/* 0129 000002FF 01C403F1 LQ.xyz      */   vf04.xyz = mem[1009].xyz;
/* 012A 000002FF 80007A30 IADD        */   vi08 = vi15;
/* 012B 000002FF 100104B9 IADDIU      */   vi01 = 1209;
/* 012C 000002FF 10040388 IADDIU      */   vi04 = 904;
/* 012E 000002FF 100F012F IADDIU      */   vi15 = 303;                         // delay slot of 012D
/* 012D 000002FF 48000800 JR          */   goto L_04B9;                        // call away through vi15, returns to 012F
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 012F 000002FF 1001028A IADDIU      */   vi01 = 650;
/* 0131 000002FF 800043F0 IADD        */   vi15 = vi08;                        // delay slot of 0130
/* 0130 000002FF 48000800 JR          */   goto L_028A;                        // vi01 holds 028A here
subroutine(lightVertex, vf15);                                         /* 0132, from 01C6 01D2 */
/* a jump at 02F1 05C6 can reach here */
/* 0132 000002FF 08230387 ILW.w       */   drawFlags_vi03 = drawFlags;
/* 0133 000002FF 10010080 IADDIU      */   vi01 = 128;
/* 0134 000002FF 0022038E LQ.w        */   flatten_vf02.w = flatten;
/* 0135 000002FF 08220381 ILW.w       */   vertexCount_vi02 = vertexCount;
/* 0136 000002FF 80011874 IAND        */   vi01 = drawFlags_vi03 & 128;
/* 0137 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0138 000002FF 5001004F IBEQ        */   take = (vi01 == 0);
/* 0139 002200EC 10210000 SUB.w       */   vf03.w = 1.0f - flatten_vf02.w;     // delay slot of 0138
/*                        IADDIU      */   vi01 = 2048;
/*                        IBEQ        */   if (take) goto L_0188;
/* 013A 000002FF 10240000 IADDIU      */   vi04 = 2048;
/* 013B 000002FF 80032134 IAND        */   vi04 = 2048 & drawFlags_vi03;
/* 013C 000002FF 01F603EB LQ.xyzw     */   lightBase_vf22.xyzw = lightBase.xyzw;
/* 013E 000002FF 01F3039C LQ.xyzw     */   color0_vf19.xyzw = color0.xyzw;     // delay slot of 013D
/* 013D 000002FF 52002005 IBNE        */   if (vi04 == 0) {
/* 013F 000002FF 01F703C8 LQ.xyzw     */     vf23.xyzw = lightNormal0.xyzw;
/* 0140 000002FF 01F803C9 LQ.xyzw     */     vf24.xyzw = lightNormal1.xyzw;
/* 0142 000002FF 01F903CA LQ.xyzw     */     vf25.xyzw = lightNormal2.xyzw;    // delay slot of 0141
/* 0141 000002FF 40000004 B           */   } else {
/* 0143 000002FF 01F70398 LQ.xyzw     */     vf23.xyzw = lightDir0.xyzw;
/* 0144 000002FF 01F80399 LQ.xyzw     */     vf24.xyzw = lightDir1.xyzw;
/* 0145 000002FF 01F9039A LQ.xyzw     */     vf25.xyzw = lightDir2.xyzw;
/*                                    */   }
/* 0146 01C2003F 01F4039D ADDAw.xyz   */   ACC.xyz = flatten_vf02.w;
/*                        LQ.xyzw     */   color1_vf20.xyzw = color1.xyzw;
/* 0147 000002FF 01F5039E LQ.xyzw     */   color2_vf21.xyzw = color2.xyzw;
/* 0148 01C3B58B 80013074 MADDw.xyz   */   lightBase_vf22.xyz = ACC.xyz + lightBase_vf22.xyz * vf03.w;
/*                        IAND        */   vi01 = drawMode_vi06 & 2048;
/* 0149 01C39CDB 8000033C MULw.xyz    */   color0_vf19.xyz = color0_vf19.xyz * vf03.w;
/* 014A 01C3A51B 52000828 MULw.xyz    */   color1_vf20.xyz = color1_vf20.xyz * vf03.w;
/* 014B 01C3AD5B 08240384 MULw.xyz    */   color2_vf21.xyz = color2_vf21.xyz * vf03.w; // delay slot of 014A
/*                        ILW.w       */   srcNormals_vi04 = srcNormals;
/*                        IBNE        */   if (vi01 != 0) goto L_0173;
/* 014C 01EF9AAA 81E3237C MUL.xyzw    */   vf10.xyzw = color0_vf19.xyzw * vf15.xyzw;
/*                        LQI.xyzw    */   vf03.xyzw = mem[srcNormals_vi04++].xyzw;
/* 014D 01EFA2EA 81E4237C MUL.xyzw    */   vf11.xyzw = color1_vf20.xyzw * vf15.xyzw;
/*                        LQI.xyzw    */   vf04.xyzw = mem[srcNormals_vi04++].xyzw;
/* 014E 01EFAB2A 10060001 MUL.xyzw    */   vf12.xyzw = color2_vf21.xyzw * vf15.xyzw;
/*                        IADDIU      */   vi06 = 1;
/* 014F 01EFB36A 800611B4 MUL.xyzw    */   vf13.xyzw = lightBase_vf22.xyzw * vf15.xyzw;
/*                        IAND        */   vi06 = vertexCount_vi02 & 1;
/* 0150 01E3B9BC 800610B1 MULAx.xyzw  */   ACC.xyzw = vf23.xyzw * vf03.x;
/*                        ISUB        */   vi02 = vi02 - vi06;
/* 0151 01E3C0BD 10050320 MADDAy.xyzw */   ACC.xyzw = ACC.xyzw + vf24.xyzw * vf03.y;
/*                        IADDIU      */   vi05 = 800;                         // vertexColor, the base
/* 0152 81E3C8CA 437F0000 MADDz.xyzw  */   vf03.xyzw = ACC.xyzw + vf25.xyzw * vf03.z;
/*                        LOI         */   I = 255.0f;
/* 0153 01E4B9BC 8000033C             */   /* MULAx.xyzw folded into 0155 */
/* 0154 01E4C0BD 8000033C             */   /* MADDAy.xyzw folded into 0155 */
/* 0155 01E4C90A 8000033C MADDz.xyzw  */   vf04.xyzw = mul3x3(vf23, vf04.xyz); // folds 0153..0155
/* 0156 01E01950 8000033C MAXx.xyzw   */   vf05.xyzw = max(vf03.xyzw, 0.0f);
  /* loop 0157..0168: vi02 -= 2 per iteration (0165) */
/*                                    */   do {
/* 0157 01E02110 8000033C MAXx.xyzw   */     vf04.xyzw = max(vf04.xyzw, 0.0f);
/* 0158 01E069BF 8000033C             */     /* MULAw.xyzw folded into 015B */
/* 0159 01E550BC 8000033C             */     /* MADDAx.xyzw folded into 015B */
/* 015A 01E558BD 8000033C             */     /* MADDAy.xyzw folded into 015B */
/* 015B 01E5614A 8000033C MADDz.xyzw  */     vf05.xyzw = vf13.xyzw + vf10.xyzw * vf05.x + vf11.xyzw * vf05.y + vf12.xyzw * vf05.z; // folds 0158..015B
/* 015C 01E451BC 8000033C MULAx.xyzw  */     ACC.xyzw = vf10.xyzw * vf04.x;
/* 015D 01E458BD 81E3237C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf11.xyzw * vf04.y;
/*                        LQI.xyzw    */     vf03.xyzw = mem[srcNormals_vi04++].xyzw;
/* 015E 01E460BE 8000033C MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf12.xyzw * vf04.z;
/* 015F 01E0295F 8000033C MINIi.xyzw  */     vf05.xyzw = min(vf05.xyzw, 255.0f);
/* 0160 01E0698B 8000033C MADDw.xyzw  */     vf06.xyzw = ACC.xyzw + vf13.xyzw;
/* 0161 01E3B9BC 81E4237C LQI.xyzw    */     vf04.xyzw = mem[srcNormals_vi04++].xyzw; // MULAx.xyzw folded into 0163
/* 0162 01E3C0BD 8000033C             */     /* MADDAy.xyzw folded into 0163 */
/* 0163 01E3C8CA 81E52B7D MADDz.xyzw  */     vf03.xyzw = mul3x3(vf23, vf03.xyz); // folds 0161..0163
/*                        SQI.xyzw    */     mem[vi05++].xyzw = vf05.xyzw;
/* 0164 01E0319F 8000033C MINIi.xyzw  */     vf06.xyzw = min(vf06.xyzw, 255.0f);
/* 0165 01E4B9BC 12021002 MULAx.xyzw  */     ACC.xyzw = vf23.xyzw * vf04.x;
/*                        ISUBIU      */     vi02 = vi02 - 2;
/* 0166 01E4C0BD 8000033C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf24.xyzw * vf04.y;
/* 0167 01E4C90A 520017EF MADDz.xyzw  */     vf04.xyzw = ACC.xyzw + vf25.xyzw * vf04.z;
/* 0168 01E01950 81E5337D MAXx.xyzw   */     vf05.xyzw = max(vf03.xyzw, 0.0f); // delay slot of 0167
/*                        SQI.xyzw    */     mem[vi05++].xyzw = vf06.xyzw;
/*                        IBNE        */   } while (vi02 != 0);
/* 016A 000002FF 8000033C             */   /* nop */                           // delay slot of 0169
/* 0169 000002FF 50003007 IBEQ        */   if (vi06 != 0) {
/* 016B 01E551BC 8000033C             */     /* MULAx.xyzw folded into 016E */
/* 016C 01E558BD 8000033C             */     /* MADDAy.xyzw folded into 016E */
/* 016D 01E560BE 8000033C             */     /* MADDAz.xyzw folded into 016E */
/* 016E 01E0694B 8000033C MADDw.xyzw  */     vf05.xyzw = vf10.xyzw * vf05.x + vf11.xyzw * vf05.y + vf12.xyzw * vf05.z + vf13.xyzw; // folds 016B..016E
/* 016F 01E0295F 8000033C MINIi.xyzw  */     vf05.xyzw = min(vf05.xyzw, 255.0f);
/* 0170 000002FF 81E52B7D SQI.xyzw    */     mem[vi05++].xyzw = vf05.xyzw;
/*                                    */   }
/* 0172 000002FF 8000033C             */   /* nop */                           // delay slot of 0171
/* 0171 000002FF 48007800 JR          */   goto_micro(vi15);
L_0173:  /* from 014A */
/* 0173 800002FF 3C000000 LOI         */   I = 0.0078125f;
/* 0174 01E07BDE 08260386 MULi.xyzw   */   vf15.xyzw = vf15.xyzw * 0.0078125f;
/*                        ILW.w       */   srcColors_vi06 = srcColors;
/* 0175 000002FF 10050320 IADDIU      */   vi05 = 800;                         // vertexColor, the base
  /* loop 0176..0185: vi02 -= 1 per iteration (0181) */
/*                                    */   do {
/* 0176 000002FF 81E3237C LQI.xyzw    */     vf03.xyzw = mem[vi04++].xyzw;
/* 0177 000002FF 81E4337C LQI.xyzw    */     vf04.xyzw = mem[vi06++].xyzw;
/* 0178 01E3B9BC 8000033C             */     /* MULAx.xyzw folded into 017A */
/* 0179 01E3C0BD 8000033C             */     /* MADDAy.xyzw folded into 017A */
/* 017A 01E3C8CA 8000033C MADDz.xyzw  */     vf03.xyzw = mul3x3(vf23, vf03.xyz); // folds 0178..017A
/* 017B 01E4213C 8000033C ITOF0.xyzw  */     vf04.xyzw = itof0(vf04.xyzw);
/* 017C 01E018D0 8000033C MAXx.xyzw   */     vf03.xyzw = max(vf03.xyzw, 0.0f);
/* 017D 01EF216A 8000033C MUL.xyzw    */     vf05.xyzw = vf04.xyzw * vf15.xyzw;
/* 017E 01E399BC 8000033C MULAx.xyzw  */     ACC.xyzw = vf19.xyzw * vf03.x;
/* 017F 01E3A0BD 8000033C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf20.xyzw * vf03.y;
/* a jump at 0193 can reach here */
/* 0180 81E3A8BE 437F0000 MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf21.xyzw * vf03.z;
/*                        LOI         */     I = 255.0f;
/* 0181 01E0B0CB 12021001 MADDw.xyzw  */     vf03.xyzw = ACC.xyzw + vf22.xyzw;
/*                        ISUBIU      */     vi02 = vi02 - 1;
/* 0182 01E518EA 8000033C MUL.xyzw    */     vf03.xyzw = vf03.xyzw * vf05.xyzw;
/* 0183 01E018DF 8000033C MINIi.xyzw  */     vf03.xyzw = min(vf03.xyzw, 255.0f);
/* 0185 000002FF 81E51B7D SQI.xyzw    */     mem[vi05++].xyzw = vf03.xyzw;     // delay slot of 0184
/* 0184 000002FF 520017F1 IBNE        */   } while (vi02 != 0);
/* 0187 000002FF 8000033C             */   /* nop */                           // delay slot of 0186
/* 0186 000002FF 48007800 JR          */   goto_micro(vi15);
L_0188:  /* from 0138 */
/* 0188 01C2003F 01F603A0 ADDAw.xyz   */   ACC.xyz = flatten_vf02.w;
/*                        LQ.xyzw     */   color4_vf22.xyzw = color4.xyzw;
/* 0189 002201BF 80013074 MULAw.w     */   ACC.w = flatten_vf02.w;
/*                        IAND        */   vi01 = drawMode_vi06 & 2048;
/* 018A 800002FF 437F0000 LOI         */   I = 255.0f;
/* 018C 01E3B0CB 10050320 MADDw.xyzw  */   vf03.xyzw = ACC.xyzw + color4_vf22.xyzw * vf03.w; // delay slot of 018B
/*                        IADDIU      */   vi05 = 800;                         // vertexColor, the base
/* 018B 000002FF 52000811 IBNE        */   if (vi01 != 0) goto L_019D;
/* 018D 000002FF 10010003 IADDIU      */   vi01 = 3;
/* 018E 000002FF 800110F4 IAND        */   vi03 = vertexCount_vi02 & 3;
/* 018F 000002FF 1004019A IADDIU      */   vi04 = 410;
/* 0190 01EF18EA 50001804 MUL.xyzw    */   vf03.xyzw = vf03.xyzw * vf15.xyzw;
/* 0191 000002FF 80032131 ISUB        */   vi04 = 410 - vi03;                  // delay slot of 0190
/*                        IBEQ        */   if (vi03 != 0) {
/* 0192 000002FF 800310B1 ISUB        */     vi02 = vi02 - vi03;
/* 0194 01E018DF 8000033C MINIi.xyzw  */     vf03.xyzw = min(vf03.xyzw, 255.0f); // delay slot of 0193
/*                                    */   }
/* 0193 000002FF 48002000 JR          */   goto_micro(vi04);
L_0195:  /* from 0199 */
/* 0195 000002FF 12021004 ISUBIU      */   vi02 = vi02 - 4;
/* 0196 000002FF 81E51B7D SQI.xyzw    */   mem[vi05++].xyzw = vf03.xyzw;
/* 0197 000002FF 81E51B7D SQI.xyzw    */   mem[vi05++].xyzw = vf03.xyzw;
/* 0198 000002FF 81E51B7D SQI.xyzw    */   mem[vi05++].xyzw = vf03.xyzw;
/* 019A 000002FF 81E51B7D SQI.xyzw    */   mem[vi05++].xyzw = vf03.xyzw;       // delay slot of 0199
/* 0199 000002FF 520017FB IBNE        */   if (vi02 != 0) goto L_0195;
/* 019C 000002FF 8000033C             */   /* nop */                           // delay slot of 019B
/* 019B 000002FF 48007800 JR          */   goto_micro(vi15);
L_019D:  /* from 018B */
/* 019D 000002FF 08260386 ILW.w       */   srcColors_vi06 = srcColors;
/* 019E 800002FF 3C000000 LOI         */   I = 0.0078125f;
/* 019F 01E07BDE 10050320 MULi.xyzw   */   vf15.xyzw = vf15.xyzw * 0.0078125f;
/*                        IADDIU      */   vi05 = 800;                         // vertexColor, the base
/* 01A0 000002FF 81E4337C LQI.xyzw    */   vf04.xyzw = mem[srcColors_vi06++].xyzw;
/* 01A1 01EF18EA 81E5337C MUL.xyzw    */   vf03.xyzw = vf03.xyzw * vf15.xyzw;
/*                        LQI.xyzw    */   vf05.xyzw = mem[srcColors_vi06++].xyzw;
/* 01A2 01E6213C 8000033C ITOF0.xyzw  */   vf06.xyzw = itof0(vf04.xyzw);
/* 01A3 01E7293C 81E4337C ITOF0.xyzw  */   vf07.xyzw = itof0(vf05.xyzw);
/*                        LQI.xyzw    */   vf04.xyzw = mem[srcColors_vi06++].xyzw;
/* 01A4 01E3322A 81E5337C MUL.xyzw    */   vf08.xyzw = vf06.xyzw * vf03.xyzw;
/*                        LQI.xyzw    */   vf05.xyzw = mem[srcColors_vi06++].xyzw;
/* 01A5 01E6213C 8000033C ITOF0.xyzw  */   vf06.xyzw = itof0(vf04.xyzw);
  /* loop 01A6..01AB: vi02 -= 2 per iteration (01A8) */
/*                                    */   do {
/* 01A6 01E33A6A 81E4337C MUL.xyzw    */     vf09.xyzw = vf07.xyzw * vf03.xyzw;
/*                        LQI.xyzw    */     vf04.xyzw = mem[srcColors_vi06++].xyzw;
/* 01A7 01E7293C 81E5437D ITOF0.xyzw  */     vf07.xyzw = itof0(vf05.xyzw);
/*                        SQI.xyzw    */     mem[vi05++].xyzw = vf08.xyzw;
/* 01A8 01E3322A 12021002 MUL.xyzw    */     vf08.xyzw = vf06.xyzw * vf03.xyzw;
/*                        ISUBIU      */     vertexCount_vi02 = vertexCount_vi02 - 2;
/* 01A9 000002FF 81E5337C LQI.xyzw    */     vf05.xyzw = mem[srcColors_vi06++].xyzw;
/* 01AA 01E6213C 5A0017FB ITOF0.xyzw  */     vf06.xyzw = itof0(vf04.xyzw);
/* 01AB 000002FF 81E54B7D SQI.xyzw    */     mem[vi05++].xyzw = vf09.xyzw;     // delay slot of 01AA
/*                        IBGTZ       */   } while (vi02 > 0);
/* 01AD 000002FF 8000033C             */   /* nop */                           // delay slot of 01AC
/* 01AC 000002FF 50001002 IBEQ        */   if (vi02 != 0) {
/* 01AE 000002FF 81E5337D SQI.xyzw    */     mem[vi05++].xyzw = vf06.xyzw;
/*                                    */   }
/* 01B0 000002FF 8000033C             */   /* nop */                           // delay slot of 01AF
/* 01AF 000002FF 48007800 JR          */   goto_micro(vi15);
/* a jump at 02F1 05C6 can reach here */
/* 01B1 000002FF 08220381 ILW.w       */   vertexCount_vi02 = vertexCount;
/* 01B2 000002FF 10050320 IADDIU      */   vi05 = 800;                         // vertexColor, the base
  /* loop 01B3..01B6: vi02 -= 1 per iteration (01B3) */
/*                                    */   do {
/* 01B3 000002FF 12021001 ISUBIU      */     vertexCount_vi02 = vertexCount_vi02 - 1;
/* 01B4 000002FF 8000033C             */     /* nop */
/* 01B6 000002FF 81E57B7D SQI.xyzw    */     mem[vi05++].xyzw = vf15.xyzw;     // delay slot of 01B5
/* 01B5 000002FF 520017FD IBNE        */   } while (vi02 != 0);
/* 01B8 000002FF 8000033C             */   /* nop */                           // delay slot of 01B7
/* 01B7 000002FF 48007800 JR          */   goto_micro(vi15);
subroutine(drawEntry, clip, mac, vf01, vf15, vf26, vf27, vf28, vf29, vf30, vf31, vi02, vi14); /* 01B9, from 000C */
/* 01BA 000002FF 08231000 ILW.w       */   vi03 = mem_w[vi02].w;               // delay slot of 01B9
/* 01B9 000002FF 420F01A3 BAL         */   unpackPointers();
/* 01BB 000002FF 08210395 ILW.w       */   materialChanged_vi01 = materialChanged;
/* 01BC 000002FF 01E403EA LQ.xyzw     */   matDiffuse_vf04.xyzw = matDiffuse.xyzw;
/* 01BD 000002FF 01F1039B LQ.xyzw     */   colorBase_vf17.xyzw = colorBase.xyzw;
/* 01BE 000002FF 01E303EC LQ.xyzw     */   matAmbient_vf03.xyzw = matAmbient.xyzw;
/* 01C0 0020015B 01F0039F MULw.w      */   vf05.w = 1.0f;                      // delay slot of 01BF
/*                        LQ.xyzw     */   color3_vf16.xyzw = color3.xyzw;
/* 01BF 000002FF 50000806 IBEQ        */   if (vi01 != 0) {
/* 01C1 01D122BE 0A200395 MULA.xyz    */     ACC.xyz = matDiffuse_vf04.xyz * colorBase_vf17.xyz;
/*                        ISW.w       */     materialChanged = 0;
/* 01C2 01D0196A 8000033C MUL.xyz     */     vf05.xyz = matAmbient_vf03.xyz * color3_vf16.xyz;
/* 01C3 01D018AD 8000033C MSUB.xyz    */     vf02.xyz = ACC.xyz - matAmbient_vf03.xyz * color3_vf16.xyz;
/* 01C4 000002FF 03E02BEB SQ.xyzw     */     lightBase.xyzw = vf05.xyzw;
/* 01C5 000002FF 03C0139C SQ.xyz      */     color0.xyz = vf02.xyz;
/*                                    */   }
/* 01C7 000002FF 01EF03ED LQ.xyzw     */   drawColor_vf15.xyzw = drawColor.xyzw; // delay slot of 01C6
/* 01C6 000002FF 420F076B BAL         */   lightVertex();
/* 01C8 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 01C9 000002FF 11010000 IADDIU      */   vi01 = 16384;
/* 01CA 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 16384;
/* 01CB 000002FF 8000033C             */   /* nop */
/* 01CD 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 01CC
/* 01CC 000002FF 50000843 IBEQ        */   if (vi01 == 0) goto L_0210;
/* 01CF 000002FF 8000033C             */   /* nop */                           // delay slot of 01CE
/* 01CE 000002FF 420F0041 BAL         */   drawFront();
/* 01D1 000002FF 8000033C             */   /* nop */                           // delay slot of 01D0
/* 01D0 000002FF 420F000E BAL         */   negateNormals();
/* 01D3 000002FF 01EF03ED LQ.xyzw     */   drawColor_vf15.xyzw = drawColor.xyzw; // delay slot of 01D2
/* 01D2 000002FF 420F075F BAL         */   lightVertex();
/* 01D4 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 01D5 000002FF 10010010 IADDIU      */   vi01 = 16;
/* 01D6 000002FF 100F04A6 IADDIU      */   vi15 = 1190;
/* 01D7 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 16;
/* 01D8 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
/* 01DA 000002FF 0B0F7000 ISW.x       */   mem_w[vi14].x = 1190;               // delay slot of 01D9
/* 01D9 000002FF 50000840 IBEQ        */   if (vi01 == 0) goto L_021A;
/* 01DC 000002FF 8000033C             */   /* nop */                           // delay slot of 01DB
/* 01DB 000002FF 420F0022 BAL         */   texcoordMatrix();
/* 01DE 000002FF 8000033C             */   /* nop */                           // delay slot of 01DD
/* 01DD 000002FF 40000046 B           */   goto L_0224;
subroutine(negateNormals, nothing);                                    /* 01DF, from 01D0 042B */
/* 01DF 000002FF 08230387 ILW.w       */   drawFlags_vi03 = drawFlags;
/* 01E0 000002FF 10010080 IADDIU      */   vi01 = 128;
/* 01E1 000002FF 08280384 ILW.w       */   srcNormals_vi08 = srcNormals;
/* 01E2 000002FF 80030874 IAND        */   vi01 = 128 & drawFlags_vi03;
/* 01E3 000002FF 08250381 ILW.w       */   vertexCount_vi05 = vertexCount;
/* 01E5 000002FF 10030001 IADDIU      */   vi03 = 1;                           // delay slot of 01E4
/* 01E4 000002FF 5000080E IBEQ        */   if (vi01 == 0) goto L_01F3;
/* 01E6 000002FF 81C2437C LQI.xyz     */   vf02.xyz = mem[srcNormals_vi08++].xyz;
/* 01E7 000002FF 81C3437C LQI.xyz     */   vf03.xyz = mem[srcNormals_vi08++].xyz;
/* 01E8 000002FF 800518F4 IAND        */   vi03 = 1 & vertexCount_vi05;
/* 01E9 01C2012C 80032971 SUB.xyz     */   vf04.xyz = -vf02.xyz;
/*                        ISUB        */   vi05 = vi05 - vi03;
  /* loop 01EA..01EF: vi05 -= 2 per iteration (01EC) */
/*                                    */   do {
/* 01EA 01C3016C 81C2437C SUB.xyz     */     vf05.xyz = -vf03.xyz;
/*                        LQI.xyz     */     vf02.xyz = mem[srcNormals_vi08++].xyz;
/* 01EB 000002FF 81C3437C LQI.xyz     */     vf03.xyz = mem[srcNormals_vi08++].xyz;
/* 01EC 000002FF 12052802 ISUBIU      */     vi05 = vi05 - 2;
/* 01ED 000002FF 03C827FC SQ.xyz      */     mem[srcNormals_vi08 - 4].xyz = vf04.xyz;
/* 01EE 01C2012C 52002FFB SUB.xyz     */     vf04.xyz = -vf02.xyz;
/* 01EF 000002FF 03C82FFD SQ.xyz      */     mem[srcNormals_vi08 - 3].xyz = vf05.xyz; // delay slot of 01EE
/*                        IBNE        */   } while (vi05 != 0);
/* 01F1 000002FF 8000033C             */   /* nop */                           // delay slot of 01F0
/* 01F0 000002FF 50001802 IBEQ        */   if (vi03 != 0) {
/* 01F2 000002FF 03C827FE SQ.xyz      */     mem[srcNormals_vi08 - 2].xyz = vf04.xyz;
/*                                    */   }
L_01F3:  /* from 01E4 */
/* 01F3 000002FF 08280382 ILW.w       */   indexList_vi08 = indexList;
/* 01F4 000002FF 08250380 ILW.w       */   faceCount_vi05 = faceCount;
  /* loop 01F5..01FB: vi05 -= 1 per iteration (01F7) */
/*                                    */   do {
/* 01F5 000002FF 08824000 ILW.y       */     vi02 = mem_w[indexList_vi08].y;
/* 01F6 000002FF 08434000 ILW.z       */     vi03 = mem_w[indexList_vi08].z;
/* 01F7 000002FF 12052801 ISUBIU      */     faceCount_vi05 = faceCount_vi05 - 1;
/* 01F8 000002FF 10084001 IADDIU      */     indexList_vi08 = indexList_vi08 + 1;
/* 01F9 000002FF 0A4247FF ISW.z       */     mem_w[indexList_vi08 - 1].z = vi02;
/* 01FB 000002FF 0A8347FF ISW.y       */     mem_w[indexList_vi08 - 1].y = vi03; // delay slot of 01FA
/* 01FA 000002FF 52002FFA IBNE        */   } while (vi05 != 0);
/* 01FD 000002FF 8000033C             */   /* nop */                           // delay slot of 01FC
/* 01FC 000002FF 48007800 JR          */   return;
subroutine(texcoordMatrix, nothing);                                   /* 01FE, from 01DB 0436 */
/* 01FE 000002FF 08270385 ILW.w       */   srcTexcoords_vi07 = srcTexcoords;
/* 01FF 000002FF 019003CE LQ.xy       */   vf16.xy = uvMatrix0.xy;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
/* 0200 000002FF 019103CF LQ.xy       */   uvMatrix1_vf17.xy = uvMatrix1.xy;
/* 0201 000002FF 019203D0 LQ.xy       */   uvMatrix2_vf18.xy = uvMatrix2.xy;
/* 0202 000002FF 81E33B7C LQI.xyzw    */   vf03.xyzw = mem[vi07++].xyzw;
/* 0203 018381BC 01E50350 LQ.xyzw     */   vf05.xyzw = texcoord[0].xyzw;       // MULAx.xy folded into 0205
/* 0204 018388BD 10080001 IADDIU      */   vi08 = 1;                           // MADDAy.xy folded into 0205
/* 0205 0180908B 01E44350 MADDw.xy    */   vf02.xy = vf16.xy * vf03.x + uvMatrix1_vf17.xy * vf03.y + uvMatrix2_vf18.xy; // folds 0203..0205
/*                        LQ.xyzw     */   vf04.xyzw = mem[1 + 848].xyzw;
/* 0206 000002FF 81E33B7C LQI.xyzw    */   vf03.xyzw = mem[vi07++].xyzw;
/* 0207 0185109A 08250381 MULz.xy     */   vf02.xy = vf02.xy * vf05.z;
/*                        ILW.w       */   vertexCount_vi05 = vertexCount;
  /* loop 0208..020D: vi08 += 1 per iteration (0208) */
/*                                    */   do {
/* 0208 018381BC 10084001 MULAx.xy    */     ACC.xy = vf16.xy * vf03.x;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 0209 018388BD 0388134E MADDAy.xy   */     ACC.xy = ACC.xy + uvMatrix1_vf17.xy * vf03.y;
/*                        SQ.xy       */     texcoord[vi08 - 2].xy = vf02.xy;
/* 020A 0180910B 81E33B7C MADDw.xy    */     vf04.xy = ACC.xy + uvMatrix2_vf18.xy;
/*                        LQI.xyzw    */     vf03.xyzw = mem[vi07++].xyzw;
/* 020B 000002FF 8042233C MOVE.z      */     vf02.z = vf04.z;
/* 020C 0184209A 52082FFB MULz.xy     */     vf02.xy = vf04.xy * vf04.z;
/* 020D 000002FF 01E44350 LQ.xyzw     */     vf04.xyzw = texcoord[vi08].xyzw;  // delay slot of 020C
/*                        IBNE        */   } while (vi05 != vi08);
/* 020F 000002FF 0388134F SQ.xy       */   texcoord[vi08 - 1].xy = vf02.xy;    // delay slot of 020E
/* 020E 000002FF 48007800 JR          */   goto_micro(vi15);
subroutine(drawFront, vf01, vf26, vf27, vf28, vf29, vi14);             /* 0210, from 01CE */
L_0210:  /* from 01CC */
/* 0210 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0211 000002FF 10010010 IADDIU      */   vi01 = 16;
/* 0212 000002FF 0B0F77FF ISW.x       */   mem_w[vi14 - 1].x = vi15;
/* a jump at 02F1 05C6 can reach here */
/* 0213 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
/* 0214 000002FF 80013074 IAND        */   vi01 = vi06 & vi01;
/* 0215 000002FF 8000033C             */   /* nop */
/* 0217 000002FF 8000033C             */   /* nop */                           // delay slot of 0216
/* 0216 000002FF 52000807 IBNE        */   if (vi01 != 0) goto L_021E;
/* 0219 000002FF 8000033C             */   /* nop */                           // delay slot of 0218
/* 0218 000002FF 420F02B8 BAL         */   transformVertices();
L_021A:  /* from 01D9 */
/* 021B 000002FF 8000033C             */   /* nop */                           // delay slot of 021A
/* 021A 000002FF 420F0101 BAL         */   packetOne();
/* 021D 000002FF 100F022F IADDIU      */   vi15 = 559;                         // delay slot of 021C
/* 021C 000002FF 40000018 B           */   goto L_0235;                        // call emitTriangles through vi15, returns to 022F
L_021E:  /* from 0216 */
/* 021E 000002FF 019003CE LQ.xy       */   vf16.xy = uvMatrix0.xy;
/* 021F 000002FF 019103CF LQ.xy       */   uvMatrix1_vf17.xy = uvMatrix1.xy;
/* a jump at 0193 can reach here */
/* 0220 000002FF 019203D0 LQ.xy       */   uvMatrix2_vf18.xy = uvMatrix2.xy;
/* 0221 000002FF 100104F6 IADDIU      */   vi01 = 1270;
/* 0223 000002FF 100F0224 IADDIU      */   vi15 = 548;                         // delay slot of 0222
/* 0222 000002FF 48000800 JR          */   goto L_04F6;                        // call away through vi15, returns to 0224
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
L_0224:  /* from 01DD */
/* 0225 000002FF 8000033C             */   /* nop */                           // delay slot of 0224
/* 0224 000002FF 420F0104 BAL         */   packetThree();
/* 0227 000002FF 8000033C             */   /* nop */                           // delay slot of 0226
/* 0226 000002FF 420F0063 BAL         */   emitTexTriangles();
/* 0228 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0229 000002FF 10410000 IADDIU      */   vi01 = 4096;
/* 022A 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 4096;
/* 022B 000002FF 10030132 IADDIU      */   vi03 = 306;
/* 022C 000002FF 100D028A IADDIU      */   vi13 = 650;
/* 022E 000002FF 100F022F IADDIU      */   vi15 = 559;                         // delay slot of 022D
/* 022D 000002FF 520008BC IBNE        */   if (vi01 != 0) goto L_02EA;
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 022F 000002FF 100105CA IADDIU      */   vi01 = 1482;
/* 0231 000002FF 100F0232 IADDIU      */   vi15 = 562;                         // delay slot of 0230
/* 0230 000002FF 48000800 JR          */   goto L_05CA;                        // call away through vi15, returns to 0232
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 0232 000002FF 090F7000 ILW.x       */   vi15 = mem_w[vi14].x;
/* 0234 000002FF 100E7001 IADDIU      */   vi14 = vi14 + 1;                    // delay slot of 0233
/* 0233 000002FF 400006A6 B           */   goto L_00DA;
subroutine(emitTriangles, mac, vf01, vf30, vf31, vi14);                /* 0235, from 021C 0487 */
L_0235:  /* from 021C, 0487 */
/* 0235 000002FF 088B03FF ILW.y       */   cursors_vi11 = cursors.y;
/* 0236 000002FF 00C203D5 LQ.yz       */   tagTriangles_vf02.yz = tagTriangles.yz;
/* 0237 000002FF 0B0F77FF ISW.x       */   mem_w[vi14 - 1].x = vi15;
/* 0238 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
/* 0239 000002FF 100A0001 IADDIU      */   vi10 = 1;
/* 023A 000002FF 082C0382 ILW.w       */   indexList_vi12 = indexList;
/* 023B 000002FF 80CB137D SQI.yz      */   mem[cursors_vi11++].yz = tagTriangles_vf02.yz;
/* 023C 000002FF 08220380 ILW.w       */   vi02 = faceCount;
/* 023D 000002FF 100703FD IADDIU      */   vi07 = 1021;
/* 023E 000002FF 09046000 ILW.x       */   vi04 = mem_w[indexList_vi12].x;
/* 023F 000002FF 08856000 ILW.y       */   vi05 = mem_w[indexList_vi12].y;
/* 0240 000002FF 08466000 ILW.z       */   vi06 = mem_w[indexList_vi12].z;
/* 0241 000002FF 01E303F3 LQ.xyzw     */   windingRef_vf03.xyzw = windingRef.xyzw;
/* 0242 000002FF 01E422C0 LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw;
/* 0243 000002FF 01E52AC0 LQ.xyzw     */   position_vf05.xyzw = position[vi05].xyzw;
/* 0244 000002FF 01E632C0 LQ.xyzw     */   position_vf06.xyzw = position[vi06].xyzw;
L_0245:  /* from 026A */
/* 0245 01C5242C 01EA2350 SUB.xyz     */   vf16.xyz = vf04.xyz - vf05.xyz;
/*                        LQ.xyzw     */   texcoord_vf10.xyzw = texcoord[vi04].xyzw;
/* 0246 01C5346C 01EB2B50 SUB.xyz     */   vf17.xyz = vf06.xyz - vf05.xyz;
/*                        LQ.xyzw     */   texcoord_vf11.xyzw = texcoord[vi05].xyzw;
/* 0247 01C51CAC 01EC3350 SUB.xyz     */   vf18.xyz = vf03.xyz - vf05.xyz;
/*                        LQ.xyzw     */   texcoord_vf12.xyzw = texcoord[vi06].xyzw;
/* 0248 01C421FF 01E72320 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, vf04.w);
/*                        LQ.xyzw     */   vertexColor_vf07.xyzw = vertexColor[vi04].xyzw;
/* 0249 01C529FF 01E82B20 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf05.xyz, vf05.w);
/*                        LQ.xyzw     */   vertexColor_vf08.xyzw = vertexColor[vi05].xyzw;
/* 024A 01D182FE 01E93320 LQ.xyzw     */   vertexColor_vf09.xyzw = vertexColor[vi06].xyzw; // OPMULA.xyz folded into 024B
/* 024B 01D08C2E 09046001 OPMSUB.xyz  */   vf16.xyz = cross(vf16, vf17);       // folds 024A..024B
/*                        ILW.x       */   vi04 = mem_w[vi12 + 1].x;
/* 024C 01C631FF 08856001 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf06.xyz, vf06.w);
/*                        ILW.y       */   vi05 = mem_w[vi12 + 1].y;
/* 024D 01CA235A 08466001 MULz.xyz    */   vf13.xyz = vf04.xyz * texcoord_vf10.z;
/*                        ILW.z       */   vi06 = mem_w[vi12 + 1].z;
/* 024E 01CB2B9A 12021001 MULz.xyz    */   vf14.xyz = vf05.xyz * texcoord_vf11.z;
/*                        ISUBIU      */   vi02 = vi02 - 1;
/* 024F 01D2842A 802D533C MUL.xyz     */   vf16.xyz = vf16.xyz * vf18.xyz;
/*                        MOVE.w      */   vf13.w = texcoord_vf10.w;
/* 0250 01CC33DA 2403FFFF MULz.xyz    */   vf15.xyz = vf06.xyz * texcoord_vf12.z;
/*                        FCAND       */   vi01 = (clip & 0x03ffff) != 0;
/* 0251 01E7397C 802E5B3C FTOI0.xyzw  */   vertexColor_vf07.xyzw = ftoi0(vertexColor_vf07.xyzw);
/*                        MOVE.w      */   vf14.w = texcoord_vf11.w;
/* 0252 01E8417C 802F633C FTOI0.xyzw  */   vertexColor_vf08.xyzw = ftoi0(vertexColor_vf08.xyzw);
/*                        MOVE.w      */   vf15.w = texcoord_vf12.w;
/* 0253 0110803D 01E422C0 LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw; // ADDAy.x folded into 0254
/* 0254 01100C0A 01E52AC0 MADDz.x     */   vf16.x = vf16.x + vf16.y + vf01.x * vf16.z; // folds 0253..0254
/*                        LQ.xyzw     */   position_vf05.xyzw = position[vi05].xyzw;
/* 0255 000002FF 01E632C0 LQ.xyzw     */   position_vf06.xyzw = position[vi06].xyzw;
/* 0256 01E9497C 03EB3800 FTOI0.xyzw  */   vertexColor_vf09.xyzw = ftoi0(vertexColor_vf09.xyzw);
/*                        SQ.xyzw     */   mem[vi11].xyzw = vertexColor_vf07.xyzw;
/* 0257 01E0F9BF 03EB4002 MULAw.xyzw  */   ACC.xyzw = vf31.xyzw;
/*                        SQ.xyzw     */   mem[vi11 + 2].xyzw = vertexColor_vf08.xyzw;
/* 0258 01DE6B69 10030080 MADD.xyz    */   vf13.xyz = ACC.xyz + vf13.xyz * vf30.xyz;
/*                        IADDIU      */   vi03 = 128;
/* 0259 01DE73A9 34031800 MADD.xyz    */   vf14.xyz = ACC.xyz + vf14.xyz * vf30.xyz;
/*                        FMAND       */   vi03 = 128 & (mac & 0xffff);
/* 025A 01DE7BE9 5200180F MADD.xyz    */   vf15.xyz = ACC.xyz + vf15.xyz * vf30.xyz;
/* 025B 000002FF 03EB4804 SQ.xyzw     */   mem[vi11 + 4].xyzw = vertexColor_vf09.xyzw; // delay slot of 025A
/*                        IBNE        */   if (vi03 != 0) goto L_026A;
/* 025C 01ED697D 50000808 FTOI4.xyzw  */   vf13.xyzw = ftoi4(vf13.xyzw);
/* 025D 01EE717D 8000033C FTOI4.xyzw  */   vf14.xyzw = ftoi4(vf14.xyzw);       // delay slot of 025C
/*                        IBEQ        */   if (vi01 != 0) {
/* 025F 000002FF 8000033C             */     /* nop */                         // delay slot of 025E
/* 025E 000002FF 420F0249 BAL         */     clipReject();
/* 0261 000002FF 8000033C             */     /* nop */                         // delay slot of 0260
/* 0260 000002FF 52000809 IBNE        */     if (vi01 != 0) goto L_026A;
/* 0262 000002FF 0A2C3800 ISW.w       */     mem_w[vi07].w = vi12;
/* 0264 000002FF 800061F0 IADD        */     vi07 = vi12;                      // delay slot of 0263
/* 0263 000002FF 40000006 B           */   } else {
/* 0265 01EF797D 100B5806 FTOI4.xyzw  */     vf15.xyzw = ftoi4(vf15.xyzw);
/*                        IADDIU      */     vi11 = vi11 + 6;
/* 0266 000002FF 100A5003 IADDIU      */     vi10 = vi10 + 3;
/* 0267 000002FF 03EB6FFB SQ.xyzw     */     mem[vi11 - 5].xyzw = vf13.xyzw;
/* 0268 000002FF 03EB77FD SQ.xyzw     */     mem[vi11 - 3].xyzw = vf14.xyzw;
/* 0269 000002FF 03EB7FFF SQ.xyzw     */     mem[vi11 - 1].xyzw = vf15.xyzw;
/*                                    */   }
L_026A:  /* from 025A, 0260 */
/* 026B 000002FF 100C6001 IADDIU      */   vi12 = vi12 + 1;                    // delay slot of 026A
/* 026A 000002FF 520017DA IBNE        */   if (vi02 != 0) goto L_0245;
/* 026C 000002FF 0A203800 ISW.w       */   mem_w[vi07].w = 0;
/* 026D 000002FF 088203FF ILW.y       */   cursors_vi02 = cursors.y;
/* 026E 000002FF 11EA57FF IADDIU      */   vi10 = vi10 + 32767;
/* 0270 000002FF 0B0A1000 ISW.x       */   mem_w[cursors_vi02].x = vi10;       // delay slot of 026F
/* 026F 000002FF 420F022C BAL         */   kick();
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 0271 000002FF 082C03FD ILW.w       */   retryHead_vi12 = retryHead;
/* 0272 000002FF 100D027A IADDIU      */   vi13 = 634;
/* 0273 000002FF 100F0271 IADDIU      */   vi15 = 625;
/* 0274 000002FF 08226000 ILW.w       */   vi02 = mem_w[retryHead_vi12].w;
/* 0276 000002FF 0A2203FD ISW.w       */   retryHead = vi02;                   // delay slot of 0275
/* 0275 000002FF 520062B9 IBNE        */   if (vi12 != 0) goto L_052F;
/* 0277 000002FF 090F7000 ILW.x       */   vi15 = mem_w[vi14].x;
/* 0279 000002FF 100E7001 IADDIU      */   vi14 = vi14 + 1;                    // delay slot of 0278
/* 0278 000002FF 48007800 JR          */   return;
/* a jump at 031A 034B 056C can reach here */
/* 027A 000002FF 088803FD ILW.y       */   clipBufA_vi08 = clipBufA;
/* 027C 000002FF 00C203D5 LQ.yz       */   tagTriangles_vf02.yz = tagTriangles.yz; // delay slot of 027B
/* 027B 000002FF 420D00C0 BAL         */   packetClipped();
  /* loop 027D..0287: vi10 -= 1 per iteration (0281) */
/*                                    */   do {
/* 027D 000002FF 81E4437C LQI.xyzw    */     vf04.xyzw = mem[clipBufA_vi08++].xyzw;
/* 027E 000002FF 81E5437C LQI.xyzw    */     vf05.xyzw = mem[clipBufA_vi08++].xyzw;
/* 027F 000002FF 81E6437C LQI.xyzw    */     vf06.xyzw = mem[clipBufA_vi08++].xyzw;
/* 0280 000002FF 81E403BC DIV         */     Q = 1.0f / vf04.w;
/* 0281 01E5297C 120A5001 FTOI0.xyzw  */     vf05.xyzw = ftoi0(vf05.xyzw);
/*                        ISUBIU      */     vi10 = vi10 - 1;
/* 0282 01C0F9BF 100B5802 MULAw.xyz   */     ACC.xyz = vf31.xyz;
/*                        IADDIU      */     vi11 = vi11 + 2;
/* 0283 000002FF 8029333C MOVE.w      */     vf09.w = vf06.w;
/* 0284 01C0221C 800003BF MULq.xyz    */     vf08.xyz = vf04.xyz * Q;
/*                        WAITQ       */     /* WAITQ */
/* 0285 01DE4269 03EB2FFE MADD.xyz    */     vf09.xyz = ACC.xyz + vf08.xyz * vf30.xyz;
/*                        SQ.xyzw     */     mem[vi11 - 2].xyzw = vf05.xyzw;
/* 0286 01E9497D 520057F6 FTOI4.xyzw  */     vf09.xyzw = ftoi4(vf09.xyzw);
/* 0287 01CA40EC 03EB4FFF SUB.xyz     */     vf03.xyz = vf08.xyz - vf10.xyz;   // delay slot of 0286
/*                        SQ.xyzw     */     mem[vi11 - 1].xyzw = vf09.xyzw;
/*                        IBNE        */   } while (vi10 != 0);
/* 0289 000002FF 8000033C             */   /* nop */                           // delay slot of 0288
/* 0288 000002FF 40000213 B           */   goto L_049C;
subroutine(emitTexTriangles, mac, vf01, vf30, vf31, vi14);             /* 028A, from 0226 048F */
/* a jump at 0002 0005 000A 001C 00FE 0101 and 8 more can reach here */
L_028A:  /* from 0101, 0130, 05EF */
/* 028A 000002FF 082C0382 ILW.w       */   indexList_vi12 = indexList;
/* 028B 000002FF 088B03FF ILW.y       */   cursors_vi11 = cursors.y;
/* 028C 000002FF 01E203D6 LQ.xyzw     */   tagTexTriangles_vf02.xyzw = tagTexTriangles.xyzw;
/* 028D 000002FF 100A0001 IADDIU      */   vi10 = 1;
/* 028E 000002FF 09046000 ILW.x       */   vi04 = mem_w[indexList_vi12].x;
/* 028F 000002FF 08856000 ILW.y       */   vi05 = mem_w[indexList_vi12].y;
/* 0290 000002FF 08466000 ILW.z       */   vi06 = mem_w[indexList_vi12].z;
/* 0291 000002FF 81EB137D SQI.xyzw    */   mem[cursors_vi11++].xyzw = tagTexTriangles_vf02.xyzw;
/* 0292 000002FF 01E422C0 LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw;
/* 0293 000002FF 01E52AC0 LQ.xyzw     */   position_vf05.xyzw = position[vi05].xyzw;
/* 0294 000002FF 01E632C0 LQ.xyzw     */   position_vf06.xyzw = position[vi06].xyzw;
/* 0295 000002FF 01E303F3 LQ.xyzw     */   windingRef_vf03.xyzw = windingRef.xyzw;
/* 0296 01C5242C 100703FD SUB.xyz     */   vf16.xyz = position_vf04.xyz - position_vf05.xyz;
/*                        IADDIU      */   vi07 = 1021;
/* 0297 01C5346C 08220380 SUB.xyz     */   vf17.xyz = position_vf06.xyz - position_vf05.xyz;
/*                        ILW.w       */   vi02 = faceCount;
/* 0298 01C51CAC 0B0F77FF SUB.xyz     */   vf18.xyz = windingRef_vf03.xyz - position_vf05.xyz;
/*                        ISW.x       */   mem_w[vi14 - 1].x = vi15;
/* 0299 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
L_029A:  /* from 02C8 */
/* 029A 01D182FE 01EA2350 LQ.xyzw     */   texcoord_vf10.xyzw = texcoord[vi04].xyzw; // OPMULA.xyz folded into 029B
/* 029B 01D08C2E 01EB2B50 OPMSUB.xyz  */   vf16.xyz = cross(vf16, vf17);       // folds 029A..029B
/*                        LQ.xyzw     */   texcoord_vf11.xyzw = texcoord[vi05].xyzw;
/* 029C 01C421FF 01EC3350 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, vf04.w);
/*                        LQ.xyzw     */   texcoord_vf12.xyzw = texcoord[vi06].xyzw;
/* 029D 01C529FF 01E72320 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf05.xyz, vf05.w);
/*                        LQ.xyzw     */   vertexColor_vf07.xyzw = vertexColor[vi04].xyzw;
/* 029E 01C631FF 01E82B20 CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf06.xyz, vf06.w);
/*                        LQ.xyzw     */   vertexColor_vf08.xyzw = vertexColor[vi05].xyzw;
/* 029F 01D2842A 01E93320 MUL.xyz     */   vf16.xyz = vf16.xyz * vf18.xyz;
/*                        LQ.xyzw     */   vertexColor_vf09.xyzw = vertexColor[vi06].xyzw;
/* 02A0 01CA235A 08856001 MULz.xyz    */   vf13.xyz = vf04.xyz * texcoord_vf10.z;
/*                        ILW.y       */   vi05 = mem_w[vi12 + 1].y;
/* 02A1 01CB2B9A 09046001 MULz.xyz    */   vf14.xyz = vf05.xyz * texcoord_vf11.z;
/*                        ILW.x       */   vi04 = mem_w[vi12 + 1].x;
/* 02A2 01CC33DA 08466001 MULz.xyz    */   vf15.xyz = vf06.xyz * texcoord_vf12.z;
/*                        ILW.z       */   vi06 = mem_w[vi12 + 1].z;
/* 02A3 0110803D 12021001 ISUBIU      */   vi02 = vi02 - 1;                    // ADDAy.x folded into 02A4
/* 02A4 01100C0A 01E52AC0 MADDz.x     */   vf16.x = vf16.x + vf16.y + vf01.x * vf16.z; // folds 02A3..02A4
/*                        LQ.xyzw     */   position_vf05.xyzw = position[vi05].xyzw;
/* 02A5 01E7397C 01E422C0 FTOI0.xyzw  */   vertexColor_vf07.xyzw = ftoi0(vertexColor_vf07.xyzw);
/*                        LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw;
/* 02A6 01E8417C 01E632C0 FTOI0.xyzw  */   vertexColor_vf08.xyzw = ftoi0(vertexColor_vf08.xyzw);
/*                        LQ.xyzw     */   position_vf06.xyzw = position[vi06].xyzw;
/* 02A7 0020535B 10030080 MULw.w      */   vf13.w = texcoord_vf10.w;
/*                        IADDIU      */   vi03 = 128;
/* 02A8 01C51CAC 34031800 SUB.xyz     */   vf18.xyz = vf03.xyz - position_vf05.xyz;
/*                        FMAND       */   vi03 = 128 & (mac & 0xffff);
/* 02A9 01C5242C 5200181E SUB.xyz     */   vf16.xyz = position_vf04.xyz - position_vf05.xyz;
/* 02AA 01C5346C 2403FFFF SUB.xyz     */   vf17.xyz = position_vf06.xyz - position_vf05.xyz; // delay slot of 02A9
/*                        FCAND       */   vi01 = (clip & 0x03ffff) != 0;
/*                        IBNE        */   if (vi03 != 0) goto L_02C8;
/* 02AB 00205B9B 50000811 MULw.w      */   vf14.w = texcoord_vf11.w;
/* 02AC 01E0F9BF 802F633C MULAw.xyzw  */   ACC.xyzw = vf31.xyzw;               // delay slot of 02AB
/*                        MOVE.w      */   vf15.w = texcoord_vf12.w;
/*                        IBEQ        */   if (vi01 != 0) {
/* 02AD 000002FF 26FDF7DF FCOR        */     vi01 = (clip | 0xfdf7df) == 0xffffff;
/* 02AE 000002FF 52010019 IBNE        */     take = (vi01 != 0);
/* 02AF 000002FF 26FEFBEF FCOR        */     vi01 = (clip | 0xfefbef) == 0xffffff; // delay slot of 02AE
/*                        IBNE        */     if (take) goto L_02C8;
/* 02B0 000002FF 52010017 IBNE        */     take = (vi01 != 0);
/* 02B1 000002FF 26FF7DF7 FCOR        */     vi01 = (clip | 0xff7df7) == 0xffffff; // delay slot of 02B0
/*                        IBNE        */     if (take) goto L_02C8;
/* 02B2 000002FF 52010015 IBNE        */     take = (vi01 != 0);
/* 02B3 000002FF 26FFBEFB FCOR        */     vi01 = (clip | 0xffbefb) == 0xffffff; // delay slot of 02B2
/*                        IBNE        */     if (take) goto L_02C8;
/* 02B4 000002FF 52010013 IBNE        */     take = (vi01 != 0);
/* 02B5 000002FF 26FFDF7D FCOR        */     vi01 = (clip | 0xffdf7d) == 0xffffff; // delay slot of 02B4
/*                        IBNE        */     if (take) goto L_02C8;
/* 02B6 000002FF 52010011 IBNE        */     take = (vi01 != 0);
/* 02B7 000002FF 26FFEFBE FCOR        */     vi01 = (clip | 0xffefbe) == 0xffffff; // delay slot of 02B6
/*                        IBNE        */     if (take) goto L_02C8;
/* 02B9 000002FF 8000033C             */     /* nop */                         // delay slot of 02B8
/* 02B8 000002FF 5201000F IBNE        */     if (vi01 != 0) goto L_02C8;
/* 02BA 000002FF 0A2C3800 ISW.w       */     mem_w[vi07].w = vi12;
/* 02BC 000002FF 800061F0 IADD        */     vi07 = vi12;                      // delay slot of 02BB
/* 02BB 000002FF 4000000C B           */   } else {
/* 02BD 01DE6B69 03EB5000 MADD.xyz    */     vf13.xyz = ACC.xyz + vf13.xyz * vf30.xyz;
/*                        SQ.xyzw     */     mem[vi11].xyzw = texcoord_vf10.xyzw;
/* 02BE 01DE73A9 03EB5803 MADD.xyz    */     vf14.xyz = ACC.xyz + vf14.xyz * vf30.xyz;
/*                        SQ.xyzw     */     mem[vi11 + 3].xyzw = texcoord_vf11.xyzw;
/* 02BF 01DE7BE9 03EB6006 MADD.xyz    */     vf15.xyz = ACC.xyz + vf15.xyz * vf30.xyz;
/*                        SQ.xyzw     */     mem[vi11 + 6].xyzw = texcoord_vf12.xyzw;
/* a jump at 0193 can reach here */
/* 02C0 01E9497C 03EB3801 FTOI0.xyzw  */     vf09.xyzw = ftoi0(vf09.xyzw);
/*                        SQ.xyzw     */     mem[vi11 + 1].xyzw = vf07.xyzw;
/* 02C1 01ED697D 03EB4004 FTOI4.xyzw  */     vf13.xyzw = ftoi4(vf13.xyzw);
/*                        SQ.xyzw     */     mem[vi11 + 4].xyzw = vf08.xyzw;
/* 02C2 01EE717D 100B5809 FTOI4.xyzw  */     vf14.xyzw = ftoi4(vf14.xyzw);
/*                        IADDIU      */     vi11 = vi11 + 9;
/* 02C3 01EF797D 100A5003 FTOI4.xyzw  */     vf15.xyzw = ftoi4(vf15.xyzw);
/*                        IADDIU      */     vi10 = vi10 + 3;
/* 02C4 000002FF 03EB4FFE SQ.xyzw     */     mem[vi11 - 2].xyzw = vf09.xyzw;
/* 02C5 000002FF 03EB6FF9 SQ.xyzw     */     mem[vi11 - 7].xyzw = vf13.xyzw;
/* 02C6 000002FF 03EB77FC SQ.xyzw     */     mem[vi11 - 4].xyzw = vf14.xyzw;
/* 02C7 000002FF 03EB7FFF SQ.xyzw     */     mem[vi11 - 1].xyzw = vf15.xyzw;
/*                                    */   }
L_02C8:  /* from 02A9, 02AE, 02B0, 02B2, 02B4, 02B6, 02B8 */
/* 02C9 000002FF 100C6001 IADDIU      */   vi12 = vi12 + 1;                    // delay slot of 02C8
/* 02C8 000002FF 520017D1 IBNE        */   if (vi02 != 0) goto L_029A;
/* 02CA 000002FF 0A203800 ISW.w       */   mem_w[vi07].w = 0;
/* 02CB 000002FF 088203FF ILW.y       */   cursors_vi02 = cursors.y;
/* 02CC 000002FF 11EA57FF IADDIU      */   vi10 = vi10 + 32767;
/* 02CE 000002FF 0B0A1000 ISW.x       */   mem_w[cursors_vi02].x = vi10;       // delay slot of 02CD
/* 02CD 000002FF 420F01CE BAL         */   kick();
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 02CF 000002FF 082C03FD ILW.w       */   retryHead_vi12 = retryHead;
/* 02D0 000002FF 100D02D8 IADDIU      */   vi13 = 728;
/* 02D1 000002FF 100F02CF IADDIU      */   vi15 = 719;
/* 02D2 000002FF 08226000 ILW.w       */   vi02 = mem_w[retryHead_vi12].w;
/* 02D4 000002FF 0A2203FD ISW.w       */   retryHead = vi02;                   // delay slot of 02D3
/* 02D3 000002FF 5200625B IBNE        */   if (vi12 != 0) goto L_052F;
/* 02D5 000002FF 090F7000 ILW.x       */   vi15 = mem_w[vi14].x;
/* 02D7 000002FF 100E7001 IADDIU      */   vi14 = vi14 + 1;                    // delay slot of 02D6
/* 02D6 000002FF 48007800 JR          */   return;
/* a jump at 031A 034B 056C can reach here */
/* 02D8 000002FF 088803FD ILW.y       */   clipBufA_vi08 = clipBufA;
/* 02DA 000002FF 00C203D6 LQ.yz       */   tagTexTriangles_vf02.yz = tagTexTriangles.yz; // delay slot of 02D9
/* 02D9 000002FF 420D0062 BAL         */   packetClipped();
  /* loop 02DB..02E7: vi10 -= 1 per iteration (02E0) */
/*                                    */   do {
/* 02DB 00200218 81E4437C MULx.w      */     vf08.w = 0.0f;
/*                        LQI.xyzw    */     vf04.xyzw = mem[clipBufA_vi08++].xyzw;
/* 02DC 000002FF 81E5437C LQI.xyzw    */     vf05.xyzw = mem[clipBufA_vi08++].xyzw;
/* 02DD 000002FF 81E6437C LQI.xyzw    */     vf06.xyzw = mem[clipBufA_vi08++].xyzw;
/* 02DE 000002FF 81E403BC DIV         */     Q = 1.0f / vf04.w;
/* 02DF 01E5297C 80460B3C FTOI0.xyzw  */     vf05.xyzw = ftoi0(vf05.xyzw);
/*                        MOVE.z      */     vf06.z = vf01.z;
/* 02E0 01C0F9BF 120A5001 MULAw.xyz   */     ACC.xyz = vf31.xyz;
/*                        ISUBIU      */     vi10 = vi10 - 1;
/* 02E1 000002FF 8029333C MOVE.w      */     vf09.w = vf06.w;
/* 02E2 01C0221C 800003BF MULq.xyz    */     vf08.xyz = vf04.xyz * Q;
/*                        WAITQ       */     /* WAITQ */
/* 02E3 01C0319C 100B5803 MULq.xyz    */     vf06.xyz = vf06.xyz * Q;
/*                        IADDIU      */     vi11 = vi11 + 3;
/* 02E4 01DE4269 03EB2FFE MADD.xyz    */     vf09.xyz = ACC.xyz + vf08.xyz * vf30.xyz;
/*                        SQ.xyzw     */     mem[vi11 - 2].xyzw = vf05.xyzw;
/* 02E5 01E9497D 03EB37FD FTOI4.xyzw  */     vf09.xyzw = ftoi4(vf09.xyzw);
/*                        SQ.xyzw     */     mem[vi11 - 3].xyzw = vf06.xyzw;
/* 02E7 000002FF 03EB4FFF SQ.xyzw     */     mem[vi11 - 1].xyzw = vf09.xyzw;   // delay slot of 02E6
/* 02E6 000002FF 520057F4 IBNE        */   } while (vi10 != 0);
/* 02E9 000002FF 8000033C             */   /* nop */                           // delay slot of 02E8
/* 02E8 000002FF 400001B3 B           */   goto L_049C;
L_02EA:  /* from 022D, 0497 */
/* 02EA 000002FF 08210392 ILW.w       */   vi01 = mem_w[914].w;
/* 02EB 000002FF 10020003 IADDIU      */   vi02 = 3;
/* 02EC 000002FF 0B0F77FF ISW.x       */   mem_w[vi14 - 1].x = vi15;
/* 02ED 000002FF 01EE03EE LQ.xyzw     */   vf14.xyzw = drawTint2.xyzw;
/* 02EF 000002FF 01EF03EF LQ.xyzw     */   drawTint_vf15.xyzw = drawTint.xyzw; // delay slot of 02EE
/* 02EE 000002FF 52011002 IBNE        */   if (vi01 == 3) {
/* a jump at 0002 0005 000A 001C 00FE 0101 and 6 more can reach here */
/* 02F0 000002FF 100301B1 IADDIU      */     vi03 = 433;
/*                                    */   }
/* 02F1 01EE7BEA 4A0F1800 MUL.xyzw    */   vf15.xyzw = vf15.xyzw * vf14.xyzw;
/*                        JALR        */   vi15 = 0x02F3;                      // link register
/* 02F2 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;                    // delay slot of 02F1
/*                        JALR        */   goto_micro(vi03);                   // call away through vi15, returns to 02F3
/* 02F3 000002FF 08260385 ILW.w       */   srcTexcoords_vi06 = srcTexcoords;
/* 02F4 000002FF 08250381 ILW.w       */   vertexCount_vi05 = vertexCount;
/* 02F5 000002FF 10080001 IADDIU      */   vi08 = 1;
/* 02F6 000002FF 0821038B ILW.w       */   primFlags_vi01 = primFlags;
/* 02F7 000002FF 088903FF ILW.y       */   cursors_vi09 = cursors.y;
/* 02F8 000002FF 81E3337C LQI.xyzw    */   vf03.xyzw = mem[srcTexcoords_vi06++].xyzw;
/* 02F9 000002FF 01E20350 LQ.xyzw     */   vf02.xyzw = texcoord[0].xyzw;
/* 02FA 000002FF 01F003D1 LQ.xyzw     */   uvMatrixB0_vf16.xyzw = uvMatrixB0.xyzw;
/* 02FB 000002FF 01F103D2 LQ.xyzw     */   uvMatrixB1_vf17.xyzw = uvMatrixB1.xyzw;
/* 02FC 000002FF 01F203D3 LQ.xyzw     */   uvMatrixB2_vf18.xyzw = uvMatrixB2.xyzw;
/* 02FD 000002FF 01C403D4 LQ.xyz      */   vf04.xyz = mem[980].xyz;
/* 02FE 018381BE 01C50383 LQ.xyz      */   vf05.xyz = mem[899].xyz;            // MULAz.xy folded into 0300
/* 02FF 018388BF 01C60384 LQ.xyz      */   vf06.xyz = mem[900].xyz;            // MADDAw.xy folded into 0300
/* 0300 0180908B 01C70385 MADDw.xy    */   vf02.xy = uvMatrixB0_vf16.xy * vf03.z + uvMatrixB1_vf17.xy * vf03.w + uvMatrixB2_vf18.xy; // folds 02FE..0300
/*                        LQ.xyz      */   vf07.xyz = mem[901].xyz;
/* 0301 000002FF 01C80386 LQ.xyz      */   vf08.xyz = mem[902].xyz;
/* 0302 000002FF 01C90387 LQ.xyz      */   vf09.xyz = mem[903].xyz;
/* 0303 000002FF 10010A03 IADDIU      */   primFlags_vi01 = primFlags_vi01 + 515;
/* 0304 0182109A 0A404806 MULz.xy     */   vf02.xy = vf02.xy * vf02.z;
/*                        ISW.z       */   mem_w[cursors_vi09 + 6].z = 0;
/* 0305 000002FF 0B014806 ISW.x       */   mem_w[cursors_vi09 + 6].x = primFlags_vi01;
/* 0306 000002FF 0A21038C ISW.w       */   lastPrim = primFlags_vi01;
/* 0307 000002FF 10010006 IADDIU      */   vi01 = 6;
/* 0308 000002FF 0B014800 ISW.x       */   mem_w[cursors_vi09].x = 6;
/* 0309 000002FF 02C92000 SQ.yz       */   mem[cursors_vi09].yz = vf04.yz;
/* 030A 000002FF 03C92801 SQ.xyz      */   mem[cursors_vi09 + 1].xyz = vf05.xyz;
/* 030B 000002FF 03C93002 SQ.xyz      */   mem[cursors_vi09 + 2].xyz = vf06.xyz;
/* 030C 000002FF 03C93803 SQ.xyz      */   mem[cursors_vi09 + 3].xyz = vf07.xyz;
/* 030D 000002FF 03C94004 SQ.xyz      */   mem[cursors_vi09 + 4].xyz = vf08.xyz;
/* 030E 000002FF 81E3337C LQI.xyzw    */   vf03.xyzw = mem[srcTexcoords_vi06++].xyzw;
/* 030F 000002FF 01E44350 LQ.xyzw     */   vf04.xyzw = mem[1 + 848].xyzw;
/* 0310 000002FF 03C94805 SQ.xyz      */   mem[cursors_vi09 + 5].xyz = vf09.xyz;
/* 0311 000002FF 10094807 IADDIU      */   cursors_vi09 = cursors_vi09 + 7;
  /* loop 0312..0316: vi08 += 1 per iteration (0312) */
/*                                    */   do {
/* 0312 018381BE 10084001 MULAz.xy    */     ACC.xy = uvMatrixB0_vf16.xy * vf03.z;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 0313 018388BF 0388134E MADDAw.xy   */     ACC.xy = ACC.xy + uvMatrixB1_vf17.xy * vf03.w;
/*                        SQ.xy       */     texcoord[vi08 - 2].xy = vf02.xy;
/* 0314 0180908B 81E3337C MADDw.xy    */     vf02.xy = ACC.xy + uvMatrixB2_vf18.xy;
/*                        LQI.xyzw    */     vf03.xyzw = mem[srcTexcoords_vi06++].xyzw;
/* 0315 0184109A 52082FFC MULz.xy     */     vf02.xy = vf02.xy * vf04.z;
/* 0316 000002FF 01E44350 LQ.xyzw     */     vf04.xyzw = texcoord[vi08].xyzw;  // delay slot of 0315
/*                        IBNE        */   } while (vi05 != vi08);
/* 0317 000002FF 0388134F SQ.xy       */   texcoord[vi08 - 1].xy = vf02.xy;
/* 0318 000002FF 090F7000 ILW.x       */   vi15 = mem_w[vi14].x;
/* 0319 000002FF 100E7001 IADDIU      */   vi14 = vi14 + 1;
/* 031B 000002FF 0A8903FF ISW.y       */   cursors.y = cursors_vi09;           // delay slot of 031A
/* 031A 000002FF 48006800 JR          */   goto_micro(vi13);
subroutine(packetOne, nothing);                                        /* 031C, from 021A 0485 */
/* 031C 000002FF 0821038B ILW.w       */   primFlags_vi01 = primFlags;
/* 031D 000002FF 088903FF ILW.y       */   cursors_vi09 = cursors.y;
/* 031E 000002FF 01C203D4 LQ.xyz      */   vf02.xyz = mem[980].xyz;
/* 031F 000002FF 10010803 IADDIU      */   primFlags_vi01 = primFlags_vi01 + 3;
/* a jump at 0193 02F1 05C6 can reach here */
/* 0320 000002FF 0A404801 ISW.z       */   mem_w[vi09 + 1].z = 0;
/* 0321 000002FF 0B014801 ISW.x       */   mem_w[vi09 + 1].x = vi01;
/* 0322 000002FF 0A21038C ISW.w       */   lastPrim = vi01;
/* 0323 000002FF 10010001 IADDIU      */   vi01 = 1;
/* 0324 000002FF 0B014800 ISW.x       */   mem_w[vi09].x = 1;
/* 0325 000002FF 02C91000 SQ.yz       */   mem[vi09].yz = vf02.yz;
/* 0326 000002FF 10094802 IADDIU      */   vi09 = vi09 + 2;
/* 0328 000002FF 0A8903FF ISW.y       */   cursors.y = vi09;                   // delay slot of 0327
/* 0327 000002FF 48007800 JR          */   goto_micro(vi15);
subroutine(packetThree, nothing);                                      /* 0329, from 0224 048D */
/* 0329 000002FF 0821038B ILW.w       */   primFlags_vi01 = primFlags;
/* 032A 000002FF 088903FF ILW.y       */   cursors_vi09 = cursors.y;
/* 032B 000002FF 01C203D4 LQ.xyz      */   vf02.xyz = mem[980].xyz;
/* 032C 000002FF 01C30380 LQ.xyz      */   vf03.xyz = mem[896].xyz;
/* 032D 000002FF 01C40381 LQ.xyz      */   vf04.xyz = mem[897].xyz;
/* 032E 000002FF 01C50382 LQ.xyz      */   vf05.xyz = mem[898].xyz;
/* 032F 000002FF 10010803 IADDIU      */   primFlags_vi01 = primFlags_vi01 + 3;
/* 0330 000002FF 0A404804 ISW.z       */   mem_w[cursors_vi09 + 4].z = 0;
/* 0331 000002FF 0B014804 ISW.x       */   mem_w[cursors_vi09 + 4].x = primFlags_vi01;
/* 0332 000002FF 0A21038C ISW.w       */   lastPrim = primFlags_vi01;
/* 0333 000002FF 10010004 IADDIU      */   vi01 = 4;
/* 0334 000002FF 0B014800 ISW.x       */   mem_w[cursors_vi09].x = 4;
/* 0335 000002FF 02C91000 SQ.yz       */   mem[cursors_vi09].yz = vf02.yz;
/* 0336 000002FF 03C91801 SQ.xyz      */   mem[cursors_vi09 + 1].xyz = vf03.xyz;
/* 0337 000002FF 03C92002 SQ.xyz      */   mem[cursors_vi09 + 2].xyz = vf04.xyz;
/* 0338 000002FF 03C92803 SQ.xyz      */   mem[cursors_vi09 + 3].xyz = vf05.xyz;
/* 0339 000002FF 10094805 IADDIU      */   cursors_vi09 = cursors_vi09 + 5;
/* 033B 000002FF 0A8903FF ISW.y       */   cursors.y = cursors_vi09;           // delay slot of 033A
/* 033A 000002FF 48007800 JR          */   return;
subroutine(packetClipped, vf02, vi10);                                 /* 033C, from 027B 02D9 */
/* 033C 000002FF 088B03FF ILW.y       */   cursors_vi11 = cursors.y;
/* 033D 000002FF 01C303D4 LQ.xyz      */   vf03.xyz = mem[980].xyz;
/* 033E 000002FF 0824038C ILW.w       */   lastPrim_vi04 = lastPrim;
/* 033F 000002FF 10010001 IADDIU      */   vi01 = 1;
/* 0340 000002FF 0B015800 ISW.x       */   mem_w[cursors_vi11].x = 1;
/* 0341 000002FF 02CB1800 SQ.yz       */   mem[cursors_vi11].yz = vf03.yz;
/* 0342 000002FF 11E107F8 IADDIU      */   vi01 = 32760;
/* 0343 000002FF 80012134 IAND        */   vi04 = vi04 & 32760;
/* 0344 000002FF 10042005 IADDIU      */   vi04 = vi04 + 5;
/* 0345 000002FF 0B045801 ISW.x       */   mem_w[cursors_vi11 + 1].x = vi04;
/* 0346 000002FF 0A405801 ISW.z       */   mem_w[cursors_vi11 + 1].z = 0;
/* 0347 000002FF 02CB1002 SQ.yz       */   mem[cursors_vi11 + 2].yz = vf02.yz;
/* 0348 000002FF 10045001 IADDIU      */   vi04 = vi10 + 1;
/* 0349 000002FF 11E427FF IADDIU      */   vi04 = vi04 + 32767;
/* 034A 000002FF 0B045802 ISW.x       */   mem_w[cursors_vi11 + 2].x = vi04;
/* 034C 000002FF 100B5803 IADDIU      */   cursors_vi11 = cursors_vi11 + 3;    // delay slot of 034B
/* 034B 000002FF 48006800 JR          */   return;
L_034D:  /* from 0418 */
/* 034D 000002FF 01E502F0 LQ.xyzw     */   vf05.xyzw = normal[0].xyzw;
/* 034E 01C5292A 10080001 MUL.xyz     */   vf04.xyz = vf05.xyz * vf05.xyz;
/*                        IADDIU      */   vi08 = 1;
/* 034F 0104203D 01E202F1 LQ.xyzw     */   vf02.xyzw = normal[1].xyzw;         // ADDAy.x folded into 0350
/* 0350 0104090A 8000033C MADDz.x     */   vf04.x = vf04.x + vf04.y + vf01.x * vf04.z; // folds 034F..0350
/* 0351 01C210EA 806403BE MUL.xyz     */   vf03.xyz = vf02.xyz * vf02.xyz;
/*                        RSQRT       */   Q = 1.0f / sqrtf(fabsf(vf04.x));
/* 0352 0103183D 8000033C             */   /* ADDAy.x folded into 0353 */
/* 0353 010308CA 8000033C MADDz.x     */   vf03.x = vf03.x + vf03.y + vf01.x * vf03.z; // folds 0352..0353
/* 0354 01C0291C 800003BF MULq.xyz    */   vf04.xyz = vf05.xyz * Q;
/*                        WAITQ       */   /* WAITQ */
  /* loop 0355..035A: vi08 += 1 per iteration (0357) */
/*                                    */   do {
/* 0355 01C0115B 806303BE MULw.xyz    */     vf05.xyz = vf02.xyz;
/*                        RSQRT       */     Q = 1.0f / sqrtf(fabsf(vf03.x));
/* 0356 000002FF 01E242F1 LQ.xyzw     */     vf02.xyzw = normal[vi08 + 1].xyzw;
/* 0357 01C210EA 10084001 MUL.xyz     */     vf03.xyz = vf02.xyz * vf02.xyz;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 0358 0103183D 03E822EE ADDAy.x     */     ACC.x = vf03.x + vf03.y;
/*                        SQ.xyzw     */     normal[vi08 - 2].xyzw = vf04.xyzw;
/* 0359 010308CA 52082FFB MADDz.x     */     vf03.x = ACC.x + vf01.x * vf03.z;
/* 035A 01C0291C 800003BF MULq.xyz    */     vf04.xyz = vf05.xyz * Q;          // delay slot of 0359
/*                        WAITQ       */     /* WAITQ */
/*                        IBNE        */   } while (vi05 != vi08);
/* 035C 000002FF 03E822EF SQ.xyzw     */   normal[vi08 - 1].xyzw = vf04.xyzw;  // delay slot of 035B
/* 035B 000002FF 48007800 JR          */   return;
subroutine(unpackPointers, clip, vf01, vi02);                          /* 035D, from 01B9 041F */
/* 035D 000002FF 09041000 ILW.x       */   vi04 = mem_w[vi02].x;
/* 035E 000002FF 08851000 ILW.y       */   vi05 = mem_w[vi02].y;
/* 035F 000002FF 10010020 IADDIU      */   vi01 = 32;
/* 0360 000002FF 80011874 IAND        */   vi01 = vi03 & 32;
/* 0361 000002FF 10071001 IADDIU      */   vi07 = vi02 + 1;
/* 0363 000002FF 08461000 ILW.z       */   vi06 = mem_w[vi02].z;               // delay slot of 0362
/* 0362 000002FF 50000803 IBEQ        */   if (vi01 != 0) {
/* 0364 000002FF 0A270382 ISW.w       */     indexList = vi07;
/* 0365 000002FF 800439F0 IADD        */     vi07 = vi07 + vi04;
/*                                    */   }
/* 0366 000002FF 10010080 IADDIU      */   vi01 = 128;
/* 0367 000002FF 80011874 IAND        */   vi01 = vi03 & 128;
/* 0368 000002FF 0A270383 ISW.w       */   outPosition = vi07;
/* 036A 000002FF 800539F0 IADD        */   vi07 = vi07 + vi05;                 // delay slot of 0369
/* 0369 000002FF 50010003 IBEQ        */   if (vi01 != 0) {
/* 036B 000002FF 0A270384 ISW.w       */     srcNormals = vi07;
/* 036C 000002FF 800539F0 IADD        */     vi07 = vi07 + vi05;
/*                                    */   }
/* 036D 000002FF 10010100 IADDIU      */   vi01 = 256;
/* 036E 000002FF 80011874 IAND        */   vi01 = vi03 & 256;
/* 036F 000002FF 08280393 ILW.w       */   savedPointer_vi08 = savedPointer;
/* 0371 000002FF 800831B4 IAND        */   vi06 = vi06 & savedPointer_vi08;    // delay slot of 0370
/* 0370 000002FF 50000803 IBEQ        */   if (vi01 != 0) {
/* 0372 000002FF 0A270385 ISW.w       */     srcTexcoords = vi07;
/* 0373 000002FF 800539F0 IADD        */     vi07 = vi07 + vi05;
/*                                    */   }
/* 0374 000002FF 10010200 IADDIU      */   vi01 = 512;
/* 0375 000002FF 80011874 IAND        */   vi01 = vi03 & 512;
/* 0376 000002FF 0A260388 ISW.w       */   drawMode = vi06;
/* 0378 000002FF 0A240380 ISW.w       */   faceCount = vi04;                   // delay slot of 0377
/* 0377 000002FF 50000802 IBEQ        */   if (vi01 != 0) {
/* 0379 000002FF 0A270386 ISW.w       */     srcColors = vi07;
/*                                    */   }
/* 037A 000002FF 0A250381 ISW.w       */   vertexCount = vi05;
/* 037B 000002FF 10080078 IADDIU      */   vi08 = 120;
/* 037C 000002FF 80083234 IAND        */   vi08 = vi06 & 120;
/* 037D 000002FF 10E10400 IADDIU      */   vi01 = 15360;
/* 037E 000002FF 80011874 IAND        */   vi01 = vi03 & 15360;
/* 037F 000002FF 0A230387 ISW.w       */   drawFlags = vi03;
/* 0381 000002FF 0A28038B ISW.w       */   primFlags = vi08;                   // delay slot of 0380
/* 0380 000002FF 50000899 IBEQ        */   if (vi01 == 0) goto L_041A;
/* 0382 000002FF 10210000 IADDIU      */   vi01 = 2048;
/* 0383 000002FF 80011874 IAND        */   vi01 = vi03 & 2048;
/* 0384 000002FF 1008001F IADDIU      */   vi08 = 31;
/* 0386 000002FF 80034234 IAND        */   vi08 = 31 & vi03;                   // delay slot of 0385
/* 0385 000002FF 5000085A IBEQ        */   if (vi01 == 0) goto L_03E0;
/* 0387 000002FF 082A0383 ILW.w       */   vi10 = outPosition;
/* a jump at 0193 can reach here */
/* 0388 000002FF 082B0384 ILW.w       */   srcNormals_vi11 = srcNormals;
/* 0389 000002FF 10010400 IADDIU      */   vi01 = 1024;
/* 038A 000002FF 80011874 IAND        */   vi01 = vi03 & 1024;
/* 038B 000002FF 81E2537C LQI.xyzw    */   vf02.xyzw = mem[vi10++].xyzw;
/* 038C 000002FF 81E35B7C LQI.xyzw    */   vf03.xyzw = mem[srcNormals_vi11++].xyzw;
/* a jump at 0193 can reach here */
/* 038D 000002FF 01CA43A8 LQ.xyz      */   vf10.xyz = mem[vi08 + 936].xyz;
/* 038E 000002FF 01CB43A9 LQ.xyz      */   vf11.xyz = mem[vi08 + 937].xyz;
/* 038F 000002FF 01CC43AA LQ.xyz      */   vf12.xyz = mem[vi08 + 938].xyz;
/* 0390 000002FF 01CD43AB LQ.xyz      */   vf13.xyz = mem[vi08 + 939].xyz;
/* 0391 000002FF 01CE43AC LQ.xyz      */   vf14.xyz = mem[vi08 + 940].xyz;
/* 0392 01C251BC 01CF43AD MULAx.xyz   */   ACC.xyz = vf10.xyz * vf02.x;
/*                        LQ.xyz      */   vf15.xyz = mem[vi08 + 941].xyz;
/* 0393 01C258BD 5000080F MADDAy.xyz  */   ACC.xyz = ACC.xyz + vf11.xyz * vf02.y;
/* 0394 01C260BE 01D043AE MADDAz.xyz  */   ACC.xyz = ACC.xyz + vf12.xyz * vf02.z; // delay slot of 0393
/*                        LQ.xyz      */   vf16.xyz = mem[vi08 + 942].xyz;
/*                        IBEQ        */   if (vi01 == 0) goto L_03A3;
/* 0395 01C0690B 8026133C MADDw.xyz   */   vf04.xyz = ACC.xyz + vf13.xyz;
/*                        MOVE.w      */   vf06.w = vf02.w;
/* 0396 01C371BC 81E2537C LQI.xyzw    */   vf02.xyzw = mem[vi10++].xyzw;       // MULAx.xyz folded into 0398
/* 0397 01C378BD 10080000 IADDIU      */   vi08 = 0;                           // MADDAy.xyz folded into 0398
/* 0398 01C3814A 8000033C MADDz.xyz   */   vf05.xyz = mul3x3(vf14, vf03.xyz);  // folds 0396..0398
  /* loop 0399..03A1: vi08 += 1 per iteration (039C) */
/*                                    */   do {
/* 0399 01C6211B 81E35B7C MULw.xyz    */     vf04.xyz = vf04.xyz * vf06.w;
/*                        LQI.xyzw    */     vf03.xyzw = mem[vi11++].xyzw;
/* 039A 01C251BC 8000033C MULAx.xyz   */     ACC.xyz = vf10.xyz * vf02.x;
/* 039B 01C258BD 8000033C MADDAy.xyz  */     ACC.xyz = ACC.xyz + vf11.xyz * vf02.y;
/* 039C 01C629DB 10084001 MULw.xyz    */     vf07.xyz = vf05.xyz * vf06.w;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 039D 01C260BE 03E822BF MADDAz.xyz  */     ACC.xyz = ACC.xyz + vf12.xyz * vf02.z;
/*                        SQ.xyzw     */     position[vi08 - 1].xyzw = vf04.xyzw;
/* 039E 01C0690B 8026133C MADDw.xyz   */     vf04.xyz = ACC.xyz + vf13.xyz;
/*                        MOVE.w      */     vf06.w = vf02.w;
/* 039F 01C371BC 81E2537C MULAx.xyz   */     ACC.xyz = vf14.xyz * vf03.x;
/*                        LQI.xyzw    */     vf02.xyzw = mem[vi10++].xyzw;
/* 03A0 01C378BD 52082FF8 MADDAy.xyz  */     ACC.xyz = ACC.xyz + vf15.xyz * vf03.y;
/* 03A1 01C3814A 03E83AEF MADDz.xyz   */     vf05.xyz = ACC.xyz + vf16.xyz * vf03.z; // delay slot of 03A0
/*                        SQ.xyzw     */     normal[vi08 - 1].xyzw = vf07.xyzw;
/*                        IBNE        */   } while (vi05 != vi08);
L_03A3:  /* delay slot of 03A2, reached with take clear */
/* 03A3 000002FF 10810000 IADDIU      */   vi01 = 8192;                        // delay slot of 03A2
/* 03A2 000002FF 40000103 B           */   goto L_04A6;
/* 03A4 000002FF 80011874 IAND        */   vi01 = vi03 & vi01;
/* 03A5 000002FF 8000033C             */   /* nop */
/* 03A7 000002FF 8000033C             */   /* nop */                           // delay slot of 03A6
/* 03A6 000002FF 50000817 IBEQ        */   if (vi01 == 0) goto L_03BE;
/* a jump at 02F1 05C6 can reach here */
/* 03A8 01C0698B 01E402C0 MADDw.xyz   */   vf06.xyz = ACC.xyz + vf13.xyz;
/*                        LQ.xyzw     */   vf04.xyzw = position[0].xyzw;
/* 03A9 01C371BC 8026133C MOVE.w      */   vf06.w = vf02.w;                    // MULAx.xyz folded into 03AB
/* 03AA 01C378BD 01E502F0 LQ.xyzw     */   vf05.xyzw = normal[0].xyzw;         // MADDAy.xyz folded into 03AB
/* 03AB 01C381CA 10080001 MADDz.xyz   */   vf07.xyz = mul3x3(vf14, vf03.xyz);  // folds 03A9..03AB
/*                        IADDIU      */   vi08 = 1;
/* 03AC 01C021BF 81E2537C LQI.xyzw    */   vf02.xyzw = mem[vi10++].xyzw;       // MULAw.xyz folded into 03AD
/* 03AD 01C6310B 81E35B7C MADDw.xyz   */   vf04.xyz = vf04.xyz + vf06.xyz * vf06.w; // folds 03AC..03AD
/*                        LQI.xyzw    */   vf03.xyzw = mem[vi11++].xyzw;
/* 03AE 01C029BF 8000033C             */   /* MULAw.xyz folded into 03AF */
/* 03AF 01C6394B 8000033C MADDw.xyz   */   vf05.xyz = vf05.xyz + vf07.xyz * vf06.w; // folds 03AE..03AF
  /* loop 03B0..03BA: vi08 += 1 per iteration (03B6) */
/*                                    */   do {
/* 03B0 01C251BC 8026133C MULAx.xyz   */     ACC.xyz = vf10.xyz * vf02.x;
/*                        MOVE.w      */     vf06.w = vf02.w;
/* 03B1 01C258BD 03E822BF MADDAy.xyz  */     ACC.xyz = ACC.xyz + vf11.xyz * vf02.y;
/*                        SQ.xyzw     */     position[vi08 - 1].xyzw = vf04.xyzw;
/* 03B2 01C260BE 01E442C0 MADDAz.xyz  */     ACC.xyz = ACC.xyz + vf12.xyz * vf02.z;
/*                        LQ.xyzw     */     position_vf04.xyzw = position[vi08].xyzw;
/* 03B3 01C0698B 03E82AEF MADDw.xyz   */     vf06.xyz = ACC.xyz + vf13.xyz;
/*                        SQ.xyzw     */     normal[vi08 - 1].xyzw = vf05.xyzw;
/* 03B4 01C371BC 81E2537C LQI.xyzw    */     vf02.xyzw = mem[vi10++].xyzw;     // MULAx.xyz folded into 03B6
/* 03B5 01C378BD 01E542F0 LQ.xyzw     */     normal_vf05.xyzw = normal[vi08].xyzw; // MADDAy.xyz folded into 03B6
/* 03B6 01C381CA 10084001 MADDz.xyz   */     vf07.xyz = mul3x3(vf14, vf03.xyz); // folds 03B4..03B6
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 03B7 01C021BF 81E35B7C LQI.xyzw    */     vf03.xyzw = mem[vi11++].xyzw;     // MULAw.xyz folded into 03B8
/* 03B8 01C6310B 8000033C MADDw.xyz   */     vf04.xyz = vf04.xyz + vf06.xyz * vf06.w; // folds 03B7..03B8
/* 03B9 01C029BF 52082FF6 MULAw.xyz   */     ACC.xyz = normal_vf05.xyz;
/* 03BA 01C6394B 8000033C MADDw.xyz   */     vf05.xyz = ACC.xyz + vf07.xyz * vf06.w; // delay slot of 03B9
/*                        IBNE        */   } while (vi05 != vi08);
/* 03BB 000002FF 03E822BF SQ.xyzw     */   position[vi08 - 1].xyzw = vf04.xyzw;
/* 03BC 400002FF 03E82AEF SQ.xyzw     */   normal[vi08 - 1].xyzw = vf05.xyzw;
/* 03BD 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 03BC */
L_03BE:  /* from 03A6 */
/* 03BE 01C371BC 10080000 IADDIU      */   vi08 = 0;                           // MULAx.xyz folded into 03C0
/* 03BF 01C378BD 01E542F0 LQ.xyzw     */   normal_vf05.xyzw = mem[0 + 752].xyzw; // MADDAy.xyz folded into 03C0
/* 03C0 01C381CA 8000033C MADDz.xyz   */   vf07.xyz = mul3x3(vf14, vf03.xyz);  // folds 03BE..03C0
/* 03C1 01C029BF 8000033C             */   /* MULAw.xyz folded into 03C2 */
/* 03C2 01C239CB 8000033C MADDw.xyz   */   vf07.xyz = normal_vf05.xyz + vf07.xyz * vf02.w; // folds 03C1..03C2
/* 03C3 01C73A2A 81E35B7C MUL.xyz     */   vf08.xyz = vf07.xyz * vf07.xyz;
/*                        LQI.xyzw    */   vf03.xyzw = mem[vi11++].xyzw;
/* 03C4 0108403D 8000033C             */   /* ADDAy.x folded into 03C5 */
/* 03C5 01080A0A 81F1537C MADDz.x     */   vf08.x = vf08.x + vf08.y + vf01.x * vf08.z; // folds 03C4..03C5
/*                        LQI.xyzw    */   vf17.xyzw = mem[vi10++].xyzw;
/* 03C6 01C371BC 8000033C MULAx.xyz   */   ACC.xyz = vf14.xyz * vf03.x;
/* 03C7 01C378BD 8000033C MADDAy.xyz  */   ACC.xyz = ACC.xyz + vf15.xyz * vf03.y;
  /* loop 03C8..03D6: vi08 += 1 per iteration (03CF) */
/*                                    */   do {
/* 03C8 01C380CA 806803BE MADDz.xyz   */     vf03.xyz = ACC.xyz + vf16.xyz * vf03.z;
/*                        RSQRT       */     Q = 1.0f / sqrtf(fabsf(vf08.x));
/* 03C9 01C251BC 8000033C             */     /* MULAx.xyz folded into 03CC */
/* 03CA 01C258BD 01E542F1 LQ.xyzw     */     normal_vf05.xyzw = normal[vi08 + 1].xyzw; // MADDAy.xyz folded into 03CC
/* 03CB 01C260BE 8000033C             */     /* MADDAz.xyz folded into 03CC */
/* 03CC 01C0698B 01E442C0 MADDw.xyz   */     vf06.xyz = vf10.xyz * vf02.x + vf11.xyz * vf02.y + vf12.xyz * vf02.z + vf13.xyz; // folds 03C9..03CC
/*                        LQ.xyzw     */     position_vf04.xyzw = position[vi08].xyzw;
/* 03CD 01D119BF 81E93B3C MULAw.xyz   */     ACC.xyz = vf03.xyz * vf17.w;
/*                        MOVE.xyzw   */     vf09.xyzw = vf07.xyzw;
/* 03CE 01C029CB 81E35B7C MADDw.xyz   */     vf07.xyz = ACC.xyz + normal_vf05.xyz;
/*                        LQI.xyzw    */     vf03.xyzw = mem[vi11++].xyzw;
/* 03CF 01C021BF 10084001 IADDIU      */     vi08 = vi08 + 1;                  // MULAw.xyz folded into 03D0
/* 03D0 01C2318B 8000033C MADDw.xyz   */     vf06.xyz = position_vf04.xyz + vf06.xyz * vf02.w; // folds 03CF..03D0
/* 03D1 01C73A2A 81E28B3C MUL.xyz     */     vf08.xyz = vf07.xyz * vf07.xyz;
/*                        MOVE.xyzw   */     vf02.xyzw = vf17.xyzw;
/* 03D2 01C04A5C 800003BF MULq.xyz    */     vf09.xyz = vf09.xyz * Q;
/*                        WAITQ       */     /* WAITQ */
/* 03D3 0108403D 81F1537C LQI.xyzw    */     vf17.xyzw = mem[vi10++].xyzw;     // ADDAy.x folded into 03D4
/* 03D4 01080A0A 03E832BF MADDz.x     */     vf08.x = vf08.x + vf08.y + vf01.x * vf08.z; // folds 03D3..03D4
/*                        SQ.xyzw     */     position[vi08 - 1].xyzw = vf06.xyzw;
/* 03D5 01C371BC 52082FF2 MULAx.xyz   */     ACC.xyz = vf14.xyz * vf03.x;
/* 03D6 01C378BD 03E84AEF MADDAy.xyz  */     ACC.xyz = ACC.xyz + vf15.xyz * vf03.y; // delay slot of 03D5
/*                        SQ.xyzw     */     normal[vi08 - 1].xyzw = vf09.xyzw;
/*                        IBNE        */   } while (vi05 != vi08);
/* 03D7 000002FF 100802C0 IADDIU      */   vi08 = 704;                         // position, the base
/* 03D8 000002FF 0A280383 ISW.w       */   outPosition = 704;
/* 03D9 000002FF 100102F0 IADDIU      */   vi01 = 752;                         // normal, the base
/* 03DA 000002FF 0A210384 ISW.w       */   srcNormals = 752;
/* 03DB 000002FF 01FA03E0 LQ.xyzw     */   camera0_vf26.xyzw = camera0.xyzw;
/* 03DC 000002FF 01FB03E1 LQ.xyzw     */   camera1_vf27.xyzw = camera1.xyzw;
/* 03DD 000002FF 01FC03E2 LQ.xyzw     */   camera2_vf28.xyzw = camera2.xyzw;
/* 03DF 000002FF 01FD03E3 LQ.xyzw     */   vf29.xyzw = camera3.xyzw;           // delay slot of 03DE
/* 03DE 000002FF 48007800 JR          */   goto_micro(vi15);
L_03E0:  /* from 0385 */
/* 03E0 000002FF 10080000 IADDIU      */   vi08 = 0;
/* 03E1 000002FF 0827038F ILW.w       */   vi07 = mem_w[911].w;
/* 03E2 000002FF 10810000 IADDIU      */   vi01 = 8192;
/* 03E3 000002FF 80011874 IAND        */   vi01 = vi03 & 8192;
/* 03E4 000002FF 08240383 ILW.w       */   outPosition_vi04 = outPosition;
/* 03E5 000002FF 00293B90 LQ.w        */   vf09.w = mem[vi07 + 912].w;
/* 03E7 000002FF 08260384 ILW.w       */   srcNormals_vi06 = srcNormals;       // delay slot of 03E6
/* 03E6 000002FF 5000081C IBEQ        */   if (vi01 == 0) goto L_0403;
/* 03E8 000002FF 81E2237C LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi04++].xyzw;
/* 03E9 000002FF 81E3337C LQI.xyzw    */   vf03.xyzw = mem[srcNormals_vi06++].xyzw;
/* 03EA 000002FF 10013801 IADDIU      */   vi01 = vi07 + 1;
/* 03EC 000002FF 0A21038F ISW.w       */   mem_w[911].w = vi01;                // delay slot of 03EB
/* 03EB 000002FF 5200380B IBNE        */   if (vi07 != 0) goto L_03F7;
/* 03ED 01C9111B 8000033C MULw.xyz    */   vf04.xyz = vf02.xyz * vf09.w;
/* 03EE 01C9195B 81E2237C MULw.xyz    */   vf05.xyz = vf03.xyz * vf09.w;
/*                        LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi04++].xyzw;
  /* loop 03EF..03F4: vi08 += 1 per iteration (03F0) */
/*                                    */   do {
/* 03EF 000002FF 81E3337C LQI.xyzw    */     vf03.xyzw = mem[srcNormals_vi06++].xyzw;
/* 03F0 000002FF 10084001 IADDIU      */     vi08 = vi08 + 1;
/* 03F1 000002FF 03E822BF SQ.xyzw     */     position[vi08 - 1].xyzw = vf04.xyzw;
/* 03F2 01C9111B 03E82AEF MULw.xyz    */     vf04.xyz = vf02.xyz * vf09.w;
/*                        SQ.xyzw     */     normal[vi08 - 1].xyzw = vf05.xyzw;
/* 03F3 01C9195B 52082FFB MULw.xyz    */     vf05.xyz = vf03.xyz * vf09.w;
/* 03F4 000002FF 81E2237C LQI.xyzw    */     vf02.xyzw = mem[outPosition_vi04++].xyzw; // delay slot of 03F3
/*                        IBNE        */   } while (vi05 != vi08);
/* 03F6 000002FF 8000033C             */   /* nop */                           // delay slot of 03F5
/* 03F5 000002FF 400000B0 B           */   goto L_04A6;
L_03F7:  /* from 03EB */
/* 03F7 000002FF 01E442C0 LQ.xyzw     */   position_vf04.xyzw = mem[0 + 704].xyzw;
/* 03F8 000002FF 01E542F0 LQ.xyzw     */   normal_vf05.xyzw = mem[0 + 752].xyzw;
  /* loop 03F9..0400: vi08 += 1 per iteration (03F9) */
/*                                    */   do {
/* 03F9 01C911BF 10084001 MULAw.xyz   */     ACC.xyz = vf02.xyz * vf09.w;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 03FA 01C0218B 81E2237C MADDw.xyz   */     vf06.xyz = ACC.xyz + vf04.xyz;
/*                        LQI.xyzw    */     vf02.xyzw = mem[vi04++].xyzw;
/* 03FB 01C919BF 01E442C0 MULAw.xyz   */     ACC.xyz = vf03.xyz * vf09.w;
/*                        LQ.xyzw     */     position_vf04.xyzw = position[vi08].xyzw;
/* 03FC 01C029CB 81E3337C MADDw.xyz   */     vf07.xyz = ACC.xyz + vf05.xyz;
/*                        LQI.xyzw    */     vf03.xyzw = mem[vi06++].xyzw;
/* 03FD 000002FF 01E542F0 LQ.xyzw     */     normal_vf05.xyzw = normal[vi08].xyzw;
/* 03FE 000002FF 03E832BF SQ.xyzw     */     position[vi08 - 1].xyzw = vf06.xyzw;
/* 0400 000002FF 03E83AEF SQ.xyzw     */     normal[vi08 - 1].xyzw = vf07.xyzw; // delay slot of 03FF
/* 03FF 000002FF 52082FF9 IBNE        */   } while (vi05 != vi08);
/* 0402 000002FF 8000033C             */   /* nop */                           // delay slot of 0401
/* 0401 000002FF 400000A4 B           */   goto L_04A6;
L_0403:  /* from 03E6 */
/* 0404 000002FF 0A20038F ISW.w       */   mem_w[911].w = 0;                   // delay slot of 0403
/* 0403 000002FF 50003816 IBEQ        */   if (vi07 == 0) goto L_041A;
/* 0405 000002FF 81E2237C LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi04++].xyzw;
/* 0406 000002FF 01E442C0 LQ.xyzw     */   position_vf04.xyzw = mem[0 + 704].xyzw;
/* 0407 000002FF 81E3337C LQI.xyzw    */   vf03.xyzw = mem[srcNormals_vi06++].xyzw;
/* 0408 000002FF 01E542F0 LQ.xyzw     */   normal_vf05.xyzw = mem[0 + 752].xyzw;
  /* loop 0409..0410: vi08 += 1 per iteration (0409) */
/*                                    */   do {
/* 0409 01C411A8 10084001 ADD.xyz     */     vf06.xyz = vf02.xyz + position_vf04.xyz;
/*                        IADDIU      */     vi08 = vi08 + 1;
/* 040A 000002FF 81E2237C LQI.xyzw    */     vf02.xyzw = mem[outPosition_vi04++].xyzw;
/* 040B 01C519E8 01E442C0 ADD.xyz     */     vf07.xyz = vf03.xyz + normal_vf05.xyz;
/*                        LQ.xyzw     */     position_vf04.xyzw = position[vi08].xyzw;
/* 040C 000002FF 81E3337C LQI.xyzw    */     vf03.xyzw = mem[srcNormals_vi06++].xyzw;
/* 040D 000002FF 01E542F0 LQ.xyzw     */     normal_vf05.xyzw = normal[vi08].xyzw;
/* 040E 000002FF 03E832BF SQ.xyzw     */     position[vi08 - 1].xyzw = vf06.xyzw;
/* 0410 000002FF 03E83AEF SQ.xyzw     */     normal[vi08 - 1].xyzw = vf07.xyzw; // delay slot of 040F
/* 040F 000002FF 52082FF9 IBNE        */   } while (vi05 != vi08);
/* 0411 000002FF 100802C0 IADDIU      */   vi08 = 704;                         // position, the base
/* 0412 000002FF 0A280383 ISW.w       */   outPosition = 704;
/* 0413 000002FF 100102F0 IADDIU      */   vi01 = 752;                         // normal, the base
/* 0414 000002FF 0A210384 ISW.w       */   srcNormals = 752;
/* 0415 000002FF 01FA03C4 LQ.xyzw     */   vf26.xyzw = objectCamera0.xyzw;
/* 0416 000002FF 01FB03C5 LQ.xyzw     */   vf27.xyzw = objectCamera1.xyzw;
/* 0417 000002FF 01FC03C6 LQ.xyzw     */   vf28.xyzw = objectCamera2.xyzw;
/* 0419 000002FF 01FD03C7 LQ.xyzw     */   vf29.xyzw = objectCamera3.xyzw;     // delay slot of 0418
/* 0418 000002FF 40000734 B           */   goto L_034D;
L_041A:  /* from 0380, 0403 */
/* 041A 000002FF 01FA03C4 LQ.xyzw     */   vf26.xyzw = objectCamera0.xyzw;
/* 041B 000002FF 01FB03C5 LQ.xyzw     */   vf27.xyzw = objectCamera1.xyzw;
/* 041C 000002FF 01FC03C6 LQ.xyzw     */   vf28.xyzw = objectCamera2.xyzw;
/* 041E 000002FF 01FD03C7 LQ.xyzw     */   vf29.xyzw = objectCamera3.xyzw;     // delay slot of 041D
/* 041D 000002FF 48007800 JR          */   return;
subroutine(drawEntry2, clip, mac, vf01, vf15, vf18, vf26, vf27, vf28, vf29, vf30, vf31, vi02, vi14); /* 041F, from 0010 */
/* 0420 000002FF 08231000 ILW.w       */   vi03 = mem_w[vi02].w;               // delay slot of 041F
/* 041F 000002FF 420F073D BAL         */   unpackPointers();
/* 0422 000002FF 01EF03ED LQ.xyzw     */   drawColor_vf15.xyzw = drawColor.xyzw; // delay slot of 0421
/* 0421 000002FF 420F0018 BAL         */   lightRamp();
/* 0423 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0424 000002FF 11010000 IADDIU      */   vi01 = 16384;
/* 0425 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 16384;
/* 0426 000002FF 8000033C             */   /* nop */
/* 0428 000002FF 100F04A6 IADDIU      */   vi15 = 1190;                        // delay slot of 0427
/* 0427 000002FF 50000853 IBEQ        */   if (vi01 == 0) goto L_047B;
/* 042A 000002FF 8000033C             */   /* nop */                           // delay slot of 0429
/* 0429 000002FF 420F0051 BAL         */   drawFront2();
/* 042C 000002FF 8000033C             */   /* nop */                           // delay slot of 042B
/* 042B 000002FF 420F05B3 BAL         */   negateNormals();
/* 042E 000002FF 01EF03ED LQ.xyzw     */   drawColor_vf15.xyzw = drawColor.xyzw; // delay slot of 042D
/* 042D 000002FF 420F000C BAL         */   lightRamp();
/* 042F 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0430 000002FF 10010010 IADDIU      */   vi01 = 16;
/* 0431 000002FF 100F04A6 IADDIU      */   vi15 = 1190;
/* 0432 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 16;
/* 0433 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
/* 0435 000002FF 0B0F7000 ISW.x       */   mem_w[vi14].x = 1190;               // delay slot of 0434
/* 0434 000002FF 50000850 IBEQ        */   if (vi01 == 0) goto L_0485;
/* 0437 000002FF 8000033C             */   /* nop */                           // delay slot of 0436
/* 0436 000002FF 420F05C7 BAL         */   texcoordMatrix();
/* 0439 000002FF 8000033C             */   /* nop */                           // delay slot of 0438
/* 0438 000002FF 40000054 B           */   goto L_048D;
subroutine(lightRamp, vf15);                                           /* 043A, from 0421 042D */
/* a jump at 02F1 05C6 can reach here */
/* 043A 800002FF 437F0000 LOI         */   I = 255.0f;
/* 043B 000002FF 01E303EC LQ.xyzw     */   matAmbient_vf03.xyzw = matAmbient.xyzw;
/* 043C 002001D8 01C6039F MULx.w      */   vf07.w = 0.0f;
/*                        LQ.xyz      */   vf06.xyz = color3.xyz;
/* 043D 000002FF 01C7039B LQ.xyz      */   colorBase_vf07.xyz = colorBase.xyz;
/* 043E 000002FF 01E403EF LQ.xyzw     */   drawTint_vf04.xyzw = drawTint.xyzw;
/* 043F 002F019B 0028038E MULw.w      */   vf06.w = drawColor_vf15.w;
/*                        LQ.w        */   flatten_vf08.w = flatten;
/* 0440 01C331AA 010203F6 MUL.xyz     */   vf06.xyz = vf06.xyz * matAmbient_vf03.xyz;
/*                        LQ.x        */   rampGainAdd_vf02.x = rampGainAdd;
/* 0441 01CF39EA 002A03D4 MUL.xyz     */   colorBase_vf07.xyz = colorBase_vf07.xyz * drawColor_vf15.xyz;
/*                        LQ.w        */   rampGain_vf10.w = rampGain;
/* 0442 000002FF 010B03F7 LQ.x        */   rampBiasAdd_vf11.x = rampBiasAdd;
/* 0443 000002FF 002503D5 LQ.w        */   rampBias_vf05.w = rampBias;
/* 0444 000002FF 08230387 ILW.w       */   drawFlags_vi03 = drawFlags;
/* 0445 01C431AA 10210000 MUL.xyz     */   vf06.xyz = vf06.xyz * drawTint_vf04.xyz;
/*                        IADDIU      */   vi01 = 2048;
/* 0446 00225280 80030874 ADDx.w      */   rampGain_vf10.w = rampGain_vf10.w + rampGainAdd_vf02.x;
/*                        IAND        */   vi01 = 2048 & drawFlags_vi03;
/* 0447 002B2940 10030320 ADDx.w      */   rampBias_vf05.w = rampBias_vf05.w + rampBiasAdd_vf11.x;
/*                        IADDIU      */   vi03 = 800;                         // vertexColor, the base
/* 0448 01C839BF 50000805 MULAw.xyz   */   ACC.xyz = colorBase_vf07.xyz * flatten_vf08.w;
/* 0449 01C030BF 08220384 MADDAw.xyz  */   ACC.xyz = ACC.xyz + vf06.xyz;       // delay slot of 0448
/*                        ILW.w       */   srcNormals_vi02 = srcNormals;
/*                        IBEQ        */   if (vi01 != 0) {
/* 044A 002A0293 01F70398 MAXw.w      */     rampGain_vf10.w = max(1.0f, rampGain_vf10.w);
/*                        LQ.xyzw     */     vf23.xyzw = lightDir0.xyzw;
/* 044B 01C8318F 01F80399 MSUBw.xyz   */     vf06.xyz = ACC.xyz - vf06.xyz * flatten_vf08.w;
/*                        LQ.xyzw     */     vf24.xyzw = lightDir1.xyzw;
/* 044C 01E039DF 40000004 MINIi.xyzw  */     colorBase_vf07.xyzw = min(colorBase_vf07.xyzw, 255.0f);
/* 044D 000002FF 01F9039A LQ.xyzw     */     vf25.xyzw = lightDir2.xyzw;       // delay slot of 044C
/*                        B           */   } else {
/* 044E 002A0293 01F703C8 MAXw.w      */     rampGain_vf10.w = max(1.0f, rampGain_vf10.w);
/*                        LQ.xyzw     */     vf23.xyzw = lightNormal0.xyzw;
/* 044F 01C8318F 01F803C9 MSUBw.xyz   */     vf06.xyz = ACC.xyz - vf06.xyz * flatten_vf08.w;
/*                        LQ.xyzw     */     vf24.xyzw = lightNormal1.xyzw;
/* 0450 01E039DF 01F903CA MINIi.xyzw  */     colorBase_vf07.xyzw = min(colorBase_vf07.xyzw, 255.0f);
/*                        LQ.xyzw     */     vf25.xyzw = lightNormal2.xyzw;
/*                                    */   }
/* 0451 00375098 81C8137C MULx.w      */   vf02.w = rampGain_vf10.w * vf23.x;
/*                        LQI.xyz     */   vf08.xyz = mem[srcNormals_vi02++].xyz;
/* 0452 002A296A 8000033C MUL.w       */   rampBias_vf05.w = rampBias_vf05.w * rampGain_vf10.w;
/* 0453 01E0319F 8000033C MINIi.xyzw  */   vf06.xyzw = min(vf06.xyzw, 255.0f);
/* 0454 003850D8 08250381 MULx.w      */   vf03.w = rampGain_vf10.w * vf24.x;
/*                        ILW.w       */   vertexCount_vi05 = vertexCount;
/* 0455 00395118 100402F0 MULx.w      */   vf04.w = rampGain_vf10.w * vf25.x;
/*                        IADDIU      */   vi04 = 752;                         // normal, the base
/* 0456 01C639EC 81CB137C SUB.xyz     */   colorBase_vf07.xyz = colorBase_vf07.xyz - vf06.xyz;
/*                        LQI.xyz     */   vf11.xyz = mem[srcNormals_vi02++].xyz;
/* 0457 002029BF 10060001 IADDIU      */   vi06 = 1;                           // MULAw.w folded into 045A
/* 0458 002810BC 800531B4 IAND        */   vi06 = 1 & vertexCount_vi05;        // MADDAx.w folded into 045A
/* 0459 002818BD 80062971 ISUB        */   vi05 = vi05 - vi06;                 // MADDAy.w folded into 045A
/* 045A 0028228A 8000033C MADDz.w     */   vf10.w = rampBias_vf05.w + vf02.w * vf08.x + vf03.w * vf08.y + vf04.w * vf08.z; // folds 0457..045A
/* 045B 002029BF 8000033C MULAw.w     */   ACC.w = rampBias_vf05.w;
  /* loop 045C..046E: vi05 -= 2 per iteration (046B) */
/*                                    */   do {
/* 045C 002B10BC 81C8137C MADDAx.w    */     ACC.w = ACC.w + vf02.w * vf11.x;
/*                        LQI.xyz     */     vf08.xyz = mem[srcNormals_vi02++].xyz;
/* 045D 002B18BD 8000033C MADDAy.w    */     ACC.w = ACC.w + vf03.w * vf11.y;
/* 045E 002B234A 8000033C MADDz.w     */     vf13.w = ACC.w + vf04.w * vf11.z;
/* 045F 00205250 81CB137C MAXx.w      */     vf09.w = max(vf10.w, 0.0f);
/*                        LQI.xyz     */     vf11.xyz = mem[srcNormals_vi02++].xyz;
/* 0460 002A503F 8000033C             */     /* ADDAw.w folded into 0461 */
/* 0461 0020028F 8000033C MSUBw.w     */     vf10.w = vf10.w + vf10.w - 1.0f;  // folds 0460..0461
/* 0462 00206B10 8000033C MAXx.w      */     vf12.w = max(vf13.w, 0.0f);
/* 0463 002D683F 8000033C             */     /* ADDAw.w folded into 0464 */
/* 0464 0020038F 8000033C MSUBw.w     */     vf14.w = vf13.w + vf13.w - 1.0f;  // folds 0463..0464
/* 0465 00204A57 8024537D MINIw.w     */     vf09.w = min(vf09.w, 1.0f);
/*                        SQI.w       */     mem[vi04++].w = vf10.w;
/* 0466 00206317 8000033C MINIw.w     */     vf12.w = min(vf12.w, 1.0f);
/* 0467 01E031BF 8000033C             */     /* MULAw.xyzw folded into 0468 */
/* 0468 01E93A4B 8024737D MADDw.xyzw  */     vf09.xyzw = vf06.xyzw + colorBase_vf07.xyzw * vf09.w; // folds 0467..0468
/*                        SQI.w       */     mem[vi04++].w = vf14.w;
/* 0469 01EC3B0B 8000033C MADDw.xyzw  */     vf12.xyzw = ACC.xyzw + colorBase_vf07.xyzw * vf12.w;
/* 046A 002029BF 8000033C MULAw.w     */     ACC.w = rampBias_vf05.w;
/* 046B 002810BC 12052802 MADDAx.w    */     ACC.w = ACC.w + vf02.w * vf08.x;
/*                        ISUBIU      */     vi05 = vi05 - 2;
/* 046C 002818BD 81E34B7D MADDAy.w    */     ACC.w = ACC.w + vf03.w * vf08.y;
/*                        SQI.xyzw    */     mem[vi03++].xyzw = vf09.xyzw;
/* 046D 0028228A 52002FEE MADDz.w     */     vf10.w = ACC.w + vf04.w * vf08.z;
/* 046E 002029BF 81E3637D MULAw.w     */     ACC.w = rampBias_vf05.w;          // delay slot of 046D
/*                        SQI.xyzw    */     mem[vi03++].xyzw = vf12.xyzw;
/*                        IBNE        */   } while (vi05 != 0);
/* 0470 000002FF 8000033C             */   /* nop */                           // delay slot of 046F
/* 046F 000002FF 50003009 IBEQ        */   if (vi06 != 0) {
/* 0471 00205250 8000033C MAXx.w      */     vf09.w = max(vf10.w, 0.0f);
/* 0472 002A503F 8000033C             */     /* ADDAw.w folded into 0473 */
/* 0473 0020028F 8000033C MSUBw.w     */     vf10.w = vf10.w + vf10.w - 1.0f;  // folds 0472..0473
/* 0474 00204A57 8000033C MINIw.w     */     vf09.w = min(vf09.w, 1.0f);
/* 0475 01E031BF 8000033C             */     /* MULAw.xyzw folded into 0476 */
/* 0476 01E93A4B 8024537D MADDw.xyzw  */     vf09.xyzw = vf06.xyzw + colorBase_vf07.xyzw * vf09.w; // folds 0475..0476
/*                        SQI.w       */     mem[vi04++].w = vf10.w;
/* 0478 000002FF 81E34B7D SQI.xyzw    */     mem[vi03++].xyzw = vf09.xyzw;     // delay slot of 0477
/*                                    */   }
/* 0477 000002FF 48007800 JR          */   return;
/* 047A 000002FF 8000033C             */   /* nop */                           // delay slot of 0479
/* 0479 000002FF 48007800 JR          */   return;
subroutine(drawFront2, mac, vf01, vf18, vf26, vf27, vf28, vf29, vf30, vf31, vi14); /* 047B, from 0429 */
L_047B:  /* from 0427 */
/* 047B 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 047C 000002FF 10010010 IADDIU      */   vi01 = 16;
/* 047D 000002FF 0B0F77FF ISW.x       */   mem_w[vi14 - 1].x = vi15;
/* 047E 000002FF 120E7001 ISUBIU      */   vi14 = vi14 - 1;
/* 047F 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 16;
/* 0480 000002FF 8000033C             */   /* nop */
/* 0482 000002FF 8000033C             */   /* nop */                           // delay slot of 0481
/* 0481 000002FF 52000807 IBNE        */   if (vi01 != 0) goto L_0489;
/* 0484 000002FF 8000033C             */   /* nop */                           // delay slot of 0483
/* 0483 000002FF 420F004D BAL         */   transformVertices();
L_0485:  /* from 0434 */
/* 0486 000002FF 8000033C             */   /* nop */                           // delay slot of 0485
/* 0485 000002FF 420F0696 BAL         */   packetOne();
/* 0488 000002FF 100F0499 IADDIU      */   vi15 = 1177;                        // delay slot of 0487
/* 0487 000002FF 400005AD B           */   goto L_0235;                        // call emitTriangles through vi15, returns to 0499
L_0489:  /* from 0481 */
/* 0489 000002FF 019003CE LQ.xy       */   vf16.xy = uvMatrix0.xy;
/* 048A 000002FF 019103CF LQ.xy       */   uvMatrix1_vf17.xy = uvMatrix1.xy;
/* 048C 000002FF 019203D0 LQ.xy       */   uvMatrix2_vf18.xy = uvMatrix2.xy;   // delay slot of 048B
/* 048B 000002FF 420F006A BAL         */   transformVerticesUv();
L_048D:  /* from 0438 */
/* 048E 000002FF 8000033C             */   /* nop */                           // delay slot of 048D
/* 048D 000002FF 420F069B BAL         */   packetThree();
/* 0490 000002FF 8000033C             */   /* nop */                           // delay slot of 048F
/* 048F 000002FF 420F05FA BAL         */   emitTexTriangles();
/* 0491 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 0492 000002FF 10410000 IADDIU      */   vi01 = 4096;
/* 0493 000002FF 01EF03ED LQ.xyzw     */   drawColor_vf15.xyzw = drawColor.xyzw;
/* 0494 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 4096;
/* 0495 000002FF 1003043A IADDIU      */   vi03 = 1082;
/* 0496 000002FF 100D028A IADDIU      */   vi13 = 650;
/* 0498 000002FF 100F0499 IADDIU      */   vi15 = 1177;                        // delay slot of 0497
/* 0497 000002FF 52000E52 IBNE        */   if (vi01 != 0) goto L_02EA;
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
/* 0499 000002FF 090F7000 ILW.x       */   vi15 = mem_w[vi14].x;
/* 049B 000002FF 100E7001 IADDIU      */   vi14 = vi14 + 1;                    // delay slot of 049A
/* 049A 000002FF 4000043F B           */   goto L_00DA;
subroutine(kick, nothing);                                             /* 049C, from 026F 02CD 04A4 */
L_049C:  /* from 0288, 02E8 */
/* 049C 000002FF 084403FF ILW.z       */   cursors_vi04 = cursors.z;
/* 049D 000002FF 082503FF ILW.w       */   cursors_vi05 = cursors.w;
/* 049E 000002FF 800026FC XGKICK      */   xgkick(cursors_vi04);
/* 049F 000002FF 0A2403FF ISW.w       */   cursors.w = cursors_vi04;
/* 04A0 000002FF 0A4503FF ISW.z       */   cursors.z = cursors_vi05;
/* 04A1 000002FF 0A8503FF ISW.y       */   cursors.y = cursors_vi05;
L_04A2:  /* from 04F1, 0520, 05CE */
/* 04A3 000002FF 8000033C             */   /* nop */                           // delay slot of 04A2
/* 04A2 000002FF 48007800 JR          */   goto_micro(vi15);
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_04A4:  /* from 001C, 0630, 0665, 06AB, 06E9, 0739, 073D, 0757, 075A */
/* 04A5 000002FF 8000033C             */   /* nop */                           // delay slot of 04A4
/* 04A4 000002FF 420F07F7 BAL         */   kick();
/* a jump at 008D 00E4 0171 0186 019B 01AF and 21 more can reach here */
L_04A6:  /* from 03A2, 03F5, 0401 */
/* 04A6 400002FF 100E0400 IADDIU      */   vi14 = 1024;
/* 04A7 000002FF 8000033C             */   /* nop */
/*                                    */   return;  /* [E] at 04A6 */
subroutine(clipReject, clip);                                          /* 04A8, from 025E */
/* 04A8 000002FF 26FDF7DF FCOR        */   vi01 = (clip | 0xfdf7df) == 0xffffff;
/* 04A9 000002FF 5201000B IBNE        */   take = (vi01 != 0);
/* 04AA 000002FF 26FEFBEF FCOR        */   vi01 = (clip | 0xfefbef) == 0xffffff; // delay slot of 04A9
/*                        IBNE        */   if (!take) {
/* 04AB 000002FF 52010009 IBNE        */     take = (vi01 != 0);
/* 04AC 000002FF 26FF7DF7 FCOR        */     vi01 = (clip | 0xff7df7) == 0xffffff; // delay slot of 04AB
/*                        IBNE        */     if (!take) {
/* 04AD 000002FF 52010007 IBNE        */       take = (vi01 != 0);
/* 04AE 000002FF 26FFBEFB FCOR        */       vi01 = (clip | 0xffbefb) == 0xffffff; // delay slot of 04AD
/*                        IBNE        */       if (!take) {
/* 04AF 000002FF 52010005 IBNE        */         take = (vi01 != 0);
/* 04B0 000002FF 26FFDF7D FCOR        */         vi01 = (clip | 0xffdf7d) == 0xffffff; // delay slot of 04AF
/*                        IBNE        */         if (!take) {
/* 04B2 000002FF 8000033C             */           /* nop */                   // delay slot of 04B1
/* 04B1 000002FF 52010003 IBNE        */           if (vi01 == 0) {
/* 04B4 000002FF 26FFEFBE FCOR        */             vi01 = (clip | 0xffefbe) == 0xffffff; // delay slot of 04B3
/*                                    */           }
/*                                    */         }
/*                                    */       }
/*                                    */     }
/*                                    */   }
/* 04B3 000002FF 48007800 JR          */   goto_micro(vi15);
/* 04B6 000002FF 10010001 IADDIU      */   vi01 = 1;                           // delay slot of 04B5
/* 04B5 000002FF 48007800 JR          */   goto_micro(vi15);                   // call away through vi01, returns to 0001
subroutine(packetSix, nothing);                                        /* 04B7, from 05EC */
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_04B7:  /* from 00FE */
/* 04B7 000002FF 01C32000 LQ.xyz      */   vf03.xyz = mem[vi04].xyz;
/* 04B8 000002FF 01C42001 LQ.xyz      */   vf04.xyz = mem[vi04 + 1].xyz;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_04B9:  /* from 012D */
/* 04B9 000002FF 0821038B ILW.w       */   primFlags_vi01 = primFlags;
/* 04BA 000002FF 088903FF ILW.y       */   cursors_vi09 = cursors.y;
/* 04BB 000002FF 01C203D4 LQ.xyz      */   vf02.xyz = mem[980].xyz;
/* 04BC 000002FF 01C52002 LQ.xyz      */   vf05.xyz = mem[vi04 + 2].xyz;
/* 04BD 000002FF 01C62003 LQ.xyz      */   vf06.xyz = mem[vi04 + 3].xyz;
/* 04BE 000002FF 01C72004 LQ.xyz      */   vf07.xyz = mem[vi04 + 4].xyz;
/* 04BF 000002FF 10030213 IADDIU      */   vi03 = 531;
/* 04C0 000002FF 80030875 IOR         */   vi01 = vi01 | 531;
/* 04C1 000002FF 12030021 ISUBIU      */   vi03 = -33;
/* 04C2 000002FF 80030874 IAND        */   vi01 = vi01 & 65503;
/* 04C3 000002FF 0A404806 ISW.z       */   mem_w[cursors_vi09 + 6].z = 0;
/* 04C4 000002FF 0B014806 ISW.x       */   mem_w[cursors_vi09 + 6].x = vi01;
/* 04C5 000002FF 0A21038C ISW.w       */   lastPrim = vi01;
/* 04C6 000002FF 10010006 IADDIU      */   vi01 = 6;
/* 04C7 000002FF 0B014800 ISW.x       */   mem_w[cursors_vi09].x = 6;
/* 04C8 000002FF 02C91000 SQ.yz       */   mem[cursors_vi09].yz = vf02.yz;
/* 04C9 000002FF 03C91801 SQ.xyz      */   mem[cursors_vi09 + 1].xyz = vf03.xyz;
/* 04CA 000002FF 03C92002 SQ.xyz      */   mem[cursors_vi09 + 2].xyz = vf04.xyz;
/* 04CB 000002FF 03C92803 SQ.xyz      */   mem[cursors_vi09 + 3].xyz = vf05.xyz;
/* 04CC 000002FF 03C93004 SQ.xyz      */   mem[cursors_vi09 + 4].xyz = vf06.xyz;
/* 04CD 000002FF 03C93805 SQ.xyz      */   mem[cursors_vi09 + 5].xyz = vf07.xyz;
/* 04CE 000002FF 10094807 IADDIU      */   cursors_vi09 = cursors_vi09 + 7;
/* 04D0 000002FF 0A8903FF ISW.y       */   cursors.y = cursors_vi09;           // delay slot of 04CF
/* 04CF 000002FF 48007800 JR          */   goto_micro(vi15);
subroutine(transformVertices, vf01, vf26, vf27, vf28, vf29);           /* 04D1, from 0218 0483 */
/* 04D1 01E0091B 08250383 MULw.xyzw   */   vf04.xyzw = vf01.xyzw;
/*                        ILW.w       */   outPosition_vi05 = outPosition;
/* 04D2 01E00A1B 81E22B7C MULw.xyzw   */   vf08.xyzw = vf01.xyzw;
/*                        LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi05++].xyzw;
/* 04D3 01E2D1BC 81E62B7C MULAx.xyzw  */   ACC.xyzw = vf26.xyzw * vf02.x;
/*                        LQI.xyzw    */   vf06.xyzw = mem[outPosition_vi05++].xyzw;
/* 04D4 01E2D8BD 01EF0397 MADDAy.xyzw */   ACC.xyzw = ACC.xyzw + vf27.xyzw * vf02.y;
/*                        LQ.xyzw     */   fogParams_vf15.xyzw = fogParams.xyzw;
/* 04D5 01E2E0BE 08220381 MADDAz.xyzw */   ACC.xyzw = ACC.xyzw + vf28.xyzw * vf02.z;
/*                        ILW.w       */   vertexCount_vi02 = vertexCount;
/* 04D6 01E0EA8B 81E22B7C MADDw.xyzw  */   vf10.xyzw = ACC.xyzw + vf29.xyzw;
/*                        LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi05++].xyzw;
/* 04D7 01E6D1BC 100402C0 IADDIU      */   vi04 = 704;                         // MULAx.xyzw folded into 04DA
/* 04D8 01E6D8BD 80247B3C MOVE.w      */   vf04.w = fogParams_vf15.w;          // MADDAy.xyzw folded into 04DA
/* 04D9 01E6E0BE 80287B3C MOVE.w      */   vf08.w = fogParams_vf15.w;          // MADDAz.xyzw folded into 04DA
/* 04DA 01E0E9CB 81EA03BC MADDw.xyzw  */   vf07.xyzw = vf26.xyzw * vf06.x + vf27.xyzw * vf06.y + vf28.xyzw * vf06.z + vf29.xyzw; // folds 04D7..04DA
/*                        DIV         */   Q = 1.0f / vf10.w;
/* 04DB 01EF01BE 10060350 IADDIU      */   vi06 = 848;                         // MULAz.xyzw folded into 04DC
/* 04DC 01E02161 800003BF MADDq.xyzw  */   vf05.xyzw = vf00.xyzw * fogParams_vf15.z + vf04.xyzw * Q; // folds 04DB..04DC
/*                        WAITQ       */   /* WAITQ */
/* 04DD 01E2D1BC 81E703BC MULAx.xyzw  */   ACC.xyzw = vf26.xyzw * vf02.x;
/*                        DIV         */   Q = 1.0f / vf07.w;
/* 04DE 01E2D8BD 10080001 MADDAy.xyzw */   ACC.xyzw = ACC.xyzw + vf27.xyzw * vf02.y;
/*                        IADDIU      */   vi08 = 1;
/* 04DF 002F2950 80081234 MAXx.w      */   vf05.w = max(vf05.w, fogParams_vf15.x);
/*                        IAND        */   vi08 = vertexCount_vi02 & 1;
/* 04E0 01E2E0BE 800810B1 MADDAz.xyzw */   ACC.xyzw = ACC.xyzw + vf28.xyzw * vf02.z;
/*                        ISUB        */   vi02 = vi02 - vi08;
  /* loop 04E1..04F0: vi02 -= 2 per iteration (04EC) */
/*                                    */   do {
/* 04E1 01E0E8CB 81E62B7C MADDw.xyzw  */     vf03.xyzw = ACC.xyzw + vf29.xyzw;
/*                        LQI.xyzw    */     vf06.xyzw = mem[outPosition_vi05++].xyzw;
/* 04E2 01EF01BE 81E4537D MULAz.xyzw  */     ACC.xyzw = vf00.xyzw * fogParams_vf15.z;
/*                        SQI.xyzw    */     mem[vi04++].xyzw = vf10.xyzw;
/* 04E3 002F2955 81E43B7D MINIy.w     */     vf05.w = min(vf05.w, fogParams_vf15.y);
/*                        SQI.xyzw    */     mem[vi04++].xyzw = vf07.xyzw;
/* 04E4 01E04261 800003BF MADDq.xyzw  */     vf09.xyzw = ACC.xyzw + vf08.xyzw * Q;
/*                        WAITQ       */     /* WAITQ */
/* 04E5 01E6D1BC 81E303BC MULAx.xyzw  */     ACC.xyzw = vf26.xyzw * vf06.x;
/*                        DIV         */     Q = 1.0f / vf03.w;
/* 04E6 01E6D8BD 81EA1B3C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf27.xyzw * vf06.y;
/*                        MOVE.xyzw   */     vf10.xyzw = vf03.xyzw;
/* 04E7 01E6E0BE 81E62B7D MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf28.xyzw * vf06.z;
/*                        SQI.xyzw    */     mem[vi06++].xyzw = vf05.xyzw;
/* 04E8 002F4A50 8000033C MAXx.w      */     vf09.w = max(vf09.w, fogParams_vf15.x);
/* 04E9 01E0E9CB 8000033C MADDw.xyzw  */     vf07.xyzw = ACC.xyzw + vf29.xyzw;
/* 04EA 01EF01BE 81E22B7C LQI.xyzw    */     vf02.xyzw = mem[outPosition_vi05++].xyzw; // MULAz.xyzw folded into 04EB
/* 04EB 01E02161 800003BF MADDq.xyzw  */     vf05.xyzw = vf00.xyzw * fogParams_vf15.z + vf04.xyzw * Q; // folds 04EA..04EB
/*                        WAITQ       */     /* WAITQ */
/* 04EC 002F4A55 12021002 MINIy.w     */     vf09.w = min(vf09.w, fogParams_vf15.y);
/*                        ISUBIU      */     vi02 = vi02 - 2;
/* 04ED 01E2D1BC 81E703BC MULAx.xyzw  */     ACC.xyzw = vf26.xyzw * vf02.x;
/*                        DIV         */     Q = 1.0f / vf07.w;
/* 04EE 01E2D8BD 8000033C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf27.xyzw * vf02.y;
/* 04EF 002F2950 520017F1 MAXx.w      */     vf05.w = max(vf05.w, fogParams_vf15.x);
/* 04F0 01E2E0BE 81E64B7D MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf28.xyzw * vf02.z; // delay slot of 04EF
/*                        SQI.xyzw    */     mem[vi06++].xyzw = vf09.xyzw;
/*                        IBNE        */   } while (vi02 != 0);
/* 04F2 000002FF 8000033C             */   /* nop */                           // delay slot of 04F1
/* 04F1 000002FF 500047B0 IBEQ        */   if (vi08 == 0) goto L_04A2;
/* 04F3 002F2955 81E4537D MINIy.w     */   vf05.w = min(vf05.w, fogParams_vf15.y);
/*                        SQI.xyzw    */   mem[vi04++].xyzw = vf10.xyzw;
/* 04F5 000002FF 81E62B7D SQI.xyzw    */   mem[vi06++].xyzw = vf05.xyzw;       // delay slot of 04F4
/* 04F4 000002FF 48007800 JR          */   return;
subroutine(transformVerticesUv, vf01, vf16, vf17, vf18, vf26, vf27, vf28, vf29); /* 04F6, from 048B */
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_04F6:  /* from 0222 */
/* 04F6 000002FF 08240383 ILW.w       */   outPosition_vi04 = outPosition;
/* 04F7 01E0091B 81E2237C MULw.xyzw   */   vf04.xyzw = vf01.xyzw;
/*                        LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi04++].xyzw;
/* 04F8 01E00A1B 08270385 MULw.xyzw   */   vf08.xyzw = vf01.xyzw;
/*                        ILW.w       */   srcTexcoords_vi07 = srcTexcoords;
/* 04F9 01E2D1BC 81843B7C LQI.xy      */   vf04.xy = mem[srcTexcoords_vi07++].xy; // MULAx.xyzw folded into 04FC
/* 04FA 01E2D8BD 01EF0397 LQ.xyzw     */   fogParams_vf15.xyzw = fogParams.xyzw; // MADDAy.xyzw folded into 04FC
/* 04FB 01E2E0BE 08220381 ILW.w       */   vertexCount_vi02 = vertexCount;     // MADDAz.xyzw folded into 04FC
/* 04FC 01E0E8CB 81E6237C MADDw.xyzw  */   vf03.xyzw = vf26.xyzw * vf02.x + vf27.xyzw * vf02.y + vf28.xyzw * vf02.z + vf29.xyzw; // folds 04F9..04FC
/*                        LQI.xyzw    */   vf06.xyzw = mem[outPosition_vi04++].xyzw;
/* 04FD 018481BC 8000033C             */   /* MULAx.xy folded into 04FF */
/* 04FE 018488BD 81E303BC DIV         */   Q = 1.0f / vf03.w;                  // MADDAy.xy folded into 04FF
/* 04FF 0180910B 100502C0 MADDw.xy    */   vf04.xy = vf16.xy * vf04.x + vf17.xy * vf04.y + uvMatrix2_vf18.xy; // folds 04FD..04FF
/*                        IADDIU      */   vi05 = 704;                         // position, the base
/* 0500 01E6D1BC 10060350 IADDIU      */   vi06 = 848;                         // MULAx.xyzw folded into 0503
/* 0501 01E6D8BD 80247B3C MOVE.w      */   vf04.w = fogParams_vf15.w;          // MADDAy.xyzw folded into 0503
/* 0502 01E6E0BE 81883B7C LQI.xy      */   vf08.xy = mem[srcTexcoords_vi07++].xy; // MADDAz.xyzw folded into 0503
/* 0503 01E0E9CB 80287B3C MADDw.xyzw  */   vf07.xyzw = vf26.xyzw * vf06.x + vf27.xyzw * vf06.y + vf28.xyzw * vf06.z + vf29.xyzw; // folds 0500..0503
/*                        MOVE.w      */   vf08.w = fogParams_vf15.w;
/* 0504 01EF01BE 81E2237C LQI.xyzw    */   vf02.xyzw = mem[outPosition_vi04++].xyzw; // MULAz.xyzw folded into 0505
/* 0505 01E02161 800003BF MADDq.xyzw  */   vf05.xyzw = vf00.xyzw * fogParams_vf15.z + vf04.xyzw * Q; // folds 0504..0505
/*                        WAITQ       */   /* WAITQ */
/* 0506 018881BC 10080001 IADDIU      */   vi08 = 1;                           // MULAx.xy folded into 0508
/* 0507 018888BD 81E703BC DIV         */   Q = 1.0f / vf07.w;                  // MADDAy.xy folded into 0508
/* 0508 0180920B 80081234 MADDw.xy    */   vf08.xy = vf16.xy * vf08.x + vf17.xy * vf08.y + uvMatrix2_vf18.xy; // folds 0506..0508
/*                        IAND        */   vi08 = vertexCount_vi02 & 1;
/* 0509 01E2D1BC 800810B1 MULAx.xyzw  */   ACC.xyzw = vf26.xyzw * vf02.x;
/*                        ISUB        */   vi02 = vi02 - vi08;
  /* loop 050A..051F: vi02 -= 2 per iteration (0516) */
/*                                    */   do {
/* 050A 01E2D8BD 81E51B7D MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf27.xyzw * vf02.y;
/*                        SQI.xyzw    */     mem[vi05++].xyzw = vf03.xyzw;
/* 050B 01E2E0BE 81E53B7D MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf28.xyzw * vf02.z;
/*                        SQI.xyzw    */     mem[vi05++].xyzw = vf07.xyzw;
/* 050C 01E0E8CB 81843B7C MADDw.xyzw  */     vf03.xyzw = ACC.xyzw + vf29.xyzw;
/*                        LQI.xy      */     vf04.xy = mem[srcTexcoords_vi07++].xy;
/* 050D 01EF01BE 8000033C             */     /* MULAz.xyzw folded into 050E */
/* 050E 01E04261 800003BF MADDq.xyzw  */     vf09.xyzw = vf00.xyzw * fogParams_vf15.z + vf08.xyzw * Q; // folds 050D..050E
/*                        WAITQ       */     /* WAITQ */
/* 050F 002F2950 81E6237C MAXx.w      */     vf05.w = max(vf05.w, fogParams_vf15.x);
/*                        LQI.xyzw    */     vf06.xyzw = mem[outPosition_vi04++].xyzw;
/* 0510 018481BC 81E303BC DIV         */     Q = 1.0f / vf03.w;                // MULAx.xy folded into 0512
/* 0511 018488BD 8000033C             */     /* MADDAy.xy folded into 0512 */
/* 0512 0180910B 8000033C MADDw.xy    */     vf04.xy = vf16.xy * vf04.x + vf17.xy * vf04.y + uvMatrix2_vf18.xy; // folds 0510..0512
/* 0513 002F2955 8000033C MINIy.w     */     vf05.w = min(vf05.w, fogParams_vf15.y);
/* 0514 01E6D1BC 8000033C MULAx.xyzw  */     ACC.xyzw = vf26.xyzw * vf06.x;
/* 0515 01E6D8BD 8000033C MADDAy.xyzw */     ACC.xyzw = ACC.xyzw + vf27.xyzw * vf06.y;
/* 0516 01E6E0BE 12021002 MADDAz.xyzw */     ACC.xyzw = ACC.xyzw + vf28.xyzw * vf06.z;
/*                        ISUBIU      */     vi02 = vi02 - 2;
/* 0517 002F4A50 81E62B7D MAXx.w      */     vf09.w = max(vf09.w, fogParams_vf15.x);
/*                        SQI.xyzw    */     mem[vi06++].xyzw = vf05.xyzw;
/* 0518 01E0E9CB 81883B7C MADDw.xyzw  */     vf07.xyzw = ACC.xyzw + vf29.xyzw;
/*                        LQI.xy      */     vf08.xy = mem[srcTexcoords_vi07++].xy;
/* 0519 01EF01BE 8000033C             */     /* MULAz.xyzw folded into 051A */
/* 051A 01E02161 800003BF MADDq.xyzw  */     vf05.xyzw = vf00.xyzw * fogParams_vf15.z + vf04.xyzw * Q; // folds 0519..051A
/*                        WAITQ       */     /* WAITQ */
/* 051B 002F4A55 81E2237C MINIy.w     */     vf09.w = min(vf09.w, fogParams_vf15.y);
/*                        LQI.xyzw    */     vf02.xyzw = mem[outPosition_vi04++].xyzw;
/* 051C 018881BC 8000033C MULAx.xy    */     ACC.xy = vf16.xy * vf08.x;
/* 051D 018888BD 81E703BC MADDAy.xy   */     ACC.xy = ACC.xy + vf17.xy * vf08.y;
/*                        DIV         */     Q = 1.0f / vf07.w;
/* 051E 0180920B 520017EB MADDw.xy    */     vf08.xy = ACC.xy + uvMatrix2_vf18.xy;
/* 051F 01E2D1BC 81E64B7D MULAx.xyzw  */     ACC.xyzw = vf26.xyzw * vf02.x;    // delay slot of 051E
/*                        SQI.xyzw    */     mem[vi06++].xyzw = vf09.xyzw;
/*                        IBNE        */   } while (vi02 != 0);
/* 0520 002F2950 50004781 MAXx.w      */   vf05.w = max(vf05.w, fogParams_vf15.x);
/* 0521 000002FF 8000033C             */   /* nop */                           // delay slot of 0520
/*                        IBEQ        */   if (vi08 == 0) goto L_04A2;
/* 0522 002F2955 81E51B7D MINIy.w     */   vf05.w = min(vf05.w, fogParams_vf15.y);
/*                        SQI.xyzw    */   mem[vi05++].xyzw = vf03.xyzw;
/* 0524 000002FF 81E62B7D SQI.xyzw    */   mem[vi06++].xyzw = vf05.xyzw;       // delay slot of 0523
/* 0523 000002FF 48007800 JR          */   return;
/* 0525 000002FF 01E422C0 LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw;
/* 0526 000002FF 01E52350 LQ.xyzw     */   texcoord_vf05.xyzw = texcoord[vi04].xyzw;
/* 0527 000002FF 01E62320 LQ.xyzw     */   vertexColor_vf06.xyzw = vertexColor[vi04].xyzw;
/* 0528 000002FF 01E72AC0 LQ.xyzw     */   position_vf07.xyzw = position[vi05].xyzw;
/* 0529 000002FF 01E82B50 LQ.xyzw     */   texcoord_vf08.xyzw = texcoord[vi05].xyzw;
/* 052A 000002FF 01E92B20 LQ.xyzw     */   vertexColor_vf09.xyzw = vertexColor[vi05].xyzw;
/* 052B 000002FF 01EA32C0 LQ.xyzw     */   position_vf10.xyzw = position[vi06].xyzw;
/* 052C 000002FF 01EB3350 LQ.xyzw     */   texcoord_vf11.xyzw = texcoord[vi06].xyzw;
/* 052E 000002FF 01EC3320 LQ.xyzw     */   vertexColor_vf12.xyzw = vertexColor[vi06].xyzw; // delay slot of 052D
/* 052D 000002FF 4000000D B           */   goto L_053B;
L_052F:  /* from 0275, 02D3 */
/* 052F 000002FF 09046000 ILW.x       */   vi04 = mem_w[retryHead_vi12].x;
/* 0530 000002FF 08856000 ILW.y       */   vi05 = mem_w[retryHead_vi12].y;
/* 0531 000002FF 08466000 ILW.z       */   vi06 = mem_w[retryHead_vi12].z;
/* 0532 000002FF 01E422C0 LQ.xyzw     */   position_vf04.xyzw = position[vi04].xyzw;
/* 0533 000002FF 01E52350 LQ.xyzw     */   texcoord_vf05.xyzw = texcoord[vi04].xyzw;
/* 0534 000002FF 01E72AC0 LQ.xyzw     */   position_vf07.xyzw = position[vi05].xyzw;
/* 0535 000002FF 01E82B50 LQ.xyzw     */   texcoord_vf08.xyzw = texcoord[vi05].xyzw;
/* 0536 000002FF 01EA32C0 LQ.xyzw     */   position_vf10.xyzw = position[vi06].xyzw;
/* 0537 0184295B 01EB3350 MULw.xy     */   texcoord_vf05.xy = texcoord_vf05.xy * position_vf04.w;
/*                        LQ.xyzw     */   texcoord_vf11.xyzw = texcoord[vi06].xyzw;
/* 0538 000002FF 01E62320 LQ.xyzw     */   vertexColor_vf06.xyzw = vertexColor[vi04].xyzw;
/* 0539 0187421B 01E92B20 MULw.xy     */   texcoord_vf08.xy = texcoord_vf08.xy * position_vf07.w;
/*                        LQ.xyzw     */   vertexColor_vf09.xyzw = vertexColor[vi05].xyzw;
/* 053A 018A5ADB 01EC3320 MULw.xy     */   texcoord_vf11.xy = texcoord_vf11.xy * position_vf10.w;
/*                        LQ.xyzw     */   vertexColor_vf12.xyzw = vertexColor[vi06].xyzw;
L_053B:  /* from 052D */
/* 053B 01C421FF 084803FF CLIP.xyz    */   clip = (clip << 6) | clip_bits(position_vf04.xyz, position_vf04.w);
/*                        ILW.z       */   cursors_vi08 = cursors.z;
/* 053C 000002FF 01EF0397 LQ.xyzw     */   fogParams_vf15.xyzw = fogParams.xyzw;
/* 053D 000002FF 10084010 IADDIU      */   cursors_vi08 = cursors_vi08 + 16;
/* 053E 000002FF 10094048 IADDIU      */   vi09 = cursors_vi08 + 72;
/* 053F 01C739FF 0A4803FD CLIP.xyz    */   clip = (clip << 6) | clip_bits(position_vf07.xyz, position_vf07.w);
/*                        ISW.z       */   clipBufB = cursors_vi08;
/* 0540 000002FF 24000010 FCAND       */   vi01 = (clip & 0x000010) != 0;
/* 0542 01CA51FF 0A8903FD CLIP.xyz    */   clip = (clip << 6) | clip_bits(position_vf10.xyz, position_vf10.w); // delay slot of 0541
/*                        ISW.y       */   clipBufA = vi09;
/* 0541 000002FF 50000802 IBEQ        */   if (vi01 != 0) {
/* 0543 002F0159 8000033C MULy.w      */     vf05.w = fogParams_vf15.y;
/*                                    */   }
/* 0544 000002FF 24000010 FCAND       */   vi01 = (clip & 0x000010) != 0;
/* 0546 000002FF 03E92000 SQ.xyzw     */   mem[vi09].xyzw = position_vf04.xyzw; // delay slot of 0545
/* 0545 000002FF 50000802 IBEQ        */   if (vi01 != 0) {
/* 0547 002F0219 8000033C MULy.w      */     vf08.w = fogParams_vf15.y;
/*                                    */   }
/* 0548 000002FF 24000010 FCAND       */   vi01 = (clip & 0x000010) != 0;
/* 054A 000002FF 03E93001 SQ.xyzw     */   mem[vi09 + 1].xyzw = vertexColor_vf06.xyzw; // delay slot of 0549
/* 0549 000002FF 50000802 IBEQ        */   if (vi01 != 0) {
/* 054B 002F02D9 8000033C MULy.w      */     vf11.w = fogParams_vf15.y;
/*                                    */   }
/* 054C 000002FF 03E92802 SQ.xyzw     */   mem[vi09 + 2].xyzw = vf05.xyzw;
/* 054D 000002FF 03E93803 SQ.xyzw     */   mem[vi09 + 3].xyzw = position_vf07.xyzw;
/* 054E 000002FF 03E94804 SQ.xyzw     */   mem[vi09 + 4].xyzw = vertexColor_vf09.xyzw;
/* 054F 000002FF 03E94005 SQ.xyzw     */   mem[vi09 + 5].xyzw = vf08.xyzw;
/* 0550 000002FF 03E95006 SQ.xyzw     */   mem[vi09 + 6].xyzw = position_vf10.xyzw;
/* 0551 000002FF 03E96007 SQ.xyzw     */   mem[vi09 + 7].xyzw = vertexColor_vf12.xyzw;
/* 0552 000002FF 03E95808 SQ.xyzw     */   mem[vi09 + 8].xyzw = vf11.xyzw;
/* 0553 000002FF 03E92009 SQ.xyzw     */   mem[vi09 + 9].xyzw = position_vf04.xyzw;
/* 0554 000002FF 03E9300A SQ.xyzw     */   mem[vi09 + 10].xyzw = vertexColor_vf06.xyzw;
/* 0555 000002FF 03E9280B SQ.xyzw     */   mem[vi09 + 11].xyzw = vf05.xyzw;
/* 0556 000002FF 100A0003 IADDIU      */   vi10 = 3;
/* 0557 000002FF 100B0020 IADDIU      */   vi11 = 32;
/* 0558 01C00343 10060400 ADDw.xyz    */   vf13.xyz = 1.0f;
/*                        IADDIU      */   vi06 = 1024;
/* 0559 00200340 42030014 ADDx.w      */   vf13.w = 1.0f;
/* 055A 000002FF 10070010 IADDIU      */   vi07 = 16;                          // delay slot of 0559
/*                        BAL         */   clipPlane();
/* 055B 01C00347 10260000 SUBw.xyz    */   vf13.xyz = -1.0f;
/*                        IADDIU      */   vi06 = 2048;
/* 055C 00200344 42030011 SUBx.w      */   vf13.w = 1.0f;
/* 055D 000002FF 10070020 IADDIU      */   vi07 = 32;                          // delay slot of 055C
/*                        BAL         */   clipPlane();
/* 055E 000002FF 100B0080 IADDIU      */   vi11 = 128;
/* 055F 01C00343 10060040 ADDw.xyz    */   vf13.xyz = 1.0f;
/*                        IADDIU      */   vi06 = 64;
/* 0560 00200340 4203000D ADDx.w      */   vf13.w = 1.0f;
/* 0561 000002FF 10070001 IADDIU      */   vi07 = 1;                           // delay slot of 0560
/*                        BAL         */   clipPlane();
/* 0562 01C00347 10060080 SUBw.xyz    */   vf13.xyz = -1.0f;
/*                        IADDIU      */   vi06 = 128;
/* 0563 00200344 4203000A SUBx.w      */   vf13.w = 1.0f;
/* 0564 000002FF 10070002 IADDIU      */   vi07 = 2;                           // delay slot of 0563
/*                        BAL         */   clipPlane();
/* 0565 000002FF 100B0040 IADDIU      */   vi11 = 64;
/* 0566 01C00343 10060100 ADDw.xyz    */   vf13.xyz = 1.0f;
/*                        IADDIU      */   vi06 = 256;
/* 0567 00200340 42030006 ADDx.w      */   vf13.w = 1.0f;
/* 0568 000002FF 10070004 IADDIU      */   vi07 = 4;                           // delay slot of 0567
/*                        BAL         */   clipPlane();
/* 0569 01C00347 10060200 SUBw.xyz    */   vf13.xyz = -1.0f;
/*                        IADDIU      */   vi06 = 512;
/* 056A 00200344 42030003 SUBx.w      */   vf13.w = 1.0f;
/* 056B 000002FF 10070008 IADDIU      */   vi07 = 8;                           // delay slot of 056A
/*                        BAL         */   clipPlane();
/* 056D 000002FF 8000033C             */   /* nop */                           // delay slot of 056C
/* 056C 000002FF 48006800 JR          */   goto_micro(vi13);                   // call away through vi07, returns to 0008
subroutine(clipPlane, mac, vf13, vi06, vi10, vi11);                    /* 056E, from 0559 055C 0560 0563 0567 056A */
/* 056E 000002FF 084903FD ILW.z       */   clipBufB_vi09 = clipBufB;
/* 056F 000002FF 088803FD ILW.y       */   clipBufA_vi08 = clipBufA;
/* 0570 000002FF 800050B0 IADD        */   vi02 = vi10;
/* 0571 000002FF 800002B0 IADD        */   vi10 = 0;
/* 0572 000002FF 0A8903FD ISW.y       */   clipBufA = clipBufB_vi09;
/* 0573 000002FF 0A4803FD ISW.z       */   clipBufB = clipBufA_vi08;
/* 0574 000002FF 81E4437C LQI.xyzw    */   vf04.xyzw = mem[clipBufA_vi08++].xyzw;
/* 0575 000002FF 81E5437C LQI.xyzw    */   vf05.xyzw = mem[clipBufA_vi08++].xyzw;
/* 0576 000002FF 81E6437C LQI.xyzw    */   vf06.xyzw = mem[clipBufA_vi08++].xyzw;
L_0577:  /* from 05BD */
/* 0577 01C421FF 81E7233C CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, vf04.w);
/*                        MOVE.xyzw   */   vf07.xyzw = vf04.xyzw;
/* 0578 000002FF 81E4437C LQI.xyzw    */   vf04.xyzw = mem[clipBufA_vi08++].xyzw;
/* 0579 01C421FF 81E82B3C CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, vf04.w);
/*                        MOVE.xyzw   */   vf08.xyzw = vf05.xyzw;
/* 057A 000002FF 81E9333C MOVE.xyzw   */   vf09.xyzw = vf06.xyzw;
/* 057B 000002FF 81E5437C LQI.xyzw    */   vf05.xyzw = mem[clipBufA_vi08++].xyzw;
/* 057C 000002FF 81E6437C LQI.xyzw    */   vf06.xyzw = mem[clipBufA_vi08++].xyzw;
/* 057D 000002FF 38010000 FCGET       */   vi01 = clip & 0xfff;
/* 057E 01E7202C 80060934 SUB.xyzw    */   vf00.xyzw = vf04.xyzw - vf07.xyzw;
/*                        IAND        */   vi04 = vi01 & vi06;
/* 057F 000002FF 80070974 IAND        */   vi05 = vi01 & vi07;
/* 0581 000002FF 12021001 ISUBIU      */   vi02 = vi02 - 1;                    // delay slot of 0580
/* 0580 000002FF 52002008 IBNE        */   if (vi04 == 0) {
/* 0582 000002FF 81E93B7D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf07.xyzw;
/* 0583 000002FF 81E9437D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf08.xyzw;
/* 0584 000002FF 81E94B7D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf09.xyzw;
/* 0586 000002FF 800A5072 IADDI       */     vi10 = vi10 + 1;                  // delay slot of 0585
/* 0585 000002FF 50002837 IBEQ        */     if (vi05 == 0) goto L_05BD;
/* 0588 000002FF 8000033C             */     /* nop */                         // delay slot of 0587
/* 0587 000002FF 40000003 B           */   } else {
/* 058A 000002FF 8000033C             */     /* nop */                         // delay slot of 0589
/*                                    */   }
/* 0589 000002FF 52002833 IBNE        */   if (vi05 != 0) goto L_05BD;
/* 058B 000002FF 34015800 FMAND       */   vi01 = vi11 & (mac & 0xffff);
/* 058D 000002FF 8000033C             */   /* nop */                           // delay slot of 058C
/* 058C 000002FF 52000817 IBNE        */   if (vi01 != 0) goto L_05A4;
/* 058E 01E76BDB 10040030 MULw.xyzw   */   vf15.xyzw = vf13.xyzw * vf07.w;
/*                        IADDIU      */   vi04 = 48;
/* 058F 01E46B9B 80072134 MULw.xyzw   */   vf14.xyzw = vf13.xyzw * vf04.w;
/*                        IAND        */   vi04 = 48 & vi07;
/* 0590 01E722AC 1005000C SUB.xyzw    */   vf10.xyzw = vf04.xyzw - vf07.xyzw;
/*                        IADDIU      */   vi05 = 12;
/* 0591 01E82AEC 80072974 SUB.xyzw    */   vf11.xyzw = vf05.xyzw - vf08.xyzw;
/*                        IAND        */   vi05 = 12 & vi07;
/* 0592 01EF3BEC 8000033C SUB.xyzw    */   vf15.xyzw = vf07.xyzw - vf15.xyzw;
/* 0593 01EE23AC 8000033C SUB.xyzw    */   vf14.xyzw = vf04.xyzw - vf14.xyzw;
/* 0594 01E9332C 52002007 SUB.xyzw    */   vf12.xyzw = vf06.xyzw - vf09.xyzw;
/* 0595 01EF73AC 8000033C SUB.xyzw    */   vf14.xyzw = vf14.xyzw - vf15.xyzw;  // delay slot of 0594
/*                        IBNE        */   if (vi04 != 0) goto L_059C;
/* 0597 000002FF 8000033C             */   /* nop */                           // delay slot of 0596
/* 0596 000002FF 52002803 IBNE        */   if (vi05 == 0) {
/* 0599 000002FF 800E7BBC DIV         */     Q = vf15.x / vf14.x;              // delay slot of 0598
/*                                    */   }
/* 0598 000002FF 40000004 B           */   goto L_059D;
/* 059B 000002FF 80AE7BBC DIV         */   Q = vf15.y / vf14.y;                // delay slot of 059A
/* 059A 000002FF 40000002 B           */   goto L_059D;
L_059C:  /* from 0594 */
/* 059C 000002FF 814E7BBC DIV         */   Q = vf15.z / vf14.z;
L_059D:  /* from 0598, 059A */
/* 059D 010003A0 800003BF ADDq.x      */   vf14.x = Q;
/*                        WAITQ       */   /* WAITQ */
/* 059E 010E71FD 8000033C ABS.x       */   vf14.x = fabsf(vf14.x);
/* 059F 01E039BF 8000033C             */   /* MULAw.xyzw folded into 05A0 */
/* 05A0 01EE5288 8000033C MADDx.xyzw  */   vf10.xyzw = vf07.xyzw + vf10.xyzw * vf14.x; // folds 059F..05A0
/* 05A1 01E041BF 8000033C MULAw.xyzw  */   ACC.xyzw = vf08.xyzw;
/* 05A2 01EE5AC8 40000017 MADDx.xyzw  */   vf11.xyzw = ACC.xyzw + vf11.xyzw * vf14.x;
/* 05A3 01E049BF 800A5072 MULAw.xyzw  */   ACC.xyzw = vf09.xyzw;               // delay slot of 05A2
/*                        IADDI       */   vi10 = vi10 + 1;
/*                        B           */   goto L_05BA;
L_05A4:  /* from 058C */
/* 05A4 01E46BDB 10040030 MULw.xyzw   */   vf15.xyzw = vf13.xyzw * vf04.w;
/*                        IADDIU      */   vi04 = 48;
/* 05A5 01E76B9B 80072134 MULw.xyzw   */   vf14.xyzw = vf13.xyzw * vf07.w;
/*                        IAND        */   vi04 = 48 & vi07;
/* 05A6 01E43AAC 1005000C SUB.xyzw    */   vf10.xyzw = vf07.xyzw - vf04.xyzw;
/*                        IADDIU      */   vi05 = 12;
/* 05A7 01E542EC 80072974 SUB.xyzw    */   vf11.xyzw = vf08.xyzw - vf05.xyzw;
/*                        IAND        */   vi05 = 12 & vi07;
/* 05A8 01EF23EC 8000033C SUB.xyzw    */   vf15.xyzw = vf04.xyzw - vf15.xyzw;
/* 05A9 01EE3BAC 8000033C SUB.xyzw    */   vf14.xyzw = vf07.xyzw - vf14.xyzw;
/* 05AA 01E64B2C 52002007 SUB.xyzw    */   vf12.xyzw = vf09.xyzw - vf06.xyzw;
/* 05AB 01EF73AC 8000033C SUB.xyzw    */   vf14.xyzw = vf14.xyzw - vf15.xyzw;  // delay slot of 05AA
/*                        IBNE        */   if (vi04 != 0) goto L_05B2;
/* 05AD 000002FF 8000033C             */   /* nop */                           // delay slot of 05AC
/* 05AC 000002FF 52002803 IBNE        */   if (vi05 == 0) {
/* 05AF 000002FF 800E7BBC DIV         */     Q = vf15.x / vf14.x;              // delay slot of 05AE
/*                                    */   }
/* 05AE 000002FF 40000004 B           */   goto L_05B3;
/* 05B1 000002FF 80AE7BBC DIV         */   Q = vf15.y / vf14.y;                // delay slot of 05B0
/* 05B0 000002FF 40000002 B           */   goto L_05B3;
L_05B2:  /* from 05AA */
/* 05B2 000002FF 814E7BBC DIV         */   Q = vf15.z / vf14.z;
L_05B3:  /* from 05AE, 05B0 */
/* 05B3 010003A0 800003BF ADDq.x      */   vf14.x = Q;
/*                        WAITQ       */   /* WAITQ */
/* 05B4 010E71FD 8000033C ABS.x       */   vf14.x = fabsf(vf14.x);
/* 05B5 01E021BF 8000033C             */   /* MULAw.xyzw folded into 05B6 */
/* 05B6 01EE5288 8000033C MADDx.xyzw  */   vf10.xyzw = vf04.xyzw + vf10.xyzw * vf14.x; // folds 05B5..05B6
/* 05B7 01E029BF 8000033C             */   /* MULAw.xyzw folded into 05B8 */
/* 05B8 01EE5AC8 8000033C MADDx.xyzw  */   vf11.xyzw = vf05.xyzw + vf11.xyzw * vf14.x; // folds 05B7..05B8
/* 05B9 01E031BF 800A5072 MULAw.xyzw  */   ACC.xyzw = vf06.xyzw;
/*                        IADDI       */   vi10 = vi10 + 1;
L_05BA:  /* from 05A2 */
/* 05BA 01EE6308 81E9537D MADDx.xyzw  */   vf12.xyzw = ACC.xyzw + vf12.xyzw * vf14.x;
/*                        SQI.xyzw    */   mem[clipBufB_vi09++].xyzw = vf10.xyzw;
/* 05BB 000002FF 81E95B7D SQI.xyzw    */   mem[clipBufB_vi09++].xyzw = vf11.xyzw;
/* 05BC 000002FF 81E9637D SQI.xyzw    */   mem[clipBufB_vi09++].xyzw = vf12.xyzw;
L_05BD:  /* from 0585, 0589 */
/* 05BE 000002FF 8000033C             */   /* nop */                           // delay slot of 05BD
/* 05BD 000002FF 520207B9 IBNE        */   if (vi02 != 0) goto L_0577;
/* 05C0 000002FF 088803FD ILW.y       */   clipBufA_vi08 = clipBufA;           // delay slot of 05BF
/* 05BF 000002FF 50005008 IBEQ        */   if (vi10 != 0) {
/* 05C1 000002FF 81E4437C LQI.xyzw    */     vf04.xyzw = mem[clipBufA_vi08++].xyzw;
/* 05C2 000002FF 81E5437C LQI.xyzw    */     vf05.xyzw = mem[clipBufA_vi08++].xyzw;
/* 05C3 000002FF 81E6437C LQI.xyzw    */     vf06.xyzw = mem[clipBufA_vi08++].xyzw;
/* 05C4 000002FF 81E9237D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf04.xyzw;
/* 05C5 000002FF 81E92B7D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf05.xyzw;
/* 05C7 000002FF 81E9337D SQI.xyzw    */     mem[clipBufB_vi09++].xyzw = vf06.xyzw; // delay slot of 05C6
/*                                    */   }
/* 05C6 000002FF 48001800 JR          */   return;
/* 05C9 000002FF 8000033C             */   /* nop */                           // delay slot of 05C8
/* 05C8 000002FF 48007800 JR          */   goto_micro(vi15);
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_05CA:  /* from 0230 */
/* 05CA 000002FF 08260388 ILW.w       */   drawMode_vi06 = drawMode;
/* 05CB 000002FF 10010002 IADDIU      */   vi01 = 2;
/* 05CC 000002FF 80013074 IAND        */   vi01 = drawMode_vi06 & 2;
/* 05CD 000002FF 8000033C             */   /* nop */
/* 05CF 000002FF 8000033C             */   /* nop */                           // delay slot of 05CE
/* 05CE 000002FF 50000ED3 IBEQ        */   if (vi01 == 0) goto L_04A2;
/* 05D0 000002FF 08240387 ILW.w       */   drawFlags_vi04 = drawFlags;
/* 05D1 000002FF 08230384 ILW.w       */   srcNormals_vi03 = srcNormals;
/* 05D2 800002FF 3F000000 LOI         */   I = 0.5f;
/* 05D3 01C003E2 10210000 ADDi.xyz    */   vf15.xyz = 0.5f;
/*                        IADDIU      */   vi01 = 2048;
/* 05D4 000002FF 80012074 IAND        */   vi01 = drawFlags_vi04 & 2048;
/* 05D5 000002FF 01EE03CD LQ.xyzw     */   reflectColor_vf14.xyzw = reflectColor.xyzw;
/* 05D7 000002FF 01E303EF LQ.xyzw     */   drawTint_vf03.xyzw = drawTint.xyzw; // delay slot of 05D6
/* 05D6 000002FF 52000805 IBNE        */   if (vi01 == 0) {
/* 05D8 000002FF 01F003E4 LQ.xyzw     */     vf16.xyzw = viewNormal0.xyzw;
/* 05D9 000002FF 01F103E5 LQ.xyzw     */     vf17.xyzw = viewNormal1.xyzw;
/* 05DB 000002FF 01F203E6 LQ.xyzw     */     vf18.xyzw = viewNormal2.xyzw;     // delay slot of 05DA
/* 05DA 000002FF 40000004 B           */   } else {
/* 05DC 000002FF 01F003DC LQ.xyzw     */     vf16.xyzw = cameraNormal0.xyzw;
/* 05DD 000002FF 01F103DD LQ.xyzw     */     vf17.xyzw = cameraNormal1.xyzw;
/* 05DE 000002FF 01F203DE LQ.xyzw     */     vf18.xyzw = cameraNormal2.xyzw;
/*                                    */   }
/* 05DF 018F8418 81E21B7C MULx.xy     */   vf16.xy = vf16.xy * vf15.x;
/*                        LQI.xyzw    */   vf02.xyzw = mem[srcNormals_vi03++].xyzw;
/* 05E0 018F8C58 08220381 MULx.xy     */   vf17.xy = vf17.xy * vf15.x;
/*                        ILW.w       */   vertexCount_vi02 = vertexCount;
/* 05E1 018F9498 10050350 MULx.xy     */   vf18.xy = vf18.xy * vf15.x;
/*                        IADDIU      */   vi05 = 848;                         // texcoord, the base
/* 05E2 01E373AA 10040320 MUL.xyzw    */   reflectColor_vf14.xyzw = reflectColor_vf14.xyzw * drawTint_vf03.xyzw;
/*                        IADDIU      */   vi04 = 800;                         // vertexColor, the base
/* 05E3 018079BF 8000033C MULAw.xy    */   ACC.xy = vf15.xy;
  /* loop 05E4..05EA: vi02 -= 1 per iteration (05E5) */
/*                                    */   do {
/* 05E4 018280BC 00432800 MADDAx.xy   */     ACC.xy = ACC.xy + vf16.xy * vf02.x;
/*                        LQ.z        */     vf03.z = mem[vi05].z;
/* 05E5 018288BD 12021001 MADDAy.xy   */     ACC.xy = ACC.xy + vf17.xy * vf02.y;
/*                        ISUBIU      */     vertexCount_vi02 = vertexCount_vi02 - 1;
/* 05E6 8182910A 3F800000 MADDz.xy    */     vf04.xy = ACC.xy + vf18.xy * vf02.z;
/*                        LOI         */     I = 1.0f;
/* 05E7 000002FF 81E4737D SQI.xyzw    */     mem[vi04++].xyzw = reflectColor_vf14.xyzw;
/* 05E8 0183211A 81E21B7C MULz.xy     */     vf04.xy = vf04.xy * vf03.z;
/*                        LQI.xyzw    */     vf02.xyzw = mem[srcNormals_vi03++].xyzw;
/* 05EA 018079BF 8185237D MULAw.xy    */     ACC.xy = vf15.xy;                 // delay slot of 05E9
/*                        SQI.xy      */     mem[vi05++].xy = vf04.xy;
/* 05E9 000002FF 520017FA IBNE        */   } while (vi02 != 0);
/* 05EB 000002FF 80007A30 IADD        */   vi08 = vi15;
/* 05ED 000002FF 1004038D IADDIU      */   vi04 = 909;                         // delay slot of 05EC
/* 05EC 000002FF 420F06CA BAL         */   packetSix();
/* 05EE 000002FF 1001028A IADDIU      */   vi01 = 650;
/* 05F0 000002FF 800043F0 IADD        */   vi15 = vi08;                        // delay slot of 05EF
/* 05EF 000002FF 48000800 JR          */   goto L_028A;                        // call away through vi04, returns to 038D
/* 05F2 000002FF 0A8203FC ISW.y       */   spriteTop = vi02;                   // delay slot of 05F1
/* 05F1 000002FF 40000009 B           */   goto L_05FB;
/* 05F4 000002FF 0A8203FC ISW.y       */   spriteTop = vi02;                   // delay slot of 05F3
/* 05F3 000002FF 40000015 B           */   goto L_0609;
/* 05F6 000002FF 0A8203FC ISW.y       */   spriteTop = vi02;                   // delay slot of 05F5
/* 05F5 000002FF 4000003C B           */   goto L_0632;
/* 05F8 000002FF 0A8203FC ISW.y       */   spriteTop = vi02;                   // delay slot of 05F7
/* 05F7 000002FF 4000008A B           */   goto L_0682;
/* 05FA 000002FF 0A8203FC ISW.y       */   spriteTop = vi02;                   // delay slot of 05F9
/* 05F9 000002FF 400000B3 B           */   goto L_06AD;
L_05FB:  /* from 05F1 */
/* 05FB 000002FF 01FA03E0 LQ.xyzw     */   camera0_vf26.xyzw = camera0.xyzw;
/* 05FC 000002FF 01FB03E1 LQ.xyzw     */   camera1_vf27.xyzw = camera1.xyzw;
/* 05FD 000002FF 01FC03E2 LQ.xyzw     */   camera2_vf28.xyzw = camera2.xyzw;
/* 05FE 000002FF 01FD03E3 LQ.xyzw     */   vf29.xyzw = camera3.xyzw;
/* 05FF 019AD2BE 01FE03A6 LQ.xyzw     */   screenScale_vf30.xyzw = screenScale.xyzw; // MULA.xy folded into 0601
/* 0600 019BDABD 01F71000 LQ.xyzw     */   vf23.xyzw = mem[vi02].xyzw;         // MADDA.xy folded into 0601
/* 0601 019CE0A9 01F81001 MADD.xy     */   vf02.xy = camera0_vf26.xy * camera0_vf26.xy + camera1_vf27.xy * camera1_vf27.xy + camera2_vf28.xy * camera2_vf28.xy; // folds 05FF..0601
/*                        LQ.xyzw     */   vf24.xyzw = mem[vi02 + 1].xyzw;
/* 0602 000002FF 01F91002 LQ.xyzw     */   vf25.xyzw = mem[vi02 + 2].xyzw;
/* 0603 000002FF 802017BC ESQRT       */   P = sqrtf(vf02.y);
/* 0604 000002FF 800203BD SQRT        */   Q = sqrtf(fabsf(vf02.x));
/* 0605 010000A0 800007BF ADDq.x      */   vf02.x = Q;
/*                        WAITP       */   /* WAITP */
/* 0606 000002FF 8082067C MFP.y       */   vf02.y = P;
/* 0607 4182BDEA 8000033C MUL.xy      */   vf23.xy = vf23.xy * vf02.xy;
/* 0608 0182C62A 8000033C MUL.xy      */   vf24.xy = vf24.xy * vf02.xy;
/*                                    */   return;  /* [E] at 0607 */
L_0609:  /* from 05F3 */
/* 0609 01E00358 09031000 MULx.xyzw   */   vf13.xyzw = 0.0f;
/*                        ILW.x       */   vi03 = mem_w[vi02].x;
/* 060A 01E00318 01E21000 MULx.xyzw   */   vf12.xyzw = 0.0f;
/*                        LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 060B 0060BDD8 088C03FF MULx.zw     */   vf23.zw = 0.0f;
/*                        ILW.y       */   cursors_vi12 = cursors.y;
/* 060C 0060C618 09061001 MULx.zw     */   vf24.zw = 0.0f;
/*                        ILW.x       */   vi06 = mem_w[vi02 + 1].x;
/* 060D 0060CE58 10071002 MULx.zw     */   vf25.zw = 0.0f;
/*                        IADDIU      */   vi07 = vi02 + 2;
/* 060E 000002FF 80033A30 IADD        */   vi08 = vi07 + vi03;
/* 060F 000002FF 81EC137D SQI.xyzw    */   mem[cursors_vi12++].xyzw = vf02.xyzw;
/* 0610 000002FF 80034270 IADD        */   vi09 = vi08 + vi03;
/* 0611 000002FF 81F63B7C LQI.xyzw    */   vf22.xyzw = mem[vi07++].xyzw;
/* 0612 000002FF 80031930 IADD        */   vi04 = vi03 + vi03;
/* 0613 000002FF 80042130 IADD        */   vi04 = vi04 + vi04;
/* 0614 000002FF 80044AB0 IADD        */   vi10 = vi09 + vi04;
/* 0615 01F6D1BC 10050001 MULAx.xyzw  */   ACC.xyzw = vf26.xyzw * vf22.x;
/*                        IADDIU      */   vi05 = 1;
L_0616:  /* from 062D */
/* 0616 01F6D8BD 81ED437C MADDAy.xyzw */   ACC.xyzw = ACC.xyzw + vf27.xyzw * vf22.y;
/*                        LQI.xyzw    */   vf13.xyzw = mem[vi08++].xyzw;
/* 0617 01F6E0BE 8182537C MADDAz.xyzw */   ACC.xyzw = ACC.xyzw + vf28.xyzw * vf22.z;
/*                        LQI.xy      */   vf02.xy = mem[vi10++].xy;
/* 0618 01E0EBCB 8183537C MADDw.xyzw  */   vf15.xyzw = ACC.xyzw + vf29.xyzw;
/*                        LQI.xy      */   vf03.xy = mem[vi10++].xy;
/* 0619 000002FF 8184537C LQI.xy      */   vf04.xy = mem[vi10++].xy;
/* 061A 000002FF 8185537C LQI.xy      */   vf05.xy = mem[vi10++].xy;
/* 061B 000002FF 12031801 ISUBIU      */   vi03 = vi03 - 1;
/* 061C 01CF79FF 81EF03BC CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf15.xyz, vf15.w);
/*                        DIV         */   Q = 1.0f / vf15.w;
/* 061D 00601098 81E64B7C MULx.zw     */   vf02.zw = 0.0f;
/*                        LQI.xyzw    */   vf06.xyzw = mem[vi09++].xyzw;
/* 061E 006018D8 81E74B7C MULx.zw     */   vf03.zw = 0.0f;
/*                        LQI.xyzw    */   vf07.xyzw = mem[vi09++].xyzw;
/* 061F 00602118 81E84B7C MULx.zw     */   vf04.zw = 0.0f;
/*                        LQI.xyzw    */   vf08.xyzw = mem[vi09++].xyzw;
/* 0620 00602958 24000030 MULx.zw     */   vf05.zw = 0.0f;
/*                        FCAND       */   vi01 = (clip & 0x000030) != 0;
/* 0621 000002FF 81E94B7C LQI.xyzw    */   vf09.xyzw = mem[vi09++].xyzw;
/* 0623 000002FF 81F63B7C LQI.xyzw    */   vf22.xyzw = mem[vi07++].xyzw;       // delay slot of 0622
/* 0622 000002FF 5200080A IBNE        */   if (vi01 == 0) {
/* 0625 000002FF 8000033C             */     /* nop */                         // delay slot of 0624
/* 0624 000002FF 420F0042 BAL         */     spriteProject();
/* 0627 000002FF 03EC1003 SQ.xyzw     */     mem[cursors_vi12 + 3].xyzw = vf02.xyzw; // delay slot of 0626
/* 0626 000002FF 52001006 IBNE        */     if (vi02 == 0) {
/* 0628 000002FF 03EC1805 SQ.xyzw     */       mem[cursors_vi12 + 5].xyzw = vf03.xyzw;
/* 0629 000002FF 03EC2007 SQ.xyzw     */       mem[cursors_vi12 + 7].xyzw = vf04.xyzw;
/* 062A 000002FF 03EC2809 SQ.xyzw     */       mem[cursors_vi12 + 9].xyzw = vf05.xyzw;
/* 062B 000002FF 100C600A IADDIU      */       cursors_vi12 = cursors_vi12 + 10;
/* 062C 000002FF 10052801 IADDIU      */       vi05 = vi05 + 1;
/*                                    */     }
/*                                    */   }
/* 062E 01F6D1BC 088403FF MULAx.xyzw  */   ACC.xyzw = vf26.xyzw * vf22.x;      // delay slot of 062D
/*                        ILW.y       */   cursors_vi04 = cursors.y;
/* 062D 000002FF 52001FE8 IBNE        */   if (vi03 != 0) goto L_0616;
/* 062F 000002FF 11E52FFF IADDIU      */   vi05 = vi05 + 32767;
/* 0631 000002FF 0B052000 ISW.x       */   mem_w[cursors_vi04].x = vi05;       // delay slot of 0630
/* 0630 000002FF 40000673 B           */   goto L_04A4;
L_0632:  /* from 05F5 */
/* 0632 01E00358 09031000 MULx.xyzw   */   vf13.xyzw = 0.0f;
/*                        ILW.x       */   vi03 = mem_w[vi02].x;
/* 0633 01E00318 01E21000 MULx.xyzw   */   vf12.xyzw = 0.0f;
/*                        LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 0634 0060BDD8 088C03FF MULx.zw     */   vf23.zw = 0.0f;
/*                        ILW.y       */   cursors_vi12 = cursors.y;
/* 0635 0060C618 09061001 MULx.zw     */   vf24.zw = 0.0f;
/*                        ILW.x       */   vi06 = mem_w[vi02 + 1].x;
/* 0636 0060CE58 10071002 MULx.zw     */   vf25.zw = 0.0f;
/*                        IADDIU      */   vi07 = vi02 + 2;
/* 0637 000002FF 80033A30 IADD        */   vi08 = vi07 + vi03;
/* 0638 000002FF 81EC137D SQI.xyzw    */   mem[cursors_vi12++].xyzw = vf02.xyzw;
/* 0639 000002FF 80031930 IADD        */   vi04 = vi03 + vi03;
/* 063A 000002FF 80044270 IADD        */   vi09 = vi08 + vi04;
/* 063B 000002FF 81F63B7C LQI.xyzw    */   vf22.xyzw = mem[vi07++].xyzw;
/* 063C 000002FF 80042130 IADD        */   vi04 = vi04 + vi04;
/* 063D 000002FF 80044AB0 IADD        */   vi10 = vi09 + vi04;
/* 063E 000002FF 10050001 IADDIU      */   vi05 = 1;
/* 063F 000002FF 100B3200 IADDIU      */   vi11 = vi06 + 512;
L_0640:  /* from 0662 */
/* 0640 01F6D1BC 81ED437C LQI.xyzw    */   vf13.xyzw = mem[vi08++].xyzw;       // MULAx.xyzw folded into 0643
/* 0641 01F6D8BD 8182537C LQI.xy      */   vf02.xy = mem[vi10++].xy;           // MADDAy.xyzw folded into 0643
/* 0642 01F6E0BE 8183537C LQI.xy      */   vf03.xy = mem[vi10++].xy;           // MADDAz.xyzw folded into 0643
/* 0643 01E0EBCB 8184537C MADDw.xyzw  */   vf15.xyzw = vf26.xyzw * vf22.x + vf27.xyzw * vf22.y + vf28.xyzw * vf22.z + vf29.xyzw; // folds 0640..0643
/*                        LQI.xy      */   vf04.xy = mem[vi10++].xy;
/* 0644 000002FF 8185537C LQI.xy      */   vf05.xy = mem[vi10++].xy;
/* 0645 000002FF 12031801 ISUBIU      */   vi03 = vi03 - 1;
/* 0646 000002FF 81EE437C LQI.xyzw    */   vf14.xyzw = mem[vi08++].xyzw;
/* 0647 01CF79FF 81EF03BC CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf15.xyz, vf15.w);
/*                        DIV         */   Q = 1.0f / vf15.w;
/* 0648 00601098 81E64B7C MULx.zw     */   vf02.zw = 0.0f;
/*                        LQI.xyzw    */   vf06.xyzw = mem[vi09++].xyzw;
/* 0649 006018D8 81E74B7C MULx.zw     */   vf03.zw = 0.0f;
/*                        LQI.xyzw    */   vf07.xyzw = mem[vi09++].xyzw;
/* 064A 00602118 81E84B7C MULx.zw     */   vf04.zw = 0.0f;
/*                        LQI.xyzw    */   vf08.xyzw = mem[vi09++].xyzw;
/* 064B 00602958 24000030 MULx.zw     */   vf05.zw = 0.0f;
/*                        FCAND       */   vi01 = (clip & 0x000030) != 0;
/* 064C 000002FF 81E94B7C LQI.xyzw    */   vf09.xyzw = mem[vi09++].xyzw;
/* 064E 000002FF 81F63B7C LQI.xyzw    */   vf22.xyzw = mem[vi07++].xyzw;       // delay slot of 064D
/* 064D 000002FF 52000814 IBNE        */   if (vi01 == 0) {
/* 0650 000002FF 8000033C             */     /* nop */                         // delay slot of 064F
/* 064F 000002FF 420F0017 BAL         */     spriteProject();
/* 0652 000002FF 03EC1003 SQ.xyzw     */     mem[cursors_vi12 + 3].xyzw = vf02.xyzw; // delay slot of 0651
/* 0651 000002FF 52001010 IBNE        */     if (vi02 == 0) {
/* 0653 000002FF 03EC1805 SQ.xyzw     */       mem[cursors_vi12 + 5].xyzw = vf03.xyzw;
/* 0654 000002FF 03EC2007 SQ.xyzw     */       mem[cursors_vi12 + 7].xyzw = vf04.xyzw;
/* 0655 000002FF 03EC2809 SQ.xyzw     */       mem[cursors_vi12 + 9].xyzw = vf05.xyzw;
/* 0656 000002FF 0B0B600A ISW.x       */       mem_w[cursors_vi12 + 10].x = vi11;
/* 0657 000002FF 03EC700B SQ.xyzw     */       mem[cursors_vi12 + 11].xyzw = vf14.xyzw;
/* 0658 000002FF 03EC300C SQ.xyzw     */       mem[cursors_vi12 + 12].xyzw = vf06.xyzw;
/* 0659 000002FF 03EC100D SQ.xyzw     */       mem[cursors_vi12 + 13].xyzw = vf02.xyzw;
/* 065A 000002FF 03EC380E SQ.xyzw     */       mem[cursors_vi12 + 14].xyzw = vf07.xyzw;
/* 065B 000002FF 03EC180F SQ.xyzw     */       mem[cursors_vi12 + 15].xyzw = vf03.xyzw;
/* 065C 000002FF 03EC4010 SQ.xyzw     */       mem[cursors_vi12 + 16].xyzw = vf08.xyzw;
/* 065D 000002FF 03EC2011 SQ.xyzw     */       mem[cursors_vi12 + 17].xyzw = vf04.xyzw;
/* 065E 000002FF 03EC4812 SQ.xyzw     */       mem[cursors_vi12 + 18].xyzw = vf09.xyzw;
/* 065F 000002FF 03EC2813 SQ.xyzw     */       mem[cursors_vi12 + 19].xyzw = vf05.xyzw;
/* 0660 000002FF 100C6014 IADDIU      */       cursors_vi12 = cursors_vi12 + 20;
/* 0661 000002FF 10052802 IADDIU      */       vi05 = vi05 + 2;
/*                                    */     }
/*                                    */   }
/* 0663 000002FF 088403FF ILW.y       */   cursors_vi04 = cursors.y;           // delay slot of 0662
/* 0662 000002FF 52001FDD IBNE        */   if (vi03 != 0) goto L_0640;
/* 0664 000002FF 11E52FFF IADDIU      */   vi05 = vi05 + 32767;
/* 0666 000002FF 0B052000 ISW.x       */   mem_w[cursors_vi04].x = vi05;       // delay slot of 0665
/* 0665 000002FF 4000063E B           */   goto L_04A4;
subroutine(spriteProject, Q, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf13, vf15, vf23, vf24, vf25, vf30, vf31, vi06, vi12); /* 0667, from 0624 064F */
/* 0667 01D97B28 8000033C ADD.xyz     */   vf12.xyz = vf15.xyz + vf25.xyz;
/* 0668 01C0F2DC 800003BF MULq.xyz    */   vf11.xyz = vf30.xyz * Q;
/*                        WAITQ       */   /* WAITQ */
/* 0669 01C2B9BC 0B066000 ISW.x       */   mem_w[cursors_vi12].x = vi06;       // MULAx.xyz folded into 066B
/* 066A 01C2C0BD 03EC6801 SQ.xyzw     */   mem[cursors_vi12 + 1].xyzw = vf13.xyzw; // MADDAy.xyz folded into 066B
/* 066B 01C0608B 03EC3002 MADDw.xyz   */   vf02.xyz = vf23.xyz * vf02.x + vf24.xyz * vf02.y + vf12.xyz; // folds 0669..066B
/*                        SQ.xyzw     */   mem[cursors_vi12 + 2].xyzw = vf06.xyzw;
/* 066C 01C3B9BC 03EC3804 SQ.xyzw     */   mem[cursors_vi12 + 4].xyzw = vf07.xyzw; // MULAx.xyz folded into 066E
/* 066D 01C3C0BD 03EC4006 SQ.xyzw     */   mem[cursors_vi12 + 6].xyzw = vf08.xyzw; // MADDAy.xyz folded into 066E
/* 066E 01C060CB 03EC4808 MADDw.xyz   */   vf03.xyz = vf23.xyz * vf03.x + vf24.xyz * vf03.y + vf12.xyz; // folds 066C..066E
/*                        SQ.xyzw     */   mem[cursors_vi12 + 8].xyzw = vf09.xyzw;
/* 066F 01CF11FF 81E6333D CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf02.xyz, vf15.w);
/*                        MR32.xyzw   */   vf06.xyzw = mr32(vf06).xyzw;
/* 0670 01C4B9BC 81E73B3D MR32.xyzw   */   vf07.xyzw = mr32(vf07).xyzw;        // MULAx.xyz folded into 0672
/* 0671 01C4C0BD 81E8433D MR32.xyzw   */   vf08.xyzw = mr32(vf08).xyzw;        // MADDAy.xyz folded into 0672
/* 0672 01C0610B 1002000F MADDw.xyz   */   vf04.xyz = vf23.xyz * vf04.x + vf24.xyz * vf04.y + vf12.xyz; // folds 0670..0672
/*                        IADDIU      */   vi02 = 15;
/* 0673 01CF19FF 38010000 FCGET       */   vi01 = clip & 0xfff;
/*                        CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf03.xyz, vf15.w);
/* 0674 01C5B9BC 800110B4 IAND        */   vi02 = 15 & vi01;                   // MULAx.xyz folded into 0676
/* 0675 01C5C0BD 81E94B3D MR32.xyzw   */   vf09.xyzw = mr32(vf09).xyzw;        // MADDAy.xyz folded into 0676
/* 0676 01C0614B 81E6333D MADDw.xyz   */   vf05.xyz = vf23.xyz * vf05.x + vf24.xyz * vf05.y + vf12.xyz; // folds 0674..0676
/*                        MR32.xyzw   */   vf06.xyzw = mr32(vf06).xyzw;
/* 0677 01CF21FF 38010000 FCGET       */   vi01 = clip & 0xfff;
/*                        CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, vf15.w);
/* 0678 01C0F9BF 800110B4 IAND        */   vi02 = vi02 & vi01;                 // MULAw.xyz folded into 0679
/* 0679 01CB10A9 81E73B3D MADD.xyz    */   vf02.xyz = vf31.xyz + vf02.xyz * vf11.xyz; // folds 0678..0679
/*                        MR32.xyzw   */   vf07.xyzw = mr32(vf07).xyzw;
/* 067A 01CF29FF 81E8433D CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf05.xyz, vf15.w);
/*                        MR32.xyzw   */   vf08.xyzw = mr32(vf08).xyzw;
/* 067B 01CB18E9 38010000 MADD.xyz    */   vf03.xyz = ACC.xyz + vf03.xyz * vf11.xyz;
/*                        FCGET       */   vi01 = clip & 0xfff;
/* 067C 01CB2129 800110B4 MADD.xyz    */   vf04.xyz = ACC.xyz + vf04.xyz * vf11.xyz;
/*                        IAND        */   vi02 = vi02 & vi01;
/* 067D 01CB2969 81E94B3D MADD.xyz    */   vf05.xyz = ACC.xyz + vf05.xyz * vf11.xyz;
/*                        MR32.xyzw   */   vf09.xyzw = mr32(vf09).xyzw;
/* 067E 01C2117D 38010000 FTOI4.xyz   */   vf02.xyz = ftoi4(vf02.xyz);
/*                        FCGET       */   vi01 = clip & 0xfff;
/* 067F 01C3197D 800110B4 FTOI4.xyz   */   vf03.xyz = ftoi4(vf03.xyz);
/*                        IAND        */   vi02 = vi02 & vi01;
/* 0680 01C4217D 48007800 FTOI4.xyz   */   vf04.xyz = ftoi4(vf04.xyz);
/* 0681 01C5297D 8000033C FTOI4.xyz   */   vf05.xyz = ftoi4(vf05.xyz);         // delay slot of 0680
/*                        JR          */   return;
L_0682:  /* from 05F7 */
/* 0682 01E00358 09031000 MULx.xyzw   */   vf13.xyzw = 0.0f;
/*                        ILW.x       */   vi03 = mem_w[vi02].x;
/* 0683 01E00318 01E21000 MULx.xyzw   */   vf12.xyzw = 0.0f;
/*                        LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 0684 0060BDD8 088C03FF MULx.zw     */   vf23.zw = 0.0f;
/*                        ILW.y       */   cursors_vi12 = cursors.y;
/* 0685 0060C618 09061001 MULx.zw     */   vf24.zw = 0.0f;
/*                        ILW.x       */   vi06 = mem_w[vi02 + 1].x;
/* 0686 0060CE58 10071002 MULx.zw     */   vf25.zw = 0.0f;
/*                        IADDIU      */   vi07 = vi02 + 2;
/* 0687 000002FF 80033A30 IADD        */   vi08 = vi07 + vi03;
/* 0688 000002FF 81EC137D SQI.xyzw    */   mem[cursors_vi12++].xyzw = vf02.xyzw;
/* 0689 000002FF 80031930 IADD        */   vi04 = vi03 + vi03;
/* 068A 000002FF 80042130 IADD        */   vi04 = vi04 + vi04;
/* 068B 000002FF 80044270 IADD        */   vi09 = vi08 + vi04;
/* 068C 000002FF 80044AB0 IADD        */   vi10 = vi09 + vi04;
/* 068D 000002FF 10050001 IADDIU      */   vi05 = 1;
L_068E:  /* from 06A8 */
/* 068E 000002FF 81ED3B7C LQI.xyzw    */   vf13.xyzw = mem[vi07++].xyzw;
/* 068F 000002FF 81EE437C LQI.xyzw    */   vf14.xyzw = mem[vi08++].xyzw;
/* 0690 000002FF 81EF437C LQI.xyzw    */   vf15.xyzw = mem[vi08++].xyzw;
/* 0691 000002FF 81F0437C LQI.xyzw    */   vf16.xyzw = mem[vi08++].xyzw;
/* 0692 01EDD1BC 81F1437C LQI.xyzw    */   vf17.xyzw = mem[vi08++].xyzw;       // MULAx.xyzw folded into 0695
/* 0693 01EDD8BD 8182537C LQI.xy      */   vf02.xy = mem[vi10++].xy;           // MADDAy.xyzw folded into 0695
/* 0694 01EDE0BE 8183537C LQI.xy      */   vf03.xy = mem[vi10++].xy;           // MADDAz.xyzw folded into 0695
/* 0695 01E0EB4B 8184537C MADDw.xyzw  */   vf13.xyzw = vf26.xyzw * vf13.x + vf27.xyzw * vf13.y + vf28.xyzw * vf13.z + vf29.xyzw; // folds 0692..0695
/*                        LQI.xy      */   vf04.xy = mem[vi10++].xy;
/* 0696 000002FF 8185537C LQI.xy      */   vf05.xy = mem[vi10++].xy;
/* 0697 000002FF 12031801 ISUBIU      */   vi03 = vi03 - 1;
/* 0698 01CD69FF 81ED03BC CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf13.xyz, vf13.w);
/*                        DIV         */   Q = 1.0f / vf13.w;
/* 0699 00601098 81E64B7C MULx.zw     */   vf02.zw = 0.0f;
/*                        LQI.xyzw    */   vf06.xyzw = mem[vi09++].xyzw;
/* 069A 006018D8 81E74B7C MULx.zw     */   vf03.zw = 0.0f;
/*                        LQI.xyzw    */   vf07.xyzw = mem[vi09++].xyzw;
/* 069B 00602118 81E84B7C MULx.zw     */   vf04.zw = 0.0f;
/*                        LQI.xyzw    */   vf08.xyzw = mem[vi09++].xyzw;
/* 069C 00602958 24000030 MULx.zw     */   vf05.zw = 0.0f;
/*                        FCAND       */   vi01 = (clip & 0x000030) != 0;
/* 069E 000002FF 81E94B7C LQI.xyzw    */   vf09.xyzw = mem[vi09++].xyzw;       // delay slot of 069D
/* 069D 000002FF 5200080A IBNE        */   if (vi01 == 0) {
/* 06A0 000002FF 8000033C             */     /* nop */                         // delay slot of 069F
/* 069F 000002FF 420F004B BAL         */     spriteProjectQ();
/* 06A2 000002FF 03EC1003 SQ.xyzw     */     mem[cursors_vi12 + 3].xyzw = vf02.xyzw; // delay slot of 06A1
/* 06A1 000002FF 52001006 IBNE        */     if (vi02 == 0) {
/* 06A3 000002FF 03EC1806 SQ.xyzw     */       mem[cursors_vi12 + 6].xyzw = vf03.xyzw;
/* 06A4 000002FF 03EC2009 SQ.xyzw     */       mem[cursors_vi12 + 9].xyzw = vf04.xyzw;
/* 06A5 000002FF 03EC280C SQ.xyzw     */       mem[cursors_vi12 + 12].xyzw = vf05.xyzw;
/* 06A6 000002FF 100C600D IADDIU      */       cursors_vi12 = cursors_vi12 + 13;
/* 06A7 000002FF 10052801 IADDIU      */       vi05 = vi05 + 1;
/*                                    */     }
/*                                    */   }
/* 06A9 000002FF 088403FF ILW.y       */   cursors_vi04 = cursors.y;           // delay slot of 06A8
/* 06A8 000002FF 52001FE5 IBNE        */   if (vi03 != 0) goto L_068E;
/* 06AA 000002FF 11E52FFF IADDIU      */   vi05 = vi05 + 32767;
/* 06AC 000002FF 0B052000 ISW.x       */   mem_w[cursors_vi04].x = vi05;       // delay slot of 06AB
/* 06AB 000002FF 400005F8 B           */   goto L_04A4;
L_06AD:  /* from 05F9 */
/* 06AD 01E00358 09031000 MULx.xyzw   */   vf13.xyzw = 0.0f;
/*                        ILW.x       */   vi03 = mem_w[vi02].x;
/* 06AE 01E00318 01E21000 MULx.xyzw   */   vf12.xyzw = 0.0f;
/*                        LQ.xyzw     */   vf02.xyzw = mem[vi02].xyzw;
/* 06AF 0060BDD8 088C03FF MULx.zw     */   vf23.zw = 0.0f;
/*                        ILW.y       */   cursors_vi12 = cursors.y;
/* 06B0 0060C618 09061001 MULx.zw     */   vf24.zw = 0.0f;
/*                        ILW.x       */   vi06 = mem_w[vi02 + 1].x;
/* 06B1 0060CE58 10071002 MULx.zw     */   vf25.zw = 0.0f;
/*                        IADDIU      */   vi07 = vi02 + 2;
/* 06B2 000002FF 80033A30 IADD        */   vi08 = vi07 + vi03;
/* 06B3 000002FF 81EC137D SQI.xyzw    */   mem[cursors_vi12++].xyzw = vf02.xyzw;
/* 06B4 000002FF 80031930 IADD        */   vi04 = vi03 + vi03;
/* 06B5 000002FF 80042130 IADD        */   vi04 = vi04 + vi04;
/* 06B6 000002FF 80044270 IADD        */   vi09 = vi08 + vi04;
/* 06B7 000002FF 80044A70 IADD        */   vi09 = vi09 + vi04;
/* 06B8 000002FF 80044AB0 IADD        */   vi10 = vi09 + vi04;
/* 06B9 000002FF 10050001 IADDIU      */   vi05 = 1;
/* 06BA 000002FF 100B3200 IADDIU      */   vi11 = vi06 + 512;
L_06BB:  /* from 06E6 */
/* 06BB 000002FF 81ED3B7C LQI.xyzw    */   vf13.xyzw = mem[vi07++].xyzw;
/* 06BC 000002FF 81EE437C LQI.xyzw    */   vf14.xyzw = mem[vi08++].xyzw;
/* 06BD 000002FF 81EF437C LQI.xyzw    */   vf15.xyzw = mem[vi08++].xyzw;
/* 06BE 000002FF 81F0437C LQI.xyzw    */   vf16.xyzw = mem[vi08++].xyzw;
/* 06BF 01EDD1BC 81F1437C LQI.xyzw    */   vf17.xyzw = mem[vi08++].xyzw;       // MULAx.xyzw folded into 06C2
/* 06C0 01EDD8BD 8182537C LQI.xy      */   vf02.xy = mem[vi10++].xy;           // MADDAy.xyzw folded into 06C2
/* 06C1 01EDE0BE 8183537C LQI.xy      */   vf03.xy = mem[vi10++].xy;           // MADDAz.xyzw folded into 06C2
/* 06C2 01E0EB4B 8184537C MADDw.xyzw  */   vf13.xyzw = vf26.xyzw * vf13.x + vf27.xyzw * vf13.y + vf28.xyzw * vf13.z + vf29.xyzw; // folds 06BF..06C2
/*                        LQI.xy      */   vf04.xy = mem[vi10++].xy;
/* 06C3 000002FF 8185537C LQI.xy      */   vf05.xy = mem[vi10++].xy;
/* 06C4 000002FF 12031801 ISUBIU      */   vi03 = vi03 - 1;
/* 06C5 000002FF 81E64B7C LQI.xyzw    */   vf06.xyzw = mem[vi09++].xyzw;
/* 06C6 01CD69FF 81ED03BC CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf13.xyz, vf13.w);
/*                        DIV         */   Q = 1.0f / vf13.w;
/* 06C7 00601098 81E74B7C MULx.zw     */   vf02.zw = 0.0f;
/*                        LQI.xyzw    */   vf07.xyzw = mem[vi09++].xyzw;
/* 06C8 006018D8 81E84B7C MULx.zw     */   vf03.zw = 0.0f;
/*                        LQI.xyzw    */   vf08.xyzw = mem[vi09++].xyzw;
/* 06C9 00602118 81E94B7C MULx.zw     */   vf04.zw = 0.0f;
/*                        LQI.xyzw    */   vf09.xyzw = mem[vi09++].xyzw;
/* 06CA 00602958 24000030 MULx.zw     */   vf05.zw = 0.0f;
/*                        FCAND       */   vi01 = (clip & 0x000030) != 0;
/* 06CB 000002FF 81F2437C LQI.xyzw    */   vf18.xyzw = mem[vi08++].xyzw;
/* 06CC 000002FF 81F3437C LQI.xyzw    */   vf19.xyzw = mem[vi08++].xyzw;
/* 06CD 000002FF 81F4437C LQI.xyzw    */   vf20.xyzw = mem[vi08++].xyzw;
/* 06CF 000002FF 81F5437C LQI.xyzw    */   vf21.xyzw = mem[vi08++].xyzw;       // delay slot of 06CE
/* 06CE 000002FF 52000817 IBNE        */   if (vi01 == 0) {
/* 06D1 000002FF 8000033C             */     /* nop */                         // delay slot of 06D0
/* 06D0 000002FF 420F001A BAL         */     spriteProjectQ();
/* 06D3 000002FF 03EC1003 SQ.xyzw     */     mem[cursors_vi12 + 3].xyzw = vf02.xyzw; // delay slot of 06D2
/* 06D2 000002FF 52001013 IBNE        */     if (vi02 == 0) {
/* 06D4 000002FF 03EC1806 SQ.xyzw     */       mem[cursors_vi12 + 6].xyzw = vf03.xyzw;
/* 06D5 000002FF 03EC2009 SQ.xyzw     */       mem[cursors_vi12 + 9].xyzw = vf04.xyzw;
/* 06D6 000002FF 03EC280C SQ.xyzw     */       mem[cursors_vi12 + 12].xyzw = vf05.xyzw;
/* 06D7 000002FF 0B0B600D ISW.x       */       mem_w[cursors_vi12 + 13].x = vi11;
/* 06D8 000002FF 03EC300E SQ.xyzw     */       mem[cursors_vi12 + 14].xyzw = vf06.xyzw;
/* 06D9 000002FF 03EC900F SQ.xyzw     */       mem[cursors_vi12 + 15].xyzw = vf18.xyzw;
/* 06DA 000002FF 03EC1010 SQ.xyzw     */       mem[cursors_vi12 + 16].xyzw = vf02.xyzw;
/* 06DB 000002FF 03EC3811 SQ.xyzw     */       mem[cursors_vi12 + 17].xyzw = vf07.xyzw;
/* 06DC 000002FF 03EC9812 SQ.xyzw     */       mem[cursors_vi12 + 18].xyzw = vf19.xyzw;
/* 06DD 000002FF 03EC1813 SQ.xyzw     */       mem[cursors_vi12 + 19].xyzw = vf03.xyzw;
/* 06DE 000002FF 03EC4014 SQ.xyzw     */       mem[cursors_vi12 + 20].xyzw = vf08.xyzw;
/* 06DF 000002FF 03ECA015 SQ.xyzw     */       mem[cursors_vi12 + 21].xyzw = vf20.xyzw;
/* 06E0 000002FF 03EC2016 SQ.xyzw     */       mem[cursors_vi12 + 22].xyzw = vf04.xyzw;
/* 06E1 000002FF 03EC4817 SQ.xyzw     */       mem[cursors_vi12 + 23].xyzw = vf09.xyzw;
/* 06E2 000002FF 03ECA818 SQ.xyzw     */       mem[cursors_vi12 + 24].xyzw = vf21.xyzw;
/* 06E3 000002FF 03EC2819 SQ.xyzw     */       mem[cursors_vi12 + 25].xyzw = vf05.xyzw;
/* 06E4 000002FF 100C601A IADDIU      */       cursors_vi12 = cursors_vi12 + 26;
/* 06E5 000002FF 10052802 IADDIU      */       vi05 = vi05 + 2;
/*                                    */     }
/*                                    */   }
/* 06E7 000002FF 088403FF ILW.y       */   cursors_vi04 = cursors.y;           // delay slot of 06E6
/* 06E6 000002FF 52001FD4 IBNE        */   if (vi03 != 0) goto L_06BB;
/* 06E8 000002FF 11E52FFF IADDIU      */   vi05 = vi05 + 32767;
/* 06EA 000002FF 0B052000 ISW.x       */   mem_w[cursors_vi04].x = vi05;       // delay slot of 06E9
/* 06E9 000002FF 400005BA B           */   goto L_04A4;
subroutine(spriteProjectQ, Q, vf02, vf03, vf04, vf05, vf06, vf07, vf08, vf09, vf13, vf14, vf15, vf16, vf17, vf23, vf24, vf25, vf30, vf31, vi06, vi12); /* 06EB, from 069F 06D0 */
/* 06EB 01C0BA9C 800003BF MULq.xyz    */   vf10.xyz = vf23.xyz * Q;
/*                        WAITQ       */   /* WAITQ */
/* 06EC 01C0C2DC 0B066000 MULq.xyz    */   vf11.xyz = vf24.xyz * Q;
/*                        ISW.x       */   mem_w[cursors_vi12].x = vi06;
/* 06ED 01C069FC 03EC3001 SQ.xyzw     */   mem[cursors_vi12 + 1].xyzw = vf06.xyzw; // MULAq.xyz folded into 06EE
/* 06EE 01C0CB21 03EC7002 MADDq.xyz   */   vf12.xyz = vf13.xyz * Q + vf25.xyz * Q; // folds 06ED..06EE
/*                        SQ.xyzw     */   mem[cursors_vi12 + 2].xyzw = vf14.xyzw;
/* 06EF 01C251BC 03EC3804 SQ.xyzw     */   mem[cursors_vi12 + 4].xyzw = vf07.xyzw; // MULAx.xyz folded into 06F1
/* 06F0 01C258BD 03EC7805 SQ.xyzw     */   mem[cursors_vi12 + 5].xyzw = vf15.xyzw; // MADDAy.xyz folded into 06F1
/* 06F1 01C0608B 03EC4007 MADDw.xyz   */   vf02.xyz = vf10.xyz * vf02.x + vf11.xyz * vf02.y + vf12.xyz; // folds 06EF..06F1
/*                        SQ.xyzw     */   mem[cursors_vi12 + 7].xyzw = vf08.xyzw;
/* 06F2 01C351BC 03EC8008 SQ.xyzw     */   mem[cursors_vi12 + 8].xyzw = vf16.xyzw; // MULAx.xyz folded into 06F4
/* 06F3 01C358BD 03EC480A SQ.xyzw     */   mem[cursors_vi12 + 10].xyzw = vf09.xyzw; // MADDAy.xyz folded into 06F4
/* 06F4 01C060CB 03EC880B MADDw.xyz   */   vf03.xyz = vf10.xyz * vf03.x + vf11.xyz * vf03.y + vf12.xyz; // folds 06F2..06F4
/*                        SQ.xyzw     */   mem[cursors_vi12 + 11].xyzw = vf17.xyzw;
/* 06F5 01C011FF 81E6333D CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf02.xyz, 1.0f);
/*                        MR32.xyzw   */   vf06.xyzw = mr32(vf06).xyzw;
/* 06F6 01C451BC 81E73B3D MR32.xyzw   */   vf07.xyzw = mr32(vf07).xyzw;        // MULAx.xyz folded into 06F8
/* 06F7 01C458BD 81E8433D MR32.xyzw   */   vf08.xyzw = mr32(vf08).xyzw;        // MADDAy.xyz folded into 06F8
/* 06F8 01C0610B 1002000F MADDw.xyz   */   vf04.xyz = vf10.xyz * vf04.x + vf11.xyz * vf04.y + vf12.xyz; // folds 06F6..06F8
/*                        IADDIU      */   vi02 = 15;
/* 06F9 01C019FF 38010000 FCGET       */   vi01 = clip & 0xfff;
/*                        CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf03.xyz, 1.0f);
/* 06FA 01C551BC 800110B4 IAND        */   vi02 = 15 & vi01;                   // MULAx.xyz folded into 06FC
/* 06FB 01C558BD 81E94B3D MR32.xyzw   */   vf09.xyzw = mr32(vf09).xyzw;        // MADDAy.xyz folded into 06FC
/* 06FC 01C0614B 81E6333D MADDw.xyz   */   vf05.xyz = vf10.xyz * vf05.x + vf11.xyz * vf05.y + vf12.xyz; // folds 06FA..06FC
/*                        MR32.xyzw   */   vf06.xyzw = mr32(vf06).xyzw;
/* 06FD 01C021FF 38010000 FCGET       */   vi01 = clip & 0xfff;
/*                        CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf04.xyz, 1.0f);
/* 06FE 01C0F9BF 800110B4 IAND        */   vi02 = vi02 & vi01;                 // MULAw.xyz folded into 06FF
/* 06FF 01DE10A9 81E73B3D MADD.xyz    */   vf02.xyz = vf31.xyz + vf02.xyz * vf30.xyz; // folds 06FE..06FF
/*                        MR32.xyzw   */   vf07.xyzw = mr32(vf07).xyzw;
/* 0700 01C029FF 81E8433D CLIP.xyz    */   clip = (clip << 6) | clip_bits(vf05.xyz, 1.0f);
/*                        MR32.xyzw   */   vf08.xyzw = mr32(vf08).xyzw;
/* 0701 01DE18E9 38010000 MADD.xyz    */   vf03.xyz = ACC.xyz + vf03.xyz * vf30.xyz;
/*                        FCGET       */   vi01 = clip & 0xfff;
/* 0702 01DE2129 800110B4 MADD.xyz    */   vf04.xyz = ACC.xyz + vf04.xyz * vf30.xyz;
/*                        IAND        */   vi02 = vi02 & vi01;
/* 0703 01DE2969 81E94B3D MADD.xyz    */   vf05.xyz = ACC.xyz + vf05.xyz * vf30.xyz;
/*                        MR32.xyzw   */   vf09.xyzw = mr32(vf09).xyzw;
/* 0704 01C2117D 38010000 FTOI4.xyz   */   vf02.xyz = ftoi4(vf02.xyz);
/*                        FCGET       */   vi01 = clip & 0xfff;
/* 0705 01C3197D 800110B4 FTOI4.xyz   */   vf03.xyz = ftoi4(vf03.xyz);
/*                        IAND        */   vi02 = vi02 & vi01;
/* 0706 01C4217D 48007800 FTOI4.xyz   */   vf04.xyz = ftoi4(vf04.xyz);
/* 0707 01C5297D 8000033C FTOI4.xyz   */   vf05.xyz = ftoi4(vf05.xyz);         // delay slot of 0706
/*                        JR          */   return;
/* a jump at 0002 0005 000A 001C 00FE 0101 and 5 more can reach here */
L_0708:  /* from 0005 */
/* 0708 000002FF 09041000 ILW.x       */   vi04 = mem_w[vi02].x;
/* 0709 000002FF 08851000 ILW.y       */   vi05 = mem_w[vi02].y;
/* 070A 000002FF 088303FF ILW.y       */   cursors_vi03 = cursors.y;
/* 070B 000002FF 10021001 IADDIU      */   vi02 = vi02 + 1;
/* 070D 000002FF 81E3137C LQI.xyzw    */   vf03.xyzw = mem[vi02++].xyzw;       // delay slot of 070C
/* 070C 000002FF 50002005 IBEQ        */   if (vi04 == 0) goto L_0712;
  /* loop 070E..0711: vi04 -= 1 per iteration (070E) */
/*                                    */   do {
/* 070E 000002FF 12042001 ISUBIU      */     vi04 = vi04 - 1;
/* 070F 000002FF 81E31B7D SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf03.xyzw;
/* 0711 000002FF 81E3137C LQI.xyzw    */     vf03.xyzw = mem[vi02++].xyzw;     // delay slot of 0710
/* 0710 000002FF 520027FD IBNE        */   } while (vi04 != 0);
L_0712:  /* from 070C */
/* 0712 000002FF 81E4137C LQI.xyzw    */   vf04.xyzw = mem[vi02++].xyzw;
/* 0713 000002FF 01EF0397 LQ.xyzw     */   fogParams_vf15.xyzw = fogParams.xyzw;
/* 0714 000002FF 003E0397 LQ.w        */   fogParams_vf30.w = fogParams.w;
/* 0715 00400143 090417FE ADDw.z      */   vf05.z = 1.0f;
/*                        ILW.x       */   vi04 = mem_w[vi02 - 2].x;
/* 0716 00400203 81E403BC ADDw.z      */   vf08.z = 1.0f;
/*                        DIV         */   Q = 1.0f / vf04.w;
/* 0717 002F07DA 11E107FF MULz.w      */   vf31.w = fogParams_vf15.z;
/*                        IADDIU      */   vi01 = 32767;
/* 0718 0020791B 81E31B7D MULw.w      */   vf04.w = fogParams_vf15.w;
/*                        SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf03.xyzw;
/* 0719 01DE212A 52002825 MUL.xyz     */   vf04.xyz = vf04.xyz * fogParams_vf30.xyz;
/* 071A 000002FF 80012134 IAND        */   vi04 = vi04 & 32767;                // delay slot of 0719
/*                        IBNE        */   if (vi05 != 0) goto L_073F;
/* 071B 000002FF 01851000 LQ.xy       */   vf05.xy = mem[vi02].xy;
/* 071C 01E0F9BF 01E71002 LQ.xyzw     */   vf07.xyzw = mem[vi02 + 2].xyzw;     // MULAw.xyzw folded into 071D
/* 071D 01E02121 800003BF MADDq.xyzw  */   vf04.xyzw = vf31.xyzw + vf04.xyzw * Q; // folds 071C..071D
/*                        WAITQ       */   /* WAITQ */
/* 071E 000002FF 01E61001 LQ.xyzw     */   vf06.xyzw = mem[vi02 + 1].xyzw;
/* 071F 01C02A9C 01881003 MULq.xyz    */   vf10.xyz = vf05.xyz * Q;
/*                        LQ.xy       */   vf08.xy = mem[vi02 + 3].xy;
/* 0720 00207C1B 81E703BC MULw.w      */   vf16.w = fogParams_vf15.w;
/*                        DIV         */   Q = 1.0f / vf07.w;
/* 0721 002F2115 01F11004 MINIy.w     */   vf04.w = min(vf04.w, fogParams_vf15.y);
/*                        LQ.xyzw     */   vf17.xyzw = mem[vi02 + 4].xyzw;
/* 0722 01DE3C2A 10060001 MUL.xyz     */   vf16.xyz = vf07.xyz * fogParams_vf30.xyz;
/*                        IADDIU      */   vi06 = 1;
/* 0723 01E6317C 10021005 FTOI0.xyzw  */   vf06.xyzw = ftoi0(vf06.xyzw);
/*                        IADDIU      */   vi02 = vi02 + 5;
/* 0724 000002FF 800621B4 IAND        */   vi06 = vi04 & 1;
/* 0725 000002FF 80062131 ISUB        */   vi04 = vi04 - vi06;
  /* loop 0726..0738: vi04 -= 2 per iteration (0734) */
/*                                    */   do {
/* 0726 01E0F9BF 8000033C MULAw.xyzw  */     ACC.xyzw = vf31.xyzw;
/* 0727 002F2110 81EC137C MAXx.w      */     vf04.w = max(vf04.w, fogParams_vf15.x);
/*                        LQI.xyzw    */     vf12.xyzw = mem[vi02++].xyzw;
/* 0728 01E081E1 800003BF MADDq.xyzw  */     vf07.xyzw = ACC.xyzw + vf16.xyzw * Q;
/*                        WAITQ       */     /* WAITQ */
/* 0729 01C042DC 8185137C MULq.xyz    */     vf11.xyz = vf08.xyz * Q;
/*                        LQI.xy      */     vf05.xy = mem[vi02++].xy;
/* 072A 01E9897C 81ED137C FTOI0.xyzw  */     vf09.xyzw = ftoi0(vf17.xyzw);
/*                        LQI.xyzw    */     vf13.xyzw = mem[vi02++].xyzw;
/* 072B 01E4217D 81EC03BC FTOI4.xyzw  */     vf04.xyzw = ftoi4(vf04.xyzw);
/*                        DIV         */     Q = 1.0f / vf12.w;
/* 072C 002F39D5 81F0137C MINIy.w     */     vf07.w = min(vf07.w, fogParams_vf15.y);
/*                        LQI.xyzw    */     vf16.xyzw = mem[vi02++].xyzw;
/* 072D 00207B1B 81E3537D MULw.w      */     vf12.w = fogParams_vf15.w;
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf10.xyzw;
/* 072E 01DE632A 81E3337D MUL.xyz     */     vf12.xyz = vf12.xyz * fogParams_vf30.xyz;
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf06.xyzw;
/* 072F 01E6697C 81E3237D FTOI0.xyzw  */     vf06.xyzw = ftoi0(vf13.xyzw);
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf04.xyzw;
/* 0730 002F39D0 8188137C MAXx.w      */     vf07.w = max(vf07.w, fogParams_vf15.x);
/*                        LQI.xy      */     vf08.xy = mem[vi02++].xy;
/* 0731 01E0F9BF 81F1137C LQI.xyzw    */     vf17.xyzw = mem[vi02++].xyzw;     // MULAw.xyzw folded into 0732
/* 0732 01E06121 800003BF MADDq.xyzw  */     vf04.xyzw = vf31.xyzw + vf12.xyzw * Q; // folds 0731..0732
/*                        WAITQ       */     /* WAITQ */
/* 0733 01C02A9C 81F003BC MULq.xyz    */     vf10.xyz = vf05.xyz * Q;
/*                        DIV         */     Q = 1.0f / vf16.w;
/* 0734 01E7397D 12042002 FTOI4.xyzw  */     vf07.xyzw = ftoi4(vf07.xyzw);
/*                        ISUBIU      */     vi04 = vi04 - 2;
/* 0735 00207C1B 81E35B7D MULw.w      */     vf16.w = fogParams_vf15.w;
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf11.xyzw;
/* 0736 002F2115 81E34B7D MINIy.w     */     vf04.w = min(vf04.w, fogParams_vf15.y);
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf09.xyzw;
/* 0737 01DE842A 520027EE MUL.xyz     */     vf16.xyz = vf16.xyz * fogParams_vf30.xyz;
/* 0738 000002FF 81E33B7D SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf07.xyzw; // delay slot of 0737
/*                        IBNE        */   } while (vi04 != 0);
/* 073A 002F2110 8000033C MAXx.w      */   vf04.w = max(vf04.w, fogParams_vf15.x); // delay slot of 0739
/* 0739 000002FF 5000356A IBEQ        */   if (vi06 == 0) goto L_04A4;
/* 073B 01E4217D 81E3537D FTOI4.xyzw  */   vf04.xyzw = ftoi4(vf04.xyzw);
/*                        SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf10.xyzw;
/* 073C 000002FF 81E3337D SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf06.xyzw;
/* 073E 000002FF 81E3237D SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf04.xyzw; // delay slot of 073D
/* 073D 000002FF 40000566 B           */   goto L_04A4;
L_073F:  /* from 0719 */
/* 073F 000002FF 81E9137C LQI.xyzw    */   vf09.xyzw = mem[vi02++].xyzw;
/* 0740 01E0F9BF 81E6137C LQI.xyzw    */   vf06.xyzw = mem[vi02++].xyzw;       // MULAw.xyzw folded into 0741
/* 0741 01E02121 800003BF MADDq.xyzw  */   vf04.xyzw = vf31.xyzw + vf04.xyzw * Q; // folds 0740..0741
/*                        WAITQ       */   /* WAITQ */
/* 0742 000002FF 81E603BC DIV         */   Q = 1.0f / vf06.w;
/* 0743 000002FF 81EB137C LQI.xyzw    */   vf11.xyzw = mem[vi02++].xyzw;
/* 0744 00207A9B 10060001 MULw.w      */   vf10.w = fogParams_vf15.w;
/*                        IADDIU      */   vi06 = 1;
/* 0745 01DE32AA 800621B4 MUL.xyz     */   vf10.xyz = vf06.xyz * fogParams_vf30.xyz;
/*                        IAND        */   vi06 = vi04 & 1;
/* 0746 002F2115 80062131 MINIy.w     */   vf04.w = min(vf04.w, fogParams_vf15.y);
/*                        ISUB        */   vi04 = vi04 - vi06;
  /* loop 0747..0756: vi04 -= 2 per iteration (074A) */
/*                                    */   do {
/* 0747 01E0F9BF 81E8137C LQI.xyzw    */     vf08.xyzw = mem[vi02++].xyzw;     // MULAw.xyzw folded into 0748
/* 0748 01E051A1 800003BF MADDq.xyzw  */     vf06.xyzw = vf31.xyzw + vf10.xyzw * Q; // folds 0747..0748
/*                        WAITQ       */     /* WAITQ */
/* 0749 002F2110 8000033C MAXx.w      */     vf04.w = max(vf04.w, fogParams_vf15.x);
/* 074A 01E5497C 12042002 FTOI0.xyzw  */     vf05.xyzw = ftoi0(vf09.xyzw);
/*                        ISUBIU      */     vi04 = vi04 - 2;
/* 074B 01DE422A 81E803BC DIV         */     Q = 1.0f / vf08.w;
/*                        MUL.xyz     */     vf08.xyz = vf08.xyz * fogParams_vf30.xyz;
/* 074C 002F3195 81E9137C MINIy.w     */     vf06.w = min(vf06.w, fogParams_vf15.y);
/*                        LQI.xyzw    */     vf09.xyzw = mem[vi02++].xyzw;
/* 074D 01E4217D 8000033C FTOI4.xyzw  */     vf04.xyzw = ftoi4(vf04.xyzw);
/* 074E 00207A1B 81EA137C MULw.w      */     vf08.w = fogParams_vf15.w;
/*                        LQI.xyzw    */     vf10.xyzw = mem[vi02++].xyzw;
/* 074F 002F3190 81E32B7D MAXx.w      */     vf06.w = max(vf06.w, fogParams_vf15.x);
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf05.xyzw;
/* 0750 01E7597C 81E3237D FTOI0.xyzw  */     vf07.xyzw = ftoi0(vf11.xyzw);
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf04.xyzw;
/* 0751 01E04121 800003BF MADDq.xyzw  */     vf04.xyzw = ACC.xyzw + vf08.xyzw * Q;
/*                        WAITQ       */     /* WAITQ */
/* 0752 01DE52AA 81EA03BC DIV         */     Q = 1.0f / vf10.w;
/*                        MUL.xyz     */     vf10.xyz = vf10.xyz * fogParams_vf30.xyz;
/* 0753 01E6317D 81EB137C FTOI4.xyzw  */     vf06.xyzw = ftoi4(vf06.xyzw);
/*                        LQI.xyzw    */     vf11.xyzw = mem[vi02++].xyzw;
/* 0754 00207A9B 81E33B7D MULw.w      */     vf10.w = fogParams_vf15.w;
/*                        SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf07.xyzw;
/* 0755 002F2115 520027F1 MINIy.w     */     vf04.w = min(vf04.w, fogParams_vf15.y);
/* 0756 000002FF 81E3337D SQI.xyzw    */     mem[cursors_vi03++].xyzw = vf06.xyzw; // delay slot of 0755
/*                        IBNE        */   } while (vi04 != 0);
/* 0757 01E5497C 5000354C FTOI0.xyzw  */   vf05.xyzw = ftoi0(vf09.xyzw);
/* 0758 002F2110 8000033C MAXx.w      */   vf04.w = max(vf04.w, fogParams_vf15.x); // delay slot of 0757
/*                        IBEQ        */   if (vi06 == 0) goto L_04A4;
/* 0759 01E4217D 81E32B7D FTOI4.xyzw  */   vf04.xyzw = ftoi4(vf04.xyzw);
/*                        SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf05.xyzw;
/* 075B 000002FF 81E3237D SQI.xyzw    */   mem[cursors_vi03++].xyzw = vf04.xyzw; // delay slot of 075A
/* 075A 000002FF 40000549 B           */   goto L_04A4;
}
