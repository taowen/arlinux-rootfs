#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "teapot_gate.h"
#include "utah_teapot.h"

/*
 * Guest GLX Utah teapot rendered by Mesa Zink.
 * TEAPOT_ONCE=1 prints fps and exits.
 */

#ifndef GL_COMPILE_STATUS
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GLX_CONTEXT_MAJOR_VERSION_ARB
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif

GLuint glCreateShader(GLenum type);
void glShaderSource(GLuint shader, GLsizei count, const char *const *string,
                    const GLint *length);
void glCompileShader(GLuint shader);
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params);
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length,
                        char *infoLog);
GLuint glCreateProgram(void);
void glAttachShader(GLuint program, GLuint shader);
void glBindAttribLocation(GLuint program, GLuint index, const char *name);
void glLinkProgram(GLuint program);
void glGetProgramiv(GLuint program, GLenum pname, GLint *params);
void glUseProgram(GLuint program);
void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                           GLboolean normalized, GLsizei stride,
                           const void *pointer);
void glEnableVertexAttribArray(GLuint index);
const GLubyte *glGetString(GLenum name);
typedef int (*glXSwapIntervalMESA_fn)(unsigned int interval);
typedef GLXContext (*create_ctx_fn)(Display *, GLXFBConfig, GLXContext, Bool,
                                    const int *);

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
#define WARMUP 30
#define SAMPLE_SEC 2.0

static int x_errors;
static int running = 1;
static int win_w = WIN_W;
static int win_h = WIN_H;

#define result teapot_result
#define now_s teapot_now
#define env_flag teapot_env_flag
#define env_int teapot_env_int
#define renderer_is_software(r) teapot_software_renderer((const char *)(r))
#define teapot_tint teapot_body_color

static int on_x_error(Display *dpy, XErrorEvent *ev)
{
    char text[128];

    XGetErrorText(dpy, ev->error_code, text, sizeof(text));
    fprintf(stderr, "teapot-glx: XError code=%u req=%u %s\n",
            ev->error_code, ev->request_code, text);
    x_errors++;
    return 0;
}

static int has_glx_ext(const char *exts, const char *name)
{
    size_t n = strlen(name);
    const char *p = exts ? exts : "";

    while (*p) {
        if (strncmp(p, name, n) == 0 && (p[n] == '\0' || p[n] == ' '))
            return 1;
        while (*p && *p != ' ')
            p++;
        while (*p == ' ')
            p++;
    }
    return 0;
}

static GLXContext create_mesa_context(Display *dpy, int screen,
                                      XVisualInfo **visual)
{
    int fb_n = 0;
    int attrib[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 16,
        None,
    };
    int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
        GLX_CONTEXT_MINOR_VERSION_ARB, 1,
        None,
    };
    GLXFBConfig *fbs;
    create_ctx_fn create_ctx;
    const char *client_exts = glXGetClientString(dpy, GLX_EXTENSIONS);

    printf("BXINFO glx-client=%.240s\n", client_exts ? client_exts : "");
    fflush(stdout);
    create_ctx = (create_ctx_fn)glXGetProcAddressARB(
        (const GLubyte *)"glXCreateContextAttribsARB");
    if (!client_exts || !has_glx_ext(client_exts, "GLX_ARB_create_context") ||
        !create_ctx)
        return NULL;
    fbs = glXChooseFBConfig(dpy, screen, attrib, &fb_n);
    if (!fbs || fb_n < 1)
        return NULL;
    *visual = glXGetVisualFromFBConfig(dpy, fbs[0]);
    if (!*visual) {
        XFree(fbs);
        return NULL;
    }
    {
        GLXContext ctx = create_ctx(dpy, fbs[0], NULL, True, ctx_attribs);

        XFree(fbs);
        return ctx;
    }
}

static Window make_window(Display *dpy, int screen, Visual *visual, int depth)
{
    XSetWindowAttributes attrs;
    int x = env_int("ARLINUX_WIN_X", 40);
    int y = env_int("ARLINUX_WIN_Y", 40);

    attrs.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), visual,
                                     AllocNone);
    attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask;
    attrs.border_pixel = 0;
    attrs.background_pixel = 0;
    Window win = XCreateWindow(dpy, RootWindow(dpy, screen), x, y, WIN_W, WIN_H,
                         0, depth, InputOutput, visual,
                         CWColormap | CWEventMask | CWBorderPixel |
                             CWBackPixel,
                         &attrs);
    XSizeHints hints;
    Atom wm_delete;

    memset(&hints, 0, sizeof(hints));
    hints.flags = USPosition | PPosition | PSize;
    hints.x = x;
    hints.y = y;
    hints.width = WIN_W;
    hints.height = WIN_H;
    XSetWMNormalHints(dpy, win, &hints);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    return win;
}

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    char log[512];

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok)
        return sh;
    glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
    fprintf(stderr, "teapot-glx: shader compile: %s\n", log);
    return 0;
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
    glClearColor(0.10f, 0.09f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, teapot_clip());
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, teapot_rgb());
    glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, teapot_nverts());
}

static void pump_x(Display *dpy, Window win, Atom wm_delete)
{
    while (XPending(dpy)) {
        XEvent ev;

        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress) {
            KeySym ks = XLookupKeysym(&ev.xkey, 0);

            if (ks == XK_q || ks == XK_Q || ks == XK_Escape)
                running = 0;
        } else if (ev.type == ConfigureNotify && ev.xconfigure.window == win) {
            if (ev.xconfigure.width > 0 && ev.xconfigure.height > 0) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
            }
        } else if (ev.type == ClientMessage &&
                   (Atom)ev.xclient.data.l[0] == wm_delete &&
                   ev.xclient.window == win) {
            running = 0;
        }
    }
    if (x_errors) {
        result("teapot-glx-swap", false, "X11/GLX error");
        exit(1);
    }
}

static int wait_size(Display *dpy, Window win, int width, int height)
{
    int i;

    for (i = 0; i < 50; i++) {
        pump_x(dpy, win, None);
        if (abs(win_w - width) <= 8 && abs(win_h - height) <= 8)
            return 0;
        XSync(dpy, False);
        usleep(40000);
    }
    return -1;
}

int main(void)
{
    Display *dpy = NULL;
    int screen;
    int attrib[] = {
        GLX_RGBA, GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        GLX_DEPTH_SIZE, 16,
        None,
    };
    XVisualInfo *visual = NULL;
    GLXContext ctx = NULL;
    Window win;
    Atom wm_delete;
    GLuint vs, fs, prog;
    static const char *vsrc_gl =
        "attribute vec3 a;\n"
        "attribute vec3 c;\n"
        "varying vec3 vc;\n"
        "void main(){ vc = c; gl_Position = vec4(a, 1.0); }\n";
    static const char *fsrc_gl =
        "varying vec3 vc;\n"
        "void main(){ gl_FragColor = vec4(vc, 1.0); }\n";
    const char *vsrc;
    const char *fsrc;
    int i;
    int frames = 0;
    double t0, t1, tbase;
    const GLubyte *vendor = NULL;
    const GLubyte *renderer = NULL;
    const GLubyte *version = NULL;

    XInitThreads();
    for (i = 0; i < 40 && !dpy; i++) {
        dpy = XOpenDisplay(NULL);
        if (!dpy)
            usleep(250000);
    }
    if (!dpy) {
        result("teapot-glx-display", false, "XOpenDisplay");
        return 1;
    }
    XSetErrorHandler(on_x_error);
    screen = DefaultScreen(dpy);
    printf("BXINFO path=x11-glx BIONICX_GPU=%s TEAPOT_KIND=%s LIBGL_DRIVERS_PATH=%s MESA_LOADER=%s\n",
           getenv("BIONICX_GPU") ? getenv("BIONICX_GPU") : "",
           getenv("TEAPOT_KIND") ? getenv("TEAPOT_KIND") : "",
           getenv("LIBGL_DRIVERS_PATH") ? getenv("LIBGL_DRIVERS_PATH") : "",
           getenv("MESA_LOADER_DRIVER_OVERRIDE") ? getenv("MESA_LOADER_DRIVER_OVERRIDE") : "");
    fflush(stdout);
    ctx = create_mesa_context(dpy, screen, &visual);
    if (!visual)
        visual = glXChooseVisual(dpy, screen, attrib);
    if (!visual) {
        result("teapot-glx-visual", false, "glXChooseVisual");
        return 1;
    }
    if (!ctx)
        ctx = glXCreateContext(dpy, visual, NULL, True);
    if (!ctx) {
        result("teapot-glx-context", false,
               "glXCreateContextAttribsARB");
        return 1;
    }
    win = make_window(dpy, screen, visual->visual, visual->depth);
    wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    {
        const char *title = getenv("ARLINUX_WIN_TITLE");

        XStoreName(dpy, win, title && title[0] ? title : "teapot");
    }
    XMapRaised(dpy, win);
    XSync(dpy, False);
    if (x_errors || !win || !glXMakeCurrent(dpy, win, ctx)) {
        result("teapot-glx-current", false, "glXMakeCurrent");
        return 1;
    }
    result("teapot-glx-current", true, NULL);
    {
        glXSwapIntervalMESA_fn swapint = (glXSwapIntervalMESA_fn)
            glXGetProcAddress((const GLubyte *)"glXSwapIntervalMESA");

        if (swapint)
            (void)swapint(0);
    }

    {
        vendor = glGetString(GL_VENDOR);
        renderer = glGetString(GL_RENDERER);
        version = glGetString(GL_VERSION);
        printf("BXINFO gpu=mesa vendor=%s renderer=%s version=%s\n",
               vendor ? (const char *)vendor : "",
               renderer ? (const char *)renderer : "",
               version ? (const char *)version : "");
        fflush(stdout);
        if (renderer_is_software(renderer)) {
            result("teapot-glx-hardware", false, "software renderer");
            return 1;
        }
        result("teapot-glx-hardware", true,
               renderer ? (const char *)renderer : "mesa");
    }

    vsrc = vsrc_gl;
    fsrc = fsrc_gl;
    vs = compile(GL_VERTEX_SHADER, vsrc);
    fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        result("teapot-glx-shader", false, "compile");
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
            result("teapot-glx-shader", false, "link");
            return 1;
        }
    }
    result("teapot-glx-shader", true, NULL);
    if (teapot_ensure() <= 0) {
        result("teapot-glx-swap", false, "teapot mesh");
        return 1;
    }

    if (teapot_gate_enabled()) {
        teapot_gate_hello();
        for (i = 0; i < 8 && running; i++) {
            draw_frame(prog, 0.f);
            glXSwapBuffers(dpy, win);
            pump_x(dpy, win, wm_delete);
        }
        teapot_gate_size("present", win_w, win_h);
        teapot_gate_phase("present");
        XResizeWindow(dpy, win, GATE_WIN_W, GATE_WIN_H);
        XFlush(dpy);
        if (wait_size(dpy, win, GATE_WIN_W, GATE_WIN_H) != 0) {
            result("teapot-glx-swap", false, "resize configure");
            return 1;
        }
        draw_frame(prog, 0.f);
        glXSwapBuffers(dpy, win);
        pump_x(dpy, win, wm_delete);
        teapot_gate_size("resize", win_w, win_h);
        teapot_gate_phase("resize");
        teapot_gate_maps();
        result("teapot-glx-swap", true, "gate");
        teapot_gate_pass();
        return 0;
    }

    tbase = now_s();
    for (i = 0; i < WARMUP && running; i++) {
        draw_frame(prog, (float)(now_s() - tbase));
        glXSwapBuffers(dpy, win);
        pump_x(dpy, win, wm_delete);
    }

    t0 = now_s();
    do {
        draw_frame(prog, (float)(now_s() - tbase));
        glXSwapBuffers(dpy, win);
        frames++;
        pump_x(dpy, win, wm_delete);
        t1 = now_s();
    } while (running && t1 - t0 < SAMPLE_SEC);

    {
        double dt = t1 - t0;
        double fps = dt > 0.0 ? (double)frames / dt : 0.0;

        printf("BXINFO fps=%.2f frames=%d seconds=%.3f\n", fps, frames, dt);
        fflush(stdout);
    }
    result("teapot-glx-swap", frames > 0, NULL);
    if (env_flag("TEAPOT_ONCE") || !running)
        return frames > 0 ? 0 : 1;

    while (running) {
        draw_frame(prog, (float)(now_s() - tbase));
        glXSwapBuffers(dpy, win);
        pump_x(dpy, win, wm_delete);
    }
    return 0;
}
