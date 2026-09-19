#define _GNU_SOURCE

#define EGL_NO_X11 1
#define WL_EGL_PLATFORM 1

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include "teapot_gate.h"
#include "utah_teapot.h"
#include "xdg-shell-client-protocol.h"

/* Guest EGL Utah teapot using the Vulkan-backed Wayland swapchain. */

#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif

#ifndef WIN_W
#define WIN_W 560
#endif
#ifndef WIN_H
#define WIN_H 400
#endif
#ifndef GATE_WIN_W
#define GATE_WIN_W 720
#endif
#ifndef GATE_WIN_H
#define GATE_WIN_H 480
#endif
#define STRINGIFY_VALUE(value) #value
#define STRINGIFY(value) STRINGIFY_VALUE(value)
#define WARMUP 30
#define SAMPLE_SEC 2.0

typedef EGLDisplay (*teapot_get_platform_display_fn)(EGLenum platform,
                                                     void *native_display,
                                                     const EGLint *attrib_list);

static int running = 1;
static int win_w = WIN_W;
static int win_h = WIN_H;

#define result teapot_result
#define now_s teapot_now
#define env_flag teapot_env_flag
#define renderer_is_software(r) teapot_software_renderer((const char *)(r))
#define teapot_tint teapot_body_color

struct wl {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_compositor *comp;
    struct xdg_wm_base *wm;
    struct wl_surface *surf;
    struct xdg_surface *xdg_surf;
    struct xdg_toplevel *top;
    struct wl_egl_window *egl_win;
    int configured;
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t ver)
{
    struct wl *s = data;

    (void)ver;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        s->comp = wl_registry_bind(reg, name, &wl_compositor_interface, 4);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        s->wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_remove,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {
    wm_ping,
};

static void xdg_configure(void *data, struct xdg_surface *surf, uint32_t serial)
{
    struct wl *s = data;

    xdg_surface_ack_configure(surf, serial);
    s->configured = 1;
}

static const struct xdg_surface_listener xdg_listener = {
    xdg_configure,
};

static void top_configure(void *data, struct xdg_toplevel *top, int32_t w,
                          int32_t h, struct wl_array *states)
{
    struct wl *s = data;

    (void)top;
    (void)states;
    if (w > 0 && h > 0) {
        win_w = w;
        win_h = h;
        if (s->egl_win)
            wl_egl_window_resize(s->egl_win, w, h, 0, 0);
    }
}

static void top_close(void *data, struct xdg_toplevel *top)
{
    (void)data;
    (void)top;
    running = 0;
}

static void top_bounds(void *data, struct xdg_toplevel *top, int32_t w,
                       int32_t h)
{
    (void)data;
    (void)top;
    (void)w;
    (void)h;
}

static void top_caps(void *data, struct xdg_toplevel *top, struct wl_array *caps)
{
    (void)data;
    (void)top;
    (void)caps;
}

static const struct xdg_toplevel_listener top_listener = {
    top_configure,
    top_close,
    top_bounds,
    top_caps,
};

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok ? sh : 0;
}

static void draw_frame(GLuint prog, float seconds)
{
    float r, g, b;

    teapot_tint(&r, &g, &b);
    teapot_update(teapot_gate_enabled() ? 0.f : seconds,
                  (float)win_w / (float)win_h, r, g, b);
    glUseProgram(prog);
    glViewport(0, 0, win_w, win_h);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.05f, 0.06f, 0.14f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, teapot_clip());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, teapot_rgb());
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, teapot_nverts());
}

static int open_wayland(struct wl *s)
{
    int i;

    memset(s, 0, sizeof(*s));
    for (i = 0; i < 40 && !s->dpy; i++) {
        s->dpy = wl_display_connect(NULL);
        if (!s->dpy)
            usleep(250000);
    }
    if (!s->dpy)
        return -1;
    s->reg = wl_display_get_registry(s->dpy);
    wl_registry_add_listener(s->reg, &registry_listener, s);
    wl_display_roundtrip(s->dpy);
    if (!s->comp || !s->wm)
        return -1;
    xdg_wm_base_add_listener(s->wm, &wm_listener, s);
    s->surf = wl_compositor_create_surface(s->comp);
    s->xdg_surf = xdg_wm_base_get_xdg_surface(s->wm, s->surf);
    xdg_surface_add_listener(s->xdg_surf, &xdg_listener, s);
    s->top = xdg_surface_get_toplevel(s->xdg_surf);
    xdg_toplevel_add_listener(s->top, &top_listener, s);
    xdg_toplevel_set_app_id(s->top, "teapot");
    {
        const char *title = getenv("ARLINUX_WIN_TITLE");

        xdg_toplevel_set_title(s->top, title && title[0] ? title : "teapot");
    }
    wl_surface_commit(s->surf);
    for (i = 0; i < 50 && !s->configured; i++) {
        if (wl_display_dispatch(s->dpy) == -1)
            return -1;
    }
    return s->configured ? 0 : -1;
}

static void present(struct wl *s, EGLDisplay display, EGLSurface surface)
{
    if (!eglSwapBuffers(display, surface) ||
        wl_display_roundtrip(s->dpy) < 0) {
        result("teapot-egl-swap", false, "swap or Wayland connection failed");
        exit(1);
    }
}

int main(void)
{
    struct wl way;
    EGLDisplay edpy = EGL_NO_DISPLAY;
    EGLConfig cfg;
    EGLContext ctx = EGL_NO_CONTEXT;
    EGLSurface esurf = EGL_NO_SURFACE;
    EGLint n = 0;
    const EGLint window_cfg_es[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE,
    };
    const EGLint ctx_es[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE,
    };
    teapot_get_platform_display_fn get_plat;
    GLuint vs, fs, prog;
    static const char *vsrc =
        "precision mediump float;\n"
        "attribute vec3 a;\n"
        "attribute vec3 c;\n"
        "varying vec3 vc;\n"
        "void main(){ vc = c; gl_Position = vec4(a, 1.0); }\n";
    static const char *fsrc =
        "precision mediump float;\n"
        "varying vec3 vc;\n"
        "void main(){ gl_FragColor = vec4(vc, 1.0); }\n";
    int i;
    int frames = 0;
    double t0, t1, tbase;
    const GLubyte *renderer = NULL;

    printf("BXINFO path=wayland-egl BIONICX_GPU=%s swapchain=wayland\n",
           getenv("BIONICX_GPU") ? getenv("BIONICX_GPU") : "");
    fflush(stdout);

    if (open_wayland(&way) != 0) {
        result("teapot-egl-map", false,
               way.dpy ? "xdg configure" : "wl_display_connect");
        return 1;
    }
    result("teapot-egl-map", true, "xdg " STRINGIFY(WIN_W) "x" STRINGIFY(WIN_H));

    {
        eglBindAPI(EGL_OPENGL_ES_API);
        get_plat = (teapot_get_platform_display_fn)
            eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!get_plat)
            get_plat = (teapot_get_platform_display_fn)
                eglGetProcAddress("eglGetPlatformDisplay");
        if (get_plat)
            edpy = get_plat(EGL_PLATFORM_WAYLAND_KHR, way.dpy,
                            NULL);
        if (edpy == EGL_NO_DISPLAY)
            edpy = eglGetDisplay((EGLNativeDisplayType)way.dpy);
        if (edpy == EGL_NO_DISPLAY || !eglInitialize(edpy, NULL, NULL)) {
            result("teapot-egl-egl", false, "eglInitialize");
            return 1;
        }
        if (!eglChooseConfig(edpy, window_cfg_es, &cfg, 1, &n) || n < 1) {
            result("teapot-egl-egl", false, "eglChooseConfig window");
            return 1;
        }
        ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_es);
        way.egl_win = wl_egl_window_create(way.surf, WIN_W, WIN_H);
        if (!way.egl_win) return 1;
        esurf = eglCreateWindowSurface(edpy, cfg,
            (EGLNativeWindowType)way.egl_win, NULL);
        if (ctx == EGL_NO_CONTEXT || esurf == EGL_NO_SURFACE ||
            !eglMakeCurrent(edpy, esurf, esurf, ctx)) {
            result("teapot-egl-egl", false, "eglMakeCurrent window");
            return 1;
        }
        result("teapot-egl-egl", true, "Wayland EGL window swapchain");
    }

    renderer = glGetString(GL_RENDERER);
    printf("BXINFO gpu=%s vendor=%s renderer=%s version=%s\n",
           "zink",
           glGetString(GL_VENDOR) ? (const char *)glGetString(GL_VENDOR) : "",
           renderer ? (const char *)renderer : "",
           glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "");
    fflush(stdout);
    if (renderer_is_software(renderer)) {
        result("teapot-egl-hardware", false, "software renderer");
        return 1;
    }
    result("teapot-egl-hardware", true,
           renderer ? (const char *)renderer : "zink");

    vs = compile(GL_VERTEX_SHADER, vsrc);
    fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        result("teapot-egl-shader", false, "compile");
        return 1;
    }
    prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a");
    glBindAttribLocation(prog, 1, "c");
    glLinkProgram(prog);
    {
        GLint ok = 0;

        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            result("teapot-egl-shader", false, "link");
            return 1;
        }
    }
    result("teapot-egl-shader", true, NULL);
    if (teapot_ensure() <= 0) {
        result("teapot-egl-swap", false, "teapot mesh");
        return 1;
    }

    if (teapot_gate_enabled()) {
        teapot_gate_hello();
        for (i = 0; i < 8 && running; i++) {
            draw_frame(prog, 0.f);
            present(&way, edpy, esurf);
        }
        teapot_gate_size("present", win_w, win_h);
        teapot_gate_phase("present");
        xdg_toplevel_set_min_size(way.top, GATE_WIN_W, GATE_WIN_H);
        xdg_toplevel_set_max_size(way.top, GATE_WIN_W, GATE_WIN_H);
        if (way.egl_win)
            wl_egl_window_resize(way.egl_win, GATE_WIN_W, GATE_WIN_H, 0, 0);
        win_w = GATE_WIN_W;
        win_h = GATE_WIN_H;
        wl_surface_commit(way.surf);
        for (i = 0; i < 50; i++) {
            if (wl_display_roundtrip(way.dpy) < 0) {
                result("teapot-egl-swap", false, "resize roundtrip");
                return 1;
            }
            if (abs(win_w - GATE_WIN_W) <= 8 && abs(win_h - GATE_WIN_H) <= 8)
                break;
            usleep(40000);
        }
        draw_frame(prog, 0.f);
        present(&way, edpy, esurf);
        teapot_gate_size("resize", win_w, win_h);
        teapot_gate_phase("resize");
        teapot_gate_maps();
        result("teapot-egl-swap", true, "gate");
        teapot_gate_pass();
        return 0;
    }

    tbase = now_s();
    for (i = 0; i < WARMUP && running; i++) {
        draw_frame(prog, (float)(now_s() - tbase));
        present(&way, edpy, esurf);
    }

    t0 = now_s();
    do {
        draw_frame(prog, (float)(now_s() - tbase));
        present(&way, edpy, esurf);
        frames++;
        t1 = now_s();
    } while (running && t1 - t0 < SAMPLE_SEC);

    {
        double dt = t1 - t0;
        double fps = dt > 0.0 ? (double)frames / dt : 0.0;

        printf("BXINFO fps=%.2f frames=%d seconds=%.3f\n", fps, frames, dt);
        fflush(stdout);
    }
    result("teapot-egl-swap", frames > 0, NULL);
    if (env_flag("TEAPOT_ONCE") || !running)
        return frames > 0 ? 0 : 1;

    while (running) {
        draw_frame(prog, (float)(now_s() - tbase));
        present(&way, edpy, esurf);
    }
    return 0;
}
