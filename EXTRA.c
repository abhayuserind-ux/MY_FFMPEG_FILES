static void check_for_corrupt_frame(AVFrame *frame)
{
    // U (Cb) and V (Cr) are the chrominance planes.
    // In valid 8-bit YUV, both should hover near 128 (neutral).
    // Extreme deviations (e.g., UAVG < 90 or VAVG > 180) indicate corruption.
    AVDictionaryEntry *u = av_dict_get(frame->metadata, "lavfi.signalstats.UAVG", NULL, 0);
    AVDictionaryEntry *v = av_dict_get(frame->metadata, "lavfi.signalstats.VAVG", NULL, 0);

    // HUEMED is the median hue angle (0–360). 
    // Green sits around 120°, purple/magenta around 300°.
    AVDictionaryEntry *hue = av_dict_get(frame->metadata, "lavfi.signalstats.HUEMED", NULL, 0);

    // BRNG = percentage of pixels outside broadcast safe range (0–100).
    // A high value (e.g., >20%) is a strong signal of corruption.
    AVDictionaryEntry *brng = av_dict_get(frame->metadata, "lavfi.signalstats.BRNG", NULL, 0);

    if (!u || !v || !hue || !brng) return;

    double uavg  = atof(u->value);
    double vavg  = atof(v->value);
    double huemed = atof(hue->value);
    double brng_pct = atof(brng->value);

    // Chrominance check: U and V near 128 but BRNG high = chroma data wiped (green artifact)
    // Or extreme U/V shift = tinting from header corruption
    int chroma_anomaly = (uavg < 90 || uavg > 170 || vavg < 90 || vavg > 170);
    int broadcast_anomaly = (brng_pct > 20.0);

    // Hue check: green ~90-150°, purple/magenta ~270-330°
    int green_hue  = (huemed >= 90.0  && huemed <= 150.0);
    int purple_hue = (huemed >= 270.0 && huemed <= 330.0);

    double pts_sec = frame->pts * av_q2d(fmt_ctx->streams[video_stream_index]->time_base);

    if (chroma_anomaly || broadcast_anomaly) {
        printf("\n[CORRUPT FRAME] at %.2f sec | UAVG=%.1f VAVG=%.1f BRNG=%.1f%% HUE=%.1f°",
               pts_sec, uavg, vavg, brng_pct, huemed);
        if (green_hue)  printf(" --> GREEN SCREEN artifact");
        if (purple_hue) printf(" --> PURPLE TINT artifact");
        printf("\n");
    }
}








#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

//THE PARAMETERS TO BE TESTED.
const char *in = "input.webm";
const char *NOISE_THRESH = "tout+vrep+brng";
const double BRNG_THRESH = 20.0;


//MAJORITY OF THE CODE REMAINS UNCHANGED FROM FILTERING CODE, BASIC DRILL. OPEN THE FILE, CREATE FORMAT CONTEXT FOR THE FILE, CODEC CONTEXT FOR THE CODEC FILTER CONTEXT FOR THE FILTER AND SO ON...
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *dec_ctx = NULL;
static AVFilterContext *buffersink_ctx = NULL;
static AVFilterContext *buffersrc_ctx = NULL;
static AVFilterGraph *filter_graph = NULL;
static int video_stream_index = -1;

static int open_input_file(const char *filename)
{
    int ret;
    const AVCodec *decoder;
    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file '%s'\n", filename);
        return ret;
    }
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream info\n");
        return ret;
    }
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Cannot find a Video stream in '%s'\n", filename);
        return ret;
    }
    video_stream_index = ret;
    dec_ctx = avcodec_alloc_context3(decoder);
    if(!dec_ctx) return AVERROR(ENOMEM);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }
    return 0;
}

static int init_filters(void)
{
    int ret;
    char args[512];

    const AVFilter *buffersource = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    filter_graph = avfilter_graph_alloc();
    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt, time_base.num, time_base.den, dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersource, "in", args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
    ret = avfilter_init_dict(buffersink_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot init buffersink\n");
        goto end;
    }

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    // SNPRINTF FOR SIGNALSTATS FILTER DESCRIPTION
    // UNLIKE freezedetect AND silencedetect, signalstats ANNOTATES EVERY FRAME, NOT JUST ANOMALOUS ONES.
    // WE PASS stat=tout+vrep+brng TO REQUEST THE THREE STATS WE CARE ABOUT:
    // tout  --- PIXELS OUTSIDE VALID LUMA/CHROMA RANGE (CLIPPING INDICATOR)
    // vrep  --- VERTICAL LINE REPETITION (SYMPTOM OF HEADER CORRUPTION)
    // brng  --- PERCENTAGE OF PIXELS OUTSIDE BROADCAST SAFE RANGE
    char filter_descr[128];
    snprintf(filter_descr, sizeof(filter_descr), "signalstats=stat=%s", NOISE_THRESH);
    av_log(NULL, AV_LOG_INFO, "Filter: %s\n", filter_descr);

    ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot parse filter graph\n");
        goto end;
    }

    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure filter graph\n");
        goto end;
    }

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static void check_for_corrupt_frame(AVFrame *frame)
{
    // READING THE CHROMINANCE PLANES U (Cb) AND V (Cr).
    // IN VALID 8-BIT YUV BOTH PLANES HOVER NEAR 128 (NEUTRAL GREY / NO COLOR SHIFT).
    // SIGNIFICANT DEVIATION FROM 128 INDICATES MISSING OR CORRUPTED COLOR DATA.
    AVDictionaryEntry *u    = av_dict_get(frame->metadata, "lavfi.signalstats.UAVG", NULL, 0);
    AVDictionaryEntry *v    = av_dict_get(frame->metadata, "lavfi.signalstats.VAVG", NULL, 0);

    // HUEMED IS THE MEDIAN HUE ANGLE ACROSS THE FRAME (0-360 DEGREES).
    // GREEN SCREEN ARTIFACTS CLUSTER AROUND 90-150 DEGREES.
    // PURPLE / MAGENTA TINTING FROM HEADER CORRUPTION CLUSTERS AROUND 270-330 DEGREES.
    AVDictionaryEntry *hue  = av_dict_get(frame->metadata, "lavfi.signalstats.HUEMED", NULL, 0);

    // BRNG IS THE PERCENTAGE OF PIXELS FALLING OUTSIDE BROADCAST SAFE RANGE.
    // ANY VALUE ABOVE OUR BRNG_THRESH (20%) IS A STRONG SIGNAL OF FRAME CORRUPTION.
    AVDictionaryEntry *brng = av_dict_get(frame->metadata, "lavfi.signalstats.BRNG", NULL, 0);

    if (!u || !v || !hue || !brng) return;

    double uavg     = atof(u->value);
    double vavg     = atof(v->value);
    double huemed   = atof(hue->value);
    double brng_pct = atof(brng->value);

    int chroma_anomaly    = (uavg < 90 || uavg > 170 || vavg < 90 || vavg > 170);
    int broadcast_anomaly = (brng_pct > BRNG_THRESH);

    // HUE RANGES FOR ARTIFACT CLASSIFICATION
    int green_hue  = (huemed >= 90.0  && huemed <= 150.0);
    int purple_hue = (huemed >= 270.0 && huemed <= 330.0);

    double pts_sec = frame->pts * av_q2d(fmt_ctx->streams[video_stream_index]->time_base);

    if (chroma_anomaly || broadcast_anomaly) {
        printf("\n[CORRUPT FRAME] at %.2f sec | UAVG=%.1f VAVG=%.1f BRNG=%.1f%% HUE=%.1f°",
               pts_sec, uavg, vavg, brng_pct, huemed);
        if (green_hue)  printf(" --> GREEN SCREEN artifact");
        if (purple_hue) printf(" --> PURPLE TINT artifact");
        printf("\n");
    }
}

int main(void)
{
    int ret;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();

    if (!packet || !frame || !filt_frame) {
        fprintf(stderr, "Failed to allocate packet/frame\n");
        return 1;
    }

    if (open_input_file(in) < 0)  return 1;
    if (init_filters() < 0)  return 1;

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index != video_stream_index) {
            av_packet_unref(packet);
            continue;
        }
        ret = avcodec_send_packet(dec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) break;
        while (avcodec_receive_frame(dec_ctx, frame) >= 0) {

            frame->pts = frame->best_effort_timestamp;

            if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error feeding video to filtergraph\n");
                break;
            }
            while (1) {
                ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    goto flush;
                check_for_corrupt_frame(filt_frame);
                av_frame_unref(filt_frame);
            }
            av_frame_unref(frame);
        }
    }

flush:
    //SAME OLD FLUSHING ALL THE BUFFERED FRAMES AND RELEASING THE FILTER BUFFERS
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        frame->pts = frame->best_effort_timestamp;
        av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(frame);
        while (av_buffersink_get_frame(buffersink_ctx, filt_frame) >= 0) {
            check_for_corrupt_frame(filt_frame);
            av_frame_unref(filt_frame);
        }
    }
    av_buffersrc_add_frame_flags(buffersrc_ctx, NULL, 0);
    while (av_buffersink_get_frame(buffersink_ctx, filt_frame) >= 0) {
        check_for_corrupt_frame(filt_frame);
        av_frame_unref(filt_frame);
    }
    printf("Done.\n");

    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    return 0;
}