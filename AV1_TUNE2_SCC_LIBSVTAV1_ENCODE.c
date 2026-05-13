#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

const char *in  = "input_for_tune1.mp4";
const char *out = "output_av1_Tune_2_SCC.mp4"; 

int main() {
    AVFormatContext *i = NULL, *o = NULL;
    AVPacket *p  = av_packet_alloc();
    AVFrame  *f  = av_frame_alloc();
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;
    int video_index = -1;

    avformat_open_input(&i, in, 0, 0);
    avformat_find_stream_info(i, 0);
    avformat_alloc_output_context2(&o, 0, 0, out);

    for (int s = 0; s < i->nb_streams; s++) {
        AVStream *is = i->streams[s];

        if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index == -1) {
            video_index = s;

            const AVCodec *decoder = avcodec_find_decoder(is->codecpar->codec_id);
            dec_ctx = avcodec_alloc_context3(decoder);
            avcodec_parameters_to_context(dec_ctx, is->codecpar);
            avcodec_open2(dec_ctx, decoder, 0);

            const AVCodec *encoder = avcodec_find_encoder_by_name("libsvtav1");
            if (!encoder) {
                fprintf(stderr, "libsvtav1 not found. Is FFmpeg built with --enable-libsvtav1?\n");
                return 1;
            }

            AVStream *os = avformat_new_stream(o, encoder);
            enc_ctx = avcodec_alloc_context3(encoder);

            enc_ctx->height = dec_ctx->height;
            enc_ctx->width = dec_ctx->width;
            enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
            enc_ctx->framerate = av_guess_frame_rate(i, is, NULL);
            enc_ctx->time_base = is->time_base;

            enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P; 

            if (o->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                
            av_opt_set(enc_ctx->priv_data, "preset", "6", 0); 
            
            // 1. CAP THE VBR: Match your H.264 budget parameters exactly
            av_opt_set(enc_ctx->priv_data, "crf", "35", 0); 
            enc_ctx->rc_max_rate    = 400000;
            enc_ctx->rc_buffer_size = 1000000;

            // 2. ENABLE SCC: Pass SVT-AV1 specific parameters correctly
            // scm=1 is for pure screen content, scm=2 is for mixed content.
            // tune=2 sets SSIM tuning (tune=0 is visual quality/VQ).
            av_opt_set(enc_ctx->priv_data, "svtav1-params", "scm=2:tune=2", 0);
            
            enc_ctx->gop_size = 120;

            // Good practice: check the return value of avcodec_open2
            if (avcodec_open2(enc_ctx, encoder, 0) < 0) {
                fprintf(stderr, "Failed to open SVT-AV1 encoder.\n");
                return 1;
            }
            
            avcodec_parameters_from_context(os->codecpar, enc_ctx);
            os->time_base = enc_ctx->time_base;
        }
    }

    if (!(o->oformat->flags & AVFMT_NOFILE))
        avio_open(&o->pb, out, AVIO_FLAG_WRITE);
    avformat_write_header(o, 0);

    // MAIN TRANSCOING LOOP: IDENTICAL TO EVERY OTHER TRANSCODING PROGRAM
    while (av_read_frame(i, p) >= 0) {
        if (p->stream_index != video_index) {
            av_packet_unref(p);
            continue;
        }
        avcodec_send_packet(dec_ctx, p);
        av_packet_unref(p);

        while (avcodec_receive_frame(dec_ctx, f) >= 0) {
            f->pts = av_rescale_q(f->pts,
                                  i->streams[video_index]->time_base,
                                  enc_ctx->time_base);
            avcodec_send_frame(enc_ctx, f);
            av_frame_unref(f);

            while (avcodec_receive_packet(enc_ctx, p) >= 0) {
                p->stream_index = 0;
                av_packet_rescale_ts(p, enc_ctx->time_base, o->streams[0]->time_base);
                av_interleaved_write_frame(o, p);
                av_packet_unref(p);
            }
        }
    }

    avcodec_send_packet(dec_ctx, NULL);
    while (avcodec_receive_frame(dec_ctx, f) >= 0) {
        f->pts = av_rescale_q(f->pts,
                              i->streams[video_index]->time_base,
                              enc_ctx->time_base);
        avcodec_send_frame(enc_ctx, f);
        av_frame_unref(f);
        while (avcodec_receive_packet(enc_ctx, p) >= 0) {
            p->stream_index = 0;
            av_packet_rescale_ts(p, enc_ctx->time_base, o->streams[0]->time_base);
            av_interleaved_write_frame(o, p);
            av_packet_unref(p);
        }
    }

    avcodec_send_frame(enc_ctx, NULL);
    while (avcodec_receive_packet(enc_ctx, p) >= 0) {
        p->stream_index = 0;
        av_packet_rescale_ts(p, enc_ctx->time_base, o->streams[0]->time_base);
        av_interleaved_write_frame(o, p);
        av_packet_unref(p);
    }

    av_write_trailer(o);

    av_packet_free(&p);
    av_frame_free(&f);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    avformat_close_input(&i);
    if (!(o->oformat->flags & AVFMT_NOFILE))
        avio_closep(&o->pb);
    avformat_free_context(o);
    return 0;
}