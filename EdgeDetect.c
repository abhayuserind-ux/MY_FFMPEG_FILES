#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avstring.h>
#include <libavutil/opt.h>

const char *in_file = "Blur_Input.mp4";

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
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a Video stream\n");
        return ret;
    }
    video_stream_index = ret;
    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) return AVERROR(ENOMEM);
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);
    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }
    return 0;
}

static int init_filter(void)
{
    int ret;
    char args[512];

    const AVFilter *buffersource = avfilter_get_by_name("buffer");
    const AVFilter *buffersink  = avfilter_get_by_name("buffersink");

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    filter_graph = avfilter_graph_alloc();

    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;
    snprintf(args, sizeof(args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
        time_base.num, time_base.den,
        dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersource, "in", args, NULL, filter_graph);
    if (ret < 0) goto end;

    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, NULL, filter_graph);
    if (ret < 0) goto end;

    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = buffersink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;

    /* Keep setparams to avoid the "reserved" metadata crash.
       Edgedetect thresholds: low=0.1, high=0.4
    */
    const char *filter_descr = "setparams=color_primaries=bt709:color_trc=bt709:colorspace=bt709,edgedetect=low=0.1:high=0.4";
    
    ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &outputs, NULL);
    if (ret < 0) goto end;

    ret = avfilter_graph_config(filter_graph, NULL);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

static void save_pgm_frame(AVFrame *frame, int frame_num)
{
    char filename[64];
    // This creates files like frame_0000.pgm, frame_0001.pgm, etc.
    snprintf(filename, sizeof(filename), "frame_%04d.pgm", frame_num);

    FILE *f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s for writing\n", filename);
        return;
    }

    // Header for PGM (Binary P5)
    fprintf(f, "P5\n%d %d\n255\n", frame->width, frame->height);

    // Write the Luma (Y) plane row by row
    for (int i = 0; i < frame->height; i++) {
        fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, f);
    }
    fclose(f);

    if (frame_num % 10 == 0) {
        printf("Saved %s\n", filename);
    }
}

int main(void)
{
    int ret;
    int frame_count = 0;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();

    if (open_input_file(in_file) < 0) return 1;
    if (init_filter() < 0) return 1;

    printf("Starting edge detection export. Please wait...\n");

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) break;

            while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    break;
                }

                while (1) {
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) exit(1);

                    // Logic: Save and move immediately to the next frame
                    save_pgm_frame(filt_frame, frame_count++);
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }

    /* Flush filters and decoder */
    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
        av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(frame);
        while (av_buffersink_get_frame(buffersink_ctx, filt_frame) >= 0) {
            save_pgm_frame(filt_frame, frame_count++);
            av_frame_unref(filt_frame);
        }
    }

    // Cleanup
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    printf("\nFinished! Exported %d frames.\n", frame_count);
    return 0;
}