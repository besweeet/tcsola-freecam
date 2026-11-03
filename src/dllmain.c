/* True Crime: Streets of LA - free camera
 * dinput8.dll proxy. Target exe: MD5 8663533808a653d81049a08b8df3b241 (image base 0x400000).
 *
 * How it works
 *  1. The game imports DirectInput8Create, so this DLL is loaded automatically.
 *  2. At load it patches the game's generic 4x4 matrix copy (0x4010A0). The camera's
 *     transform is written through that function. While free cam is on, the copy that
 *     targets the *current camera node* is replaced with our own matrix.
 *  3. DirectInput devices are hooked so the game sees an idle keyboard/mouse while
 *     free cam is on (the character stays put); mouse movement is used for looking.
 */
#define WIN32_LEAN_AND_MEAN
#define __USE_MINGW_ANSI_STDIO 1
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freecam_math.h"

/* ---- addresses inside TrueCrime.exe ---- */
#define ADDR_CAM_PTR     0x6D9570u  /* pointer to the current camera node            */
#define ADDR_PLAYER_PTR  0x6D9578u  /* pointer to the player node                    */
#define ADDR_HOOK        0x4010A0u  /* Matrix4::operator= (thiscall, 1 stack arg)    */
#define ADDR_RESUME      0x4010A6u
#define CAM_MATRIX_OFS   0x10u      /* node + 0x10: 4 rows, position row at +0x40    */
#define NODE_PARENT_OFS  0x58u
#define NODE_POS_OFS     0x40u

static const uint8_t k_hookOrig[6] = { 0x8B, 0xC1, 0x8B, 0x4C, 0x24, 0x04 };
uint32_t g_resume = ADDR_RESUME;

/* ---- config ---- */
static struct {
    int toggle, fwd, back, left, right, up, down, fast, slow;
    float speed, fastMul, slowMul, mouseSens;
    int invX, invY, blockInput, useMouse, forceFi, forceSF, log;
    float maxDist;
    int crashGuard, queryGuard;
} cfg;

static char g_iniPath[MAX_PATH], g_logPath[MAX_PATH];

static void Log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    HANDLE h;
    DWORD w;
    if (!cfg.log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    strcat(buf, "\r\n");
    h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, (DWORD)strlen(buf), &w, NULL);
    CloseHandle(h);
}

static int  CfgInt(const char *k, int d) { return (int)GetPrivateProfileIntA("FreeCam", k, d, g_iniPath); }
static float CfgFloat(const char *k, float d)
{
    char def[32], out[64];
    snprintf(def, sizeof def, "%g", d);
    GetPrivateProfileStringA("FreeCam", k, def, out, sizeof out, g_iniPath);
    return (float)atof(out);
}

static void LoadConfig(HMODULE self)
{
    char *slash;
    GetModuleFileNameA(self, g_iniPath, MAX_PATH);
    slash = strrchr(g_iniPath, '\\');
    if (slash) *(slash + 1) = 0;
    strcpy(g_logPath, g_iniPath);
    strcat(g_iniPath, "freecam.ini");
    strcat(g_logPath, "freecam.log");

    cfg.toggle = CfgInt("ToggleKey", VK_F9);
    cfg.fwd = CfgInt("Forward", 'W');  cfg.back = CfgInt("Back", 'S');
    cfg.left = CfgInt("Left", 'A');    cfg.right = CfgInt("Right", 'D');
    cfg.up = CfgInt("Up", 'E');        cfg.down = CfgInt("Down", 'Q');
    cfg.fast = CfgInt("Fast", VK_SHIFT); cfg.slow = CfgInt("Slow", VK_CONTROL);
    cfg.speed = CfgFloat("Speed", 15.0f);
    cfg.fastMul = CfgFloat("FastMultiplier", 5.0f);
    cfg.slowMul = CfgFloat("SlowMultiplier", 0.2f);
    cfg.mouseSens = CfgFloat("MouseSensitivity", 0.0025f);
    cfg.invX = CfgInt("InvertMouseX", 0);
    cfg.invY = CfgInt("InvertMouseY", 0);
    cfg.blockInput = CfgInt("BlockGameInput", 1);
    cfg.useMouse = CfgInt("UseMouse", 1);
    cfg.forceFi = CfgInt("ForwardRow", -1);
    cfg.forceSF = CfgInt("ForwardSign", 0);
    cfg.log = CfgInt("Log", 1);
    cfg.maxDist = CfgFloat("MaxDistance", 0.0f);
    cfg.crashGuard = CfgInt("CrashGuard", 1);
    cfg.queryGuard = CfgInt("QueryGuard", 1);
}

/* ---- free cam state ---- */
static volatile LONG g_active = 0;
static volatile LONG g_mouseDX = 0, g_mouseDY = 0;
static FreeCam g_fc;
static LARGE_INTEGER g_freq, g_last;
static int g_prevToggle = 0;

static int Down(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

static int GameFocused(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static int GetPlayerPos(V3 *out)
{
    char *p = *(char *volatile *)ADDR_PLAYER_PTR;
    int i;
    if (!p) return 0;
    for (i = 0; i < 16; i++) {
        char *par;
        if (IsBadReadPtr(p + NODE_PARENT_OFS, 4)) return 0;
        par = *(char **)(p + NODE_PARENT_OFS);
        if (!par) break;
        p = par;
    }
    if (IsBadReadPtr(p + NODE_POS_OFS, 12)) return 0;
    {
        float *f = (float *)(p + NODE_POS_OFS);
        *out = v3(f[0], f[1], f[2]);
    }
    return 1;
}


/* ---- crash diagnostics: write what happened to freecam.log ---- */
static LONG g_excLogged = 0;

static void DumpException(const char *tag, EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    char name[MAX_PATH] = "?";
    HMODULE mod = NULL;
    const char *base;
    int i, found = 0;
    MEMORYSTATUSEX ms;
    void *cam = *(void *volatile *)ADDR_CAM_PTR;

    Log("=== %s: code=0x%08lX at %p", tag, (unsigned long)er->ExceptionCode, er->ExceptionAddress);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        Log("  access violation: %s of address %p",
            er->ExceptionInformation[0] == 0 ? "READ" : er->ExceptionInformation[0] == 1 ? "WRITE" : "EXECUTE",
            (void *)er->ExceptionInformation[1]);
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod) && mod) {
        GetModuleFileNameA(mod, name, MAX_PATH);
        base = strrchr(name, '\\');
        Log("  in module %s + 0x%lX", base ? base + 1 : name, (unsigned long)((char *)er->ExceptionAddress - (char *)mod));
    }
    Log("  EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX", (unsigned long)c->Eax, (unsigned long)c->Ebx, (unsigned long)c->Ecx, (unsigned long)c->Edx);
    Log("  ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX", (unsigned long)c->Esi, (unsigned long)c->Edi, (unsigned long)c->Ebp, (unsigned long)c->Esp);
    if (!IsBadReadPtr(er->ExceptionAddress, 16)) {
        const uint8_t *b = (const uint8_t *)er->ExceptionAddress;
        Log("  code bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
            b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
    }
    /* likely return addresses: stack values that point into the exe's code */
    if (!IsBadReadPtr((void *)c->Esp, 4 * 512)) {
        const uint32_t *st = (const uint32_t *)c->Esp;
        char line[256]; line[0] = 0;
        for (i = 0; i < 512 && found < 14; i++) {
            if (st[i] >= 0x401000u && st[i] < 0x676000u) {
                char t[16];
                snprintf(t, sizeof t, " %08lX", (unsigned long)st[i]);
                strcat(line, t);
                found++;
            }
        }
        Log("  exe return-address candidates (nearest first):%s", line);
    }
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms))
        Log("  memory: %lu MB virtual address space free, %lu MB physical free",
            (unsigned long)(ms.ullAvailVirtual >> 20), (unsigned long)(ms.ullAvailPhys >> 20));
    Log("  freecam active=%ld pos=(%.1f %.1f %.1f) camNode=%p", (long)g_active, g_fc.pos.x, g_fc.pos.y, g_fc.pos.z, cam);
}


/* ---------------------------------------------------------------------------
 * Collision-query try/catch
 *
 * 0x524580 is the game's collision-world query (thiscall, 3 stack args, "ret 0xC", result is a
 * float left in ST0; no-hit result = the constant at 0x67FF90). Free-cam crashes happen when a
 * query reaches collision data that was unloaded/half-loaded far from the player.
 * We wrap the entry point: on entry we remember the callee-saved registers and divert the return
 * address through a thunk (so the record is popped on a normal return). If anything inside faults,
 * the exception handler restores those registers, unwinds to the caller and returns "no hit".
 * ------------------------------------------------------------------------- */
#define ADDR_QUERY       0x524580u
#define ADDR_QRESUME     0x524586u
#define ADDR_NOHIT_CONST 0x67FF90u
static const uint8_t k_queryOrig[9] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC, 0x64 };
uint32_t g_qResume = ADDR_QRESUME;
uint32_t g_retTmp = 0;

typedef struct { uint32_t slot, origRet, ebx, esi, edi, ebp; } QGuard;
static QGuard g_qg[32];
static volatile int g_qgDepth = 0;
static DWORD g_mainTid = 0;
static LONG g_qgRecovered = 0;

__attribute__((used)) int __stdcall GuardEnter(uint32_t slot, uint32_t ebx, uint32_t esi, uint32_t edi, uint32_t ebp)
{
    QGuard *g;
    if (!g_mainTid) g_mainTid = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_mainTid || g_qgDepth >= 32) return 0;
    g = &g_qg[g_qgDepth++];
    g->slot = slot;
    g->origRet = *(uint32_t *)(uintptr_t)slot;
    g->ebx = ebx; g->esi = esi; g->edi = edi; g->ebp = ebp;
    return 1;
}

__attribute__((used)) void __stdcall GuardLeave(void)
{
    if (g_qgDepth > 0) g_retTmp = g_qg[--g_qgDepth].origRet;
}

extern void QueryStub(void);
extern void QueryRetThunk(void);
extern void QueryRecover(void);
__asm__(
    ".intel_syntax noprefix\n"
    /* entry: replaces "push ebp / mov ebp,esp / and esp,-16" (first 6 bytes of 0x524580) */
    ".globl _QueryStub\n"
    "_QueryStub:\n"
    "    push ecx\n"
    "    push edx\n"
    "    lea  eax, [esp+8]\n"             /* slot that holds the return address */
    "    push ebp\n"
    "    push edi\n"
    "    push esi\n"
    "    push ebx\n"
    "    push eax\n"
    "    call _GuardEnter@20\n"
    "    test eax, eax\n"
    "    jz   2f\n"
    "    mov  eax, offset _QueryRetThunk\n"
    "    mov  [esp+8], eax\n"             /* divert the return address */
    "2:\n"
    "    pop  edx\n"
    "    pop  ecx\n"
    "    push ebp\n"
    "    mov  ebp, esp\n"
    "    and  esp, 0xfffffff0\n"
    "    jmp  dword ptr [_g_qResume]\n"
    /* normal return path: pop the record, then continue to the real caller. ST0 is untouched. */
    ".globl _QueryRetThunk\n"
    "_QueryRetThunk:\n"
    "    push eax\n"
    "    push ecx\n"
    "    push edx\n"
    "    call _GuardLeave@0\n"
    "    pop  edx\n"
    "    pop  ecx\n"
    "    pop  eax\n"
    "    jmp  dword ptr [_g_retTmp]\n"
    /* fault path: registers/stack were already restored by the exception handler */
    ".globl _QueryRecover\n"
    "_QueryRecover:\n"
    "    sub  esp, 4\n"
    "    fnstcw word ptr [esp]\n"
    "    fninit\n"                        /* the faulting code may have left the x87 stack in any state */
    "    fldcw word ptr [esp]\n"
    "    add  esp, 4\n"
    "    fld  dword ptr ds:[0x67FF90]\n"  /* result = "no hit" constant */
    "    xor  eax, eax\n"
    "    jmp  dword ptr [_g_retTmp]\n"
    ".att_syntax\n"
);

static int TryRecoverQuery(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    QGuard *g;
    uint32_t esp = (uint32_t)c->Esp, newEsp;
    NT_TIB *tib;
    LONG n;
    if (!cfg.queryGuard || g_qgDepth <= 0 || GetCurrentThreadId() != g_mainTid) return 0;
    g = &g_qg[g_qgDepth - 1];
    if (esp >= g->slot || g->slot - esp > 0x200000u) return 0;      /* fault must be inside the query's stack extent */
    newEsp = g->slot + 16;                                           /* return address + 3 args (ret 0xC) */
    n = InterlockedIncrement(&g_qgRecovered);
    if (n <= 12 || (n % 500) == 0)
        Log("query guard: aborted a collision query that faulted (%s of %p at %08lX, query called from %08lX) [count=%ld]",
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : "write",
            (void *)ep->ExceptionRecord->ExceptionInformation[1], (unsigned long)c->Eip, (unsigned long)g->origRet, (long)n);
    c->Ebx = g->ebx; c->Esi = g->esi; c->Edi = g->edi; c->Ebp = g->ebp;
    c->Esp = newEsp;
    g_retTmp = g->origRet;
    c->Eip = (DWORD)(uintptr_t)QueryRecover;
    g_qgDepth--;
    /* drop any SEH frames that lived in the stack region we just unwound */
    tib = (NT_TIB *)NtCurrentTeb();
    while (tib->ExceptionList && (uintptr_t)tib->ExceptionList != (uintptr_t)-1 &&
           (uint32_t)(uintptr_t)tib->ExceptionList < newEsp)
        tib->ExceptionList = tib->ExceptionList->Next;
    return 1;
}

static int InstallQueryGuard(void)
{
    uint8_t *t = (uint8_t *)(uintptr_t)ADDR_QUERY;
    DWORD old;
    int32_t rel;
    if (IsBadReadPtr(t, 9) || memcmp(t, k_queryOrig, 9) != 0) { Log("query guard: bytes at %p do not match, not installed", t); return 0; }
    if (!VirtualProtect(t, 6, PAGE_EXECUTE_READWRITE, &old)) return 0;
    rel = (int32_t)((uintptr_t)QueryStub - ((uintptr_t)t + 5));
    t[0] = 0xE9;
    memcpy(t + 1, &rel, 4);
    t[5] = 0x90;
    VirtualProtect(t, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, 6);
    Log("query guard installed");
    return 1;
}

/* Crash guard for a known fault: two tiny engine helpers (0x54BDD0 and 0x54BE70) transform a
 * vector by a matrix. When the game runs a collision query against a collision object that has
 * been unloaded, the matrix pointer is stale and the read faults. Both functions use the same
 * frame (push ebp / mov ebp,esp / and esp,-16 ... mov esp,ebp / pop ebp / ret 8), so we can
 * return from them as if they had finished: output = input (identity transform). */
static LONG g_guardCount = 0;
static int TryGuardTransform(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    uint32_t eip = (uint32_t)c->Eip, ebp = (uint32_t)c->Ebp, ret, out, in, savedEbp;
    LONG n;
    if (!cfg.crashGuard) return 0;
    if (!((eip >= 0x54BDD0u && eip < 0x54BE6Bu) || (eip >= 0x54BE70u && eip < 0x54BF0Bu))) return 0;
    if (IsBadReadPtr((void *)(uintptr_t)ebp, 16)) return 0;
    savedEbp = *(uint32_t *)(uintptr_t)ebp;
    ret = *(uint32_t *)(uintptr_t)(ebp + 4);
    out = *(uint32_t *)(uintptr_t)(ebp + 8);
    in  = *(uint32_t *)(uintptr_t)(ebp + 12);
    if (ret < 0x401000u || ret >= 0x676000u) return 0;
    if (out && !IsBadWritePtr((void *)(uintptr_t)out, 16)) {
        if (in && !IsBadReadPtr((void *)(uintptr_t)in, 16)) memcpy((void *)(uintptr_t)out, (void *)(uintptr_t)in, 16);
        else memset((void *)(uintptr_t)out, 0, 16);
    }
    c->Eip = ret;            /* pop ebp ; ret 8 */
    c->Esp = ebp + 16;
    c->Ebp = savedEbp;
    n = InterlockedIncrement(&g_guardCount);
    if (n <= 10 || (n % 500) == 0)
        Log("crash guard: skipped faulting matrix transform at %08lX (stale matrix ecx=%08lX), caller %08lX  [count=%ld]",
            (unsigned long)eip, (unsigned long)c->Ecx, (unsigned long)ret, (long)n);
    return 1;
}

static LONG CALLBACK VehHandler(EXCEPTION_POINTERS *ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_ACCESS_VIOLATION && TryRecoverQuery(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (c == EXCEPTION_ACCESS_VIOLATION && TryGuardTransform(ep)) return EXCEPTION_CONTINUE_EXECUTION;
    if (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_ILLEGAL_INSTRUCTION ||
        c == EXCEPTION_INT_DIVIDE_BY_ZERO || c == EXCEPTION_STACK_OVERFLOW ||
        c == EXCEPTION_PRIV_INSTRUCTION || c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
        if (InterlockedIncrement(&g_excLogged) <= 8) DumpException("exception (may be handled by the game)", ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    DumpException("UNHANDLED CRASH", ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Called from the asm stub for every matrix copy in the game (hundreds per frame).
 * dest = destination matrix, src = source matrix.
 * Return 1 = we handled it (skip the original copy), 0 = run the original. */
__attribute__((used)) int __stdcall CamHookC(float *dest, const float *src)
{
    void *cam = *(void *volatile *)ADDR_CAM_PTR;
    int tog, focused;
    LARGE_INTEGER now;
    float dt, dyaw, dpitch, fwd, strafe, vert, mul;

    if (!cam || dest != (float *)((char *)cam + CAM_MATRIX_OFS)) return 0; /* fast path */

    focused = GameFocused();
    tog = focused && Down(cfg.toggle);
    if (tog && !g_prevToggle) {
        if (!g_active) {
            V3 pl; int have = GetPlayerPos(&pl);
            fc_init(&g_fc, dest, have ? &pl : NULL, cfg.forceFi, cfg.forceSF);
            InterlockedExchange(&g_mouseDX, 0);
            InterlockedExchange(&g_mouseDY, 0);
            QueryPerformanceCounter(&g_last);
            g_active = 1;
            Log("free cam ON  player=%d fi=%d ui=%d ri=%d sF=%.0f sU=%.0f sR=%.0f yaw=%.3f pitch=%.3f pos=(%.2f %.2f %.2f)",
                have, g_fc.fi, g_fc.ui, g_fc.ri, g_fc.sF, g_fc.sU, g_fc.sR, g_fc.yaw, g_fc.pitch,
                g_fc.pos.x, g_fc.pos.y, g_fc.pos.z);
        } else {
            g_active = 0;
            Log("free cam OFF");
        }
    }
    g_prevToggle = tog;
    if (!g_active) return 0;

    QueryPerformanceCounter(&now);
    dt = (float)((double)(now.QuadPart - g_last.QuadPart) / (double)g_freq.QuadPart);
    g_last = now;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    dyaw = dpitch = fwd = strafe = vert = 0.0f;
    if (cfg.useMouse) {
        LONG dx = InterlockedExchange(&g_mouseDX, 0);
        LONG dy = InterlockedExchange(&g_mouseDY, 0);
        dyaw += (float)dx * cfg.mouseSens * (cfg.invX ? -1.0f : 1.0f);
        dpitch -= (float)dy * cfg.mouseSens * (cfg.invY ? -1.0f : 1.0f);
    }
    if (focused) {
        if (Down(VK_RIGHT)) dyaw += 1.8f * dt;     /* arrow keys always work for looking */
        if (Down(VK_LEFT))  dyaw -= 1.8f * dt;
        if (Down(VK_UP))    dpitch += 1.8f * dt;
        if (Down(VK_DOWN))  dpitch -= 1.8f * dt;
        if (Down(cfg.fwd))   fwd += 1.0f;
        if (Down(cfg.back))  fwd -= 1.0f;
        if (Down(cfg.right)) strafe += 1.0f;
        if (Down(cfg.left))  strafe -= 1.0f;
        if (Down(cfg.up))    vert += 1.0f;
        if (Down(cfg.down))  vert -= 1.0f;
    }
    mul = 1.0f;
    if (focused && Down(cfg.fast)) mul *= cfg.fastMul;
    if (focused && Down(cfg.slow)) mul *= cfg.slowMul;

    fc_update(&g_fc, dyaw, dpitch, fwd, strafe, vert, cfg.speed * mul * dt);
    {
        V3 pl; int have = GetPlayerPos(&pl);
        if (have && cfg.maxDist > 0.0f) {                       /* optional: stay within N units of the player */
            V3 d = v3_sub(g_fc.pos, pl);
            float l = v3_len(d);
            if (l > cfg.maxDist) g_fc.pos = v3(pl.x + d.x * (cfg.maxDist / l), pl.y + d.y * (cfg.maxDist / l), pl.z + d.z * (cfg.maxDist / l));
        }
        {   /* status line every 2 seconds, so a crash can be tied to where the camera was */
            static DWORD lastStatus;
            DWORD t = GetTickCount();
            if (t - lastStatus > 2000) {
                MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
                lastStatus = t;
                if (GlobalMemoryStatusEx(&ms))
                    Log("status: cam=(%.1f %.1f %.1f) dist-from-player=%.1f free-virtual=%luMB",
                        g_fc.pos.x, g_fc.pos.y, g_fc.pos.z, have ? v3_len(v3_sub(g_fc.pos, pl)) : -1.0f,
                        (unsigned long)(ms.ullAvailVirtual >> 20));
            }
        }
    }
    memcpy(dest, src, 64);   /* keep the w components and anything else the game put there */
    fc_write(&g_fc, dest);
    return 1;
}

/* Entry stub: replaces the first 6 bytes of the game's matrix copy.
 *   original:  mov eax,ecx / mov ecx,[esp+4]  then falls into 0x4010A6
 * The original function returns `this` in eax, so both paths set eax = dest. */
extern void CamHookStub(void);
__asm__(
    ".intel_syntax noprefix\n"
    ".globl _CamHookStub\n"
    "_CamHookStub:\n"
    "    push edx\n"
    "    push ecx\n"
    "    mov  eax, [esp+12]\n"       /* src */
    "    push eax\n"
    "    push ecx\n"                 /* dest */
    "    call _CamHookC@8\n"
    "    pop  ecx\n"
    "    pop  edx\n"
    "    test eax, eax\n"
    "    jnz  1f\n"
    "    mov  eax, ecx\n"
    "    mov  ecx, [esp+4]\n"
    "    jmp  dword ptr [_g_resume]\n"
    "1:\n"
    "    mov  eax, ecx\n"
    "    ret  4\n"
    ".att_syntax\n"
);

static int InstallCameraHook(void)
{
    uint8_t *target = (uint8_t *)(uintptr_t)ADDR_HOOK;
    DWORD old;
    int32_t rel;
    if ((uintptr_t)GetModuleHandleA(NULL) != 0x400000u) { Log("host exe is not at 0x400000, camera hook skipped"); return 0; }
    if (IsBadReadPtr(target, 6) || memcmp(target, k_hookOrig, 6) != 0) {
        Log("bytes at %p do not match - wrong exe version? camera hook skipped", target);
        return 0;
    }
    if (!VirtualProtect(target, 6, PAGE_EXECUTE_READWRITE, &old)) { Log("VirtualProtect failed"); return 0; }
    rel = (int32_t)((uintptr_t)CamHookStub - ((uintptr_t)target + 5));
    target[0] = 0xE9;
    memcpy(target + 1, &rel, 4);
    target[5] = 0x90;
    VirtualProtect(target, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 6);
    Log("camera hook installed (stub %p)", (void *)CamHookStub);
    return 1;
}

/* ---------------------------------------------------------------------------
 * DirectInput hooks (vtable patching of the real dinput8 objects)
 * ------------------------------------------------------------------------- */
typedef HRESULT (__stdcall *PFN_DI8Create)(HINSTANCE, DWORD, const IID *, LPVOID *, void *);
typedef HRESULT (__stdcall *PFN_CreateDevice)(void *, const GUID *, void **, void *);
typedef HRESULT (__stdcall *PFN_GetState)(void *, DWORD, void *);
typedef HRESULT (__stdcall *PFN_GetData)(void *, DWORD, void *, DWORD *, DWORD);

static const GUID k_guidMouse    = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID k_guidKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

typedef struct { void **vtbl; void *orig3, *orig9, *orig10; } VtEntry;
static VtEntry g_vt[32]; static int g_nvt;
static struct { void *dev; int type; } g_dev[64]; static int g_ndev;
static PFN_DI8Create g_realDI8Create;

static VtEntry *FindVt(void *obj)
{
    void **vt = *(void ***)obj;
    int i;
    for (i = 0; i < g_nvt; i++) if (g_vt[i].vtbl == vt) return &g_vt[i];
    return NULL;
}

static void PatchSlot(void **slot, void *hook)
{
    DWORD old;
    VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old);
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old, &old);
}

static int DevType(void *dev)
{
    int i;
    for (i = g_ndev - 1; i >= 0; i--) if (g_dev[i].dev == dev) return g_dev[i].type;
    return 0;
}

static void RegisterDev(void *dev, int type)
{
    int i;
    for (i = 0; i < g_ndev; i++) if (g_dev[i].dev == dev) { g_dev[i].type = type; return; }
    if (g_ndev < 64) { g_dev[g_ndev].dev = dev; g_dev[g_ndev].type = type; g_ndev++; }
}

/* 1 = keyboard, 2 = mouse, 0 = other */
static int Classify(void *dev, const GUID *rguid)
{
    uint8_t inst[1200];
    void **vt = *(void ***)dev;
    HRESULT hr;
    if (rguid) {
        if (!memcmp(rguid, &k_guidKeyboard, sizeof(GUID))) return 1;
        if (!memcmp(rguid, &k_guidMouse, sizeof(GUID))) return 2;
    }
    memset(inst, 0, sizeof inst);
    *(DWORD *)inst = 580;                                    /* DIDEVICEINSTANCEA */
    hr = ((HRESULT (__stdcall *)(void *, void *))vt[15])(dev, inst); /* GetDeviceInfo */
    if (FAILED(hr)) {
        memset(inst, 0, sizeof inst);
        *(DWORD *)inst = 1100;                               /* DIDEVICEINSTANCEW */
        hr = ((HRESULT (__stdcall *)(void *, void *))vt[15])(dev, inst);
    }
    if (SUCCEEDED(hr)) {
        DWORD t = *(DWORD *)(inst + 36) & 0xFF;
        if (t == 3) return 1;
        if (t == 2) return 2;
    }
    return 0;
}

static HRESULT __stdcall hkGetState(void *self, DWORD cb, void *data)
{
    VtEntry *e = FindVt(self);
    HRESULT hr;
    int t;
    if (!e) return E_FAIL;
    hr = ((PFN_GetState)e->orig9)(self, cb, data);
    if (FAILED(hr) || !g_active || !data) return hr;
    t = DevType(self);
    if (t == 1 && cb == 256) {
        uint8_t *k = (uint8_t *)data, esc = k[1];            /* keep Esc so pause still works */
        memset(k, 0, 256);
        k[1] = esc;
    } else if (t == 2 && cb >= 8) {
        LONG *l = (LONG *)data;
        InterlockedExchangeAdd(&g_mouseDX, l[0]);
        InterlockedExchangeAdd(&g_mouseDY, l[1]);
        memset(data, 0, cb);
    }
    return hr;
}

static HRESULT __stdcall hkGetData(void *self, DWORD cbObj, void *rgdod, DWORD *pdw, DWORD flags)
{
    VtEntry *e = FindVt(self);
    HRESULT hr;
    int t;
    uint8_t *base;
    DWORD n, i, k;
    if (!e) return E_FAIL;
    hr = ((PFN_GetData)e->orig10)(self, cbObj, rgdod, pdw, flags);
    if (FAILED(hr) || !g_active || !rgdod || !pdw || *pdw == 0 || cbObj < 8 || (flags & 1)) return hr;
    t = DevType(self);
    base = (uint8_t *)rgdod;
    n = *pdw;
    if (t == 2) {
        for (i = 0; i < n; i++) {
            DWORD ofs = *(DWORD *)(base + i * cbObj);
            LONG d = *(LONG *)(base + i * cbObj + 4);
            if (ofs == 0) InterlockedExchangeAdd(&g_mouseDX, d);
            else if (ofs == 4) InterlockedExchangeAdd(&g_mouseDY, d);
        }
        *pdw = 0;
    } else if (t == 1) {
        for (i = 0, k = 0; i < n; i++) {                     /* drop key-downs, keep releases and Esc */
            DWORD ofs = *(DWORD *)(base + i * cbObj);
            DWORD val = *(DWORD *)(base + i * cbObj + 4);
            if ((val & 0x80) == 0 || ofs == 1) {
                if (k != i) memmove(base + k * cbObj, base + i * cbObj, cbObj);
                k++;
            }
        }
        *pdw = k;
    }
    return hr;
}

static void PatchDevice(void *dev)
{
    void **vt = *(void ***)dev;
    if (FindVt(dev) || g_nvt >= 32) return;
    g_vt[g_nvt].vtbl = vt;
    g_vt[g_nvt].orig9 = vt[9];
    g_vt[g_nvt].orig10 = vt[10];
    g_nvt++;
    PatchSlot(&vt[9], (void *)hkGetState);
    PatchSlot(&vt[10], (void *)hkGetData);
}

static HRESULT __stdcall hkCreateDevice(void *self, const GUID *rguid, void **ppDev, void *outer)
{
    VtEntry *e = FindVt(self);
    HRESULT hr;
    if (!e) return E_FAIL;
    hr = ((PFN_CreateDevice)e->orig3)(self, rguid, ppDev, outer);
    if (SUCCEEDED(hr) && ppDev && *ppDev) {
        int type = Classify(*ppDev, rguid);
        RegisterDev(*ppDev, type);
        Log("DirectInput device created: %p type=%d (1=keyboard 2=mouse)", *ppDev, type);
        if (type) PatchDevice(*ppDev);
    }
    return hr;
}

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, const IID *riid,
                                                        LPVOID *ppv, void *outer)
{
    HRESULT hr;
    if (!g_realDI8Create) {
        char path[MAX_PATH];
        HMODULE h;
        GetSystemDirectoryA(path, MAX_PATH);
        strcat(path, "\\dinput8.dll");
        h = LoadLibraryA(path);
        if (!h) { Log("cannot load %s", path); return E_FAIL; }
        g_realDI8Create = (PFN_DI8Create)GetProcAddress(h, "DirectInput8Create");
        if (!g_realDI8Create) return E_FAIL;
    }
    hr = g_realDI8Create(hinst, ver, riid, ppv, outer);
    Log("DirectInput8Create -> 0x%08lX", (unsigned long)hr);
    if (SUCCEEDED(hr) && ppv && *ppv && cfg.blockInput && !FindVt(*ppv) && g_nvt < 32) {
        void **vt = *(void ***)*ppv;
        g_vt[g_nvt].vtbl = vt;
        g_vt[g_nvt].orig3 = vt[3];
        g_nvt++;
        PatchSlot(&vt[3], (void *)hkCreateDevice);
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        QueryPerformanceFrequency(&g_freq);
        LoadConfig(inst);
        Log("---- freecam v1.0 loaded ----");
        AddVectoredExceptionHandler(1, VehHandler);
        SetUnhandledExceptionFilter(CrashFilter);
        InstallCameraHook();
        if (cfg.queryGuard) InstallQueryGuard();
    }
    return TRUE;
}
