/*
 * libchams — GTA SA Android Chams Mod
 * Target : com.sampmobilerp.game (armeabi-v7a, Thumb2)
 * Author : brruham
 *
 * Pass 1 — depth GL_ALWAYS + warna chams  → visible through-wall
 * Pass 2 — depth GL_LEQUAL + warna asli   → render normal
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <android/log.h>
#include <GLES2/gl2.h>
#include "mod/amlmod.h"

// ─── Config ────────────────────────────────────────────────────────────────

#define LOGFILE  "/storage/emulated/0/chams_log.txt"
#define LOGTAG   "libchams"

// Warna pass 1 (through-wall): R G B A  (0.0 – 1.0)
#define CHAMS_R  1.0f
#define CHAMS_G  0.0f
#define CHAMS_B  0.0f
#define CHAMS_A  1.0f

// ─── Offsets libGTASA.so (armeabi-v7a) ────────────────────────────────────

#define OFF_RenderPedCB     0x005d605cU
#define OFF_RenderPlayerCB  0x005d602cU
#define OFF_SetGlobalColor  0x001b4fd0U   // SetGlobalColor(float,float,float,float)
#define OFF_GlobalColor     0x006b3398U   // float[4] — color state (r,g,b,a)

// ─── AML ───────────────────────────────────────────────────────────────────

MYMOD(brruham.libchams, ChamsMod, 1.0, brruham)

// ─── Types ─────────────────────────────────────────────────────────────────

typedef void*  RpAtomic;
typedef void* (*RenderCB_t)(RpAtomic*);
typedef void  (*SetGlobalColor_t)(float, float, float, float);
typedef void  (*glDepthFunc_t)(GLenum);
typedef void  (*glDepthMask_t)(GLboolean);

// ─── Pointers ──────────────────────────────────────────────────────────────

static RenderCB_t       orig_RenderPedCB    = nullptr;
static RenderCB_t       orig_RenderPlayerCB = nullptr;
static SetGlobalColor_t pSetGlobalColor     = nullptr;
static float*           pGlobalColor        = nullptr;
static glDepthFunc_t    real_glDepthFunc    = nullptr;
static glDepthMask_t    real_glDepthMask    = nullptr;

// ─── State ─────────────────────────────────────────────────────────────────

static bool g_chamsEnabled = true;
static bool g_inChamsPass  = false;

// ─── Log ───────────────────────────────────────────────────────────────────

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

// ─── Chams core ────────────────────────────────────────────────────────────

static void* chams_do(RpAtomic* atomic, RenderCB_t orig) {
    if (!g_chamsEnabled || g_inChamsPass || !orig)
        return orig ? orig(atomic) : nullptr;

    g_inChamsPass = true;

    // Snapshot GlobalColor
    float saved[4] = { 1.f, 1.f, 1.f, 1.f };
    if (pGlobalColor) memcpy(saved, pGlobalColor, 4 * sizeof(float));

    // Pass 1 — through-wall, warna chams
    real_glDepthFunc(GL_ALWAYS);
    real_glDepthMask(GL_FALSE);
    if (pSetGlobalColor) pSetGlobalColor(CHAMS_R, CHAMS_G, CHAMS_B, CHAMS_A);
    orig(atomic);

    // Pass 2 — normal depth, warna asli
    real_glDepthFunc(GL_LEQUAL);
    real_glDepthMask(GL_TRUE);
    if (pSetGlobalColor) pSetGlobalColor(saved[0], saved[1], saved[2], saved[3]);
    orig(atomic);

    g_inChamsPass = false;
    return atomic;
}

static void* hooked_RenderPedCB(RpAtomic* a)    { return chams_do(a, orig_RenderPedCB); }
static void* hooked_RenderPlayerCB(RpAtomic* a) { return chams_do(a, orig_RenderPlayerCB); }

// ─── Lifecycle ─────────────────────────────────────────────────────────────

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    log_write("[CHAMS] =====================");
    log_write("[CHAMS] PreLoad v1.0");
}

ON_MOD_LOAD() {
    log_write("[CHAMS] OnModLoad start");

    // Dobby
    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) {
        log_write("[CHAMS] ERROR: libdobby.so");
        aml->ShowToast(false, "[CHAMS] FAIL: libdobby");
        return;
    }
    auto dobbyHook = (int(*)(void*, void*, void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) {
        log_write("[CHAMS] ERROR: DobbyHook sym");
        aml->ShowToast(false, "[CHAMS] FAIL: DobbyHook sym");
        return;
    }
    log_write("[CHAMS] Dobby OK");

    // libGTASA base — via aml->GetLib, bukan dlopen handle
    uintptr_t base = aml->GetLib("libGTASA.so");
    if (!base) {
        log_write("[CHAMS] ERROR: libGTASA not in memory");
        aml->ShowToast(false, "[CHAMS] FAIL: libGTASA");
        return;
    }
    log_fmt("[CHAMS] base=0x%08x", (unsigned)base);

    // SetGlobalColor + GlobalColor — |1 untuk Thumb function pointer
    pSetGlobalColor = (SetGlobalColor_t)((base + OFF_SetGlobalColor) | 1U);
    pGlobalColor    = (float*)(base + OFF_GlobalColor);
    log_fmt("[CHAMS] SetGlobalColor=0x%08x", (unsigned)(base + OFF_SetGlobalColor));
    log_fmt("[CHAMS] GlobalColor=0x%08x [%.2f %.2f %.2f %.2f]",
            (unsigned)(uintptr_t)pGlobalColor,
            pGlobalColor[0], pGlobalColor[1], pGlobalColor[2], pGlobalColor[3]);

    // GL funcs — langsung dari libGLESv2, bukan via PLT libGTASA
    void* hGLES = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES) {
        log_write("[CHAMS] ERROR: libGLESv2.so");
        aml->ShowToast(false, "[CHAMS] FAIL: GLESv2");
        return;
    }
    real_glDepthFunc = (glDepthFunc_t)dlsym(hGLES, "glDepthFunc");
    real_glDepthMask = (glDepthMask_t)dlsym(hGLES, "glDepthMask");
    log_fmt("[CHAMS] glDepthFunc=%p  glDepthMask=%p",
            (void*)real_glDepthFunc, (void*)real_glDepthMask);
    if (!real_glDepthFunc || !real_glDepthMask) {
        log_write("[CHAMS] ERROR: GL depth funcs");
        aml->ShowToast(false, "[CHAMS] FAIL: GL funcs");
        return;
    }

    // Hook RenderPedCB
    void* addrPed = (void*)(base + OFF_RenderPedCB);
    int r1 = dobbyHook(addrPed, (void*)hooked_RenderPedCB, (void**)&orig_RenderPedCB);
    log_fmt("[CHAMS] RenderPedCB hook=%d orig=%p", r1, (void*)orig_RenderPedCB);
    if (r1 != 0 || !orig_RenderPedCB) {
        log_write("[CHAMS] ERROR: RenderPedCB hook failed");
        aml->ShowToast(false, "[CHAMS] FAIL: PedCB");
        return;
    }

    // Hook RenderPlayerCB (non-fatal jika gagal)
    void* addrPlayer = (void*)(base + OFF_RenderPlayerCB);
    int r2 = dobbyHook(addrPlayer, (void*)hooked_RenderPlayerCB, (void**)&orig_RenderPlayerCB);
    log_fmt("[CHAMS] RenderPlayerCB hook=%d orig=%p", r2, (void*)orig_RenderPlayerCB);
    if (r2 != 0) log_write("[CHAMS] WARN: RenderPlayerCB hook failed (non-fatal)");

    log_write("[CHAMS] =====================");
    log_write("[CHAMS] All hooks OK");
    log_fmt("[CHAMS] enabled=%d R=%.1f G=%.1f B=%.1f",
            (int)g_chamsEnabled, CHAMS_R, CHAMS_G, CHAMS_B);
    aml->ShowToast(false, "[CHAMS] Loaded OK");
}
