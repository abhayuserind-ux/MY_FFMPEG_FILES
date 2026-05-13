#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

const char *in  = "input.webm";
const char *out = "output_roi_x264.mp4";

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

            const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
            if (!encoder) {
                fprintf(stderr, "libaom-av1 not found. Is FFmpeg built with --enable-libaom-av1?\n");
                return 1;
            }

            AVStream *os = avformat_new_stream(o, encoder);
            enc_ctx = avcodec_alloc_context3(encoder);

            enc_ctx->height = dec_ctx->height;
            enc_ctx->width = dec_ctx->width;
            enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;

            enc_ctx->pix_fmt = encoder->pix_fmts[0]; 

            enc_ctx->framerate = av_guess_frame_rate(i, is, NULL);
            enc_ctx->time_base = av_inv_q(enc_ctx->framerate);

            if (o->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                
            av_opt_set(enc_ctx->priv_data, "crf",    "45", 0);
            av_opt_set(enc_ctx->priv_data, "preset", "medium", 0);
            avcodec_open2(enc_ctx, encoder, 0);
            avcodec_parameters_from_context(os->codecpar, enc_ctx);
            os->time_base = enc_ctx->time_base;
        }
    }

    if (!(o->oformat->flags & AVFMT_NOFILE))
        avio_open(&o->pb, out, AVIO_FLAG_WRITE);
    avformat_write_header(o, 0);

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

            AVFrameSideData *sd = av_frame_new_side_data(f, AV_FRAME_DATA_REGIONS_OF_INTEREST, sizeof(AVRegionOfInterest) * 2);
            if (!sd) {
                fprintf(stderr, "FAILED: av_frame_new_side_data returned NULL\n");
                return 1;
            }

            AVRegionOfInterest *roi = (AVRegionOfInterest *)sd->data;
            roi[0].self_size = sizeof(AVRegionOfInterest);
            roi[0].top    = 360;  
            roi[0].bottom = 720;
            roi[0].left   = 640;
            roi[0].right  = 1280;
            roi[0].qoffset = (AVRational){-1, 1};
            
            roi[1].self_size = sizeof(AVRegionOfInterest);
            roi[1].top       = 0;
            roi[1].bottom    = 1080;
            roi[1].left      = 0;
            roi[1].right     = 1920;   
            roi[1].qoffset   = (AVRational){1, 1}; 
            avcodec_send_frame(enc_ctx, f);
            av_frame_unref(f);

            static int frame_num = 0;
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