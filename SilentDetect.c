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

//DEFINING THE PARAMETERS TO BE TESTED, ANY SOUND QUITER THAN NOISE_THRESH WILL BE TREATED AS SILENCE. THE SILENCE SHOULD SUSTAIN FOR A FEW SECONDS TO BE CONSIDERED AS A PERIOD OF SILENCE, THUS THE MIN_DURATION METRIC. BOTH THESE METHODS ARE GOING TO BE TESTED BY THE FILTER.
const char *in = "input.webm";
const char *NOISE_THRESH  = "-30dB";
const double MIN_DURATION = 0.5;

//MAJORITY OF THE CODE REMAINS UNCHANGED FROM FILTERING CODE, BASIC DRILL. OPEN THE FILE, CREATE FORMAT CONTEXT FOR THE FILE, CODEC CONTEXT FOR THE CODEC FILTER CONTEXT FOR THE FILTER AND SO ON...
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *dec_ctx = NULL;
static AVFilterContext *buffersink_ctx = NULL;
static AVFilterContext *buffersrc_ctx = NULL;
static AVFilterGraph *filter_graph = NULL;
static int audio_stream_index = -1;

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
    //MOST OF THIS NEW CODE IS GOING TO BE THE SAME AS WE ARE WRITING ANOTHER FILTERING CODE, EXCEPT A FEW CHANGES, SUCH AS THE FOLLOWING --- WE NEED TO CHACK FOR AUDIO STREAMS THIS TIME AND THUS THE PARAMETERS ARE GOING TO CHANGE FOR THE av_find_best_stream() FUNCTION.
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in '%s'\n", filename);
        return ret;
    }
    audio_stream_index = ret;

    dec_ctx = avcodec_alloc_context3(decoder);
    if (!dec_ctx) return AVERROR(ENOMEM);

    avcodec_parameters_to_context(dec_ctx,
        fmt_ctx->streams[audio_stream_index]->codecpar);

    if ((ret = avcodec_open2(dec_ctx, decoder, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        return ret;
    }
    return 0;
}

static int init_filters(void)
{
    int ret;
    char args[512];

    const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    filter_graph = avfilter_graph_alloc();

    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;

    char ch_layout_str[64];
    av_channel_layout_describe(&dec_ctx->ch_layout, ch_layout_str, sizeof(ch_layout_str));

    snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s", time_base.num, time_base.den, dec_ctx->sample_rate,av_get_sample_fmt_name(dec_ctx->sample_fmt), ch_layout_str);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffersrc, "in", args, NULL, filter_graph);
    if(ret < 0){
        av_log(NULL, AV_LOG_ERROR, "Cannot create abuffer source\n");
        goto end;
    }
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "out");
    ret = avfilter_init_dict(buffersink_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot init abuffersink\n");
        goto end;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;


    //THE FILTER_DESC FOR OUR DETECTION FILTER. A FILTER LIKE THIS WILL NOT RETURN FRAMES TO THE ENCODER, INSTEAD THE PROCESSED FRAMES ARE JUST ANNONATED BY THE FILTER AS ONE OF THE FOLLOWING STRING OF CHARS:
    // lavfi.silence_start --- ONLY ONE FRAME WILL HAVE THIS ANNONATION, IT MARKS ON WHICH FRAME DID THE PERIOD OF SILENCE BEGAN. IT ACTUALLY MEASURES THE TIME STAMP IN SECS.
    // lavfi.silence_end --- AGAIN ONLY ONE FRAME USUALLY HAS THIS ANNONATION, IT MARKS WHEN THE SILENCE ENDED, ANOTHER TIMESTAMP IN SECS.
    // lavfi.silence_duration --- TOTAL TIME PERIOD OF SILENCE.
    char filter_descr[128];
    snprintf(filter_descr, sizeof(filter_descr), "silencedetect=noise=%s:duration=%g", NOISE_THRESH, MIN_DURATION);
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

static void check_for_silence(AVFrame *frame)
{
    AVDictionaryEntry *e = NULL;
    e = av_dict_get(frame->metadata, "lavfi.silence_start", NULL, 0);
    if(e){
        printf("\nPeriod of silence started at %.2f seconds\n", atof(e->value));
    }
    e = av_dict_get(frame->metadata, "lavfi.silence_end", NULL, 0);
    if(e){
        printf("\nPeriod of silence ended at %.2f seconds.\n", atof(e->value));
    }
    AVDictionaryEntry *dur = av_dict_get(frame->metadata, "lavfi.silence_duration", NULL, 0);
    if(dur){
        printf("\nPeriod of silence lasted for %.2f seconds.\n", atof(dur->value));
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

    // printf("\nScanning '%s' for silence (threshold=%s, min_duration=%.1fs)...\n\n", in, NOISE_THRESH, MIN_DURATION);

    while (av_read_frame(fmt_ctx, packet) >= 0) {
        if (packet->stream_index != audio_stream_index) {
            av_packet_unref(packet);
            continue;
        }
        ret = avcodec_send_packet(dec_ctx, packet);
        av_packet_unref(packet);
        if (ret < 0) break;
        while (avcodec_receive_frame(dec_ctx, frame) >= 0) {

            // best_effort_timestamp IS THE CORRECTED PTS AS FILES LIKE WEBM MAY HAVE IRREGULAR PTS VALUES.
            frame->pts = frame->best_effort_timestamp;

            if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error feeding audio to filtergraph\n");
                break;
            }
            while (1) {
                ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                    goto flush;
                check_for_silence(filt_frame);
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
            check_for_silence(filt_frame);
            av_frame_unref(filt_frame);
        }
    }
    av_buffersrc_add_frame_flags(buffersrc_ctx, NULL, 0);
    while (av_buffersink_get_frame(buffersink_ctx, filt_frame) >= 0) {
        check_for_silence(filt_frame);
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

