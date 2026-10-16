// Debug modes for SLUS_209.11, loaded as their own ELF segment: noclip flight for the field player
// and a free camera. Hold L3 and press R3 (noclip) or Select (free camera) to toggle one; while one
// is driving, the game sees a neutral pad.
#include "sdf.h"

typedef struct {
    float x, y, z, w;
} __attribute__((aligned(16))) Vec4;

#define sdf3DMakeScreenParam ((void (*)(volatile float*))0x2BA070)
#define VIEW_TARGET ((volatile Vec4*)0x2F5A50)  // aViewTarget, aViewEye, aViewDown
#define VIEW_EYE ((volatile Vec4*)0x2F5A60)
#define VIEW_DOWN ((volatile Vec4*)0x2F5A70)

// dds3RunProcessList's snapshot, copied to every process: per button, bit 0 held, 0x80 new.
#define PAD_BUTTONS ((volatile u8*)0x2F57D0)     // 16 per port, then the same with the stick as d-pad
#define PAD_ANALOG ((volatile u8*)0x54E8F8)      // RX, RY, LX, LY per port; 0x80 is centre
#define PAD_DEFLECTED ((volatile u8*)0x54E900)
#define PAD_BIT_TABLE ((volatile u8*)0x5298C8)   // dds3PadBitTable, which event scripts read
#define PAD_DATA ((volatile u16*)0x5518D8)
#define PAD_RAW_ANALOG ((volatile u8*)0x5518E0)
enum { SQUARE, CROSS, TRIANGLE, CIRCLE, LEFT, RIGHT, UP, DOWN, L1, L2, R1, R2, START, SELECT, L3, R3 };

#define dds3RunProcessList ((void (*)(void))0x103B30)
#define sdfSin ((float (*)(float))0x2C0060)
#define sdfCos ((float (*)(float))0x2C0160)
#define sdfAtan2 ((float (*)(float, float))0x2C0258)
#define sdfAsin ((float (*)(float))0x2C0310)

static struct {
    u8 buttons[16];
    float lx, ly, rx, ry;  // -1..1, past a dead zone; negative is left or up
    u8 noclip, freecam;
    u8 driving;            // noclip moved the player last frame
    u8 aim;                // the free camera takes the game's camera at its next frame
    int collision;         // l_UseCollision before noclip
    Vec4 eye;
    float yaw, pitch, fovy;
    u8 fall;         // 1 falling after noclip let go, 2 lying where the ragdoll landed
    int fallFrames;
    float vy;        // fall speed, +Y is down
    Vec4 release;    // where noclip let go, to go back to when there is nothing below
    void* root;      // the player's sdfModelRootNode_t this frame
} d;

// The loader need not zero what the segment leaves out of the file, so pad_hook does it once.
extern u32 bss_start[], segment_end[];
static int cleared __attribute__((section(".data")));

static int held(int button) {
    return d.buttons[button] & 1;
}

static int pressed(int button) {
    return d.buttons[button] & 0x80;
}

static float axis(u8 value) {
    float a = ((int)value - 128) * (1.0f / 127);
    if (a > 0.2f) return (a > 1.0f ? 1.0f : a - 0.2f) * 1.25f;
    if (a < -0.2f) return (a < -1.0f ? -1.0f : a + 0.2f) * 1.25f;
    return 0.0f;
}

#define L_USE_COLLISION (*(volatile int*)0x54EAF8)

#ifdef NOCLIP
static void start_fall(void);
#endif

static void set_noclip(int on) {
    if (on == d.noclip) return;
    d.noclip = on;
    d.driving = 0;
    if (on) {
        d.collision = L_USE_COLLISION;
        L_USE_COLLISION = 0;  // also keeps the field camera from pulling in against walls
    } else {
        L_USE_COLLISION = d.collision;
    }
#ifdef NOCLIP
    if (!on) start_fall();
#endif
}

static void set_freecam(int on) {
    d.freecam = on;
    d.aim = on;
}

static void hide_pad(void) {
    for (int i = 0; i < 64; i++) PAD_BUTTONS[i] = 0;
    for (int i = 0; i < 32; i++) PAD_BIT_TABLE[i] = 0;
    for (int i = 0; i < 8; i++) PAD_ANALOG[i] = PAD_RAW_ANALOG[i] = 0x80;
    for (int i = 0; i < 4; i++) PAD_DEFLECTED[i] = 0;
    PAD_DATA[0] = PAD_DATA[1] = 0;
}

// dds3KernelMain's call of dds3RunProcessList, before any process runs this frame.
void pad_hook(void) {
    if (!cleared) {
        for (volatile u32* p = bss_start; p < segment_end; p++) *p = 0;
        cleared = 1;
    }
    dds3RunProcessList();
    for (int i = 0; i < 16; i++) d.buttons[i] = PAD_BUTTONS[i];
    d.rx = axis(PAD_ANALOG[0]);
    d.ry = axis(PAD_ANALOG[1]);
    d.lx = axis(PAD_ANALOG[2]);
    d.ly = axis(PAD_ANALOG[3]);
    if (held(L3)) {
#ifdef NOCLIP
        if (pressed(R3)) {
            set_noclip(!d.noclip);
            set_freecam(0);
        }
#endif
#ifdef FREECAM
        if (pressed(SELECT)) {
            set_freecam(!d.freecam);
            set_noclip(0);
        }
#endif
    }
    if (held(L3) || d.freecam || d.driving) hide_pad();
}

#ifdef NOCLIP
#define fldPlayerCalc ((int (*)(void))0x134B20)
#define fldEveHit_ChkEveHit ((void (*)(void*))0x13ECD0)
#define fldEncountTestFlag ((int (*)(int))0x1259F8)
#define fldAutoMapChkMode ((int (*)(void))0x149288)
#define fldHitChkUnit ((void (*)(const Vec4*))0x139D90)
#define dds3SetPosPoseSet ((void (*)(void*, const Vec4*))0x10DEA8)
#define dds3SetRotPoseSet ((void (*)(void*, const Vec4*))0x10DED8)

#define UNIT_CONTROL ((void* volatile*)0x2FC350)  // the player's dds3Basic_t, then its reflection
#define FIELD_ID (*(volatile int*)0x2FBF84)       // under 10 is the world map
#define MENU_ON (*(volatile int*)0x2FC0C0)        // fldDebug_t.MenuOn
#define CAM_YAW (*(volatile float*)0x54EA64)      // degrees
#define PLAYER_POS ((volatile float*)0x2FC080)    // x, y, z, then the previous frame's
#define PLAYER_FACING (*(volatile float*)0x2FC0A4)
#define PLAYER_POS_ROT ((volatile Vec4*)0x2FC320)  // where the game puts the player back
#define HIT_POS ((volatile float*)0x2FC098)       // fldHitChkUnit's result
#define HIT_UNIT (*(volatile int*)0x2FC0B4)
#define ENCOUNT_CHECK_COUNT (*(volatile int*)0x2FBFDC)
#define DEG (3.14f / 180)                         // the game's own conversion
#define GRAVITY 1.5f                              // per frame, +Y is down
#define FALL_SPEED 40.0f

static volatile Vec4* pose_of(void* basic) {
    return *(Vec4* volatile*)((u8*)basic + 0x1C);
}

// The pose, the position globals and the copy the game restores from, as the walk sets them.
static void place_player(void* basic, const Vec4* from, const Vec4* to) {
    for (int i = 0; i < 3; i++) {
        PLAYER_POS[3 + i] = (&from->x)[i];
        PLAYER_POS[i] = (&to->x)[i];
    }
    dds3SetPosPoseSet(basic, to);
    pose_of(basic)[7] = *to;  // pos2, the smoothed copy
    PLAYER_POS_ROT[0] = *to;
}

static void fly_player(void* basic) {
    const float yaw = CAM_YAW + d.rx * 4.0f;
    CAM_YAW = yaw < 0.0f ? yaw + 360.0f : yaw >= 360.0f ? yaw - 360.0f : yaw;

    // Camera relative: forward is (-sin, 0, -cos) of the yaw, right is (-cos, 0, sin).
    const float s = sdfSin(CAM_YAW * DEG), c = sdfCos(CAM_YAW * DEG);
    const float speed = held(CROSS) ? 90.0f : 30.0f;
    const float mx = s * d.ly - c * d.lx, mz = c * d.ly + s * d.lx;
    const float my = (held(L1) ? 1.0f : 0.0f) - (held(R1) ? 1.0f : 0.0f);  // -Y is up

    const Vec4 from = pose_of(basic)[4];  // dds3PoseSet_t.pos
    Vec4 to = from;
    to.x += mx * speed;
    to.y += my * speed;
    to.z += mz * speed;
    place_player(basic, &from, &to);

    if (mx != 0.0f || mz != 0.0f) {
        // The game walks x += sin(k), z -= cos(k) for facing k.
        PLAYER_FACING = sdfAtan2(mx, -mz) / DEG;
        const float half = PLAYER_FACING * DEG * 0.5f;  // FromRotation about (0, 1, 0)
        Vec4 rot = {0.0f, sdfSin(half), 0.0f, sdfCos(half)};
        dds3SetRotPoseSet(basic, &rot);
        if (UNIT_CONTROL[1]) dds3SetRotPoseSet(UNIT_CONTROL[1], &rot);
        PLAYER_POS_ROT[1] = rot;
    }
}

static int any_input(void) {
    for (int i = 0; i < 16; i++) {
        if (held(i)) return 1;
    }
    return d.lx != 0.0f || d.ly != 0.0f || d.rx != 0.0f || d.ry != 0.0f;
}

#ifdef RAGDOLL
static int ragdoll_active(void);
static void ragdoll_land(float floor);
static int ragdoll_resting(void);
static void ragdoll_centre(Vec4* at);
static void ragdoll_stop(void);
static void* model_root(void* basic);
#else
static int ragdoll_active(void) { return 0; }
static void ragdoll_land(float floor) { (void)floor; }
static int ragdoll_resting(void) { return 1; }
static void ragdoll_centre(Vec4* at) { (void)at; }
static void ragdoll_stop(void) {}
static void* model_root(void* basic) { (void)basic; return 0; }
#endif

// After noclip lets go: gravity, with fldHitChkUnit finding the floor as fldPlayerFastSet does.
static void fall(void* basic) {
    const Vec4 from = pose_of(basic)[4];
    if (d.fallFrames++ == 0) d.release = from;
    d.vy = d.vy + GRAVITY < FALL_SPEED ? d.vy + GRAVITY : FALL_SPEED;
    Vec4 to = from;
    to.y += d.vy;
    for (int i = 0; i < 3; i++) {
        PLAYER_POS[3 + i] = (&from.x)[i];
        PLAYER_POS[i] = (&to.x)[i];
    }
    Vec4 probe = to;  // fldHitChkUnit pushes it out of walls; only a floor stops the fall
    HIT_UNIT = 0;
    fldHitChkUnit(&probe);
    if (HIT_UNIT) {
        HIT_UNIT = 0;
        to.x = HIT_POS[0];
        to.y = HIT_POS[1] + 44.0f;
        to.z = HIT_POS[2];
        d.fall = ragdoll_active() ? 2 : 0;
        ragdoll_land(to.y);
    } else if (d.fallFrames > 300) {  // nothing below
        to = d.release;
        d.fall = 0;
        ragdoll_stop();
    }
    place_player(basic, &from, &to);
}

static void stand_up(void* basic) {
    const Vec4 from = pose_of(basic)[4];
    Vec4 to = from;
    ragdoll_centre(&to);
    place_player(basic, &from, &to);
    d.fall = 0;
    ragdoll_stop();
}

static void start_fall(void) {
    d.fall = 1;
    d.fallFrames = 0;
    d.vy = 0.0f;
}

// ProcSequence's call of fldPlayerCalc: flight, the fall after it, or the game's own walk.
int player_calc(void) {
    void* basic = UNIT_CONTROL[0];
    const int control = basic && fldEncountTestFlag(0x40) && FIELD_ID >= 10 && !MENU_ON &&
                        !fldAutoMapChkMode();
    d.root = control ? model_root(basic) : 0;
    d.driving = control && (d.noclip || d.fall);
    if (!d.driving) {
        if (!control) {
            d.fall = 0;
            ragdoll_stop();
        }
        return fldPlayerCalc();
    }
    if (d.noclip) {
        fly_player(basic);
    } else if (d.fall == 1) {
        fall(basic);
    } else if (ragdoll_resting() && any_input()) {
        stand_up(basic);
    }
    ENCOUNT_CHECK_COUNT = 2;  // no encounter roll from the distance flown or fallen
    return 0;
}

// ProcSequence's call of fldEveHit_ChkEveHit: doors and events stay shut while flying.
void event_hit(void* basic) {
    if (!d.driving) fldEveHit_ChkEveHit(basic);
}
#endif

#ifdef RAGDOLL
#define dds3GetBaseControlWorkData ((u8* (*)(void*))0x1122A0)
#define sdfModelEvalTm ((void (*)(void*))0x2B17E8)
#define MAX_BONES 64
#define MAX_LINKS 256
#define MAX_PAIRS 768
#define TUMBLE 0.04f  // radians a frame, forward about the camera's right, when the body lets go

typedef struct {
    float m[3][3];
} Mat3;

// sdfModelNode_t and sdfModelRootNode_t
#define NODE_PARENT(n) (*(u8**)((n) + 0x8))
#define NODE_BOUNDS(n) (*(volatile float**)((n) + 0x34))  // min xyz, max xyz in the node's space
#define NODE_POS(n) ((volatile Vec4*)((n) + 0x60))
#define NODE_SCALE(n) ((volatile Vec4*)((n) + 0x70))
#define NODE_LOCAL(n) ((volatile Vec4*)((n) + 0x80))  // localNoScaledTm
#define NODE_TM(n) ((volatile Vec4*)((n) + 0xC0))     // world, rows then translation
#define ROOT_NODE_LIST(root) (*(u8**)(root))
#define ROOT_TM(root) ((volatile Vec4*)((u8*)(root) + 0x20))
#define ROOT_SCALE(root) ((volatile Vec4*)((u8*)(root) + 0x70))

static struct {
    int active, count, links;
    int grounded, still;  // frames on the floor, frames barely moving
    float floor;
    void* root;
    u8* node[MAX_BONES];
    short parent[MAX_BONES], child[MAX_BONES], child2[MAX_BONES];
    Vec4 p[MAX_BONES], prev[MAX_BONES];  // joints, Verlet
    Vec4 restA[MAX_BONES], restB[MAX_BONES];
    Mat3 rest[MAX_BONES];  // world rotation when the ragdoll took over
    Mat3 turn[MAX_BONES];  // rotation since
    Mat3 world[MAX_BONES];
    Vec4 animated[MAX_BONES][4];  // localNoScaledTm and pos as the motion left them
    int camera;                   // the view has been taken from the game's camera
    Vec4 eye, target;
    short a[MAX_LINKS], b[MAX_LINKS];
    float length[MAX_LINKS], stiffness[MAX_LINKS];
    float radius[MAX_BONES];  // the thickness of the node's own mesh
    float drag[MAX_BONES];    // small parts slow most in the air, so hands and feet trail
    int pairs;                // joints kept apart by their radii
    short pa[MAX_PAIRS], pb[MAX_PAIRS];
    float apart[MAX_PAIRS];
} r;

static float root_of(float v) {
    float out;
    __asm__("sqrt.s %0, %1" : "=f"(out) : "f"(v));
    return out;
}

static Vec4 sub(Vec4 a, Vec4 b) {
    Vec4 out = {a.x - b.x, a.y - b.y, a.z - b.z, 0.0f};
    return out;
}

static float dot(Vec4 a, Vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec4 cross(Vec4 a, Vec4 b) {
    Vec4 out = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f};
    return out;
}

static Vec4 unit(Vec4 a) {
    const float l = root_of(dot(a, a));
    Vec4 out = {a.x / l, a.y / l, a.z / l, 0.0f};
    return l > 1e-6f ? out : a;
}

// Rows e0 along a, e1 towards b, e2 across both.
static Mat3 frame(Vec4 a, Vec4 b) {
    const Vec4 e0 = unit(a);
    const float along = dot(b, e0);
    Vec4 toward = {b.x - e0.x * along, b.y - e0.y * along, b.z - e0.z * along, 0.0f};
    const Vec4 e1 = unit(toward), e2 = cross(e0, e1);
    Mat3 f = {{{e0.x, e0.y, e0.z}, {e1.x, e1.y, e1.z}, {e2.x, e2.y, e2.z}}};
    return f;
}

// Row vectors throughout (v' = v M), as sdfMatrix4_t.
static Mat3 multiply(const Mat3* a, const Mat3* b) {
    Mat3 out;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
        }
    }
    return out;
}

static Mat3 transpose(const Mat3* a) {
    Mat3 out;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out.m[i][j] = a->m[j][i];
    }
    return out;
}

// The shortest rotation taking unit a to unit b.
static Mat3 arc(Vec4 a, Vec4 b) {
    const float c = dot(a, b);
    Mat3 out;
    if (c < -0.999f) {  // half a turn, about any axis across a
        Vec4 other = {1.0f, 0.0f, 0.0f, 0.0f};
        if (a.x > 0.9f || a.x < -0.9f) other.x = 0.0f, other.y = 1.0f;
        const Vec4 k = unit(cross(a, other));
        const float v[3] = {k.x, k.y, k.z};
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) out.m[i][j] = 2.0f * v[i] * v[j] - (i == j ? 1.0f : 0.0f);
        }
        return out;
    }
    // Rodrigues with k = a x b, transposed for row vectors.
    const Vec4 k = cross(a, b);
    const float s = 1.0f / (1.0f + c);
    const float v[3] = {k.x, k.y, k.z};
    const float skew[3][3] = {{0.0f, -k.z, k.y}, {k.z, 0.0f, -k.x}, {-k.y, k.x, 0.0f}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            const float squared = v[i] * v[j] - (i == j ? dot(k, k) : 0.0f);
            out.m[j][i] = (i == j ? 1.0f : 0.0f) + skew[i][j] + squared * s;
        }
    }
    return out;
}

static Mat3 inverse(const Mat3* a) {
    const float(*m)[3] = a->m;
    Mat3 out;
    out.m[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    out.m[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    out.m[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    out.m[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    out.m[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    out.m[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    out.m[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    out.m[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    out.m[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    const float det = m[0][0] * out.m[0][0] + m[0][1] * out.m[1][0] + m[0][2] * out.m[2][0];
    if (det < 1e-4f && det > -1e-4f) return transpose(a);  // the bones' own are orthogonal
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) out.m[i][j] /= det;
    }
    return out;
}

static Vec4 times(Vec4 v, const Mat3* m) {
    Vec4 out = {v.x * m->m[0][0] + v.y * m->m[1][0] + v.z * m->m[2][0],
                v.x * m->m[0][1] + v.y * m->m[1][1] + v.z * m->m[2][1],
                v.x * m->m[0][2] + v.y * m->m[1][2] + v.z * m->m[2][2], 0.0f};
    return out;
}

// The player's modelType is 0: pModel is a dds3ModelHandle_t, which mdlLink evaluates.
static void* model_root(void* basic) {
    u8* work = dds3GetBaseControlWorkData(basic);  // dds3ObjectBaseControlWorkData_t
    u8* model = work ? *(u8**)(work + 0xC) : 0;
    if (!model) return 0;
    return *(u32*)(work + 8) == 1 ? model : *(void**)(model + 0x18);
}

static void link(int a, int b, float stiffness) {
    const Vec4 d = sub(r.p[a], r.p[b]);
    if (r.links == MAX_LINKS) return;
    r.a[r.links] = a;
    r.b[r.links] = b;
    r.length[r.links] = root_of(dot(d, d));
    r.stiffness[r.links++] = stiffness;
}

static float distance(int a, int b) {
    const Vec4 d = sub(r.p[a], r.p[b]);
    return root_of(dot(d, d));
}

// Joints two bones apart or less are held by the links; the rest keep out of each other.
static int nearby(int i, int j) {
    const int pi = r.parent[i], pj = r.parent[j];
    return i == j || pi == j || pj == i || (pi >= 0 && (pi == pj || r.parent[pi] == j)) ||
           (pj >= 0 && r.parent[pj] == i);
}

// Half the mean of the two smaller half extents of the node's mesh: a limb's thickness, not its
// length. A node without a mesh is a thin joint.
static float thickness(u8* n) {
    volatile float* box = NODE_BOUNDS(n);
    if (!box) return 2.0f;
    float h[3];
    for (int k = 0; k < 3; k++) h[k] = (box[3 + k] - box[k]) * 0.5f;
    for (int k = 0; k < 2; k++) {
        for (int l = k + 1; l < 3; l++) {
            if (h[l] < h[k]) {
                const float t = h[k];
                h[k] = h[l];
                h[l] = t;
            }
        }
    }
    const float r = 0.4f * (h[0] + h[1]);
    return r < 1.0f ? 1.0f : r > 20.0f ? 20.0f : r;
}

// A joint at each node, where the last frame's animation left it, falling as the pose falls and
// starting a slow forward tumble.
static void ragdoll_start(void* root) {
    u8* list = ROOT_NODE_LIST(root);
    const int count = *(short*)(list + 4);
    u8** items = *(u8***)(list + 0xC);
    if (count < 2 || count > MAX_BONES) return;
    r.count = count;
    r.root = root;
    r.links = r.grounded = r.still = r.camera = 0;
    for (int i = 0; i < count; i++) {
        u8* n = r.node[i] = items[i];
        volatile Vec4* tm = NODE_TM(n);
        r.parent[i] = r.child[i] = r.child2[i] = -1;
        for (int j = 0; j < count; j++) {
            if (items[j] == NODE_PARENT(n)) r.parent[i] = j;
        }
        for (int row = 0; row < 3; row++) r.animated[i][row] = NODE_LOCAL(n)[row];
        r.animated[i][3] = NODE_POS(n)[0];
        r.p[i] = r.prev[i] = tm[3];
        r.radius[i] = thickness(n);
        r.drag[i] = r.radius[i] >= 12.0f ? 0.01f : r.radius[i] >= 6.0f ? 0.04f : 0.08f;
        for (int k = 0; k < 3; k++) {
            r.rest[i].m[k][0] = tm[k].x;
            r.rest[i].m[k][1] = tm[k].y;
            r.rest[i].m[k][2] = tm[k].z;
        }
    }
    for (int j = 0; j < count; j++) {
        const int k = r.parent[j];
        if (k < 0 || distance(j, k) < 1.0f) continue;
        if (r.child[k] < 0) {
            r.child[k] = j;
        } else if (r.child2[k] < 0) {
            const Vec4 across = cross(unit(sub(r.p[r.child[k]], r.p[k])), unit(sub(r.p[j], r.p[k])));
            if (dot(across, across) > 0.04f) r.child2[k] = j;
        }
    }
    for (int i = 0; i < count; i++) {
        if (r.child[i] >= 0) r.restA[i] = sub(r.p[r.child[i]], r.p[i]);
        if (r.child2[i] >= 0) r.restB[i] = sub(r.p[r.child2[i]], r.p[i]);
    }
    // Bones hold their length; a joint to its grandparent and between siblings holds the shape.
    for (int j = 0; j < count; j++) {
        const int k = r.parent[j];
        if (k < 0) continue;
        link(j, k, 1.0f);
        if (r.parent[k] >= 0) link(j, r.parent[k], 0.2f);
        for (int l = j + 1; l < count; l++) {
            if (r.parent[l] == k) link(j, l, 1.0f);
        }
    }
    // Volume: joints thick enough to matter keep their radii apart, or as far as they start.
    r.pairs = 0;
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count && r.pairs < MAX_PAIRS; j++) {
            if (r.radius[i] < 4.0f || r.radius[j] < 4.0f || nearby(i, j)) continue;
            const float want = 0.9f * (r.radius[i] + r.radius[j]), now = distance(i, j);
            r.pa[r.pairs] = i;
            r.pb[r.pairs] = j;
            r.apart[r.pairs++] = now < want ? now : want;
        }
    }
    // Falling as the pose falls, and turning forward about the camera's right, (-cos, 0, sin).
    Vec4 middle = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int i = 0; i < count; i++) {
        middle.x += r.p[i].x / count;
        middle.y += r.p[i].y / count;
        middle.z += r.p[i].z / count;
    }
    const float yaw = CAM_YAW * DEG;
    const Vec4 axis = {-sdfCos(yaw) * TUMBLE, 0.0f, sdfSin(yaw) * TUMBLE, 0.0f};
    for (int i = 0; i < count; i++) {
        const Vec4 spin = cross(axis, sub(r.p[i], middle));
        r.prev[i].x -= spin.x;
        r.prev[i].y -= spin.y + d.vy;
        r.prev[i].z -= spin.z;
    }
    r.active = 1;
}

static void ragdoll_step(void) {
    float fastest = 0.0f;
    for (int i = 0; i < r.count; i++) {
        Vec4 v = sub(r.p[i], r.prev[i]);
        const float keep = r.grounded ? 0.98f : 1.0f - r.drag[i];
        v.x *= keep, v.y *= keep, v.z *= keep;
        v.y = v.y + GRAVITY < FALL_SPEED ? v.y + GRAVITY : FALL_SPEED;
        r.prev[i] = r.p[i];
        r.p[i].x += v.x;
        r.p[i].y += v.y;
        r.p[i].z += v.z;
    }
    for (int pass = 0; pass < 10; pass++) {
        for (int l = 0; l < r.links; l++) {
            Vec4* a = &r.p[r.a[l]];
            Vec4* b = &r.p[r.b[l]];
            const Vec4 d = sub(*b, *a);
            const float length = root_of(dot(d, d));
            if (length < 1e-4f) continue;
            const float move = (length - r.length[l]) / length * 0.5f * r.stiffness[l];
            a->x += d.x * move, a->y += d.y * move, a->z += d.z * move;
            b->x -= d.x * move, b->y -= d.y * move, b->z -= d.z * move;
        }
        for (int c = 0; c < r.pairs; c++) {
            Vec4* a = &r.p[r.pa[c]];
            Vec4* b = &r.p[r.pb[c]];
            const Vec4 d = sub(*b, *a);
            const float squared = dot(d, d), apart = r.apart[c];
            if (squared >= apart * apart || squared < 1e-6f) continue;
            const float length = root_of(squared), move = (apart - length) / length * 0.5f;
            a->x -= d.x * move, a->y -= d.y * move, a->z -= d.z * move;
            b->x += d.x * move, b->y += d.y * move, b->z += d.z * move;
        }
        if (!r.grounded) continue;
        for (int i = 0; i < r.count; i++) {
            const float floor = r.floor - r.radius[i];
            if (r.p[i].y <= floor) continue;
            r.p[i].y = floor;
            r.prev[i].x = r.p[i].x - (r.p[i].x - r.prev[i].x) * 0.8f;  // friction
            r.prev[i].z = r.p[i].z - (r.p[i].z - r.prev[i].z) * 0.8f;
        }
    }
    for (int i = 0; i < r.count; i++) {
        const Vec4 v = sub(r.p[i], r.prev[i]);
        const float speed = dot(v, v);
        if (speed > fastest) fastest = speed;
    }
    if (r.grounded) {
        r.grounded++;
        r.still = fastest < 0.04f ? r.still + 1 : 0;
    }
}

// Each node's world rotation turns as its bone did, sits at its joint, and goes back to the local
// matrix sdfModelEvalNode multiplies by its parent's.
static void ragdoll_pose(void* root) {
    u8 done[MAX_BONES];
    for (int i = 0; i < r.count; i++) {
        // A bone crushed to nothing turns with its parent; two children gone parallel give no
        // frame, so the first alone sets the turn.
        const Vec4 now = r.child[i] >= 0 ? sub(r.p[r.child[i]], r.p[i]) : r.restA[i];
        done[i] = r.child[i] >= 0 && dot(now, now) > 1.0f;
        if (!done[i]) continue;
        const Vec4 second = r.child2[i] >= 0 ? sub(r.p[r.child2[i]], r.p[i]) : now;
        const Vec4 across = cross(unit(now), unit(second));
        if (r.child2[i] >= 0 && dot(second, second) > 1.0f && dot(across, across) > 0.01f) {
            const Mat3 before = frame(r.restA[i], r.restB[i]);
            const Mat3 after = frame(now, second);
            const Mat3 back = transpose(&before);
            r.turn[i] = multiply(&back, &after);
        } else {
            r.turn[i] = arc(unit(r.restA[i]), unit(now));
        }
    }
    for (int pass = 0; pass < r.count; pass++) {  // leaves turn with their parent
        for (int i = 0; i < r.count; i++) {
            const int k = r.parent[i];
            if (done[i] || (k >= 0 && !done[k])) continue;
            if (k >= 0) {
                r.turn[i] = r.turn[k];
            } else {
                const Mat3 still = {{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};
                r.turn[i] = still;
            }
            done[i] = 1;
        }
    }
    for (int i = 0; i < r.count; i++) r.world[i] = multiply(&r.rest[i], &r.turn[i]);

    Mat3 top;  // sdfModelEvalTm2's parent for the top nodes: the root's tm, scaled
    for (int k = 0; k < 3; k++) {
        const float scale = (&ROOT_SCALE(root)->x)[k];
        top.m[k][0] = ROOT_TM(root)[k].x * scale;
        top.m[k][1] = ROOT_TM(root)[k].y * scale;
        top.m[k][2] = ROOT_TM(root)[k].z * scale;
    }
    const Vec4 topAt = ROOT_TM(root)[3];
    for (int i = 0; i < r.count; i++) {
        const int k = r.parent[i];
        const Mat3 parentInverse = inverse(k >= 0 ? &r.world[k] : &top);
        const Mat3 local = multiply(&r.world[i], &parentInverse);
        const Vec4 at = times(sub(r.p[i], k >= 0 ? r.p[k] : topAt), &parentInverse);
        u8* n = r.node[i];
        for (int row = 0; row < 3; row++) {
            const float scale = (&NODE_SCALE(n)->x)[row];
            Vec4 v = {local.m[row][0] / scale, local.m[row][1] / scale, local.m[row][2] / scale, 0.0f};
            NODE_LOCAL(n)[row] = v;
        }
        Vec4 pos = {at.x, at.y, at.z, 1.0f};
        NODE_LOCAL(n)[3] = pos;
        NODE_POS(n)[0] = pos;
    }
}

static int ragdoll_active(void) {
    return r.active;
}

static void ragdoll_land(float floor) {
    r.floor = floor;
    r.grounded = 1;
}

static int ragdoll_resting(void) {
    return !r.active || (r.grounded > 30 && r.still > 10) || r.grounded > 150;
}

// Standing back up where the body came to rest.
static void ragdoll_centre(Vec4* at) {
    if (!r.active) return;
    for (int i = 0; i < r.count; i++) {
        if (r.parent[i] < 0) {
            at->x = r.p[i].x;
            at->z = r.p[i].z;
            return;
        }
    }
}

// The motion does not rewrite every node each frame, so the nodes get back what it left them.
static void ragdoll_stop(void) {
    if (!r.active) return;
    for (int i = 0; i < r.count; i++) {
        for (int row = 0; row < 3; row++) NODE_LOCAL(r.node[i])[row] = r.animated[i][row];
        NODE_POS(r.node[i])[0] = r.animated[i][3];
        NODE_LOCAL(r.node[i])[3] = r.animated[i][3];
    }
    r.active = 0;
}

static Vec4 ease(Vec4 from, Vec4 to, float t) {
    Vec4 out = {from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t,
                from.z + (to.z - from.z) * t, 1.0f};
    return out;
}

// The field camera frames a standing player and lags a fall, so while the body is loose the view
// follows its middle from behind and above, along the field camera's yaw.
static void follow_body(void) {
    Vec4 middle = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int i = 0; i < r.count; i++) {
        middle.x += r.p[i].x / r.count;
        middle.y += r.p[i].y / r.count;
        middle.z += r.p[i].z / r.count;
    }
    const float yaw = CAM_YAW * DEG;
    // No higher than the field camera's own 170, or it looks out through the ceiling.
    Vec4 eye = {middle.x + sdfSin(yaw) * 280.0f, middle.y - 150.0f, middle.z + sdfCos(yaw) * 280.0f, 1.0f};
    Vec4 target = {middle.x, middle.y - 10.0f, middle.z, 1.0f};
    if (!r.camera) {
        r.eye = VIEW_EYE[0];
        r.target = VIEW_TARGET[0];
        r.camera = 1;
    }
    r.eye = ease(r.eye, eye, 0.2f);
    r.target = ease(r.target, target, 0.3f);
    VIEW_EYE[0] = r.eye;
    VIEW_TARGET[0] = r.target;
}

// mdlLink's call of sdfModelEvalTm, after the motion has posed the nodes.
void eval_tm(void* root) {
    if (root && root == d.root) {
        if (d.fall == 1 && d.fallFrames >= 3 && !r.active) ragdoll_start(root);
        if (r.active && r.root == root) {
            ragdoll_step();
            ragdoll_pose(root);
        }
    }
    sdfModelEvalTm(root);
}
#endif

#ifdef FREECAM
#define QUAKE_OFFSET ((volatile Vec4*)0x2F5960)
#define FOVY 3                                  // dds3ScreenParam, in floats

// Takes the camera the game set for this frame: +Y is down, forward is
// (sin yaw cos pitch, -sin pitch, cos yaw cos pitch).
static void aim_at_game_camera(volatile float* param) {
    const Vec4 eye = VIEW_EYE[0], target = VIEW_TARGET[0];
    float fx = target.x - eye.x, fy = target.y - eye.y, fz = target.z - eye.z;
    const float horizontal = fx * fx + fz * fz;
    d.eye = eye;
    d.yaw = sdfAtan2(fx, fz);
    float length;
    __asm__("sqrt.s %0, %1" : "=f"(length) : "f"(horizontal + fy * fy));
    d.pitch = sdfAsin(-fy / length);
    d.fovy = param[FOVY];
    d.aim = 0;
}

static void fly_camera(volatile float* param) {
    if (d.aim) aim_at_game_camera(param);
    d.yaw += d.rx * 0.05f;
    d.pitch -= d.ry * 0.04f;
    d.pitch = d.pitch > 1.5f ? 1.5f : d.pitch < -1.5f ? -1.5f : d.pitch;
    if (held(R2)) d.fovy *= 0.97f;
    if (held(L2)) d.fovy *= 1.03f;
    d.fovy = d.fovy < 0.1f ? 0.1f : d.fovy > 2.5f ? 2.5f : d.fovy;

    const float sy = sdfSin(d.yaw), cy = sdfCos(d.yaw), sp = sdfSin(d.pitch), cp = sdfCos(d.pitch);
    const float fx = sy * cp, fy = -sp, fz = cy * cp;
    const float speed = held(CROSS) ? 60.0f : held(SQUARE) ? 4.0f : 20.0f;
    const float ahead = -d.ly * speed, across = d.lx * speed;
    d.eye.x += fx * ahead + cy * across;  // right is (cos yaw, 0, -sin yaw)
    d.eye.y += fy * ahead + ((held(L1) ? 1.0f : 0.0f) - (held(R1) ? 1.0f : 0.0f)) * speed;
    d.eye.z += fz * ahead - sy * across;

    VIEW_EYE[0] = d.eye;
    Vec4 target = {d.eye.x + fx * 600.0f, d.eye.y + fy * 600.0f, d.eye.z + fz * 600.0f, d.eye.w};
    VIEW_TARGET[0] = target;
    Vec4 down = {0.0f, 1.0f, 0.0f, VIEW_DOWN[0].w};
    VIEW_DOWN[0] = down;
    Vec4 still = {0.0f, 0.0f, 0.0f, 0.0f};
    QUAKE_OFFSET[0] = still;
    param[FOVY] = d.fovy;
}

#endif

#if defined(FREECAM) || defined(RAGDOLL)
// dds3DrawFirstProcess's call of sdf3DMakeScreenParam, just before it builds the view matrix
// from aViewEye, aViewTarget and aViewDown.
void screen_param(volatile float* param) {
#ifdef FREECAM
    if (d.freecam) fly_camera(param);
#endif
#ifdef RAGDOLL
    if (!d.freecam && r.active) follow_body();
#endif
    sdf3DMakeScreenParam(param);
}
#endif
