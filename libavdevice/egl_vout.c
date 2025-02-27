/*
 * Copyright (c) 2020 John Cox for Raspberry Pi Trading
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */


// *** This module is a work in progress and its utility is strictly
//     limited to testing.
//     Amongst other issues it doesn't wait for the pic to be displayed before
//     returning the buffer so flikering does occur.

#include <epoxy/gl.h>
#include <epoxy/egl.h>

#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/hwcontext_drm.h"
#include "libavformat/internal.h"
#include "avdevice.h"

#include "pthread.h"
#include <semaphore.h>
#include <stdatomic.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "libavutil/rpi_sand_fns.h"

#define TRACE_ALL 0

struct egl_setup {
    int conId;

    Display *dpy;
    EGLDisplay egl_dpy;
    EGLContext ctx;
    EGLSurface surf;
    Window win;

    uint32_t crtcId;
    int crtcIdx;
    uint32_t planeId;
    struct {
        int x, y, width, height;
    } compose;
};

typedef struct egl_aux_s {
    int fd;
    GLuint texture;

} egl_aux_t;

typedef struct egl_display_env_s {
    AVClass *class;

    struct egl_setup setup;
    enum AVPixelFormat avfmt;

    int show_all;
    int window_width, window_height;
    int window_x, window_y;
    int fullscreen;

    egl_aux_t aux[32];

    pthread_t q_thread;
    pthread_mutex_t q_lock;
    sem_t display_start_sem;
    sem_t q_sem;
    int q_terminate;
    AVFrame *q_this;
    AVFrame *q_next;

} egl_display_env_t;


/**
 * Remove window border/decorations.
 */
static void
no_border(Display *dpy, Window w)
{
    static const unsigned MWM_HINTS_DECORATIONS = (1 << 1);
    static const int PROP_MOTIF_WM_HINTS_ELEMENTS = 5;

    typedef struct {
        unsigned long       flags;
        unsigned long       functions;
        unsigned long       decorations;
        long                inputMode;
        unsigned long       status;
    } PropMotifWmHints;

    PropMotifWmHints motif_hints;
    Atom prop, proptype;
    unsigned long flags = 0;

    /* setup the property */
    motif_hints.flags = MWM_HINTS_DECORATIONS;
    motif_hints.decorations = flags;

    /* get the atom for the property */
    prop = XInternAtom(dpy, "_MOTIF_WM_HINTS", True);
    if (!prop) {
        /* something went wrong! */
        return;
    }

    /* not sure this is correct, seems to work, XA_WM_HINTS didn't work */
    proptype = prop;

    XChangeProperty(dpy, w,                         /* display, window */
                    prop, proptype,                 /* property, type */
                    32,                             /* format: 32-bit datums */
                    PropModeReplace,                /* mode */
                    (unsigned char *)&motif_hints, /* data */
                    PROP_MOTIF_WM_HINTS_ELEMENTS    /* nelements */
                   );
}


/*
 * Create an RGB, double-buffered window.
 * Return the window and context handles.
 */
static int
make_window(struct AVFormatContext *const s,
            egl_display_env_t *const de,
            Display *dpy, EGLDisplay egl_dpy, const char *name,
            Window *winRet, EGLContext *ctxRet, EGLSurface *surfRet)
{
    int scrnum = DefaultScreen(dpy);
    XSetWindowAttributes attr;
    unsigned long mask;
    Window root = RootWindow(dpy, scrnum);
    Window win;
    EGLContext ctx;
    const int fullscreen = de->fullscreen;
    EGLConfig config;
    int x = de->window_x;
    int y = de->window_y;
    int width = de->window_width ? de->window_width : 1280;
    int height = de->window_height ? de->window_height : 720;


    if (fullscreen) {
        int scrnum = DefaultScreen(dpy);

        x = 0; y = 0;
        width = DisplayWidth(dpy, scrnum);
        height = DisplayHeight(dpy, scrnum);
    }

    {
        EGLint num_configs;
        static const EGLint attribs[] = {
            EGL_RED_SIZE, 1,
            EGL_GREEN_SIZE, 1,
            EGL_BLUE_SIZE, 1,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };

        if (!eglChooseConfig(egl_dpy, attribs, &config, 1, &num_configs)) {
            av_log(s, AV_LOG_ERROR, "Error: couldn't get an EGL visual config\n");
            return -1;
        }
    }

    {
        EGLint vid;
        if (!eglGetConfigAttrib(egl_dpy, config, EGL_NATIVE_VISUAL_ID, &vid)) {
            av_log(s, AV_LOG_ERROR, "Error: eglGetConfigAttrib() failed\n");
            return -1;
        }

        {
            XVisualInfo visTemplate = {
                .visualid = vid,
            };
            int num_visuals;
            XVisualInfo *visinfo = XGetVisualInfo(dpy, VisualIDMask,
                                                  &visTemplate, &num_visuals);

            /* window attributes */
            attr.background_pixel = 0;
            attr.border_pixel = 0;
            attr.colormap = XCreateColormap(dpy, root, visinfo->visual, AllocNone);
            attr.event_mask = StructureNotifyMask | ExposureMask | KeyPressMask;
            /* XXX this is a bad way to get a borderless window! */
            mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;

            win = XCreateWindow(dpy, root, x, y, width, height,
                                0, visinfo->depth, InputOutput,
                                visinfo->visual, mask, &attr);
            XFree(visinfo);
        }
    }

    if (fullscreen)
        no_border(dpy, win);

    /* set hints and properties */
    {
        XSizeHints sizehints;
        sizehints.x = x;
        sizehints.y = y;
        sizehints.width  = width;
        sizehints.height = height;
        sizehints.flags = USSize | USPosition;
        XSetNormalHints(dpy, win, &sizehints);
        XSetStandardProperties(dpy, win, name, name,
                               None, (char **)NULL, 0, &sizehints);
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    {
        static const EGLint ctx_attribs[] = {
            EGL_CONTEXT_CLIENT_VERSION, 2,
            EGL_NONE
        };
        ctx = eglCreateContext(egl_dpy, config, EGL_NO_CONTEXT, ctx_attribs);
        if (!ctx) {
            av_log(s, AV_LOG_ERROR, "Error: eglCreateContext failed\n");
            return -1;
        }
    }


    XMapWindow(dpy, win);

    {
        EGLSurface surf = eglCreateWindowSurface(egl_dpy, config, (EGLNativeWindowType)win, NULL);
        if (!surf) {
            av_log(s, AV_LOG_ERROR, "Error: eglCreateWindowSurface failed\n");
            return -1;
        }

        if (!eglMakeCurrent(egl_dpy, surf, surf, ctx)) {
            av_log(s, AV_LOG_ERROR, "Error: eglCreateContext failed\n");
            return -1;
        }

        *winRet = win;
        *ctxRet = ctx;
        *surfRet = surf;
    }

    return 0;
}

static GLint
compile_shader(struct AVFormatContext *const avctx, GLenum target, const char *source)
{
    GLuint s = glCreateShader(target);

    if (s == 0) {
        av_log(avctx, AV_LOG_ERROR, "Failed to create shader\n");
        return 0;
    }

    glShaderSource(s, 1, (const GLchar **)&source, NULL);
    glCompileShader(s);

    {
        GLint ok;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

        if (!ok) {
            GLchar *info;
            GLint size;

            glGetShaderiv(s, GL_INFO_LOG_LENGTH, &size);
            info = malloc(size);

            glGetShaderInfoLog(s, size, NULL, info);
            av_log(avctx, AV_LOG_ERROR, "Failed to compile shader: %ssource:\n%s\n", info, source);

            return 0;
        }
    }

    return s;
}

static GLuint link_program(struct AVFormatContext *const s, GLint vs, GLint fs)
{
    GLuint prog = glCreateProgram();

    if (prog == 0) {
        av_log(s, AV_LOG_ERROR, "Failed to create program\n");
        return 0;
    }

    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    {
        GLint ok;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            /* Some drivers return a size of 1 for an empty log.  This is the size
             * of a log that contains only a terminating NUL character.
             */
            GLint size;
            GLchar *info = NULL;
            glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &size);
            if (size > 1) {
                info = malloc(size);
                glGetProgramInfoLog(prog, size, NULL, info);
            }

            av_log(s, AV_LOG_ERROR, "Failed to link: %s\n",
                   (info != NULL) ? info : "<empty log>");
            return 0;
        }
    }

    return prog;
}

static int
gl_setup(struct AVFormatContext *const s)
{
    const char *vs =
        "attribute vec4 pos;\n"
        "varying vec2 texcoord;\n"
        "\n"
        "void main() {\n"
        "  gl_Position = pos;\n"
        "  texcoord.x = (pos.x + 1.0) / 2.0;\n"
        "  texcoord.y = (-pos.y + 1.0) / 2.0;\n"
        "}\n";
    const char *fs =
        "#extension GL_OES_EGL_image_external : enable\n"
        "precision mediump float;\n"
        "uniform samplerExternalOES s;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(s, texcoord);\n"
        "}\n";

    GLuint vs_s;
    GLuint fs_s;
    GLuint prog;

    if (!(vs_s = compile_shader(s, GL_VERTEX_SHADER, vs)) ||
        !(fs_s = compile_shader(s, GL_FRAGMENT_SHADER, fs)) ||
        !(prog = link_program(s, vs_s, fs_s)))
        return -1;

    glUseProgram(prog);

    {
        static const float verts[] = {
            -1, -1,
            1, -1,
            1,  1,
            -1,  1,
        };
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
    }

    glEnableVertexAttribArray(0);
    return 0;
}

static int egl_vout_write_trailer(AVFormatContext *s)
{
#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "%s\n", __func__);
#endif

    return 0;
}

static int egl_vout_write_header(AVFormatContext *s)
{
    const AVCodecParameters *const par = s->streams[0]->codecpar;

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "%s\n", __func__);
#endif
    if (s->nb_streams > 1
        || par->codec_type != AVMEDIA_TYPE_VIDEO
        || par->codec_id   != AV_CODEC_ID_WRAPPED_AVFRAME) {
        av_log(s, AV_LOG_ERROR, "Only supports one wrapped avframe stream\n");
        return AVERROR(EINVAL);
    }

    return 0;
}


static int do_display(AVFormatContext *const s, egl_display_env_t *const de, AVFrame *const frame)
{
    const AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    egl_aux_t *da = NULL;
    unsigned int i;

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "<<< %s\n", __func__);
#endif

    for (i = 0; i != 32; ++i) {
        if (de->aux[i].fd == -1 || de->aux[i].fd == desc->objects[0].fd) {
            da = de->aux + i;
            break;
        }
    }

    if (da == NULL) {
        av_log(s, AV_LOG_INFO, "%s: Out of handles\n", __func__);
        return AVERROR(EINVAL);
    }

    if (da->texture == 0) {
        EGLint attribs[50];
        EGLint *a = attribs;
        int i, j;
        static const EGLint anames[] = {
            EGL_DMA_BUF_PLANE0_FD_EXT,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE1_FD_EXT,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
            EGL_DMA_BUF_PLANE2_FD_EXT,
            EGL_DMA_BUF_PLANE2_OFFSET_EXT,
            EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT,
        };
        const EGLint *b = anames;

        *a++ = EGL_WIDTH;
        *a++ = av_frame_cropped_width(frame);
        *a++ = EGL_HEIGHT;
        *a++ = av_frame_cropped_height(frame);
        *a++ = EGL_LINUX_DRM_FOURCC_EXT;
        *a++ = desc->layers[0].format;

        for (i = 0; i < desc->nb_layers; ++i) {
            for (j = 0; j < desc->layers[i].nb_planes; ++j) {
                const AVDRMPlaneDescriptor *const p = desc->layers[i].planes + j;
                const AVDRMObjectDescriptor *const obj = desc->objects + p->object_index;
                *a++ = *b++;
                *a++ = obj->fd;
                *a++ = *b++;
                *a++ = p->offset;
                *a++ = *b++;
                *a++ = p->pitch;
                if (obj->format_modifier == 0) {
                    b += 2;
                }
                else {
                    *a++ = *b++;
                    *a++ = (EGLint)(obj->format_modifier & 0xFFFFFFFF);
                    *a++ = *b++;
                    *a++ = (EGLint)(obj->format_modifier >> 32);
                }
            }
        }

        *a = EGL_NONE;

#if TRACE_ALL
        for (a = attribs, i = 0; *a != EGL_NONE; a += 2, ++i) {
            av_log(s, AV_LOG_INFO, "[%2d] %4x: %d\n", i, a[0], a[1]);
        }
#endif
        {
            const EGLImage image = eglCreateImageKHR(de->setup.egl_dpy,
                                                     EGL_NO_CONTEXT,
                                                     EGL_LINUX_DMA_BUF_EXT,
                                                     NULL, attribs);
            if (!image) {
                av_log(s, AV_LOG_ERROR, "Failed to import fd %d\n", desc->objects[0].fd);
                return -1;
            }

            glGenTextures(1, &da->texture);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, da->texture);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

            eglDestroyImageKHR(de->setup.egl_dpy, image);
        }

        da->fd = desc->objects[0].fd;
    }

    glClearColor(0.5, 0.5, 0.5, 0.5);
    glClear(GL_COLOR_BUFFER_BIT);

    glBindTexture(GL_TEXTURE_EXTERNAL_OES, da->texture);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    eglSwapBuffers(de->setup.egl_dpy, de->setup.surf);

    glDeleteTextures(1, &da->texture);
    da->texture = 0;
    da->fd = -1;

    return 0;
}

static void* display_thread(void *v)
{
    AVFormatContext *const s = v;
    egl_display_env_t *const de = s->priv_data;

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "<<< %s\n", __func__);
#endif
    {
        EGLint egl_major, egl_minor;

        de->setup.dpy = XOpenDisplay(NULL);
        if (!de->setup.dpy) {
            av_log(s, AV_LOG_ERROR, "Couldn't open X display\n");
            goto fail;
        }

        de->setup.egl_dpy = eglGetDisplay(de->setup.dpy);
        if (!de->setup.egl_dpy) {
            av_log(s, AV_LOG_ERROR, "eglGetDisplay() failed\n");
            goto fail;
        }

        if (!eglInitialize(de->setup.egl_dpy, &egl_major, &egl_minor)) {
            av_log(s, AV_LOG_ERROR, "Error: eglInitialize() failed\n");
            goto fail;
        }

        av_log(s, AV_LOG_INFO, "EGL version %d.%d\n", egl_major, egl_minor);

        if (!epoxy_has_egl_extension(de->setup.egl_dpy, "EGL_KHR_image_base")) {
            av_log(s, AV_LOG_ERROR, "Missing EGL KHR image extension\n");
            goto fail;
        }
    }

    if (!de->window_width || !de->window_height) {
        de->window_width = 1280;
        de->window_height = 720;
    }
    if (make_window(s, de, de->setup.dpy, de->setup.egl_dpy, "ffmpeg-vout",
                    &de->setup.win, &de->setup.ctx, &de->setup.surf)) {
        av_log(s, AV_LOG_ERROR, "%s: make_window failed\n", __func__);
        goto fail;
    }

    if (gl_setup(s)) {
        av_log(s, AV_LOG_ERROR, "%s: gl_setup failed\n", __func__);
        goto fail;
    }

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "--- %s: Start done\n", __func__);
#endif
    sem_post(&de->display_start_sem);

    for (;;) {
        AVFrame *frame;

        while (sem_wait(&de->q_sem) != 0) {
            av_assert0(errno == EINTR);
        }

        if (de->q_terminate)
            break;

        pthread_mutex_lock(&de->q_lock);
        frame = de->q_next;
        de->q_next = NULL;
        pthread_mutex_unlock(&de->q_lock);

        do_display(s, de, frame);

        av_frame_free(&de->q_this);
        de->q_this = frame;
    }

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, ">>> %s\n", __func__);
#endif

    return NULL;

fail:
#if TRACE_ALL
    av_log(s, AV_LOG_INFO, ">>> %s: FAIL\n", __func__);
#endif
    de->q_terminate = 1;
    sem_post(&de->display_start_sem);

    return NULL;
}

static int egl_vout_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    const AVFrame *const src_frame = (AVFrame *)pkt->data;
    AVFrame *frame;
    egl_display_env_t *const de = s->priv_data;

#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "%s\n", __func__);
#endif

    if (src_frame->format == AV_PIX_FMT_DRM_PRIME) {
        frame = av_frame_alloc();
        av_frame_ref(frame, src_frame);
    }
    else if (src_frame->format == AV_PIX_FMT_VAAPI) {
        frame = av_frame_alloc();
        frame->format = AV_PIX_FMT_DRM_PRIME;
        if (av_hwframe_map(frame, src_frame, 0) != 0) {
            av_log(s, AV_LOG_WARNING, "Failed to map frame (format=%d) to DRM_PRiME\n", src_frame->format);
            av_frame_free(&frame);
            return AVERROR(EINVAL);
        }
    }
    else {
        av_log(s, AV_LOG_WARNING, "Frame (format=%d) not DRM_PRiME\n", src_frame->format);
        return AVERROR(EINVAL);
    }

    // Really hacky sync
    while (de->show_all && de->q_next) {
        usleep(3000);
    }

    pthread_mutex_lock(&de->q_lock);
    {
        AVFrame *const t = de->q_next;
        de->q_next = frame;
        frame = t;
    }
    pthread_mutex_unlock(&de->q_lock);

    if (frame == NULL)
        sem_post(&de->q_sem);
    else
        av_frame_free(&frame);

    return 0;
}

static int egl_vout_write_frame(AVFormatContext *s, int stream_index, AVFrame **ppframe,
                                unsigned flags)
{
#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "%s: idx=%d, flags=%#x\n", __func__, stream_index, flags);
#endif

    /* egl_vout_write_header() should have accepted only supported formats */
    if ((flags & AV_WRITE_UNCODED_FRAME_QUERY))
        return 0;

    return 0;
}

static int egl_vout_control_message(AVFormatContext *s, int type, void *data, size_t data_size)
{
#if TRACE_ALL
    av_log(s, AV_LOG_INFO, "%s: %d\n", __func__, type);
#endif
    switch (type) {
    case AV_APP_TO_DEV_WINDOW_REPAINT:
        return 0;
    default:
        break;
    }
    return AVERROR(ENOSYS);
}

// deinit is called if init fails so no need to clean up explicity here
static int egl_vout_init(struct AVFormatContext *s)
{
    egl_display_env_t *const de = s->priv_data;
    unsigned int i;

    av_log(s, AV_LOG_DEBUG, "<<< %s\n", __func__);

    de->setup = (struct egl_setup) { 0 };

    for (i = 0; i != 32; ++i) {
        de->aux[i].fd = -1;
    }

    de->q_terminate = 0;
    pthread_mutex_init(&de->q_lock, NULL);
    sem_init(&de->q_sem, 0, 0);
    sem_init(&de->display_start_sem, 0, 0);
    av_assert0(pthread_create(&de->q_thread, NULL, display_thread, s) == 0);

    sem_wait(&de->display_start_sem);
    if (de->q_terminate) {
        av_log(s, AV_LOG_ERROR, "%s: Display startup failure\n", __func__);
        return -1;
    }

    av_log(s, AV_LOG_DEBUG, ">>> %s\n", __func__);

    return 0;
}

static void egl_vout_deinit(struct AVFormatContext *s)
{
    egl_display_env_t *const de = s->priv_data;

    av_log(s, AV_LOG_DEBUG, "<<< %s\n", __func__);

    de->q_terminate = 1;
    sem_post(&de->q_sem);
    pthread_join(de->q_thread, NULL);
    sem_destroy(&de->q_sem);
    pthread_mutex_destroy(&de->q_lock);

    av_frame_free(&de->q_next);
    av_frame_free(&de->q_this);

    av_log(s, AV_LOG_DEBUG, ">>> %s\n", __func__);
}

#define OFFSET(x) offsetof(egl_display_env_t, x)
static const AVOption options[] = {
    { "show_all", "show all frames", OFFSET(show_all), AV_OPT_TYPE_BOOL, { .i64 = 0 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "window_size",  "set window forced size", OFFSET(window_width), AV_OPT_TYPE_IMAGE_SIZE, { .str = NULL }, 0, 0, AV_OPT_FLAG_ENCODING_PARAM },
    { "window_x",     "set window x offset",    OFFSET(window_x),     AV_OPT_TYPE_INT,    { .i64 = 0 }, -INT_MAX, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "window_y",     "set window y offset",    OFFSET(window_y),     AV_OPT_TYPE_INT,    { .i64 = 0 }, -INT_MAX, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "fullscreen",   "set fullscreen display", OFFSET(fullscreen),   AV_OPT_TYPE_BOOL,   { .i64 = 0 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL }

};

static const AVClass egl_vout_class = {
    .class_name = "egl vid outdev",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
    .category   = AV_CLASS_CATEGORY_DEVICE_VIDEO_OUTPUT,
};

AVOutputFormat ff_vout_egl_muxer = {
    .name           = "vout_egl",
    .long_name      = NULL_IF_CONFIG_SMALL("Egl video output device"),
    .priv_data_size = sizeof(egl_display_env_t),
    .audio_codec    = AV_CODEC_ID_NONE,
    .video_codec    = AV_CODEC_ID_WRAPPED_AVFRAME,
    .write_header   = egl_vout_write_header,
    .write_packet   = egl_vout_write_packet,
    .write_uncoded_frame = egl_vout_write_frame,
    .write_trailer  = egl_vout_write_trailer,
    .control_message = egl_vout_control_message,
    .flags          = AVFMT_NOFILE | AVFMT_VARIABLE_FPS | AVFMT_NOTIMESTAMPS,
    .priv_class     = &egl_vout_class,
    .init           = egl_vout_init,
    .deinit         = egl_vout_deinit,
};

