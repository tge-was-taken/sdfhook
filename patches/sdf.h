// SLUS_209.11 engine types, addresses and display setup shared by weave.c and native.c.
#pragma once

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

typedef struct GsSlice {
    struct GsSlice* prev;
    struct GsSlice* next;
    int type;  // 0 free
    u32 addr;  // GS words
    int size;
    short w, h;
    u32 pixelFormat;
} GsSlice;

typedef struct {
    short w, h, buffH;
    u8 pixelFormat, zbuffFormat;
    GsSlice* frameHandle[2];
    GsSlice* zbuffHandle;
} FrameSet;

typedef struct {
    void* tag;
    void* preDrawHead;
    u8 mode;  // passes drawn
    u8 buffId;
    u8 frameId;
    u8 halfOffset;
    int pad;
} DrawState;

#define FRAME_SET ((volatile FrameSet*)0x529388)          // aSrcBuffWidth
#define DRAW_STATE ((volatile DrawState*)0x5293F8)         // sdfGrDrawState[3]
#define DRAW_REQ_INDEX (*(volatile u8*)0x551890)           // sdfDrawReqQueueIndex
#define DRAW_FRAME_ID (*(volatile u8*)0x551889)            // sdfGrDrawFrameId
#define DISP_FRAME_ID ((volatile s8*)0x551858)             // sdfDispFrameId[2]
#define DRAW_BUSY (*(volatile s8*)0x55185A)                // sdfGrDrawBusy
#define GS_HEAD (*(GsSlice* volatile*)0x552090)            // gsMemHandleLinkHead
#define DISP_ENV ((volatile u64*)0x580FF0)                 // PMODE, SMODE2, DISPFB, DISPLAY, BGCOLOR
#define GS_REVISION (*(volatile u16*)0x529F26)             // sceGsGParam, from GS_CSR
#define GS_DISPFB1 (*(volatile u64*)0x12000070)
#define GS_DISPLAY1 (*(volatile u64*)0x12000080)
#define D1_CHCR (*(volatile u32*)0x10009000)
#define D1_QWC (*(volatile u32*)0x10009020)
#define D1_TADR (*(volatile u32*)0x10009030)
#define UNCACHED(addr) (*(volatile u64*)((addr) | 0x20000000))  // the DMA reads memory, not cache

#define sdfGsAllocFrameBuff ((GsSlice* (*)(int, int, int))0x2AA8D0)
#define sdfGsMemFree ((void (*)(GsSlice*))0x2AA7F0)
#define sdfGrDrawFrame ((void (*)(void))0x2ADB98)
#define sceGsPutDispEnv ((void (*)(volatile u64*))0x2C87D8)
#define GetThreadId ((int (*)(void))0x2DBD90)
#define SleepThread ((int (*)(void))0x2DBDC0)
#define CancelWakeupThread ((int (*)(int))0x2DBDF0)
#define DIntr ((int (*)(void))0x2E4070)
#define EIntr ((int (*)(void))0x2E40B8)

#define W_SIZE 0x38000   // 512x448 PSMCT32, in words
#define W_MARGIN 0x8000  // left free when W returns after gs_full, or the next load takes it
#define CHCR_STR 0x100
#define DISPLAY 0x001BF59F010330E8ull  // DX 232, DY 51, MAGH 2 (3 VCK a column), DW 1439, DH 447
#define MAGV_1 (1ull << 27)
#define DISPFB_WIDTH (8u << 9 | 16ull << 32)  // FBW 8, PSMCT32, DBX 16

#ifndef SMOOTH
#define SMOOTH 0
#endif
#define PMODE_CIRCUIT2 0x66  // EN2, MMOD and AMOD as the game sets them
#define PMODE_BLEND 0x8067   // EN1 too, circuit 1 mixed in at ALP 0x80
// Circuit 1 a line down and 1 VCK (a third of a column) right, one line and VCK shorter so it
// stays inside the picture and the active line.
#define SMOOTH_SHIFT ((1u << 12) + 1u - (1ull << 32) - (1ull << 44))

static inline void put_display(u32 addr, u64 display) {
    // libgraph drives circuit 1 alone on GS revision 1, so there is no second circuit to mix.
    const int blend = SMOOTH && GS_REVISION != 1;
    DISP_ENV[0] = blend ? PMODE_BLEND : PMODE_CIRCUIT2;
    DISP_ENV[1] = 2;  // SMODE2, progressive
    DISP_ENV[2] = (addr >> 11 & 0x1FF) | DISPFB_WIDTH;
    DISP_ENV[3] = display;
    DISP_ENV[4] = 0;
    if (blend) {
        GS_DISPFB1 = DISP_ENV[2];
        GS_DISPLAY1 = display + SMOOTH_SHIFT;
    }
    sceGsPutDispEnv(DISP_ENV);
}

static inline int has_room(int size) {
    for (GsSlice* s = GS_HEAD; s; s = s->next) {
        if (s->type == 0 && s->size >= size) return 1;
    }
    return 0;
}

// W from a free slice that holds it, since a failed allocation would reach gs_full with nothing
// to give; after gs_full has taken it back, also with the margin, or the next load takes it again.
static inline GsSlice* allocate_w(u32 fallbacks) {
    GsSlice* w = 0;
    const int enabled = DIntr();  // the slice list holds still
    if (has_room(fallbacks ? W_SIZE + W_MARGIN : W_SIZE)) w = sdfGsAllocFrameBuff(512, 448, 0);
    if (enabled) EIntr();
    return w;
}

// sdfGsErrorHandler: with W given back (gs_give_back), restarts the failed allocation at its
// DIntr with aType from the allocator's s3; with nothing to give, the original failure follows.
#define GS_FULL_HANDLER                                                                          \
    __asm__("	.set	push\n"                                                                    \
            "	.set	noreorder\n"                                                               \
            "	.globl	gs_full\n"                                                                \
            "gs_full:\n"                                                                        \
            "	addiu	$sp, $sp, -16\n"                                                         \
            "	sd	$ra, 0($sp)\n"                                                             \
            "	jal	gs_give_back\n"                                                           \
            "	nop\n"                                                                          \
            "	ld	$ra, 0($sp)\n"                                                             \
            "	beqz	$2, 2f\n"                                                                  \
            "	addiu	$sp, $sp, 16\n"                                                          \
            "	move	$5, $19\n"                                                                 \
            "	li	$8, 0x2AA494\n" /* where sdfGsMemAllocFromHead's handler call returns */     \
            "	beq	$31, $8, 1f\n"                                                            \
            "	nop\n"                                                                          \
            "	j	0x2AA578\n" /* sdfGsMemAllocFromTail: jal DIntr */                           \
            "	nop\n"                                                                          \
            "1:	j	0x2AA458\n" /* sdfGsMemAllocFromHead: jal DIntr */                          \
            "	nop\n"                                                                          \
            "2:	jr	$ra\n"                                                                     \
            "	nop\n"                                                                          \
            "	.set	pop\n")
