/*
 * libchams v1.6 — GTA SA Android Chams Mod
 * Author : brruham
 * Approach: replace skin texture dengan 1x1 solid color texture
 *           + glDrawElements/glDrawArrays double-render untuk through-wall
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

// Warna chams: RGBA 0-255
#define CHAMS_R_B  0
#define CHAMS_G_B  255
#define CHAMS_B_B  0
#define CHAMS_A_B  255

// Offsets libGTASA.so
#define OFF_RenderPedCB     0x005d605cU
#define OFF_RenderPlayerCB  0x005d602cU

// GOT offsets
#define GOT_glDepthFunc     0x0066ed70U
#define GOT_glDepthMask     0x00670094U
#define GOT_glDrawElements  0x00674b80U
#define GOT_glDrawArrays    0x0066ea00U
#define GOT_glBindTexture   0x00672264U

MYMOD(brruham.libchams, ChamsMod, 1.6, brruham)

typedef void*  RpAtomic;
typedef void* (*RenderCB_t)(RpAtomic*);
typedef void  (*glDepthFunc_t)(GLenum);
typedef void  (*glDepthMask_t)(GLboolean);
typedef void  (*glDrawElements_t)(GLenum, GLsizei, GLenum, const void*);
typedef void  (*glDrawArrays_t)(GLenum, GLint, GLsizei);
typedef void  (*glBindTexture_t)(GLenum, GLuint);
typedef void  (*glGenTextures_t)(GLsizei, GLuint*);
typedef void  (*glTexImage2D_t)(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*);
typedef void  (*glTexParameteri_t)(GLenum, GLenum, GLint);

static RenderCB_t       orig_RenderPedCB    = nullptr;
static RenderCB_t       orig_RenderPlayerCB = nullptr;
static glDepthFunc_t    real_glDepthFunc    = nullptr;
static glDepthMask_t    real_glDepthMask    = nullptr;
static glDrawElements_t real_glDrawElements = nullptr;
static glDrawArrays_t   real_glDrawArrays   = nullptr;
static glBindTexture_t  real_glBindTexture  = nullptr;
static glGenTextures_t  real_glGenTextures  = nullptr;
static glTexImage2D_t   real_glTexImage2D   = nullptr;
static glTexParameteri_t real_glTexParameteri = nullptr;

static bool   g_chamsEnabled  = true;
static bool   g_inPedRender   = false;
static GLuint g_chamsTexture  = 0;
static int    g_callCount     = 0;

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

static bool patch_got(uintptr_t addr, void* fn) {
    uintptr_t page = addr & ~0xFFFU;
    if (mprotect((void*)page, 0x1000, PROT_READ | PROT_WRITE) != 0) return false;
    *(void**)addr = fn;
    __builtin___clear_cache((char*)addr, (char*)addr + 4);
    return true;
}

// Buat 1x1 solid color texture — dipanggil lazy saat pertama render ped
static void init_chams_texture() {
    if (g_chamsTexture != 0) return;

    real_glGenTextures(1, &g_chamsTexture);
    real_glBindTexture(GL_TEXTURE_2D, g_chamsTexture);

    GLubyte pixel[4] = { CHAMS_R_B, CHAMS_G_B, CHAMS_B_B, CHAMS_A_B };
    real_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                      GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    real_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    log_fmt("[CHAMS] chamsTexture created id=%u", g_chamsTexture);
}

// Hook glBindTexture — ganti skin texture saat ped render
static void hooked_glBindTexture(GLenum target, GLuint texture) {
    if (g_inPedRender && g_chamsEnabled && target == GL_TEXTURE_2D && g_chamsTexture != 0)
        real_glBindTexture(target, g_chamsTexture);
    else
        real_glBindTexture(target, texture);
}

// Double-render helper
static inline void do_draw_elements(GLenum mode, GLsizei count, GLenum type, const void* idx) {
    if (!g_inPedRender || !g_chamsEnabled) {
        real_glDrawElements(mode, count, type, idx);
        return;
    }
    real_glDepthFunc(GL_ALWAYS);
    real_glDepthMask(GL_FALSE);
    real_glDrawElements(mode, count, type, idx);
    real_glDepthFunc(GL_LEQUAL);
    real_glDepthMask(GL_TRUE);
    real_glDrawElements(mode, count, type, idx);
}

static inline void do_draw_arrays(GLenum mode, GLint first, GLsizei count) {
    if (!g_inPedRender || !g_chamsEnabled) {
        real_glDrawArrays(mode, first, count);
        return;
    }
    real_glDepthFunc(GL_ALWAYS);
    real_glDepthMask(GL_FALSE);
    real_glDrawArrays(mode, first, count);
    real_glDepthFunc(GL_LEQUAL);
    real_glDepthMask(GL_TRUE);
    real_glDrawArrays(mode, first, count);
}

static void hooked_glDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    do_draw_elements(m, c, t, i);
}
static void hooked_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    do_draw_arrays(m, f, c);
}

// RenderPedCB — set flag + lazy init texture
static void* hooked_RenderPedCB(RpAtomic* a) {
    if (!orig_RenderPedCB) return nullptr;

    g_callCount++;
    if (g_callCount == 1) {
        init_chams_texture();
        log_write("[CHAMS] first RenderPedCB, texture init done");
    }

    if (!g_chamsEnabled) return orig_RenderPedCB(a);

    g_inPedRender = true;
    void* r = orig_RenderPedCB(a);
    g_inPedRender = false;
    return r;
}

static void* hooked_RenderPlayerCB(RpAtomic* a) {
    if (!orig_RenderPlayerCB) return orig_RenderPlayerCB ? orig_RenderPlayerCB(a) : nullptr;
    if (!g_chamsEnabled) return orig_RenderPlayerCB(a);
    g_inPedRender = true;
    void* r = orig_RenderPlayerCB(a);
    g_inPedRender = false;
    return r;
}

ON_MOD_PRELOAD() {
    remove(LOGFILE);
    log_write("[CHAMS] PreLoad v1.6");
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
    real_glDrawArrays   = (glDrawArrays_t)  dlsym(hGLES, "glDrawArrays");
    real_glBindTexture  = (glBindTexture_t) dlsym(hGLES, "glBindTexture");
    real_glGenTextures  = (glGenTextures_t) dlsym(hGLES, "glGenTextures");
    real_glTexImage2D   = (glTexImage2D_t)  dlsym(hGLES, "glTexImage2D");
    real_glTexParameteri= (glTexParameteri_t)dlsym(hGLES,"glTexParameteri");

    if (!real_glDepthFunc || !real_glDrawElements || !real_glBindTexture || !real_glGenTextures) {
        log_write("[CHAMS] ERROR: GL funcs missing");
        aml->ShowToast(false,"[CHAMS] FAIL: GLfuncs");
        return;
    }

    // GOT patches
    bool r1 = patch_got(base + GOT_glDrawElements, (void*)hooked_glDrawElements);
    bool r2 = patch_got(base + GOT_glDrawArrays,   (void*)hooked_glDrawArrays);
    bool r3 = patch_got(base + GOT_glBindTexture,  (void*)hooked_glBindTexture);
    log_fmt("[CHAMS] GOT DrawElem=%d DrawArrays=%d BindTex=%d", (int)r1,(int)r2,(int)r3);
    if (!r1 || !r3) { log_write("[CHAMS] ERROR: GOT patch"); aml->ShowToast(false,"[CHAMS] FAIL: GOT"); return; }

    // Dobby hooks RenderPedCB + RenderPlayerCB
    void* addrPed    = (void*)((base + OFF_RenderPedCB)    | 1U);
    void* addrPlayer = (void*)((base + OFF_RenderPlayerCB) | 1U);
    int h1 = dobbyHook(addrPed,    (void*)((uintptr_t)hooked_RenderPedCB    | 1U), (void**)&orig_RenderPedCB);
    int h2 = dobbyHook(addrPlayer, (void*)((uintptr_t)hooked_RenderPlayerCB | 1U), (void**)&orig_RenderPlayerCB);
    log_fmt("[CHAMS] PedCB hook=%d orig=%p", h1, (void*)orig_RenderPedCB);
    log_fmt("[CHAMS] PlayerCB hook=%d orig=%p", h2, (void*)orig_RenderPlayerCB);
    if (h1 != 0) { log_write("[CHAMS] ERROR: PedCB"); aml->ShowToast(false,"[CHAMS] FAIL: PedCB"); return; }

    log_write("[CHAMS] All hooks OK v1.6");
    aml->ShowToast(false, "[CHAMS] v1.6 Loaded OK");
}
