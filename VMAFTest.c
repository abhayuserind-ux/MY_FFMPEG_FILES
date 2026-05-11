#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>


//SOURCES FOR VMAF:
//https://github.com/Netflix/vmaf --- THIS SHOULD BE ENOUGH
//https://netflixtechblog.com/toward-a-practical-perceptual-video-quality-metric-653f208b9652
//https://netflixtechblog.com/vmaf-the-journey-continues-44b51ee9ed12
//https://netflixtechblog.com/toward-a-better-quality-metric-for-the-video-community-7ed94e752a30
//https://ffmpeg.org/ffmpeg-filters.html#libvmaf
//https://docs.google.com/presentation/d/1ZVQPsA4N6K8uGW3aFgw4Ei9w953nYORUUPvgpigOq58/edit?slide=id.g8f2ec97ec9_0_571#slide=id.g8f2ec97ec9_0_571

// THE TWO FILES WE ARE COMPARING. ref IS THE ORIGINAL "GROUND TRUTH" VIDEO THAT THE
// ENCODER HAS NOT TOUCHED. dist IS THE "DISTORTED" VIDEO THE OUTPUT OF OUR TRANSCODING
// CODE. VMAF WILL COMPARE THESE TWO FILES FRAME BY FRAME AND TELL US HOW MUCH PERCEPTUAL
// QUALITY WAS LOST DURING THE TRANSCODE.
const char *ref_filename  = "input_short.webm";
const char *dist_filename = "output_short.mkv";

// THIS IS THE FILTER DESCRIPTION STRING. UNLIKE THE FILTERING CODE WHERE WE HAD "scale=1280:720"
// WHICH IS A SINGLE-INPUT FILTER, libvmaf IS A TWO-INPUT FILTER. THE [dist] AND [ref] INSIDE
// THE BRACKETS ARE NAMED PAD LABELS. THEY ARE NOT RANDOM STRINGS. THEY MUST EXACTLY MATCH
// THE NAMES WE ASSIGN TO OUR TWO AVFilterInOut STRUCTS IN init_filters() BECAUSE THAT IS HOW
// avfilter_graph_parse_ptr() KNOWS WHICH BUFFER SOURCE TO WIRE TO WHICH INPUT PAD OF libvmaf.
// log_path=vmaf.json TELLS THE libvmaf FILTER WHERE TO WRITE ITS OUTPUT.
// log_fmt=json TELLS IT TO USE JSON FORMAT FOR THAT OUTPUT.
// THIS IS THE ONLY THING THIS ENTIRE PROGRAM PRODUCES ,A SCORE IN A JSON FILE.
const char *filter_descr = "[dist]setparams=range=tv[dist_n];[dist_n][ref]libvmaf=log_path=vmaf.json:log_fmt=json:model=path=/home/abhay-zstch1561/vmaf_models/vmaf_v0.6.1.json";

// TWO FORMAT CONTEXTS INSTEAD OF ONE. IN THE FILTERING CODE WE HAD ONE fmt_ctx BECAUSE
// WE WERE READING FORM ONE FILE. HERE WE HAVE TWO SEPARATE FILES TO OPEN AND READ FROM SIMULTAENOUSLY. EACH FORMAT CONTEXT IS THE CONTEXT --- ref_fmt_ctx HOLDS ALL THE CONTAINER AND STREAM INFORMATION FOR input.webm AND dist_fmt_ctx DOES THE SAME FOR output.mkv.
static AVFormatContext *ref_fmt_ctx  = NULL;
static AVFormatContext *dist_fmt_ctx = NULL;

// SAME CONCEPT AS dec_ctx IN THE FILTERING CODE, BUT DOUBLED. WE NEED ONE DECODER ENGINE PER FILE BECAUSE EACH FILE USES A DIFFERENT CODEC.
static AVCodecContext *ref_dec_ctx  = NULL;
static AVCodecContext *dist_dec_ctx = NULL;

// JUST LIKE video_stream_index IN THE FILTERING CODE, WE NEED TO TRACK WHICH STREAM INDEX IS THE VIDEO STREAM INSIDE EACH FILE. EVEN THOUGH BOTH FILES HAVE VIDEO, THE INDEX MIGHT BR DIFFERENT IF ONE OF THEM HAD AN AUDIO STREAM THAT SHIFTED THINGS.
// IT IS ALWAYS CORRECT TO TRACK THIS PER-FILE.
static int ref_video_index  = -1;
static int dist_video_index = -1;

// THIS IS THE KEY STRUCTURAL DIFFERENCE FROM THE FILTERING CODE. IN THE FILTERING CODE WE HAD ONE buffersrc_ctx BECAUSE THE GRAPH HAD ONE ENTRY GATE FOR ONE INPUT STREAM. libvmaf IS A DIFFERENT KIND OF FILTER IT IS A TWO-INPUT FILTER. IT DOES NOT TRANSFORM VIDEO, IT COMPARES TWO VIDEO STREAMS SIMULTANEOUSLY. AND SOTHE GRAPH NEEDS TWO SEPARATE ENTRY GATES: ONE FOR THE DISTORTED VIDEO AND ONE FOR THE REFERENCE VIDEO. EVERY FRAME FROM dist_dec_ctx GOES THROUGH buffersrc_dist. EVERY FRAME FROM ref_dec_ctx GOES THROUGH buffersrc_ref.
AVFilterContext *buffersrc_dist = NULL;
AVFilterContext *buffersrc_ref  = NULL;

// SAME CONCEPT AS buffersink_ctx IN THE FILTERING CODE IT IS THE EXIT GATE OF THE GRAPH. THE CRITICAL DIFFERENCE IS WHAT WE DO WITH THE FRAMES THAT COME OUT OF IT. IN THE FILTERING CODE: frames out of sink -> encode_and_write() -> output.mkv ON DISK. HERE: frames out of sink -> av_frame_unref() -> THROWN AWAY IMMEDIATELY. THIS IS BECAUSE libvmaf DOES ALL ITS WORK INTERNALLY INSIDE THE FILTER NODE BEFORE THE FRAME EVEN REACHES THE SINK. BY THE TIME av_buffersink_get_frame() RETURNS, VMAF HAS ALREADY COMPARED THE FRAME PAIR AND WRITEN THE SCORE TO vmaf.json. THE FRAME WE GET BACK IS JUST THE MECHANICAL CONSEQUENCE OF THE GRAPH NEEDING SOMEWHERE TO PUT ITS OUTPUT.
AVFilterContext *buffersink_ctx = NULL;

// SAME WRAPPER STRUCTURE AS IN THE FILTERING CODE. THE GRAPH HOLDS ALL THE FILTER NODES,
// THE LINKS BETWEEN THEM, AND THE CONFIGURATION OF THE ENTIRE PIPELINE.
AVFilterGraph *filter_graph = NULL;


//THIS FUNCTION IS A GENREALIZED VERSION OF open_input_file() FROM THE FILTERING CODE. IN THE FILTERING CODE, open_input_file() OPERATED ON GLOBAL VARIABLES (fmt_ctx, dec_ctx, video_stream_index) DIRECTLY BECAUSE THERE WAS ONLY ONE INPUT FILE. HERE WE HAVE TWO
//INPUT FILES AND WE NEED TO CALL THE SAME SETIP LOGIC FOR BOTH. INSTEAD OF WRITING TWO IDENTICAL FUNCTIONS WITH DIFFERENT VARIABLE NAMES, WE PASS THE DESTINATION POINTERS AS ARGUMENTS SO THE SAME FUNCTION CAN SET UP BOTH FILE EASILY. BASICALLY A SINGLE FUNCTION TO OPEN FILES AND CREATE CONTEXT.
static int open_input_file(const char *filename,AVFormatContext **fmt_ctx, AVCodecContext **dec_ctx, int  *video_index)
{
    // JUST LIKE IN THE FITLERING CODE, WE NEED THE DECODER POINTER TO PASS TO av_find_best_stream()
    // SO IT CAN FILL IT WITH THE CORRECT DECODER FOR WHATEVER CODEC IT FINDS IN THE FILE.
    const AVCodec *decoder;
    int ret;

    // EXACT SAME PATTERN AS THE FILTERING CODE. avformat_open_input() READS THE FILE HEADER,
    // IDENTIFIES THE CONTAINER FORMAT, AND FILLS THE FORMAT CONTEXT. THE DIFFERENCE IS THAT
    // WE ARE NOW PASSING IN fmt_ctx AS A PARAMETER (POINTER TO POINTER OR DOUBLE POINTER) INSTEAD OF USING THE
    // GLOBAL DIRECTLY. *fmt_ctx IS THE SAME AS WRITING ref_fmt_ctx OR dist_fmt_ctx AT THE CALL SITE.
    if ((ret = avformat_open_input(fmt_ctx, filename, 0, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file: %s\n", filename);
        return ret;
    }
    // SAME AS FILTERING CODE. FINDING THE STREAM INFO.
    if ((ret = avformat_find_stream_info(*fmt_ctx, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream info in: %s\n", filename);
        return ret;
    }

    // SAME AS FILTERING CODE. FINDS THE BEST VIDEO STREAM AND ALSO FILLS decoder WITH THE APPROPRIATE CODEC POINTER. THE RETURN VALUE IS THE STREAM INDEX, WHICH WE STORE IN *video_index
    ret = av_find_best_stream(*fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find video stream in: %s\n", filename);
        return ret;
    }
    *video_index = ret;

    // EXACT SAME THREE-STEP DECODER SETUP AS THE FILTERING CODE:
    // ALLOACTE THE CONTEXT MEMORY USING THE CODC BLUEPRINT THEN COPY THE STREAM'S CODEC PARAMETERS INTO THE CONTEXT AND FINALLY OPEN THE DECODER ENGINE (avcodec_open2 IS THE MOMENT THE CODEC BECOMES LIVE)
    *dec_ctx = avcodec_alloc_context3(decoder);
    avcodec_parameters_to_context(*dec_ctx, (*fmt_ctx)->streams[*video_index]->codecpar);

    if ((ret = avcodec_open2(*dec_ctx, decoder, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open decoder for: %s\n", filename);
        return ret;
    }
    return 0;
}
// THIS IS THE FUNCTION THAT BUILDS THE FILTER GRAPH. THE CONCEPT IS THE SAME AS init_filters()
// IN THE FILTERING CODE BUT WITH ONE CRITICAL STRUTCURAL DIFFERENCE: INSTEAD OF ONE BUFFER SOURCE WE NOW CREATE TWO, AND INSTEAD OF A SINGLE AVFilterInOut FOR THE OUTPUT SIDE WE NEED A LINKED LIST WITH TWO ENTRIES ONE PER BUFFER SOURCE.
static int init_filters(void)
{
    // WE NEED TWO SEPARATE args STRINGS THIS TIME, ONE FOR EACH BUFFER SOURCE, BECAUSE EACH BUFFER SOURCE DESCRIBES THE PROPERTIES OF A DIFFERENT FILE'S DECODED FRAMES. input.webm AND output.mkv MAY HAVE DIFFERENT RESOLUTIONS, PIXEL FORMATS, OR TIMEBSAE. EACH buffersrc MUST BE TOLD
    // EXACTLY WHAT KIND OF FRAMES IT IS GOING TO RECEIVE FROM ITS RESPECTIVE DECODER.
    char dist_args[512];
    char ref_args[512];
    int  ret;

    // JUST LIKE IN THE FILTERING CODE, WE CREATE THE buffer AND buffersink FILTERS.
    const AVFilter *buffersource = avfilter_get_by_name("buffer");
    const AVFilter *buffersink   = avfilter_get_by_name("buffersink");

    // IN THE FILTERING CODE WE HAD ONE AVFilterInOut FOR inputs AND ONE FOR outputs. HERE WE STILL HAVE ONE FOR inputs (THE SINK SIDE REMEMBER THE CONFUSING NAMING FROM THE FILTERING CODE: inputs IS FROM THE PERSPECTIVE OF THE FILTER STRING, NOT OUR PROGRAM). BUT ON THE outputs SIDE WE NEED TWO ENTRIES FOR EACH BUFFER SOURCE. THESE TWO ENTRIES FORM A LINKED LIST USING THE ->next POINTER. ------MORE ELABORATION NEEDED HERE -----
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterInOut *out_dist = avfilter_inout_alloc();
    AVFilterInOut *out_ref = avfilter_inout_alloc();

    filter_graph = avfilter_graph_alloc();

    // BUILD THE args STRING FOR THE DISTORTED VIDEO'S BUFFER SOURCE. THE FRAME PROPERTIES COME FROM dist_dec_ctx BECAUSE THAT IS THE DECODER WE ARE USING TO DECODE output.mkv. THE TIME BASE COMES FROM THE STREAM ITSELF, SAME REASON AS THE FILTERING CODE THE STREAM'S TIME BASE IS THE UNIT OF MEASUREMENT FOR ALL TIMESTAMPS IN THAT FILE'S PACKETS.
    AVRational dist_tb = dist_fmt_ctx->streams[dist_video_index]->time_base;
    snprintf(dist_args, sizeof(dist_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             dist_dec_ctx->width, dist_dec_ctx->height, dist_dec_ctx->pix_fmt,
             dist_tb.num, dist_tb.den,
             dist_dec_ctx->sample_aspect_ratio.num, dist_dec_ctx->sample_aspect_ratio.den);

    // SAME args STRING FOR THE REFERENCE VIDEO'S BUFFER SOURCE BUT BUILT FROM ref_dec_ctx AND ref_fmt_ctx. THE TWO ARGS STRINGS MIGHT BE DIFFERENT FROM EACH OTHER FOR EXAMPLE IF output.mkv WAS ENCODED AT A LOWER RESOLUTION THAN input.webm, THE WIDTH AND HEIGHT IN dist_args WILL BE SMALLER. libvmaf CAN HANDLE RESOLUTION DIFFERENCES BUT IDEALLY BOTH VIDEOS SHOULD BE THE SAME RESOLUTION FOR FAIR COMPARISON. REGARDLESS OUR PREVIOUS PROGRAM WAS PRINTING THE 
    AVRational ref_tb = ref_fmt_ctx->streams[ref_video_index]->time_base;
    snprintf(ref_args, sizeof(ref_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             ref_dec_ctx->width, ref_dec_ctx->height, ref_dec_ctx->pix_fmt,
             ref_tb.num, ref_tb.den,
             ref_dec_ctx->sample_aspect_ratio.num, ref_dec_ctx->sample_aspect_ratio.den);

    // CREATING THE DISTORTED BUFFER SOURCE NODE. SAME FUNCTION AS IN THE FILTERING CODE ---- avfilter_graph_create_filter() ALLOCATES THE AVFilterContext, INITIALISES IT WITH THE BUFFER BLUEPRINT, AND REGISTERS IT INSIDE filter_graph. THE NAME "dist" IS THE LABEL WE GIVE THIS SPECIFIC INSTANCE. THIS NAME IS WHAT CONNECTS TO [dist] IN THE filter_descr STRING WHEN avfilter_graph_parse_ptr() IS CALLED LATER.
    ret = avfilter_graph_create_filter(&buffersrc_dist, buffersource, "dist", dist_args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create distorted buffer source\n");
        return ret;
    }
    // SAME THING FOR THE REFERENCE BUFFER SOURCE. THE NAME "ref" CONNECTS TO [ref] IN THE filter_descr STRING. THE TWO BUFFER SOURCES ARE NOW REGISTERED IN THE GRAPH AS TWO INDEPENDENT ENTRY GATES, EACH WAITING TO RECEIVE FRAMES FROM THEIR RESPECTIVE DECODERS.
    ret = avfilter_graph_create_filter(&buffersrc_ref, buffersource, "ref", ref_args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create reference buffer source\n");
        return ret;
    }

    // SAME AS FILTERING CODE WE USE avfilter_graph_alloc_filter() + avfilter_init_dict() INSTEAD OF avfilter_graph_create_filter() FOR THE SINK BECAUSE WE WANT THE WINDOW TO SET OPTIONS BEFORE INITIALISATION IF NEEDED. WE PASS NULL HERE BECAUSE WE HAVE NO ADDITIONAL OPTIONS TO SET ON THE SINK.
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
    avfilter_init_dict(buffersink_ctx, NULL);

    // NOW COMES THE PART THAT IS DIFFERENT FROM THE FILTERING CODE. IN THE FILTERING CODE WE HAD ONE outputs STRUCT POINTING TO ONE buffersrc_ctx. HERE WE NEED TWO ENTRIES IN THE outputs LINKED LIST ONE FOR EACH BUFFER SOURCE BECAUSE avfilter_graph_parse_ptr() NEEDS TO KNOW WHERE TO WIRE BOTH ENDS OF THE [dist][ref]libvmaf CHAIN. THE NAMING CONFUSION IS THE SAME AS IN THE FILTERING CODE: out_dist AND out_ref ARE CALLED "outputs" FROM THE PERSPECTIVE OF THE FILTER STRING. THEY REPRESENT THE OUTPUT ENDPOINTS OF OUR PRE-CREATED NODES (THE TWO BUFFER SOURCES) THAT NEED TO BE WIRED TO THE INPUT SIDE OF THE PARSED FILTER CHAIN. THE BUFFER SOURCES FEED DATA OUT INTO THE FILTER STRING SO FROM THE STRING'S PERSPECTIVE, THEY ARE ON THE "OUTPUT" SIDE.
    // out_dist REPRESENTS THE ENTRY FOR "dist" THE NAME MUST MATCH [dist] IN filter_descr.
    out_dist->name = av_strdup("dist");
    out_dist->filter_ctx = buffersrc_dist;
    out_dist->pad_idx = 0;
    // THIS IS THE NEW PART COMPARED TO FILTERING CODE. ->next CHAINS THE TWO ENTRIES INTO A LINKED LIST. avfilter_graph_parse_ptr() WALKS THIS LIST TO FIND ALL NAMED OUTPUT PADS. IF YOU SET out_dist->next = NULL HERE, THE PARSER WOULD ONLY SEE ONE OUTPUT PAD AND FAIL BECAUSE [ref] WOULD BE UNRESOLVED IN THE FILTER STRING.
    out_dist->next = out_ref;

    // out_ref IS THE SECOND ENTRY IN THE LINKED LIST. SAME STRUCTURE AS out_dist BUT POINTING TO buffersrc_ref AND NAMED "ref" TO MATCH [ref] IN filter_descr. THIS IS THE LAST ENTRY SO ->next IS NULL TO TERMINATE THE LINKED LIST.
    out_ref->name = av_strdup("ref");
    out_ref->filter_ctx = buffersrc_ref;
    out_ref->pad_idx  = 0;
    out_ref->next = NULL;

    // THE SINK SIDE IS IDENTICAL TO THE FILTERING CODE. ONE inputs ENTRY, NAMED "out", POINTING TO buffersink_ctx. THIS IS WHERE THE FILTER STRING'S OUTPUT END WILL BE WIRED.
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    // SAME AS FILTERING CODE. avfilter_graph_parse_ptr() READS filter_descr, CREATES THE libvmaf FILTER NODE INTERNALLY, AND WIRES EVERYTHING TOGETHER: buffersrc_dist -> libvmaf (input pad 0, [dist]) buffersrc_ref -> libvmaf (input pad 1, [ref]) libvmaf -> buffersink_ctx AFTER THIS CALL THE ENTIRE GRAPH EXISTS IN MEMORY BUT IS NOT YET VALIDATED. NOTE: WE PASS &out_dist AS THE OUTPUTS ARGUMENT IT IS THE HEAD OF THE LINKED LIST. THE FUNCTION WILL WALK THE LIST USING ->next TO FIND ALL ENTRIES.
    ret = avfilter_graph_parse_ptr(filter_graph, filter_descr, &inputs, &out_dist, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot parse filter graph\n");
        return ret;
    }

    // SAME AS FILTERING CODE. VALIDATES ALL CONNECTIONS, NEGOTIATES FORMATS BETWEEN ALL FILTER NODES, AND FINALISES THE GRAPH. WITHOUT THIS CALL THE GRAPH IS JUST A STRUCTURE IN MEMORY WITH NO GUARANTEE THAT THE CONNECTIONS ARE ACTUALLY COMPATIBLE.
    ret = avfilter_graph_config(filter_graph, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot configure filter graph\n");
        return ret;
    }

    // SAME CLEANUP AS FILTERING CODE. avfilter_graph_parse_ptr() HAS ALREADY CONSUMED AND WIRED UP ALL THE CONNECTIONS FROM THESE STRUCTS INTO THE GRAPH. THE GRAPH OWNS THOSE LINKS NOW. THESE TEMPORARY HELPER STRUCTS ARE NO LONGER NEEDED.
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&out_dist);

    return 0;
}

// THIS IS A BRAND NEW HELPER FUNCTION THAT DID NOT EXIST IN EITHER THE TRANSCODING CODE OR THE FILTERING CODE. IN THOSE CODES, THE PACKET READING AND FRAME DECODING HAPPENED INSIDE THE MAIN LOOP DIRECTLY BECAUSE THERE WAS ONLY ONE FILE TO READ FROM. HERE WE HAVE TWO FILES AND WE NEED ONE FRAME FROM EACH OF THEM PER "ROUND" OF THE MAIN LOOP. RATHER THAN DUPLICATING THE ENTIRE READ-DECODE PATTERN TWICE INSIDE MAIN, WE EXTRACT IT INTO THIS HELPER FUNCTION THAT CAN BE CALLED FOR BOTH FILES. WHAT THIS FUNCTION DOES: IT KEEPS READING PACKETS FROM THE GIVEN FORMAT CONTEXT UNTIL IT SUCCESSFULLY GETS A DECODED FRAME OUT OF THE DECODER, THEN IT RETURNS THAT FRAME IN *frame. IT SKIPS NON-VIDEO PACKETS (AUDIO, SUBTITLES) EXACTLY LIKE THE main() LOOP DID IN PREVIOUS CODES. WHEN THE FILE IS EXHAUSTED, IT SENDS NULL TO THE DECODER TO FLUSH THE LAST BUFFERED FRAMES, AND RETURNS AVERROR_EOF WHEN TRULY DONE.

// PARAMETERS:   fmt_ctx     - THE FORMAT CONTEXT FOR THE FILE WE ARE READING FROM
//   dec_ctx - THE DECODER CONTEXT FOR THAT FILE'S VIDEO STREAM
//   video_index - THE STREAM INDEX OF THE VIDEO STREAM IN THAT FILE
//   pkt - A PRE-ALLOCATED PACKET TO USE AS THE READ BUFFER (REUSED ACROSS CALLS)
//   frame - THE AVFrame TO FILL WITH THE DECODED OUTPUT
// RETURNS 0 ON SUCCESS (frame IS VALID), AVERROR_EOF WHEN THE FILE IS DONE, NEGATIVE ON ERROR.
static int decode_one_frame(AVFormatContext *fmt_ctx, AVCodecContext  *dec_ctx, int video_index, AVPacket *pkt, AVFrame *frame)
{   int ret;
    while (1) {
        // TRY TO READ THE NEXT PACKET FROM THE FILE. IF THIS RETURNS AVERROR_EOF IT MEANS WE HAVE EXHAUSTED ALL PACKETS IN THE FILE. IN THAT CASE WE SEND NULL TO THE DECODER TO FLUSH ITS INTERNAL BUFFER JUST LIKE THE FLUSHING LOOPS AT THE END OF BOTH THE TRANSCODING CODE AND THE FILTERING CODE. THE DECODER HOLDS BACK A FEW FRAMES INTERNALLY TO HANDLE B-FRAME REORDERING, AND NULL IS THE SIGNAL TO RELEASE THEM.
        ret = av_read_frame(fmt_ctx, pkt);
        if (ret == AVERROR_EOF) {
            avcodec_send_packet(dec_ctx, NULL);
        } else if (ret < 0) {
            return ret;
        } else {
            // SAME CHECK AS IN BOTH PREVIOUS CODES WE ONLY WANT VIDEO PACKETS. AUDIO AND SUBTITLE PACKETS ARE SKIPPED.
            if (pkt->stream_index != video_index) {
                av_packet_unref(pkt);
                continue;
            }
            // SAME PUSH HALF OF THE DECODE LOOP AS IN ALL PREVIOUS CODES. HAND THE COMPRESSED PACKET TO THE DECODER AND LET IT START WORKING.
            ret = avcodec_send_packet(dec_ctx, pkt);
            av_packet_unref(pkt);
            if (ret < 0) return ret;
        }

        // SAME PULL HALF OF THE DECODE LOOP. TRY TO GET A DECODED FRAME OUT. AVERROR(EAGAIN) MEANS THE DECODER NEEDS MORE PACKETS BEFORE IT CAN GIVE US A FRAME. IN THAT CASE WE LOOP BACK AND READ ANOTHER PACKET. THIS IS IDENTICAL IN CONCEPT TO THE while (avcodec_receive_frame()) PATTERN IN THE TRANSCODING AND FILTERING CODES. AVERROR_EOF MEANS THE DECODER IS FULLY FLUSHED AND HAS NOTHING MORE TO GIVE US.
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == 0)  return 0;
        if (ret ==AVERROR_EOF)  return AVERROR_EOF;
        if (ret == AVERROR(EAGAIN))  continue;
        return ret;
    }
}
int main(void)
{
    // JUST LIKE THE FILTERING CODE WE NEED PACKET AND FRAME CONTAINERS. BUT NOW WE NEED TWO SETS ONE FOR EACH FILE. ref_pkt AND ref_frame ARE THE READ BUFFER AND DECODED OUTPUT FOR input.webm. dist_pkt AND dist_frame ARE THE READ BUFFER AND DECODED OUTPUT FOR output.mkv. filt_frame IS THE FRAME WE PULL FROM THE BUFFERSINK WE DISCARD IT IMMEDIATELY, BUT WE NEED THE CONTAINER TO CALL av_buffersink_get_frame().
    AVPacket *ref_pkt = av_packet_alloc();
    AVPacket *dist_pkt = av_packet_alloc();
    AVFrame *ref_frame  = av_frame_alloc();
    AVFrame *dist_frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();

    int ret;
    int frame_count = 0;

    // OPEN BOTH INPUT FILES. WE CALL THE SAME open_input_file() FUNCTION TWICE, PASSING DIFFERENT DESTINATION POINTERS EACH TIME. THE FIRST CALL SETS UP EVERYTHING FOR input.webm (ref) AND THE SECOND SETS UP EVERYTHING FOR output.mkv (dist).
    if (open_input_file(ref_filename,  &ref_fmt_ctx,  &ref_dec_ctx,  &ref_video_index)  < 0) return 1;
    if (open_input_file(dist_filename, &dist_fmt_ctx, &dist_dec_ctx, &dist_video_index) < 0) return 1;

    // BUILD THE FILTER GRAPH WITH TWO BUFFER SOURCES AND THE libvmaf FILTER. MUST BE CALLED AFTER BOTH open_input_file() CALLS BECAUSE init_filters() READS FROM BOTH DECODER CONTEXTS AND BOTH FORMAT CONTEXTS TO BUILD THE args STRINGS.
    if (init_filters() < 0) return 1;

    printf("Starting VMAF analysis...\n");

    // THIS IS THE MAIN LOOP. THE LOGIC IS FUNDAMENTALLY DIFFERENT FROM THE FILTERING CODE WHERE WE HAD ONE FILE TO READ FROM. HERE WE NEED ONE DECODED FRAME FROM EACH FILE PER ITERATION A MATCHED PAIR BECAUSE libvmaf COMPARES THEM FRAME BY FRAME. IF ONE FILE RUNS OUT BEFORE THE OTHER, WE STOP. THE FILES SHOULD HAVE THE SAME NUMBER OF FRAMES SINCE output.mkv WAS TRANSCODED DIRECTLY FROM input.webm.
    while (1) {
        // GET ONE DECODED FRAME FROM THE DISTORTED FILE (output.mkv). decode_one_frame() HANDLES ALL THE PACKET READING, STREAM FILTERING, AND DECODE LOOP INTERNALLY. IT RETURNS 0 WITH A VALID FRAME, OR AVERROR_EOF WHEN DONE.
        ret = decode_one_frame(dist_fmt_ctx, dist_dec_ctx, dist_video_index, dist_pkt, dist_frame);
        if (ret == AVERROR_EOF) break;
        if (ret < 0){
            goto end;
        } 

        // GET ONE DECODED FRAME FROM THE REFERENCE FILE (input.webm).
        // SAME FUNCTION, DIFFERENT FILE ARGUMENTS.
        ret = decode_one_frame(ref_fmt_ctx, ref_dec_ctx, ref_video_index, ref_pkt, ref_frame);
        if (ret == AVERROR_EOF) break;
        if (ret < 0){
            goto end;
        }
        // NOW WE PUSH BOTH FRAMES INTO THEIR RESPECTIVE BUFFER SOURCES.THIS IS THE SAME FUNCTION AS IN THE FILTERING CODE: av_buffersrc_add_frame_flags()PUSHES A RAW DECODED FRAME THROUGH THE ENTRY GATE INTO THE FILTER GRAPH.THE KEEP_REF FLAG TELLS THE GRAPH TO MAKE ITS OWN REFERENCE TO THE FRAME DATA SO WE CAN SAFELY av_frame_unref() OUR COPY WITHOUT DESTROYING THE GRAPH'S COPY.WE PUSH dist_frame INTO buffersrc_dist THIS IS THE [dist] PAD OF libvmaf.WE PUSH ref_frame INTO buffersrc_ref THIS IS THE [ref] PAD OF libvmaf.
        ret = av_buffersrc_add_frame_flags(buffersrc_dist, dist_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) goto end;
        ret = av_buffersrc_add_frame_flags(buffersrc_ref, ref_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (ret < 0) goto end;

        // NOW WE DRAIN THE BUFFERSINK. THIS IS THE SAME PULL PATTERN AS THE FILTERING CODE. THE REASON WE NEED THIS LOOP IS THAT av_buffersrc_add_frame_flags() ALONE DOES NOT TRIGGER libvmaf TO RUN. THE COMPUTATION HAPPENS WHEN THE GRAPH IS PULLED FROM THE SINK SIDE. CALLING av_buffersink_get_frame() IS WHAT ACTUALLY TRIGGERS libvmaf TO TAKE THE FRAMES FROM BOTH BUFFERSRCS, COMPARE THEM, AND LOG THE VMAF SCORE. ONCE av_buffersink_get_frame() RETURNS, vmaf.json HAS BEEN UPDATED WITH THE SCORE FOR THIS FRAME PAIR. WE IMMEDIATELY DISCARD filt_frame BECAUSE WE DO NOT NEED IT.
        while (1) {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                goto end;

            // THIS IS THE KEY DIFFERENCE FROM THE FILTERING CODE. IN THE FILTERING CODE THIS IS WHERE WE CALLED encode_and_write(filt_frame). HERE WE DO NOTHING WITH THE FRAME. VMAF HAS ALREADY DONE ITS JOB INSIDE THE FILTER NODE THE SCORE IS WRITTEN TO vmaf.json. WE JUST NEED TO RELEASE THE FRAME MEMORY AND MOVE ON.
            av_frame_unref(filt_frame);
            frame_count++;
        }

        // RELEASE OUR REFERENCES TO THE DECODED FRAMES AFTER THE SINK IS DRAINED. SAME REASON AS FILTERING CODE AV_BUFFERSRC_FLAG_KEEP_REF MEANS THE GRAPH HAS ITS OWN COPY, SO WE CAN SAFELY RELEASE OURS HERE.
        av_frame_unref(dist_frame);
        av_frame_unref(ref_frame);
    }
end:
    // CLEANUP SAME ORDER AS THE FILTERING CODE: FILTER GRAPH FIRST BECAUSE IT REFERENCES THE CODEC CONTEXTS, THEN CODEC CONTEXTS, THEN FORMAT CONTEXTS, THEN FRAMES AND PACKETS. NOTE THERE IS NO av_write_trailer(), NO avio_closep(), NO avformat_free_context() FOR AN OUTPUT FILE BECAUSE THIS PROGRAM NEVER OPENED AN OUTPUT CONTAINER IN THE FIRST PLACE.
    avfilter_graph_free(&filter_graph);

    avcodec_free_context(&ref_dec_ctx);
    avcodec_free_context(&dist_dec_ctx);

    avformat_close_input(&ref_fmt_ctx);
    avformat_close_input(&dist_fmt_ctx);

    av_frame_free(&ref_frame);
    av_frame_free(&dist_frame);
    av_frame_free(&filt_frame);

    av_packet_free(&ref_pkt);
    av_packet_free(&dist_pkt);

    return (ret < 0 && ret != AVERROR_EOF) ? 1 : 0;
}