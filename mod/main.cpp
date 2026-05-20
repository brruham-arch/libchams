/*
 * libchams v1.4 — GTA SA Android Chams Mod
 * Author : brruham
 * v1.4: hook glDrawElements GOT (bukan depth state di RenderPedCB)
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
#define GOT_glDepthFunc     0x0066ed70U
#define GOT_glDepthMask     0x00670094U
#define GOT_glDrawElements  0x00674b80U
#define GOT_glUniform4fv    0x00673c98U

MYMOD(brruham.libchams, ChamsMod, 1.5, brruham)

typedef void*  RpAtomic;
typedef void* (*RenderCB_t)(RpAtomic*);
typedef void  (*glDepthFunc_t)(GLenum);
typedef void  (*glDepthMask_t)(GLboolean);
typedef void  (*glDrawElements_t)(GLenum, GLsizei, GLenum, const void*);
typedef void  (*glUniform4fv_t)(GLint, GLsizei, const GLfloat*);

static RenderCB_t       orig_RenderPedCB    = nullptr;
static RenderCB_t       orig_RenderPlayerCB = nullptr;
static glDepthFunc_t    real_glDepthFunc    = nullptr;
static glDepthMask_t    real_glDepthMask    = nullptr;
static glDrawElements_t real_glDrawElements = nullptr;
static glUniform4fv_t    real_glUniform4fv   = nullptr;

static bool g_chamsEnabled = true;
static bool g_inPedRender  = false;
static int  g_callCount    = 0;

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

// ── glUniform4fv hook — override warna saat ped render ────────────────────────
static const GLfloat chams_color[] = { CHAMS_R, CHAMS_G, CHAMS_B, CHAMS_A };
static void hooked_glUniform4fv(GLint loc, GLsizei count, const GLfloat* val) {
    if (g_inPedRender && g_chamsEnabled)
        real_glUniform4fv(loc, 1, chams_color);
    else
        real_glUniform4fv(loc, count, val);
}

// ── glDrawElements hook — titik actual draw ────────────────────────────────
static void hooked_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    if (!g_chamsEnabled || !g_inPedRender) {
        real_glDrawElements(mode, count, type, indices);
        return;
    }

    // Pass 1: through-wall (depth ALWAYS, no depth write)
    real_glDepthFunc(GL_ALWAYS);
    real_glDepthMask(GL_FALSE);
    real_glDrawElements(mode, count, type, indices);

    // Pass 2: normal
    real_glDepthFunc(GL_LEQUAL);
    real_glDepthMask(GL_TRUE);
    real_glDrawElements(mode, count, type, indices);
}

// ── RenderPedCB — set flag saja, jangan ubah GL state ─────────────────────
static void* hooked_RenderPedCB(RpAtomic* a) {
    g_callCount++;
    if (g_callCount == 1 || g_callCount % 1000 == 0)
        log_fmt("[CHAMS] PedCB #%d", g_callCount);

    if (!g_chamsEnabled || !orig_RenderPedCB) {
        return orig_RenderPedCB ? orig_RenderPedCB(a) : nullptr;
    }
    g_inPedRender = true;
    void* r = orig_RenderPedCB(a);
    g_inPedRender = false;
    return r;
}

static void* hooked_RenderPlayerCB(RpAtomic* a) {
    if (!g_chamsEnabled || !orig_RenderPlayerCB) {
        return orig_RenderPlayerCB ? orig_RenderPlayerCB(a) : nullptr;
    }
    g_inPedRender = true;
    void* r = orig_RenderPlayerCB(a);
    g_inPedRender = false;
    return r;
}

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    log_write("[CHAMS] PreLoad v1.5");
}

ON_MOD_LOAD() {
    log_write("[CHAMS] OnModLoad start");

    void* hDobby = dlopen("libdobby.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hDobby) { log_write("[CHAMS] ERROR: libdobby"); aml->ShowToast(false,"[CHAMS] FAIL: dobby"); return; }
    auto dobbyHook = (int(*)(void*,void*,void**))dlsym(hDobby, "DobbyHook");
    if (!dobbyHook) { log_write("[CHAMS] ERROR: DobbyHook"); aml->ShowToast(false,"[CHAMS] FAIL: DobbyHook"); return; }

    uintptr_t base = aml->GetLib("libGTASA.so");
    if (!base) { log_write("[CHAMS] ERROR: libGTASA"); aml->ShowToast(false,"[CHAMS] FAIL: base"); return; }
    log_fmt("[CHAMS] base=0x%08x", (unsigned)base);

    void* hGLES = dlopen("libGLESv2.so", RTLD_NOW | RTLD_GLOBAL);
    if (!hGLES) { log_write("[CHAMS] ERROR: GLESv2"); aml->ShowToast(false,"[CHAMS] FAIL: GLESv2"); return; }
    real_glDepthFunc    = (glDepthFunc_t)   dlsym(hGLES, "glDepthFunc");
    real_glDepthMask    = (glDepthMask_t)   dlsym(hGLES, "glDepthMask");
    real_glDrawElements = (glDrawElements_t)dlsym(hGLES, "glDrawElements");
    real_glUniform4fv   = (glUniform4fv_t)  dlsym(hGLES, "glUniform4fv");
    if (!real_glDepthFunc || !real_glDepthMask || !real_glDrawElements) {
        log_write("[CHAMS] ERROR: GL funcs");
        aml->ShowToast(false,"[CHAMS] FAIL: GLfuncs");
        return;
    }
    log_fmt("[CHAMS] glDrawElements=%p", (void*)real_glDrawElements);

    // GOT patch: glDepthFunc, glDepthMask, glDrawElements
    bool g1 = patch_got(base + GOT_glDepthFunc,    (void*)real_glDepthFunc);   // restore real
    bool g2 = patch_got(base + GOT_glDepthMask,    (void*)real_glDepthMask);   // restore real
    bool g3 = patch_got(base + GOT_glDrawElements, (void*)hooked_glDrawElements);
    bool g4 = patch_got(base + GOT_glUniform4fv,    (void*)hooked_glUniform4fv);
    log_fmt("[CHAMS] GOT Uniform4fv=%d", (int)g4);
    log_fmt("[CHAMS] GOT: DepthFunc=%d DepthMask=%d DrawElements=%d", (int)g1, (int)g2, (int)g3);
    if (!g3) { log_write("[CHAMS] ERROR: GOT DrawElements"); aml->ShowToast(false,"[CHAMS] FAIL: GOT"); return; }

    // Hook RenderPedCB + RenderPlayerCB
    void* addrPed    = (void*)((base + OFF_RenderPedCB)    | 1U);
    void* addrPlayer = (void*)((base + OFF_RenderPlayerCB) | 1U);
    int r1 = dobbyHook(addrPed,    (void*)((uintptr_t)hooked_RenderPedCB    | 1U), (void**)&orig_RenderPedCB);
    int r2 = dobbyHook(addrPlayer, (void*)((uintptr_t)hooked_RenderPlayerCB | 1U), (void**)&orig_RenderPlayerCB);
    log_fmt("[CHAMS] RenderPedCB hook=%d orig=%p", r1, (void*)orig_RenderPedCB);
    log_fmt("[CHAMS] RenderPlayerCB hook=%d orig=%p", r2, (void*)orig_RenderPlayerCB);
    if (r1 != 0) { log_write("[CHAMS] ERROR: PedCB"); aml->ShowToast(false,"[CHAMS] FAIL: PedCB"); return; }

    log_write("[CHAMS] All hooks OK v1.5");
    aml->ShowToast(false, "[CHAMS] v1.5 Loaded OK");
}
