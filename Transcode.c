#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

const char *in = "input.webm";
const char *out = "output.mp4";

int main() {
    //WE ARE USING THIS LATER IN THE LAST WHILE LOOP
    static int frame_count = 0;
    AVFormatContext *i = NULL, *o = NULL;
    AVPacket *p = av_packet_alloc();
        //AVFRAME FOR FRAMES
    AVFrame *f = av_frame_alloc();
    int count_of_I_frames;

        //THE ACTUAL ENCODER CANNOT BE DIRECTLY CALLED , WE NEED A CONTEXT OR LIKE A CONTAINER FOR THE CODEC TO WORK, FOR THAT WE HAVE THIS POINTER, SET TO NULL FOR NOW, WE WILL FIND THE CODECS AND ASSIGN THEM.
    AVCodecContext *dec_ctx = NULL;
    AVCodecContext *enc_ctx = NULL;

        //SAME AS TRANSMUXING, OPEN INPUT FILE, CHECK FOR FORMAT AND STREAM INFO
    avformat_open_input(&i, in, 0, 0);
    avformat_find_stream_info(i, 0);

    avformat_alloc_output_context2(&o, 0, 0, out);
        //VIDEO STREAM VALUE, FOR NOW IT IS SET TO -1
    int video_index = -1;

    for (int s = 0; s < i->nb_streams; s++) {
        AVStream *is = i->streams[s];

            //FIND THE FIRST VIDEO STREAM AND ASSIGN THAT VALUE TO THE VIDEO_INDEX VARIABLE, WE NEED THAT VARIBALE FOR FUTURE PROCESSCES
        if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_index == -1) {
            video_index = s;
                // Find a registered decoder with a matching codec ID.
            const AVCodec *decoder = avcodec_find_decoder(is->codecpar->codec_id);
                //NOW THAT WE KNOW WHICH DECODE WE ARE GOING TO USE, ALLOCATE THE CONTEXT OF THAT DECODER TO OUR ENCODER POINTER THAT WE HAD SET EARLIER.
            dec_ctx = avcodec_alloc_context3(decoder);
                //AND SET THE PARAMETER OF THAT DECODER THE SAME AS THE CODEC PARAMETER
            avcodec_parameters_to_context(dec_ctx, is->codecpar);
            avcodec_open2(dec_ctx, decoder, 0);

                //NOW WE OPEN A POINTER FOR THE ACTUAL ENCODER, THIS IS THE REAL CODEC -- WHICH WE ARE GOING TO USE TO ENCODE OUR FILE.
            const AVCodec *encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
                //CREATE AN OUTPUT STREAM POINTER FOR WRITING STREAMS 
            AVStream *os = avformat_new_stream(o, encoder);
                //FINALLY ADD THE CONTEXT FOR THE ENCODER
            enc_ctx = avcodec_alloc_context3(encoder);
                //AASIGN THE PARAMETERS THAT ARE GOING TO REMAIN THE SAME AS-IS.
            enc_ctx->height = dec_ctx->height;
            enc_ctx->width = dec_ctx->width;
            enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                //PIXEL FORMAT, FOR SETTING THE PIXEL FORMAT SAME AS THE INPUT STREAM , PIX_FMTS[0] MEANS THAT THERE IS A LIST OF SUPPOSRTED PIXEL FORMATS IN INPUT WE NEED TO USE THE FIRST ONE TO ENCODE.
            enc_ctx->pix_fmt = encoder->pix_fmts[0];
                //ECODE THE TIME BASE AND FRAMERATE AS 1/30 AND 30FPS, RESPECTIVELY
            enc_ctx->time_base = (AVRational){1, 25};
            enc_ctx->framerate = (AVRational){25, 1};
                //DOUBTS REGARDING THE CONVERSION OF TIME_BASE AND FRAMERATE


            //CHECKING TWO THINGS HERE, FIRST THAT OUR OUTPUT FORMAT NEEDS THE GLOBAL HEADER AND IS NOT A STREAM BASED OUTPUT FORMAT. BECAUSE STREAM BASED FORMATS NEED HEADER DATA WRITTEN ON EVERY I-FRAME, WHILE THE FORMATS LIKE MP4 ONLY NEED THEM FOR GLOBAL HEADER.
            if (o->oformat->flags & AVFMT_GLOBALHEADER)
                //THIS IS THE EXPLICIT INSTRUCTION TO WRITE THE HEADER IN GLOBAL
                enc_ctx->flags = enc_ctx->flags | AV_CODEC_FLAG_GLOBAL_HEADER;

            avcodec_open2(enc_ctx, encoder, 0);
            avcodec_parameters_from_context(os->codecpar, enc_ctx);
            os->time_base = enc_ctx->time_base;
        }
    }
    //THE SAME FILE TAG CHECKING AS REMUXXING
    if (!(o->oformat->flags & AVFMT_NOFILE))
        avio_open(&o->pb, out, AVIO_FLAG_WRITE);
        //WRITING THE HEADER, AGAIN VERY SIMILAR TO REMUXXING
    avformat_write_header(o, 0);

    
    while (av_read_frame(i, p) >= 0) {
        if (p->stream_index != video_index) {
            av_packet_unref(p);
            continue;
        }
        avcodec_send_packet(dec_ctx, p);
        av_packet_unref(p);

       while (avcodec_receive_frame(dec_ctx, f) >= 0) {
            // MAIN LOOP RESCALING
            f->pts = av_rescale_q(f->pts, i->streams[video_index]->time_base, enc_ctx->time_base);
            if( av_get_picture_type_char(f->pict_type) == 'I'){
                printf("Frame type: %c \n", av_get_picture_type_char(f->pict_type));    
                count_of_I_frames++;
            }
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
    printf("Total I frames: %d\n", count_of_I_frames);
    //THIS IS THE IMPORTANT FLUSHING SET OF LOOPS, THE ENCODER AND DECODER BUFFERS HOLD A FEW EXTRA FRAMES WHICH DO NOT GET FLUSHED EVEN AT THE TIME OF CLOSING, WE SEND EXTRA NULL FRAMES TO BOTH ENCODER AND DECODER TO SO THAT IT CAN BE FLUSHED PROPERLY.

    // SENDING EXTRA NULL PACKET TO DECODER
    avcodec_send_packet(dec_ctx, NULL);
    //RECIEVE FRAMES FROM THE PACKET WE JUST SENT TO THE DECODER
    while (avcodec_receive_frame(dec_ctx, f) >= 0) {
        //SAME PTS RESCALLING WE DID IN MAIN LOOP
        f->pts = av_rescale_q(f->pts, i->streams[video_index]->time_base, enc_ctx->time_base);
        //SEND THOSE BUFFER FRAMES TO THE ENCODER
        avcodec_send_frame(enc_ctx, f);
        av_frame_unref(f);

        //SEND THE BUFFERED PACKETS, RESCALE THE TIME STAMPS, WRITE THEM IN FILE AND UNREF.
        while (avcodec_receive_packet(enc_ctx, p) >= 0) {
            p->stream_index = 0;
            av_packet_rescale_ts(p, enc_ctx->time_base, o->streams[0]->time_base);
            av_interleaved_write_frame(o, p);
            av_packet_unref(p);
        }
    }

    //REPEATING THE SAME ENTIRE LOOP FOR THE ENCODER, SAME SENDING EXTRA NULL VALUES TO REPLACE THE BUFFER AND WRITING THE BUFFERED VALUES INTO THE FILE
    avcodec_send_frame(enc_ctx, NULL);

    while (avcodec_receive_packet(enc_ctx, p) >= 0) {
        p->stream_index = 0;
        av_packet_rescale_ts(p, enc_ctx->time_base, o->streams[0]->time_base);
        av_interleaved_write_frame(o, p);
        av_packet_unref(p);
    }
    av_write_trailer(o);
    //FREEING THE MEMORY
    av_packet_free(&p);
    av_frame_free(&f);
    avcodec_free_context(&dec_ctx);
    avcodec_free_context(&enc_ctx);
    avformat_close_input(&i);
    // CLOSE THE PHYSICAL FILE ON THE HARD DRIVE
    if (!(o->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&o->pb);
    }
    //FREE THE OUTPUT CONTEXT
    avformat_free_context(o);
    return 0;
}