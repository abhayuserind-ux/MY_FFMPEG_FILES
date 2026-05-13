/*
 * ffmpeg_frame_viewer.c
 *
 * Frame-by-frame video viewer using the FFmpeg C API + SDL2.
 * Frames are decoded into memory, displayed, and immediately discarded —
 * nothing ever touches disk.
 *
 * Controls (keyboard):
 *   Space / →    Next frame
 *   ←            Previous frame  (re-seeks to nearest keyframe, re-decodes)
 *   G            Go to frame number (type in terminal, press Enter)
 *   Home         Jump to frame 0
 *   End          Jump to last frame (estimated)
 *   F            Print current frame info to terminal
 *   Q / Escape   Quit
 *
 * Compile (Ubuntu / Debian):
 *   sudo apt install libavformat-dev libavcodec-dev libswscale-dev \
 *                    libavutil-dev libsdl2-dev
 *
 *   gcc -o ffmpeg_frame_viewer ffmpeg_frame_viewer.c \
 *       $(pkg-config --cflags --libs libavformat libavcodec libswscale libavutil sdl2) \
 *       -lm -Wall -O2
 *
 * Usage:
 *   ./ffmpeg_frame_viewer <video_file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/timestamp.h>

#include <SDL2/SDL.h>

/* ─── Player state ──────────────────────────────────────────────────────── */

typedef struct {
    /* FFmpeg */
    AVFormatContext  *fmt_ctx;
    AVCodecContext   *codec_ctx;
    int               vsi;            /* video stream index                 */
    AVStream         *vs;             /* video stream pointer               */
    struct SwsContext *sws_ctx;

    /* SDL */
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;

    /* Geometry */
    int      width, height;

    /* Frame counters */
    int64_t  current_frame;          /* 0-based frame number of last shown */
    int64_t  total_frames;           /* estimated (may be 0 if unknown)    */
    double   fps;
} Player;

static Player P = {0};

/* ─── Helpers ───────────────────────────────────────────────────────────── */

/* pts (in stream time_base units) → frame number */
static inline int64_t pts_to_frame(int64_t pts)
{
    if (pts == AV_NOPTS_VALUE) return -1;
    return av_rescale_q(pts, P.vs->time_base, av_inv_q(P.vs->r_frame_rate));
}

/* frame number → pts (in stream time_base units) */
static inline int64_t frame_to_pts(int64_t frame_num)
{
    return av_rescale_q(frame_num, av_inv_q(P.vs->r_frame_rate), P.vs->time_base);
}

/* seconds → "HH:MM:SS.mmm" string */
static void sec_to_str(double secs, char *buf, size_t len)
{
    int h = (int)(secs / 3600);
    int m = (int)(secs / 60) % 60;
    int s = (int)secs % 60;
    int ms = (int)((secs - floor(secs)) * 1000);
    snprintf(buf, len, "%02d:%02d:%02d.%03d", h, m, s, ms);
}

/* ─── Video open ────────────────────────────────────────────────────────── */

static int open_video(const char *path)
{
    if (avformat_open_input(&P.fmt_ctx, path, NULL, NULL) < 0) {
        fprintf(stderr, "ERROR: Cannot open '%s'\n", path);
        return -1;
    }
    if (avformat_find_stream_info(P.fmt_ctx, NULL) < 0) {
        fprintf(stderr, "ERROR: Cannot find stream info\n");
        return -1;
    }

    /* Find best video stream */
    P.vsi = av_find_best_stream(P.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (P.vsi < 0) {
        fprintf(stderr, "ERROR: No video stream found\n");
        return -1;
    }
    P.vs = P.fmt_ctx->streams[P.vsi];

    /* Open codec */
    const AVCodec *codec = avcodec_find_decoder(P.vs->codecpar->codec_id);
    if (!codec) {
        fprintf(stderr, "ERROR: Codec not supported\n");
        return -1;
    }
    P.codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(P.codec_ctx, P.vs->codecpar);
    /* Enable multi-threaded decoding */
    P.codec_ctx->thread_count = 0;
    P.codec_ctx->thread_type  = FF_THREAD_FRAME;
    if (avcodec_open2(P.codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "ERROR: Cannot open codec\n");
        return -1;
    }

    P.width  = P.codec_ctx->width;
    P.height = P.codec_ctx->height;

    /* FPS */
    AVRational fr = P.vs->r_frame_rate;
    if (fr.den == 0) fr = P.vs->avg_frame_rate;
    P.fps = (fr.den > 0) ? av_q2d(fr) : 25.0;

    /* Estimate total frames */
    if (P.vs->nb_frames > 0) {
        P.total_frames = P.vs->nb_frames;
    } else if (P.fmt_ctx->duration != AV_NOPTS_VALUE) {
        double dur = (double)P.fmt_ctx->duration / AV_TIME_BASE;
        P.total_frames = (int64_t)(dur * P.fps + 0.5);
    }

    /* SWS context: decode pixel format → RGB24 for SDL */
    P.sws_ctx = sws_getContext(
        P.width, P.height, P.codec_ctx->pix_fmt,
        P.width, P.height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);
    if (!P.sws_ctx) {
        fprintf(stderr, "ERROR: Cannot create sws context\n");
        return -1;
    }

    /* Print info */
    printf("File      : %s\n", path);
    printf("Codec     : %s\n", codec->name);
    printf("Resolution: %d x %d\n", P.width, P.height);
    printf("FPS       : %.4f\n", P.fps);
    printf("Est.frames: %" PRId64 "\n\n", P.total_frames);

    return 0;
}

/* ─── SDL init ──────────────────────────────────────────────────────────── */

static int init_sdl(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "ERROR: SDL_Init: %s\n", SDL_GetError());
        return -1;
    }

    /* Limit initial window size to 90% of desktop */
    SDL_DisplayMode dm;
    int max_w = P.width, max_h = P.height;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        max_w = (int)(dm.w * 0.9);
        max_h = (int)(dm.h * 0.9);
    }
    int win_w = (P.width  > max_w) ? max_w : P.width;
    int win_h = (P.height > max_h) ? max_h : P.height;

    P.window = SDL_CreateWindow(
        "FFmpeg Frame Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!P.window) {
        fprintf(stderr, "ERROR: SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    P.renderer = SDL_CreateRenderer(P.window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!P.renderer)
        P.renderer = SDL_CreateRenderer(P.window, -1, SDL_RENDERER_SOFTWARE);
    if (!P.renderer) {
        fprintf(stderr, "ERROR: SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_RenderSetLogicalSize(P.renderer, P.width, P.height);

    /* RGB24 texture — exactly what sws_scale produces */
    P.texture = SDL_CreateTexture(P.renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        P.width, P.height);
    if (!P.texture) {
        fprintf(stderr, "ERROR: SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

/* ─── Display one AVFrame (in-memory, no disk) ──────────────────────────── */

static void display_frame(AVFrame *frame)
{
    /* Allocate a small RGB24 buffer on the heap — freed before this function
       returns.  Nothing is written to disk. */
    int    buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24,
                                               P.width, P.height, 1);
    uint8_t *rgb_buf = (uint8_t *)av_malloc(buf_size);
    if (!rgb_buf) return;

    uint8_t  *dst_data[4]     = { rgb_buf, NULL, NULL, NULL };
    int       dst_linesize[4] = { P.width * 3, 0, 0, 0 };

    sws_scale(P.sws_ctx,
              (const uint8_t * const *)frame->data, frame->linesize,
              0, P.height,
              dst_data, dst_linesize);

    /* Upload pixels → GPU texture → render → present */
    SDL_UpdateTexture(P.texture, NULL, rgb_buf, P.width * 3);
    SDL_RenderClear(P.renderer);
    SDL_RenderCopy(P.renderer, P.texture, NULL, NULL);
    SDL_RenderPresent(P.renderer);

    /* ── Discard the RGB buffer immediately ── */
    av_free(rgb_buf);

    /* Update window title with frame info */
    double ts_sec = (double)P.current_frame / P.fps;
    char ts_str[32];
    sec_to_str(ts_sec, ts_str, sizeof(ts_str));

    char title[256];
    snprintf(title, sizeof(title),
             "Frame %" PRId64 " / ~%" PRId64 "  |  %s  |"
             "  Spc/→:Next  ←:Prev  G:GoTo  Home/End  F:Info  Q:Quit",
             P.current_frame, P.total_frames, ts_str);
    SDL_SetWindowTitle(P.window, title);
}

/* ─── Decode and return the next video frame ────────────────────────────── */
/*
 * Reads packets from the demuxer, sends them to the decoder, and returns
 * the first complete frame.  Caller must av_frame_unref() the result.
 * Returns 0 on success, negative on EOF or error.
 */
static int read_next_frame(AVFrame *out_frame)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return AVERROR(ENOMEM);

    int ret = -1;
    while (av_read_frame(P.fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != P.vsi) {
            av_packet_unref(pkt);
            continue;
        }

        int send_ret = avcodec_send_packet(P.codec_ctx, pkt);
        av_packet_unref(pkt);
        if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) break;

        ret = avcodec_receive_frame(P.codec_ctx, out_frame);
        if (ret == 0)  break;   /* got a frame            */
        if (ret == AVERROR(EAGAIN)) continue; /* need more packets    */
        break; /* error or EOF                                         */
    }

    /* Drain decoder on EOF */
    if (ret != 0) {
        avcodec_send_packet(P.codec_ctx, NULL);
        ret = avcodec_receive_frame(P.codec_ctx, out_frame);
    }

    av_packet_free(&pkt);
    return ret;
}

/* ─── Advance one frame forward ─────────────────────────────────────────── */

static int next_frame(void)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    int ret = read_next_frame(frame);
    if (ret == 0) {
        int64_t fn = pts_to_frame(frame->best_effort_timestamp);
        if (fn >= 0) P.current_frame = fn;
        else         P.current_frame++;
        display_frame(frame);
    } else {
        fprintf(stderr, "End of video (or read error)\n");
    }
    av_frame_free(&frame);   /* ── discard immediately ── */
    return ret;
}

/* ─── Seek to a specific frame number ───────────────────────────────────── */
/*
 * Strategy:
 *   1. Convert target frame → PTS in stream time_base.
 *   2. Seek backward to the nearest preceding keyframe.
 *   3. Flush the decoder (mandatory after seeking).
 *   4. Decode frames, discarding each one, until the frame whose PTS
 *      matches (or exceeds) the target.
 *   5. Display that frame.
 *
 * Note: for large GOPs (e.g. H.264 with keyframe every 250 frames),
 * seeking backward one frame may require decoding many frames.
 * This is a fundamental limitation of compressed video formats.
 */
static int seek_to_frame(int64_t target_frame)
{
    if (target_frame < 0) target_frame = 0;

    int64_t target_pts = frame_to_pts(target_frame);

    /* AVSEEK_FLAG_BACKWARD → seek to keyframe *at or before* target_pts */
    int ret = av_seek_frame(P.fmt_ctx, P.vsi, target_pts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "WARNING: av_seek_frame failed (ret=%d), trying byte seek\n", ret);
        /* fallback: seek to start */
        av_seek_frame(P.fmt_ctx, P.vsi, 0, AVSEEK_FLAG_BACKWARD);
    }

    /* Must flush after every seek */
    avcodec_flush_buffers(P.codec_ctx);

    /* Decode frames until we reach target_frame */
    AVFrame *frame = av_frame_alloc();
    if (!frame) return -1;

    int found = 0;
    int max_decode_attempts = (int)(P.fps * 30); /* safety: don't loop forever */

    for (int attempt = 0; attempt < max_decode_attempts; attempt++) {
        int r = read_next_frame(frame);
        if (r < 0) break;

        int64_t fn = pts_to_frame(frame->best_effort_timestamp);

        /* Display the frame that is >= target (best we can do) */
        if (fn >= target_frame) {
            P.current_frame = (fn >= 0) ? fn : target_frame;
            display_frame(frame);
            found = 1;
            av_frame_unref(frame);   /* ── discard ── */
            break;
        }
        av_frame_unref(frame);       /* ── discard intermediate frames ── */
    }

    av_frame_free(&frame);

    if (!found) {
        fprintf(stderr, "WARNING: Could not reach frame %" PRId64 "\n", target_frame);
        return -1;
    }
    return 0;
}

/* ─── Print current frame info to terminal ──────────────────────────────── */

static void print_frame_info(void)
{
    double ts_sec = (double)P.current_frame / P.fps;
    char ts_str[32];
    sec_to_str(ts_sec, ts_str, sizeof(ts_str));
    printf("─────────────────────────────────────\n");
    printf("Current frame : %" PRId64 "\n", P.current_frame);
    printf("Timestamp     : %s\n", ts_str);
    printf("Est. total    : %" PRId64 " frames\n", P.total_frames);
    printf("FPS           : %.4f\n", P.fps);
    printf("Resolution    : %d x %d\n", P.width, P.height);
    printf("─────────────────────────────────────\n");
}

/* ─── Input: prompt for a frame number in the terminal ──────────────────── */

static int64_t prompt_frame_number(void)
{
    printf("Enter frame number [0 – %" PRId64 "]: ", P.total_frames);
    fflush(stdout);
    int64_t n = -1;
    if (scanf("%" SCNd64, &n) != 1) n = -1;
    /* consume rest of line */
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {}
    return n;
}

/* ─── Cleanup ───────────────────────────────────────────────────────────── */

static void cleanup(void)
{
    if (P.sws_ctx)   sws_freeContext(P.sws_ctx);
    if (P.codec_ctx) avcodec_free_context(&P.codec_ctx);
    if (P.fmt_ctx)   avformat_close_input(&P.fmt_ctx);

    if (P.texture)   SDL_DestroyTexture(P.texture);
    if (P.renderer)  SDL_DestroyRenderer(P.renderer);
    if (P.window)    SDL_DestroyWindow(P.window);
    SDL_Quit();
}

/* ─── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <video_file> [start_frame]\n", argv[0]);
        return 1;
    }

    if (open_video(argv[1]) < 0) return 1;
    if (init_sdl()           < 0) { cleanup(); return 1; }

    printf("Controls:\n");
    printf("  Space / Right Arrow  : Next frame\n");
    printf("  Left Arrow           : Previous frame\n");
    printf("  G                    : Go to frame number (type in terminal)\n");
    printf("  Home                 : Jump to frame 0\n");
    printf("  End                  : Jump to last frame\n");
    printf("  F                    : Print current frame info\n");
    printf("  Q / Escape           : Quit\n\n");

    /* Show first frame (or a requested start frame) */
    int64_t start = (argc >= 3) ? atoll(argv[2]) : 0;
    if (start > 0)
        seek_to_frame(start);
    else
        next_frame();

    /* ── Main event loop ── */
    SDL_Event event;
    int running = 1;

    while (running) {
        /* SDL_WaitEvent blocks until input — zero CPU burn while paused */
        if (!SDL_WaitEvent(&event)) continue;

        switch (event.type) {

        case SDL_QUIT:
            running = 0;
            break;

        case SDL_KEYDOWN:
            switch (event.key.keysym.sym) {

            /* Quit */
            case SDLK_q:
            case SDLK_ESCAPE:
                running = 0;
                break;

            /* Next frame */
            case SDLK_SPACE:
            case SDLK_RIGHT:
                next_frame();
                break;

            /* Previous frame */
            case SDLK_LEFT: {
                int64_t target = P.current_frame - 1;
                if (target < 0) target = 0;
                seek_to_frame(target);
                break;
            }

            /* Go to frame */
            case SDLK_g: {
                int64_t fn = prompt_frame_number();
                if (fn >= 0) seek_to_frame(fn);
                break;
            }

            /* Jump to first frame */
            case SDLK_HOME:
                seek_to_frame(0);
                break;

            /* Jump to last frame */
            case SDLK_END:
                if (P.total_frames > 0)
                    seek_to_frame(P.total_frames - 1);
                break;

            /* Print frame info */
            case SDLK_f:
                print_frame_info();
                break;

            default:
                break;
            }
            break; /* SDL_KEYDOWN */

        case SDL_WINDOWEVENT:
            /* Repaint on expose / resize */
            if (event.window.event == SDL_WINDOWEVENT_EXPOSED  ||
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_RenderPresent(P.renderer);
            }
            break;
        }
    }

    cleanup();
    return 0;
}