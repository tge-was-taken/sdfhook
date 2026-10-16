// Draws SLUS_209.11 at 448 lines: the two passes of a frame draw its top and bottom halves.
// A frame is either A, the field buffer pair, or B, W; the display shows the last one finished.
#include "sdf.h"

#define HALF_WORDS 0x1C000  // 512x224 PSMCT32, in words

static GsSlice bHalf[2];  // W's halves, handed to the engine as frameHandle[] while it draws B

static volatile struct {
    GsSlice* w;
    GsSlice* aHalf[2];  // the frame set's own handles
    u32 drawing;        // 1 while the frame in progress draws into B
    u32 shown;          // 1 when the last finished frame is B
    u32 fallbacks;      // times gs_full gave W back
    u32 walked;         // frames whose chain was remapped
    u32 moved;          // coordinates remapped in the last frame
    u32 scissors;       // SCISSOR writes left alone in the last frame
    u32 stopped;        // chain walks abandoned at an unknown VIF code or tag
    u32 statics;        // coordinates moved in packets outside the packet buffer, last frame
    u32 evicted;        // static coordinates forgotten for want of a slot, ever
} g;

// sdfDeliverDrawEnd 0x2A9FC8; a frame is finished once its bottom half is.
void deliver_draw_end(int mode, int frameId, int halfOffset) {
    if (mode) {
        DISP_FRAME_ID[halfOffset] = frameId;
        g.shown = g.drawing;
    } else {
        DISP_FRAME_ID[0] = frameId;
        DISP_FRAME_ID[1] = frameId;
    }
    DRAW_BUSY = -1;
}

// sdfGrSetDisp 0x2A9C08: the last finished frame, one buffer line a line.
void set_disp(int aEvenOdd) {
    (void)aEvenOdd;
    GsSlice* a = g.aHalf[0] ? g.aHalf[0] : FRAME_SET->frameHandle[0];
    put_display(g.shown && g.w ? g.w->addr : a->addr, DISPLAY);
}

#define sdfGrStdDrawEnvPredraw 0x2AD568u
#define DISP_TEX0 (0x1A0 + 0x20)  // sdfGrStdDrawEnv_t: the other buffer's TEX0 data
#define DRAW_TEX0 (0x1E0 + 0x20)  // and the buffer being drawn's
#define TEX0_TBP_TH (0x3FFFull | 0xFull << 30)

static u32 frame_base(int b) {
    return b && g.w ? g.w->addr : (g.aHalf[0] ? g.aHalf[0] : FRAME_SET->frameHandle[0])->addr;
}

// The env's predraw, with the frame textures made whole 448-line frames: the last one finished
// in place of the other field, the one being drawn in place of this field.
void predraw(volatile u8* aPre, int aEvenOdd) {
    ((void (*)(volatile u8*, int))sdfGrStdDrawEnvPredraw)(aPre, aEvenOdd);
    if (((volatile u32*)aPre)[2] != (u32)FRAME_SET) return;  // option0
    volatile u64* disp = (volatile u64*)(aPre + DISP_TEX0);
    volatile u64* draw = (volatile u64*)(aPre + DRAW_TEX0);
    *disp = (*disp & ~TEX0_TBP_TH) | frame_base(g.shown) >> 6 | 9ull << 30;
    *draw = (*draw & ~TEX0_TBP_TH) | frame_base(g.drawing) >> 6 | 9ull << 30;
}

typedef struct PreDraw {
    struct PreDraw* next;
    u32 func;
} PreDraw;

static void use_halves(GsSlice* top, GsSlice* bottom) {
    FRAME_SET->frameHandle[0] = top;
    FRAME_SET->frameHandle[1] = bottom;
}

// Called by sdfDrawThread in place of sdfGrDrawFrame, at each vblank before the next pass.
void draw_frame(void) {
    if (FRAME_SET->frameHandle[0] != &bHalf[0]) {  // after sdfGrInit or a frame set remake
        g.aHalf[0] = FRAME_SET->frameHandle[0];
        g.aHalf[1] = FRAME_SET->frameHandle[1];
    }
    if (!g.w) g.w = allocate_w(g.fallbacks);

    volatile DrawState* state = &DRAW_STATE[DRAW_REQ_INDEX];
    if (state->tag && state->mode < 2) {
        if (state->mode == 0) {
            // A new frame draws into the buffer not on screen; without W, into A.
            g.drawing = g.w && !g.shown;
        } else if (g.drawing && !g.w) {
            g.drawing = 0;  // gs_full took W between the halves
        }
        if (g.drawing) {
            for (int i = 0; i < 2; i++) {
                bHalf[i].addr = g.w->addr + i * HALF_WORDS;
                bHalf[i].w = 512;
                bHalf[i].h = 224;
                bHalf[i].pixelFormat = 0;
                bHalf[i].type = 1;
                bHalf[i].size = HALF_WORDS;
            }
            use_halves(&bHalf[0], &bHalf[1]);
        } else {
            use_halves(g.aHalf[0], g.aHalf[1]);
        }
        DRAW_FRAME_ID = state->mode;  // top half, then bottom
        for (volatile PreDraw* p = (volatile PreDraw*)state->preDrawHead; p; p = p->next) {
            if (p->func == sdfGrStdDrawEnvPredraw) p->func = (u32)predraw;
        }
    }
    sdfGrDrawFrame();
}

// sdfGrSetDrawFramePacket 0x2AD1F8 for the frame set's env: aHalfOffset picks the half.
void set_half_frame_packet(volatile u64* packet, u32 frameAddr, int w, int h, int pixelFormat,
                           u32 zbuffAddr, int zbuffFormat, int half, int context) {
    packet[0] = (u64)pixelFormat << 24 | (u64)((w + 63) >> 6) << 16 | frameAddr >> 11;
    packet[1] = context ? 0x4D : 0x4C;  // FRAME_2, FRAME_1
    packet[2] = (u64)zbuffFormat << 24 | zbuffAddr >> 11;
    packet[3] = context ? 0x4F : 0x4E;  // ZBUF
    // Screen y 2048 - h up for the top half, 2048 down for the bottom; 1/16 pixel units.
    packet[4] = (u64)(16 * (2048 - h) + (half ? 16 * h : 0)) << 32 | (u32)(8 * (4096 - w));
    packet[5] = context ? 0x19 : 0x18;  // XYOFFSET
    packet[6] = (u64)(h - 1) << 48 | (u64)(w - 1) << 16;
    packet[7] = context ? 0x41 : 0x40;  // SCISSOR
}

// sdf3DMakeViewportPacket 0x2B9E30: VU1 draws the half-height view at full height, and like
// everything else in 480 of the 512 columns. EE projections keep the view's own scale.
#define sdf3DMakeViewportPacket ((void (*)(volatile float*, volatile u32*, void*))0x2B9E30)
void viewport_packet(volatile float* packet, volatile u32* param, void* camera) {
    sdf3DMakeViewportPacket(packet, param, camera);
    if (param[0] & 1) {  // sdf3DScreenParam_t.flags, half vertical scale
        packet[0x90 / 4] *= 15.0f / 16.0f;                          // scale.x
        packet[0x94 / 4] *= 2.0f;                                   // scale.y
        packet[0xA0 / 4] = 2048.0f + (packet[0xA0 / 4] - 2048.0f) * (15.0f / 16.0f);  // trans.x
        packet[0xA4 / 4] = 2048.0f + (packet[0xA4 / 4] - 2048.0f) * 2.0f;             // trans.y
    }
}

// The frame's chain carries 2D and EE-projected primitives in the field's 224-line, 512-column
// space. Before the render thread sends it, those drawn into the frame are moved to 448 lines
// and 480 columns about the screen centre.

#define PACKET_BASE ((volatile u32*)0x551870)  // the two halves of the packet buffer
#define PACKET_SIZE (*(volatile u32*)0x55186C)  // sdfGrPacketBuffSize
#define PACKET_PTR (*(volatile u32*)0x551878)   // sdfGrPacketBuffPtr
#define sdfGrReqDraw ((void (*)(u32*, void*))0x2ADA68)

static struct {
    u32 lo, hi;       // this frame's packet buffer
    u32 fbp[4];       // the frame's four halves, as FRAME.FBP
    u8 toFrame[2];    // FRAME_1 and FRAME_2 point at the frame
    u8 fromFrame[2];  // TEX0_1 and TEX0_2 read a frame
    u8 context;       // PRIM.CTXT of the primitive in progress
    u32 cl, wl;       // VIF1 STCYCL
    u32 skip;         // VIF payload words to pass over
    u32 direct;       // DIRECT qwords still to come
    u32 left;         // GIF register slots or IMAGE qwords left in the packet
    u32 nreg, reg, flg;
    u64 regs;
    float q;             // the Q register
    float packedQ;       // a PACKED ST's Q, which the next RGBAQ write takes
    volatile float* st;  // ST into a frame not yet kicked
} w;

static inline u32 to_columns(u32 x) {
    return (u32)(32768 + (((int)x - 32768) * 15 >> 4)) & 0xFFFF;
}

static inline u32 to_lines(u32 y) {
    int v = 32768 + 2 * ((int)y - 32768);
    return v < 0 ? 0 : v > 65535 ? 65535 : (u32)v;
}

static int in_frame_packets(volatile void* q) {
    const u32 a = (u32)q;
    return a >= w.lo && a < w.hi;
}

static int moves(void) {
    return w.toFrame[w.context];
}

// A packet outside the packet buffer (dds3DrawFade's, say) may be sent again without being
// rebuilt, so a coordinate still holding what was written there last time is not moved again.
#define STATIC_SLOTS 64
static struct {
    volatile u64* at;
    u64 value;
} statics[STATIC_SLOTS];
static u32 nextStatic;

static void move(volatile u64* q, u64 moved) {
    if (in_frame_packets(q)) {
        *q = moved;
        g.moved++;
        return;
    }
    u32 i = 0;
    while (i < STATIC_SLOTS && statics[i].at != q) i++;
    if (i == STATIC_SLOTS) {
        i = nextStatic;
        nextStatic = (nextStatic + 1) % STATIC_SLOTS;
        if (statics[i].at) g.evicted++;
        statics[i].at = q;
    } else if (statics[i].value == *q) {
        return;
    }
    statics[i].value = moved;
    *q = moved;
    g.statics++;
}

// A texture coordinate into a frame, to follow the frame's own remap.
static int samples_frame(volatile void* q) {
    return in_frame_packets(q) && w.fromFrame[w.context];
}

static int is_frame(u32 fbp) {
    return fbp == w.fbp[0] || fbp == w.fbp[1] || fbp == w.fbp[2] || fbp == w.fbp[3];
}

static inline u32 to_texel_columns(u32 u) {  // 1/16 texels
    return 256 + u * 15 / 16;
}

// A frame drawn from the last one sits half a line lower, so each copy blends two lines as the
// field passes did when they read the other field.
#define FEEDBACK_V 8           // 1/16 texels
#define FEEDBACK_T (1.0f / 1024)  // half a line of 512

static int feeds_back(void) {
    return w.toFrame[w.context];
}

// s is s/Q across the frame's 512 columns, and Q is the one in force when the vertex kicks.
static void kick(void) {
    if (w.st) {
        w.st[0] = w.st[0] * (15.0f / 16.0f) + w.q * (1.0f / 32.0f);
        if (feeds_back()) w.st[1] -= w.q * FEEDBACK_T;
    }
    w.st = 0;
}

static u32 to_texel_lines(u32 v) {  // 1/16 texels
    const int t = 2 * (int)v - (feeds_back() ? FEEDBACK_V : 0);
    return t < 0 ? 0 : t > 0x3FFF ? 0x3FFF : (u32)t;
}

static float as_float(u32 bits) {
    union { u32 u; float f; } v = {bits};
    return v.f;
}

// A register write in register format: XYZ has X in bits 0-15, Y in 16-31.
__attribute__((noinline)) static void write_register(volatile u64* data, u32 addr) {
    const u64 d = *data;
    switch (addr) {
        case 0x00: w.context = d >> 9 & 1; break;  // PRIM
        case 0x01: w.q = as_float(d >> 32); break;  // RGBAQ
        case 0x02: w.st = samples_frame(data) ? (volatile float*)data : 0; break;  // ST
        case 0x04: case 0x05: case 0x0C: case 0x0D:  // XYZF2, XYZ2, XYZF3, XYZ3
            kick();
            if (moves()) {
                move(data, (d & ~0xFFFFFFFFull) | to_lines(d >> 16 & 0xFFFF) << 16 | to_columns(d & 0xFFFF));
            }
            break;
        case 0x4C: case 0x4D:  // FRAME_1, FRAME_2
            w.toFrame[addr - 0x4C] = is_frame(d & 0x1FF);
            break;
        case 0x06: case 0x07:  // TEX0_1, TEX0_2: TBP0 in 64-word blocks, FBP in 2048-word pages
            w.fromFrame[addr - 0x06] = (d & 0x1F) == 0 && is_frame((d & 0x3FFF) >> 5);
            break;
        case 0x03:  // UV: U in bits 0-13, V in 16-29
            if (samples_frame(data)) {
                const u32 u = to_texel_columns(d & 0x3FFF), v = to_texel_lines(d >> 16 & 0x3FFF);
                *data = (d & ~0x3FFF3FFFull) | v << 16 | u;
            }
            break;
        case 0x40: case 0x41:  // SCISSOR
            if (moves()) g.scissors++;
            break;
    }
}

__attribute__((noinline)) static void gif_qword(volatile u64* q) {
    if (!w.left) {
        const u64 tag = q[0];
        const u32 loops = tag & 0x7FFF;
        w.flg = tag >> 58 & 3;
        w.nreg = tag >> 60 ? tag >> 60 : 16;
        w.regs = q[1];
        w.reg = 0;
        if (tag >> 46 & 1) w.context = tag >> 56 & 1;  // PRE: PRIM bits 47-57, CTXT is PRIM bit 9
        w.left = w.flg == 2 ? loops : loops * w.nreg;
        return;
    }
    if (w.flg == 2) {  // IMAGE
        w.left--;
        return;
    }
    for (int half = 0; half < (w.flg == 1 ? 2 : 1) && w.left; half++) {
        const u32 r = w.regs >> 4 * w.reg & 0xF;
        w.reg = w.reg + 1 == w.nreg ? 0 : w.reg + 1;
        w.left--;
        if (w.flg == 1) {  // REGLIST, register format
            if (r <= 0x7 || r == 0xC || r == 0xD) write_register(q + half, r);
            continue;
        }
        const u64 d = q[0];
        switch (r) {  // PACKED
            case 0x0: w.context = d >> 9 & 1; break;
            case 0x1: w.q = w.packedQ; break;  // RGBAQ
            case 0x2:                          // ST, with a Q that waits for the next RGBAQ
                w.packedQ = as_float(q[1]);
                w.st = samples_frame(q) ? (volatile float*)q : 0;
                break;
            case 0x3:  // UV: U in bits 0-13, V in 32-45
                if (samples_frame(q)) {
                    const u32 u = to_texel_columns(d & 0x3FFF), v = to_texel_lines(d >> 32 & 0x3FFF);
                    q[0] = (d & ~0x3FFF00003FFFull) | (u64)v << 32 | u;
                }
                break;
            case 0x6: case 0x7: write_register(q, r); break;  // TEX0, same layout as the register
            case 0x4: case 0x5: case 0xC: case 0xD:  // X in bits 0-15, Y in 32-47
                kick();
                if (moves()) {
                    move(q, (d & ~0xFFFF0000FFFFull) | (u64)to_lines(d >> 32 & 0xFFFF) << 32 | to_columns(d & 0xFFFF));
                }
                break;
            case 0xE: write_register(q, q[1] & 0xFF); break;  // A+D
        }
    }
}

__attribute__((noinline)) static u32 unpack_words(u32 code) {
    const u32 cmd = code >> 24 & 0x7F;
    const u32 num = code >> 16 & 0xFF ? code >> 16 & 0xFF : 256;
    const u32 vn = cmd >> 2 & 3, vl = cmd & 3;
    const u32 bits = vn == 3 && vl == 3 ? 16 : (32 >> vl) * (vn + 1);
    u32 n = num;
    if (w.wl > w.cl) n = num / w.wl * w.cl + (num % w.wl < w.cl ? num % w.wl : w.cl);
    return (n * bits + 31) / 32;
}

// Returns 0 at a code this walk does not know, which ends it.
__attribute__((noinline)) static int vif_code(u32 code) {
    const u32 cmd = code >> 24 & 0x7F;
    const u32 imm = code & 0xFFFF;
    if (cmd >= 0x60) {
        w.skip = unpack_words(code);
        return 1;
    }
    switch (cmd) {
        case 0x00: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x10: case 0x11: case 0x13: case 0x14: case 0x15: case 0x17:
            return 1;
        case 0x01: w.cl = imm & 0xFF; w.wl = imm >> 8 & 0xFF; return 1;  // STCYCL
        case 0x20: w.skip = 1; return 1;                                   // STMASK
        case 0x30: case 0x31: w.skip = 4; return 1;                        // STROW, STCOL
        case 0x4A: w.skip = (code >> 16 & 0xFF ? code >> 16 & 0xFF : 256) * 2; return 1;  // MPG
        case 0x50: case 0x51: w.direct = imm ? imm : 65536; return 1;      // DIRECT, DIRECTHL
    }
    return 0;
}

// One DMA transfer as VIF1 sees it, from word `first` of its first qword.
static int vif_transfer(volatile u32* data, u32 qwc, u32 first) {
    u32 i = 0, k = first;
    while (i < qwc) {
        if (k == 0 && w.direct) {
            gif_qword((volatile u64*)(data + 4 * i));
            w.direct--;
            i++;
            continue;
        }
        if (w.skip) {
            if (k == 0 && w.skip >= 4) {  // pass over whole qwords of a payload unread
                const u32 whole = w.skip / 4 < qwc - i ? w.skip / 4 : qwc - i;
                i += whole;
                w.skip -= 4 * whole;
                continue;
            }
            w.skip--;
        } else {
            if (!vif_code(data[4 * i + k])) return 0;
            if (w.direct) {  // DIRECT data starts at the next qword
                k = 0;
                i++;
                continue;
            }
        }
        if (++k == 4) {
            k = 0;
            i++;
        }
    }
    return 1;
}

static volatile u32* dma_address(u32 field) {
    return (volatile u32*)(field >> 31 ? 0x70000000 | (field & 0x3FF0) : field & 0x0FFFFFF0);
}

static void walk_chain(u32 head) {
    u32 stack[2];
    int depth = 0;
    u32 tagAddr = head & 0x0FFFFFF0;
    for (int n = 0; n < 65536; n++) {
        volatile u32* tag = (volatile u32*)tagAddr;
        const u32 qwc = tag[0] & 0xFFFF;
        const u32 id = tag[0] >> 28 & 7;
        const u32 addr = tag[1];
        // TTE: the tag's upper words reach VIF1 ahead of its data.
        if (!vif_transfer(tag, 1, 2)) {
            g.stopped++;
            return;
        }
        volatile u32* data = id == 0 || id == 3 || id == 4 ? dma_address(addr) : tag + 4;
        if (!vif_transfer(data, qwc, 0)) {
            g.stopped++;
            return;
        }
        switch (id) {
            case 0: case 7: return;                                    // REFE, END
            case 1: tagAddr += 16 + 16 * qwc; break;                   // CNT
            case 2: tagAddr = (u32)dma_address(addr); break;          // NEXT
            case 3: case 4: tagAddr += 16; break;                      // REF, REFS
            case 5:                                                    // CALL
                if (depth == 2) {
                    g.stopped++;
                    return;
                }
                stack[depth++] = tagAddr + 16 + 16 * qwc;
                tagAddr = (u32)dma_address(addr);
                break;
            case 6:                                                    // RET
                if (!depth) return;
                tagAddr = stack[--depth];
                break;
        }
    }
    g.stopped++;
}

// dds3DrawProcess's call of sdfGrReqDraw, with the frame's chain remapped first.
void submit_frame(u32* aTag, void* aPreDraw) {
    const u32 ptr = PACKET_PTR, size = PACKET_SIZE;
    w.lo = ptr - PACKET_BASE[0] <= size ? PACKET_BASE[0] : PACKET_BASE[1];
    w.hi = ptr;
    GsSlice* a0 = g.aHalf[0] ? g.aHalf[0] : FRAME_SET->frameHandle[0];
    GsSlice* a1 = g.aHalf[1] ? g.aHalf[1] : FRAME_SET->frameHandle[1];
    const u32 wAddr = g.w ? g.w->addr : a0->addr;
    w.fbp[0] = a0->addr >> 11;
    w.fbp[1] = a1->addr >> 11;
    w.fbp[2] = wAddr >> 11;
    w.fbp[3] = (wAddr + HALF_WORDS) >> 11;
    w.toFrame[0] = w.toFrame[1] = 1;
    w.fromFrame[0] = w.fromFrame[1] = 0;
    w.q = w.packedQ = 1.0f;
    w.st = 0;
    w.context = 0;
    w.cl = w.wl = 1;
    w.skip = w.direct = w.left = 0;
    g.moved = g.scissors = g.statics = 0;
    if (aTag && aTag[2]) walk_chain(aTag[1]);  // sdfDrawTag_t next, head, tail
    g.walked++;
    sdfGrReqDraw(aTag, aPreDraw);
}

// Gives W back to the allocator, returning 0 when there is none to give.
int gs_give_back(void) {
    GsSlice* w = g.w;
    if (!w) return 0;
    if (g.drawing) {
        while (D1_CHCR & CHCR_STR) {}  // a half still draws into W
    }
    g.w = 0;
    g.shown = 0;
    g.fallbacks++;
    set_disp(0);  // off W before its memory is reused
    sdfGsMemFree(w);
    return 1;
}

GS_FULL_HANDLER;
