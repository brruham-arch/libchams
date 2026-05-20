/*
 * libchams v1.2 — GTA SA Android Chams Mod
 * Author : brruham
 * Fix: remove SetGlobalColor Dobby hook (SIGILL), direct GlobalColor write
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include "mod/amlmod.h"

#define LOGFILE  "/storage/emulated/0/chams_log.txt"
#define LOGTAG   "libchams"
#define CHAMS_R  1.0f
#define CHAMS_G  0.0f
#define CHAMS_B  0.0f
#define CHAMS_A  1.0f

#define OFF_RenderPedCB     0x005d605cU
#define OFF_RenderPlayerCB  0x005d602cU
#define OFF_SetGlobalColor  0x001b4fd0U
#define OFF_GlobalColor     0x006b3398U
#define GOT_glDepthFunc     0x0066ed70U
#define GOT_glDepthMask     0x00670094U

MYMOD(brruham.libchams, ChamsMod, 1.2, brruham)

typedef void*  RpAtomic;
typedef void* (*RenderCB_t)(RpAtomic*);
typedef void  (*SetGlobalColor_t)(float, float, float, float);
typedef void  (*glDepthFunc_t)(GLenum);
typedef void  (*glDepthMask_t)(GLboolean);

static RenderCB_t        orig_RenderPedCB    = nullptr;
static RenderCB_t        orig_RenderPlayerCB = nullptr;
static SetGlobalColor_t  pSetGlobalColor     = nullptr;
static float*            pGlobalColor        = nullptr;
static glDepthFunc_t     real_glDepthFunc    = nullptr;
static glDepthMask_t     real_glDepthMask    = nullptr;

static bool g_chamsEnabled = true;
static bool g_inChamsPass  = false;
static int  g_chamsPass    = 0;

static void log_write(const char* msg) {
    __android_log_print(ANDROID_LOG_DEBUG, LOGTAG, "%s", msg);
    FILE* f = fopen(LOGFILE, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}
static void log_fmt(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    log_write(buf);
}

static bool patch_got(uintptr_t got_addr, void* new_func) {
    uintptr_t page = got_addr & ~0xFFFU;
    if (mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE) != 0) return false;
    *(void**)got_addr = new_func;
    __builtin___clear_cache((char*)got_addr, (char*)got_addr + 4);
    return true;
}

static void hooked_glDepthFunc(GLenum func) {
    if (g_chamsPass == 1) { real_glDepthFunc(GL_ALWAYS); return; }
    real_glDepthFunc(func);
}
static void hooked_glDepthMask(GLboolean flag) {
    if (g_chamsPass == 1) { real_glDepthMask(GL_FALSE); return; }
    real_glDepthMask(flag);
}

static void* chams_do(RpAtomic* atomic, RenderCB_t orig_cb) {
    if (!g_chamsEnabled || g_inChamsPass || !orig_cb)
        return orig_cb ? orig_cb(atomic) : nullptr;

    g_inChamsPass = true;

    // Pass 1: through-wall
    // Panggil real_glDepthFunc langsung — tidak bergantung pada game memanggil ulang per-atomic
    real_glDepthFunc(GL_ALWAYS);
    real_glDepthMask(GL_FALSE);
    if (pGlobalColor) {
        pGlobalColor[0] = CHAMS_R;
        pGlobalColor[1] = CHAMS_G;
        pGlobalColor[2] = CHAMS_B;
        pGlobalColor[3] = CHAMS_A;
    }
    if (pSetGlobalColor) pSetGlobalColor(CHAMS_R, CHAMS_G, CHAMS_B, CHAMS_A);
    g_chamsPass = 1;
    orig_cb(atomic);

    // Pass 2: normal depth
    real_glDepthFunc(GL_LEQUAL);
    real_glDepthMask(GL_TRUE);
    g_chamsPass = 2;
    orig_cb(atomic);

    g_chamsPass = 0;
    g_inChamsPass = false;
    return atomic;
}

static void* hooked_RenderPedCB(RpAtomic* a)    { return chams_do(a, orig_RenderPedCB); }
static void* hooked_RenderPlayerCB(RpAtomic* a) { return chams_do(a, orig_RenderPlayerCB); }

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    log_write("[CHAMS] =====================");
    log_write("[CHAMS] PreLoad v1.2");
}

ON_MOD_LOAD() {
    log_write("[CHAMS] OnModLoad start");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { log_write("[CHAMS] ERROR: libdobby"); aml->ShowToast(false,"[CHAMS] FAIL: dobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { log_write("[CHAMS] ERROR: DobbyHook sym"); aml->ShowToast(false,"[CHAMS] FAIL: DobbyHook"); return; }
    log_write("[CHAMS] Dobby OK");

    uintptr_t base = aml->GetLib("libGTASA.so");
    if (!base) { log_write("[CHAMS] ERROR: libGTASA"); aml->ShowToast(false,"[CHAMS] FAIL: base"); return; }
    log_fmt("[CHAMS] base=0x%08x", (unsigned)base);

    pSetGlobalColor = (SetGlobalColor_t)((base + OFF_SetGlobalColor) | 1U);
    pGlobalColor    = (float*)(base + OFF_GlobalColor);
    log_fmt("[CHAMS] GlobalColor=0x%08x [%.2f %.2f %.2f %.2f]",
            (unsigned)(uintptr_t)pGlobalColor,
            pGlobalColor[0], pGlobalColor[1], pGlobalColor[2], pGlobalColor[3]);

    void* hGLES = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES) { log_write("[CHAMS] ERROR: GLESv2"); aml->ShowToast(false,"[CHAMS] FAIL: GLESv2"); return; }
    real_glDepthFunc = (glDepthFunc_t)dlsym(hGLES, "glDepthFunc");
    real_glDepthMask = (glDepthMask_t)dlsym(hGLES, "glDepthMask");
    if (!real_glDepthFunc || !real_glDepthMask) { log_write("[CHAMS] ERROR: GL funcs"); aml->ShowToast(false,"[CHAMS] FAIL: GLfuncs"); return; }
    log_fmt("[CHAMS] glDepthFunc=%p  glDepthMask=%p", (void*)real_glDepthFunc, (void*)real_glDepthMask);

    bool g1 = patch_got(base + GOT_glDepthFunc, (void*)hooked_glDepthFunc);
    bool g2 = patch_got(base + GOT_glDepthMask, (void*)hooked_glDepthMask);
    log_fmt("[CHAMS] GOT patch glDepthFunc=%d  glDepthMask=%d", (int)g1, (int)g2);
    if (!g1 || !g2) { log_write("[CHAMS] ERROR: GOT patch"); aml->ShowToast(false,"[CHAMS] FAIL: GOT"); return; }

    void* addrPed = (void*)(base + OFF_RenderPedCB);
    int r1 = dobbyHook(addrPed, (void*)hooked_RenderPedCB, (void**)&orig_RenderPedCB);
    log_fmt("[CHAMS] RenderPedCB hook=%d orig=%p", r1, (void*)orig_RenderPedCB);
    if (r1 != 0 || !orig_RenderPedCB) { log_write("[CHAMS] ERROR: PedCB"); aml->ShowToast(false,"[CHAMS] FAIL: PedCB"); return; }

    void* addrPlayer = (void*)(base + OFF_RenderPlayerCB);
    int r2 = dobbyHook(addrPlayer, (void*)hooked_RenderPlayerCB, (void**)&orig_RenderPlayerCB);
    log_fmt("[CHAMS] RenderPlayerCB hook=%d orig=%p", r2, (void*)orig_RenderPlayerCB);
    if (r2 != 0) log_write("[CHAMS] WARN: RenderPlayerCB hook failed (non-fatal)");

    log_write("[CHAMS] =====================");
    log_write("[CHAMS] All hooks OK v1.2");
    aml->ShowToast(false, "[CHAMS] v1.2 Loaded OK");
}
