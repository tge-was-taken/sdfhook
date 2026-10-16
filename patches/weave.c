// Weaves SLUS_209.11's two 224-line field passes into a 448-line progressive frame (W).
// Freestanding, no gp-relative data; nocturne_patch.py passes CHAIN, BBA and BBB as -D.
#include "sdf.h"

#define BITBLT_WIDTH (8u << 16)  // SBW or DBW 8, PSMCT32

static volatile struct {
    GsSlice* w;
    u32 valid;      // W holds a finished weave
    u32 pair;       // both passes of a frame are done and not yet woven
    u32 busy;       // the weave DMA is in flight
    u8 offset[2];   // half-line offset of the last pass in each buffer
    u32 fallbacks;  // times gs_full gave W back
} g;

// sdfDeliverDrawEnd 0x2A9FC8, with the pass recorded for the weave.
void deliver_draw_end(int mode, int frameId, int halfOffset) {
    g.offset[frameId] = halfOffset;
    g.pair = mode != 0;
    if (mode) {
        DISP_FRAME_ID[halfOffset] = frameId;
    } else {
        DISP_FRAME_ID[0] = frameId;
        DISP_FRAME_ID[1] = frameId;
    }
    DRAW_BUSY = -1;
}

// sdfGrSetDisp 0x2A9C08: W when it holds a weave, else field buffer aEvenOdd line-doubled.
void set_disp(int aEvenOdd) {
    GsSlice* w = g.w;
    if (w && g.valid) {
        put_display(w->addr, DISPLAY);
    } else {
        // The offset pass sits a line lower.
        put_display(FRAME_SET->frameHandle[aEvenOdd]->addr,
                    DISPLAY + MAGV_1 + ((u64)g.offset[aEvenOdd] << 12));
    }
}

// Called by sdfDrawThread in place of sdfGrDrawFrame, at each vblank before the next pass.
void draw_frame(void) {
    if (!g.w) {
        g.valid = 0;
        g.w = allocate_w(g.fallbacks);
    } else if (g.pair) {
        g.pair = 0;
        const int evenBuffer = g.offset[0] ? 1 : 0;  // even lines are the pass without the offset
        const u32 even = FRAME_SET->frameHandle[evenBuffer]->addr;
        const u32 odd = FRAME_SET->frameHandle[evenBuffer ^ 1]->addr;
        CancelWakeupThread(GetThreadId());  // the chain's end must be the only wakeup

        // W is read and the DMA started with interrupts off, so gs_full cannot free W between.
        int enabled = DIntr();
        GsSlice* w = g.w;
        if (w) {
            const u64 dest = (u64)(w->addr >> 6 | BITBLT_WIDTH) << 32;
            UNCACHED(BBA) = (even >> 6 | BITBLT_WIDTH) | dest;
            UNCACHED(BBB) = (odd >> 6 | BITBLT_WIDTH) | dest;
            g.busy = 1;
            D1_QWC = 0;
            D1_TADR = CHAIN;
            __asm__ volatile("sync.l" ::: "memory");
            D1_CHCR = 0x145;  // from memory, source chain, TTE, start
        }
        if (enabled) EIntr();
        if (g.busy) {
            do {
                SleepThread();  // sdfDrawThIntHandler wakes this thread at the chain's end
            } while (D1_CHCR & CHCR_STR);
            g.busy = 0;
            g.valid = 1;
        }
    }
    sdfGrDrawFrame();
}

// Gives W back to the allocator, returning 0 when there is none to give.
int gs_give_back(void) {
    GsSlice* w = g.w;
    if (!w) return 0;
    g.w = 0;
    g.valid = 0;
    g.fallbacks++;
    if (g.busy) {
        while (D1_CHCR & CHCR_STR) {}  // a weave in flight still writes W
    }
    const int shown = DISP_FRAME_ID[0];
    if (shown >= 0) set_disp(shown);  // off W before its memory is reused
    sdfGsMemFree(w);
    return 1;
}

GS_FULL_HANDLER;
