#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

const char *in  = "input.webm";
const char *out = "output_av1.mp4";   // REMEMBER THAT NOT ALL VIDEO FORMAT/CONTAINERS SUPPORT AV1, MP4 AND WEBM DOES, SOME MIGHT NOT SUCH AS OLDER IMPLEMENTATIONS OF MOV

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

            // --- ENCODER SIDE: SVT-AV1

            //MUST FIND THE ENCODER BY NAME AS THERE ARE MUTIPLE IMPLEMENTATIONS OF AV1 ENCODER SUCH AS AOMAV1 AND SVTAV1
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


            //PIXEL FORMAT IS COMPLEX IN AV1. 
            // NOT THAT IMPORTANT WE WILL COME BACK TO THIS LATER.
            // CHANGE 2: pix_fmt — AV1 supports 8-bit and 10-bit
            // pix_fmts[0] = yuv420p (8-bit), pix_fmts[1] = yuv420p10le (10-bit)
            // For now, use 8-bit (pix_fmts[0]) to keep it simple
            enc_ctx->pix_fmt = encoder->pix_fmts[0]; 

            enc_ctx->framerate = av_guess_frame_rate(i, is, NULL);
            enc_ctx->time_base = av_inv_q(enc_ctx->framerate);

            if (o->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                

            //ONE PARTICULARLY IMPORTANT THING ABOUT AV1 LIKE ENCODERS --- THE priv_data. priv_data IS A HIDDEN STRUCTURE PRESENT WITHIN THE FOMAT CONTEXT OF EVERY CODEC, IT IS USUALLY PRESENT IN EVERY ECONDER BUT IS REARLY USED BY US FOR THE OTHER ONES AS MOST OF THE COMMON PARAMETERS LIKE WIDTH, HEIGHT AND ETC ARE DECLARED BY US USING THE TYPICAL CONTEXT ASSIGNMENT, BUT THERE ARE SOME FIELDS IN THE FORMAT CONTEXT THAT MIGHT ONLY BE AVAILABLE FOR ONE TYPE OF ENCODER SUCH AS TILES OR GRAIN IN AV1, SUCH PARAMETERS CANNOT  BE ADDED TO THE MAIN FORMAT CONTEXT SSPERATELY FOR EACH ENCODER AS IT MIGHT MAKE THE STRUCTS TOO BIG TO WORK WITH, SO THERE IS ALWAYS A HIDDEN STRUCT INSIDE EVERY CODEC KNOWN AS priv_data WHICH CAN BE USED SET THESE UNIQUE VALUES OF AN ENCODER. REMEMBER THAT THIS STRUCTURE CAN NEVER BE ACCESSED DIRECTLY, WE DON'T EVEN KNOW THE NAMES OF THE PARAMETERS IN THIS STRUCUTRE.

            //NOW WHY DO WE NEED CRF? WHAT IS CRF CONTROLLING?
            //HERE COMES THE QUANTISATION PARAMETER.
            //FIRST WE NEED TO KNOW HOW THE ENCODING IS WWORKIGN IN A PRAGMATIC ENVIRONMENT, WHEN WE INSTRUCT AND ENCODER TO SAY ENCODE THE ENTIRE VIDEO AT A REASONABLE QULITY FOR THE ENTIRE VIDEO THEN THE ENCODER HAS TO BE SMART AND CHOOSE TO ENCODE PARTICULAR FRAME A LITTLE MORE OR A LITTLE LESS. SUPPOSE A BLACK FRAME - A COMPLETELY BLANK BLACK FRAME DOESN'T NEED TO BE ENCODED WITH THAT MUCH LOSSLESS COMPRESSION, THIS IS WHERE THE MAIN TASK OR USE OF AND ENCODER SHINES, THIS IS WHERE IT IS SUPPOSED TO LOOSE AS MUCH DATA AS POSSIBLE BECAUSE THE DATA HERE IS UNIMPORTANT. NEXT IF A FRAME IS IMPORTANT AND HAS A LOT OF EDGES INVOLVED OR MAYBE IS IMPORTANT BECAUSE OF ROI THEN WE NEED TO ENCODE IT WITHOUT LOOSING TOO MUCH DATA. THE DEGREE OF ENCOING IS DECIDED BY THE *****QUANTISATION PARAMETER*****, AND THE QUANTISATION PARAMETER VALUES CONSTANTLY CHANGE FOR EACH FRAME ACCORDING TO THE CRF(CONSTANT RATE FACTOR), THE HIGHER THE CRF THE MORE LOSSY COMPRESSION WILL HAPPEN, THE LOWER THE CRF THE MORE DATA WILL THE ENCODER RETAIN GIVING US A BETTER QUALITY VIDEO. IN ENCODERS LIKE H264 THIS VALUE RANGES FROM 0 TO 51, SO 23 IS THE DEFAULT QUALITY OF THE VIDEO, WHILE IN SVTAV1 THE VALUES RANGE FROM 0 TO 63, THE DEFAULT VALUE IS 35. AND OFFCOURSE ALL OF THIS IS DONE USING THE BITRATE VALUE. REMEMBER QP IS NOT BIT RATE ADN CRF IS NOT QP EITHER.
            
            //=========IMPORTANT DISTINCTION BETWEEN CRF, BITRATE AND QP(QUANTISATION PARAMETER)==================
            // CRF CONTROLS HOW MUCH SHOULD THE QP SHOULD BE FOR A PARTICULAR FRAME, THEN THE ENCODER CHANGES THE BIT RATE ACCORDING TO THAT QP TO ENCODE A FRAME WITH CERTAIN LOSSY OR LOSSLESS COMPRESSION DEGREE.

            //RATE CONTROL FOR BITRATE, THIS ONE IS ALSO DONE USING THE av_opt_set() FUNCTION IN THE priv_data
            av_opt_set(enc_ctx->priv_data, "crf", "35", 0);


            //WHAT IS PRESET???
            //PRESETS ARE A SET OF PREDEFINED ENCODER SETTINGS FOR ANY ENCODER ABOUT HOW TO ENCODE A PARTICULAR FRAME --- FOR EXAMPLE, WHEN THE ENCODER IS ACTUALLY WORKING AND TRYING TO ENCODE A FRAME, THEN IT WILL NOT SIMPLY ENCODE IT BASED ON WHATEVER INSTRCUTION WE HAVE GIVEN, REMEMBER THAT THE ENCODER'S JOB IS TO REDUCE FILE SIZE AND MAKE DATA TRANSFER EASIER AND FASTER, SO IT PERFORMS OPERATIONS LIKE MOTION ESTIMATION AND ETC. WHICH WE HAEV SEEN BEFORE, NOW WHEN TRYING TO LOOK FOR A PEICE OF INFO FROM THE PREVIOUS FRAME TO HELP WITH ENCODING OF THE CURRENT FRAME IT IS IMPORTANT FOR THE ENCODER TO CHOOSE HOW MUCH TIME AND COMPUTATIONAL POWER IS IT GOING TO SPEND ON LOOK FOR THIS EXTRA INFO FROM THE PREVIOUS FRAME. GAINING A LOT OF INFO FROM THE PREVIOUS FRAME MIGHT HELP ENCODE THE NEW FRAME BETTER BUT IT MIGHT TAKE TOO MUCH TIME SO THE TIME AND DATA SAVED ON ENCODING THE FRAME IS ON NO USE NOW. ALL THIS PROCESS DEPENDS ON THE PRESET VALUES, AND THERE ARE 10s OF PRESET VALUES PRESENT FOR EACH ENCODER WHICH IS WHY WE, AS IN PROGRAMMERS, USUALLY DON'T SET THEM, WE JUST CHOOSE CERTAIN PRE DEFINED VALUES FROM THE PRESET STRUCT THAT TELLS THE ENCODER TO HOW MUCH BALANCE IT SHOULD KEEP BETWEEN SPEED AND QUALITY. 
            // FOR THE STANDARD ENCODER LIKE H.264 THERE ARE PRESET VALUES SUCH AS "ultrafast" or " placebo" IN WHICH ULTRFAST MEANS -- ULTRAFAST ENCODING, OFF COURSE, WHAT A SUPRISE, AND PLACEBO MEANS VERY SLOW ENCODING BUT IT IS THE BEST QUALITY PRESERVATION MODE OF PRESET.
            //IN AV1 ENCODER THIS SAME PRESET VALUE IS DEFINED USING NUMBER SUCH AS 0,1,2,3,... ALL THE WAY UPTO 13, WHERE 13 IS THE FASTEST WORST QUALITY AND 0 IS SLOWEST BEST QUALITY ENCODING.   

            av_opt_set(enc_ctx->priv_data, "preset", "10", 0);

            // TILE PARALLELISM? WHAT ARE TILES IN AV1 ENCODER?? COMPLETELY NEW CONCEPT ======= !!!!!!!!IMPORTANT!!!!!!!

            //ANY ENCODER WILL NEVER LOOK AT A SINGLE FRAME WHOLE AT ONCE, IT WILL DIVIDE THE FRAME INTO MUTIPLE PARTS OR SMALLER CHUNKS OF A FEW HUNDRED PIXELS CALLED MACROS(USUALLY OF SIZE 16x16 PIXELS) FOR STANDARD ENCODER LIKE H.264 AND FOR AV1 BASED ENCODERS IT IS CALLED SUPERBLOCKS USUALLY OF SIZE 128 x 128 BLOCKS. NOW FOR AN ENCODER TO ENCODE AN ENTIRE FRAME IT NEEDS TO DECIDED WHICH MACRO TO ENCODE FIRST AS SOME OF THE REGIONS OF A FRAME MIGHT DEPEND ON OTHER REGIONS AS USALLY SEEN IN IMAGE PROCESSING. AND SO WE NEED  TO PROCESS EACH MACRO ONE BY ONE, THIS IS NOT GOOD FOR MODERN CPU ARCHITECTURE AS MOST OF THE MODERN ARCHITECTURE HAS MUTIPLE CORES IN A GPU AND SO IF ONLY ONE MACRO IS BEING PROCESSED ON ONE SINGLE CORE OF A GPU THEN THE REST OF THE CORE'S COMPUTAIONAL POWER IS GOING TO WASTE. BUT WE CANNOT PARALLELY PROCESS ANY MACROS AS AGAIN THEY MIGHT DEPEND ON EACH OTHER OR ATLEAST THEY MIGHT NEED DATA FROM ANOTHER MACRO TO ENCODE THIS ONE BETTER. THIS IS WHAT TYPICALLY HAPPENS IN STANDARD ENCODERS. WHICH IS WHY AV1 ENCODER HAS A COMPLETELY DIFFERNET CONCPET FOR MACRO OR SUPERBLOCK PROCESSING. IT IS CALLED TILE SPERATION OR TILE PARALLELISM. 
            // WHAT THE AV1 ENCODER DOES?
            // WE HAVE TO DIVIDE THE FRAME INTO SUPERBLOCKS, NO WORKAROUND THAT, BUT IF CAN BREAK A FRAME INTO SEPERATE REGIONS OF COMBINED SUPERBLOCKS THAT DO NOT DEPEND ON THE SBs OF OTHER REGION THEN WE CAN ALLOCATE THIS ENTIRE BLOCK TO A SINGLE CORE IN A GPU AND THIS WAY WE WOULD HAVE ACHEIVED SOME SORT OF PARALLEL PROCESSING WITH THE FRAME PROCESSING. SO IF WE CAN DIVIDE A FRAME INTO, SAY, 4 TILES OF 10S OF SB INSIDE WHICH THE SBs DEPEND ONLY ON EACH OTHER AND NOT ON THE SBs OF OTHER TILES THEN THIS SINGLE TILE HAS BECOME COMPLETELY INDEPENDENT AND CAN BE ALLOCATED TO A SINGLE CORE REDUCUNG THE PROCESSING TIME OVERALL.

            //=========VISUIALISATION FROM CLAUDE=========
            // 1920×1080 frame, split into 4 tiles (2 columns × 2 rows):
            // +──────────────────────────────────────────+
            // │                                          │
            // │   Tile 0 (0,0)    │   Tile 1 (1,0)       │
            // │   960×540         │   960×540            │
            // │                   │                      │
            // ├───────────────────┼──────────────────────┤
            // │                   │                      │
            // │   Tile 2 (0,1)    │   Tile 3 (1,1)       │
            // │   960×540         │   960×540            │
            // │                   │                      │
            // +──────────────────────────────────────────+


            //HOW TO SET THIS TILE VALUE FOR OUR CODE AND HOW IS IT DECIDED, WE CAN CHOOSE TO SET OUR OWN VALUES OF TILES USING THE KEY tile_columns AND tile_rows AGAIN IN THE SVT-AV1'S PRIV_DATA STRUCT. BOTH THE VALUES ARE LOG2 BASED IF WE WRITE TILE_COLUMNS AS 2 THEN IT WILL BECOME 2^2 = 4 COLUMNS, BASICALLY KEY=tile_columns or tile_rows AND VALUE = 2^N, WE CAN SET N. NOW DOING IT MANUALLY CAN BE DIFFICULT FOR US IN THE SENSE THAT WE AS A HUMAN MIGHT NEVER PROPERLY BE ABLE TO DECIDE HOW MUCH TILES DOES THE ENCODER AND OUR GPUs NEED WHICH IS WHY WE USUALLY LEAVE THIS MATTER TO THE SVTAV2 ENCODER ITSELF WHICH HAS INTERNAL LOGIC TO SEPERATE THE FRAMES INTO TILE EFFIECNTLY, SO WE JUST LEAVE THE DEFAULT VALUE OF 0 TO LET THE ENCODER DECIDE.

            av_opt_set(enc_ctx->priv_data, "tile_columns", "0", 0);
            av_opt_set(enc_ctx->priv_data, "tile_rows",    "0", 0);

            avcodec_open2(enc_ctx, encoder, 0);
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