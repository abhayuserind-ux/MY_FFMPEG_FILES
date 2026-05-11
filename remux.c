#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>

const char *in = "input.webm";
const char *out = "output.mkv";

int main() {
    printf("FFmpeg version: %s\n", av_version_info());
    //CREATE A POINTER FOR THE HEADERS OF BOTH INPUT AND OUTPUT FILE CONTEXT
    //i is for input, o is for output AVFormat Context
    AVFormatContext *i = NULL, *o = NULL;
    //ASSIGNING MEMEORY FOR THE PACKET
    //AVPacket is compressed set of frames, decoder will decode this to get frames.
    AVPacket *p = av_packet_alloc();

    //OPEN THE INPUT FILE'S FORMAT
    //THIRD ARGUMENT GUESSES THE FORMAT/CONTAINTER TYPE IF IT IS 0
    avformat_open_input(&i, in, 0, 0);
    //FIND THE STREAMS
    avformat_find_stream_info(i, 0);
    
    avformat_alloc_output_context2(&o, 0, 0, out);
    // AVStream *os = avformat_new_stream(o, 0);
    // struct AVCodecParameters temp = os->codecpar;
    // printf("NUMBER OF STREAMS: %d\n", i->nb_streams);
    AVStream *is;
    AVStream *os;
    for (int s = i->nb_streams - 1; s >= 0 ; s--) {
        is = i->streams[s];
        os = avformat_new_stream(o, 0);

        printf("ITERATION s = %d:\n", s);
        printf("[INPUT]  Actual Stream Index: %d | Codec Type: %s\n", is->index, av_get_media_type_string(is->codecpar->codec_type));

        // Print the OUTPUT stream before copying parameters
        printf("[OUTPUT] Actual Stream Index: %d | Codec Type (Before Copy): %s\n", os->index, av_get_media_type_string(os->codecpar->codec_type));    
        // printf("s: %d\n", s);
        avcodec_parameters_copy(os->codecpar, is->codecpar);
        os->codecpar->codec_tag = 0;
        // Print the OUTPUT stream after copying parameters
        printf("[OUTPUT] Actual Stream Index: %d | Codec Type (After Copy) : %s\n\n", os->index, av_get_media_type_string(os->codecpar->codec_type));
        // printf("%d\n",os->codecpar->ch_layout.nb_channels);
        // int val = os->codecpar->codec_id;
        // printf("%s\n", avcodec_get_name(val));
        // int num = m;
        // int den = os->r_frame_rate.den;
        // printf("%d\n", is->codecpar->sample_rate);
        // printf("%d")
        
    }
    // for (int s = i->nb_streams - 1; s >= 0 ; s--) {
    //     // printf("s: %d\n", s);
    //     avcodec_parameters_copy(os->codecpar, is->codecpar);
    //     os->codecpar->codec_tag = 0;
    //     // printf("%d\n",os->codecpar->ch_layout.nb_channels);
    //     // int val = os->codecpar->codec_id;
    //     // printf("%s\n", avcodec_get_name(val));
    //     // int num = m;
    //     // int den = os->r_frame_rate.den;
    //     // printf("%d\n", is->codecpar->sample_rate);
    //     // printf("%d")
    // }

    if (!(o->oformat->flags & AVFMT_NOFILE))
        avio_open(&o->pb, out, AVIO_FLAG_WRITE);
        //pb stands for Protocol Buffer
        // TWO MAJOR THINGS ARE HAPPENIG HERE: 1) WE ARE CHECKING THE FORMAT FOR A VERY SPECIFIC TYPE OF FLAG, WHICH IS TO CHECK IF IT NEEDS A FILE OR NOT --- THERE ARE MAJORLY TWO TYPES OF FORMATS WHEN IT COMES TO VIDEO PROCESSING -- FILE BASED, BASIC NORMAL VIDEO FILES AND STREAM BASED, THE VIDEO FORMATS THAT ARE GOIN TO BE STREAMED, THE FORMATS THAT ARE GOING TO BE STREAMED DON'T A FILE TO OPERATE, THEY CAN JUST DIRECTLY SEND THE OUTPUT TO SERVER AND TO CLIENT, BUT OUT FORMAT NEEDS FILES TO OPERATE, SO WE ARE CHECKING ----"IF MY CURRENT CHOOSEN FORMAT NEEDS A FILE, THEN DO THE FOLLOWING"

        // 2) THE SECOND THING -- AVIO_OPEN() IS FOR CREATING AN OUTPUT STREAM -- AGAIN, WE KNOW HOW COMMONLY THE COMPUTER WRITES DATA INTO THE HARDDISK, FIRST IT WILL CREATE A BUFFER AND THEN WRITE DATA IN CHUNKS INTO THE FILE IN HARDDISK -- PB HERE IS OUT BUFFER POINTER TO AVIOContext STRUCUTRE, OUT IS FILE NAME AND AVIO_FLAG_WRITE IS OUR SIGNAL TO THE OS THAT "YES ALLOW THIS FUNCTION TO WRITE INTO THE FILE", CAUSE USUALLY THE IS GUARDS A FILE UNTIL WE EXPLICITLY TELL IT OTHERWISE.
        //DOUBTS

    avformat_write_header(o, 0);

    
    while (av_read_frame(i, p) >= 0) {

        // STEP 1: CAPTURE THE INPUT STREAM BEFORE TOUCHING p->stream_index
        // p->stream_index at this point is the RAW INDEX FROM THE INPUT FILE
        // (0 = video, 1 = audio in your input.webm)
        // We need to save this now, because we are about to modify p->stream_index below.
        // If we moved this line AFTER the swap, is would point to the WRONG input stream,
        // and the time_base rescaling further below would use wrong values.
        AVStream *is = i->streams[p->stream_index];

        // SAVE THE ORIGINAL INPUT INDEX FOR DIAGNOSTIC PRINTING ONLY
        // We need this because we are about to overwrite p->stream_index in Step 2.
        // Without saving it here, we would lose the original value and could not print it.
        int original_input_index = p->stream_index;

        // STEP 2: SWAP p->stream_index TO MATCH OUR SWAPPED OUTPUT STREAM TABLE
        // Our for loop built the output stream table in reverse:
        //   o->streams[0] = AUDIO (copied from i->streams[1])
        //   o->streams[1] = VIDEO (copied from i->streams[0])
        // So when a packet comes in claiming stream_index=1 (audio), it must be 
        // redirected to output stream 0 (where the audio track now lives), and vice versa.
        // This swap also matters for av_interleaved_write_frame() at the bottom --
        // that function reads p->stream_index to decide WHICH TRACK to mux the packet into.
        // Without this fix, video packets were being written into the audio track and vice versa.
        if (p->stream_index == 1) {
            p->stream_index = 0;
        } else if (p->stream_index == 0) {
            p->stream_index = 1;
        }

        // STEP 3: NOW look up the output stream using the SWAPPED index
        // At this point p->stream_index is already the new output index,
        // so os correctly points to the matching output stream.
        AVStream *os = o->streams[p->stream_index];


        //OLD PTS TO NEW PTS new_pts = old_pts * (input_tb / output_tb)
        // printf("%s\n","PTS, DTS, DURATION, BEFORE RESCALLING");
        // printf("%d, %s\n", p->pts, "PTS");
        // printf("%d, %s\n", p->dts, "DTS");
        // printf("%d, %s\n", p->duration, "Duration");
        // printf("%s, %d, %d,%d\n","[BEFORE RESCALLING] PTS, DTS, DURATION", p->pts, p->dts, p->duration);
        // printf("%s , %d \n","INPUT TIME BASE: ", is->time_base.num);
        // printf("%s , %d \n","INPUT TIME BASE: ", is->time_base.den);

        // printf("%s , %d \n","OUTPUT TIME BASE: ", os->time_base.num);
        // printf("%s , %d \n","OUTPUT TIME BASE: ", os->time_base.den);
        // printf("video stream : %d audio stream : %d \n", AVMEDIA_TYPE_VIDEO, AVMEDIA_TYPE_AUDIO );
        // printf("video stream : %s audio stream : %s \n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO), av_get_media_type_string(AVMEDIA_TYPE_AUDIO ));

        // printf("video stream : %s || audio stream : %s \n", av_get_media_type_string(p->stream_index), av_get_media_type_string(p->stream_index));
        // printf("video stream : %s || audio stream : %s \n", av_get_media_type_string(p->stream_index), av_get_media_type_string(p->stream_index));


        // BEFORE (WRONG):  printf("stream index : %d", is->codecpar->codec_type)
        //   codec_type is an ENUM VALUE (AVMEDIA_TYPE_VIDEO=0, AVMEDIA_TYPE_AUDIO=1)
        //   It is NOT the stream index. It will ALWAYS match between input and output
        //   because audio data always has codec_type=1 no matter which index it lives at.
        //   This is why the stream index was 1 and audio on both INPUT and OUTPUT lines
        //
        // AFTER (CORRECT): print is->index and p->stream_index separately
        //   is->index = the actual track number in the INPUT  file (0 or 1)
        //   p->stream_index = the actual track number in the OUTPUT file (0 or 1, now swapped)
        //   These two numbers WILL differ when the swap is working correctly.
        // printf("%s\n", "OUTPUT STREAM");
        // printf("[INPUT]  actual stream index: %d | codec type: %s\n", original_input_index, av_get_media_type_string(is->codecpar->codec_type));

        // printf("%s\n", "INPUT STREAM");
        // printf("[OUTPUT] actual stream index: %d | codec type: %s\n", p->stream_index, av_get_media_type_string(os->codecpar->codec_type));

        // printf("%d, %d\n", p->stream_index, is->codecpar->codec_type);
        // printf("%d, %d\n", p->stream_index, os->codecpar->codec_type);


        //PRINTING THE FRAME RATE
        // if (is->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        //     int numerator = is->avg_frame_rate.num;
        //     int denumerator = is->avg_frame_rate.den;
        //     printf("[INPUT] FrameRate: %d \n", (numerator/denumerator));
        // }
        // if (os->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        //     int numerator = os->avg_frame_rate.num;
        //     int denumerator = os->avg_frame_rate.den;
        //     printf("[OUPUT] FrameRate: %d \n", (numerator/denumerator));
        // }

        // PRINTING THE SAMPLE FORMAT
        // if(is->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
        //     enum AVSampleFormat input_format = is->codecpar->format;
        //     printf("[INPUT] %s \n", av_get_sample_fmt_name(input_format));
        // }
        // if(os->codecpar->codec_type == AVMEDIA_TYPE_AUDIO){
        //     enum AVSampleFormat output_format = os->codecpar->format;
        //     printf("[OUTPUT] %s \n", av_get_sample_fmt_name(output_format));
        // }


        // STEP 4: RESCALE TIMESTAMPS FROM INPUT TIME BASE TO OUTPUT TIME BASE
        // is->time_base = the clock resolution of the ORIGINAL input stream (saved in Step 1)
        // os->time_base = the clock resolution of the DESTINATION output stream (looked up in Step 3)
        // These two time bases are independent — even though the codec is the same,
        // the container may assign a different time base to each stream.
        // av_rescale_q converts the raw integer timestamp from one clock to another.
        //IMPORTANT...
        p->pts = av_rescale_q_rnd(p->pts, is->time_base, os->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        p->dts = av_rescale_q_rnd(p->dts, is->time_base, os->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
        p->duration = av_rescale_q(p->duration, is->time_base, os->time_base);
        // printf("%s , %d , %d , %d\n","[AFTER RESCALLING] PTS, DTS, DURATION", p->pts, p->dts, p->duration);

        p->pos = -1;

        // STEP 5: WRITE THE PACKET
        // At this point p->stream_index is the SWAPPED output index (set in Step 2)
        // av_interleaved_write_frame reads p->stream_index internally to decide which
        // track to write into -- this is why Step 2 was critical.
        //WRITE THE PACKET TO THE OUTPUT FILE
        av_interleaved_write_frame(o,p);
        // printf("%d, %s\n", p->pts, "PTS");
        // printf("%d, %s\n", p->dts, "DTS");
        // printf("%d, %s\n", p->duration, "Duration");

        // printf("%d", os->avg_frame_rate.den);
        //checks timestamps
        // puts packets in correct order
        // writes them according to container rules
        // maintains proper audio-video sync
        //FREE THE PACKET
        av_packet_unref(p);

        //WHY DO WE DO REFRENCEING AND DEREFRENCING IN C?
        //THIS IS KNOWN AS REFERENCE COUNTING.
        //In order to understand why we need to understand what is wrong with the previous proposed method of creating and deleting a pointer again and again in 
        //a loop where hundereds of different packets are going to be used across several threads. When we try to create a new pointer in a loop over and over 
        //again we use expensive memory allocation and system calls such as malloc().
        //This might be fine at a smaller scale but at a larger scale it is going to take too much time and unnecessary load on the larger system's infrastrucure
        
        //SOLUTION:
        //If there is one single resource that is going to be accessed by mutiple similar pointer, usually all the pointers being in different threads, then
        //it is better to have a list of all the pointers who are currently pointing to that buffer, or simply having a count also works, this count is known as 
        //*******ref_count*********, and the process of keeping how many pointers are refering to a single buffer at a time, calling a ref and calling unref on 
        //a single buffer across several pointers is called reference counting.
        //REFERENCES: https://www.intel.com/content/www/us/en/docs/onetbb/developer-guide-api-reference/2022-0/reference-counting.html
        //https://mailund.dk/posts/c-refcount-list/#:~:text=So%2C%20my%20reference%20counting%20code,language%2C%20and%20that%20escalated%20quickly.
        //https://nullprogram.com/blog/2015/02/17/

        //TO SEE THIS HAPPENING ---- av_write_trailer() --> av_buffer_unref() --> buffer_replace() --> ref_count present in AVBuffer
        //USUALLY In multi threaded environments we use atomic data types to do the ref_count increament and decreament actions in order to avoid mutex locks which again are expensive
        //When the reference count reaches zero, the pointer is automatically destroyed in most programming languages, in C we have to mannualy call the av_packet_free() which is simply av_packet_unref() + freeing and destroying the pointer. This is the main reason for the existence of the two different menthods, why pointers should not be created and deleted again and agian in loops and why it all exists.

    }
    //COMPLETE THE REMUXING BY WRITING BACK THE INDEX AND REST DATA INTO THE OUTPUT FILE.
    // printf("%s\n", o->video_codec->name);
    av_write_trailer(o);
        // final index --- INDEX USUALLY CONTAINS THE SOME IMPORTANT FRAME AND TIME PAIR VALUES, SUPPOSE THE PLAYER HAS TO JUMP TO A RANDOM POINT IN VIDEO, IT WON'T GO THROUGH ALL THE FRAMES ONE BY ONE, IT WILL JUMP THE LEFT-CLOSEST I FRAME, THE VALUE OF THESE I FRAMES IS USUALLY IN THIS INDEX TABLE
        // packet summary
        // duration fixes
        // final timestamps
        // seek tables
        // metadata completion --- final bit rate, chapter markers, and any tags like artist name etc.
        
    return 0;

}