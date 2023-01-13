#include <cstdio>
#include <cstdint>
#include <vector>
#include <deque>
#include <optional>
#include <string>
#include <fstream>
#include <thread>
#include <chrono>
#include "cinepunk.h"
#include "lodepng.h"
#include "riff.hpp"

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
#endif

void test_still(std::string basename) {
    fprintf(stderr,"Test %s\n",basename.c_str());
    std::vector<uint8_t> in_buffer;
    unsigned width,height;
    lodepng::decode(in_buffer,width,height,basename+".png",LCT_RGB);
    //lodepng::encode("alice_raw.png",in_buffer,width,height,LCT_RGB);

    std::vector<CPYuvBlock> yuv_buffer(in_buffer.size()/12);
    std::vector<CPYuvBlock> yuv_buffer2(in_buffer.size()/12);
    std::vector<uint8_t> out_buffer(in_buffer.size());

    CP_rgb2yuv_fast(yuv_buffer.data(),in_buffer.data(),width/2,height/2);
    CP_yuv2rgb(out_buffer.data(),yuv_buffer.data(),width/2,height/2);
    lodepng::encode(basename+"_fastyuv.png",out_buffer,width,height,LCT_RGB);
    //CP_yuv2gray(out_buffer.data(),yuv_buffer.data(),width/2,height/2);
    //lodepng::encode(basename+"_fastyuv_gray.png",out_buffer,width,height,LCT_GREY);

    CP_rgb2yuv_hq(yuv_buffer.data(),in_buffer.data(),width/2,height/2);
    CP_yuv2rgb(out_buffer.data(),yuv_buffer.data(),width/2,height/2);
    lodepng::encode(basename+"_hqyuv.png",out_buffer,width,height,LCT_RGB);
    //CP_yuv2gray(out_buffer.data(),yuv_buffer.data(),width/2,height/2);
    //lodepng::encode(basename+"_hqyuv_gray.png",out_buffer,width,height,LCT_GREY);

    CP_yuv_downscale_fast(yuv_buffer2.data(),yuv_buffer.data(),width/4,height/4,(width/2)&1);
    CP_yuv2rgb(out_buffer.data(),yuv_buffer2.data(),width/4,height/4);
    lodepng::encode(basename+"_downscale.png",out_buffer,width/4*2,height/4*2,LCT_RGB);

    {
        std::vector<uint8_t> cinep_buffer(CP_BUFFER_SIZE(width,height,1));
        auto encoder = CP_create_encoder(width,height,1);
        CP_push_frame(encoder,CP_YUVBLOCK,yuv_buffer.data());
        CP_push_frame(encoder,CP_YUVBLOCK,nullptr);
        auto cinep_size = CP_pull_frame(encoder,cinep_buffer.data());
        cinep_buffer.resize(cinep_size);
        CP_destroy_encoder(encoder);
        lodepng::save_file(cinep_buffer,basename+"_bad.cinep");

        
        auto decoder = CP_create_decoder(width,height);
        CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
        CP_destroy_decoder(decoder);
        lodepng::encode(basename+"_bad.cinep.png",out_buffer,width,height,LCT_RGB);
        decoder = CP_create_decoder(width,height);
        CP_set_decoder_debug(decoder,CP_DECDEBUG_CRYPTOMATTE);
        CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
        CP_destroy_decoder(decoder);
        lodepng::encode(basename+"_bad_cryptomatte.cinep.png",out_buffer,width,height,LCT_RGB);
    }
    {
        std::vector<uint8_t> cinep_buffer(CP_BUFFER_SIZE(width,height,3));
        auto encoder = CP_create_encoder(width,height,3);
        CP_push_frame(encoder,CP_YUVBLOCK,yuv_buffer.data());
        CP_push_frame(encoder,CP_YUVBLOCK,nullptr);
        auto cinep_size = CP_pull_frame(encoder,cinep_buffer.data());
        cinep_buffer.resize(cinep_size);
        CP_destroy_encoder(encoder);
        lodepng::save_file(cinep_buffer,basename+"_3strip.cinep");

        auto decoder = CP_create_decoder(width,height);
        CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
        CP_destroy_decoder(decoder);
        lodepng::encode(basename+"_3strip.cinep.png",out_buffer,width,height,LCT_RGB);
        decoder = CP_create_decoder(width,height);
        CP_set_decoder_debug(decoder,CP_DECDEBUG_CRYPTOMATTE);
        CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
        CP_destroy_decoder(decoder);
        lodepng::encode(basename+"_3strip_cryptomatte.cinep.png",out_buffer,width,height,LCT_RGB);
    }
    

}

inline std::optional<std::string> get_string(std::deque<std::string> &args) {
    if (args.empty()) return {};
    auto str = args.front();
    args.pop_front();
    return str;
}

static void argFail(const char *exename) {
    fprintf(stderr,"Invalid arguments\n\n");
    fprintf(stderr,"  %s [encode/encstill] [input file] [output file]\n",exename);
    fprintf(stderr,"  %s [decode/decstill] [input file] [output file]\n",exename);
    exit(-1);
}

int main(int argc, char **argv) {
    fprintf(stderr,"Cinepunk VQ Video Encoder!\n");

    std::deque<std::string> args(argv + 1, argv + argc);

    auto mode = get_string(args);
    if (mode) fprintf(stderr,"mode: %s\n",mode.value().c_str());
    if (mode == "encstill" || mode == "encraw") {
        unsigned width = 640, height = 480, max_strips = 3, rate = 30;
        bool makeAvi = false;
        std::optional<std::string> infile,outfile;
        while (!args.empty()) {
            auto arg = get_string(args);
            if (arg == "-q") {
                auto argval = get_string(args);
                if (!argval) argFail(argv[0]);
                fprintf(stderr,"Quality level NYI\n");
            } else if (arg == "-strips") {
                auto argval = get_string(args);
                if (!argval) argFail(argv[0]);
                max_strips = std::stoi(argval.value());
            } else if (arg == "-w" && mode == "encraw") {
                auto argval = get_string(args);
                if (!argval) argFail(argv[0]);
                width = std::stoi(argval.value());
            } else if (arg == "-h" && mode == "encraw") {
                auto argval = get_string(args);
                if (!argval) argFail(argv[0]);
                height = std::stoi(argval.value());
            } else if (arg == "-r" && mode == "encraw") {
                auto argval = get_string(args);
                if (!argval) argFail(argv[0]);
                rate = std::stoi(argval.value());
            } else if (arg == "-avi" && mode == "encraw") {
                makeAvi = true;
            } else if (!infile) {
                infile = arg;
            } else if (!outfile) {
                outfile = arg;
            } else {
                argFail(argv[0]);
            }
        }
        if (!(infile && outfile)) argFail(argv[0]);
        if (mode == "encstill") {
            std::vector<uint8_t> rgb_buffer;
            auto in_err = lodepng::decode(rgb_buffer,width,height,infile.value(),LCT_RGB);
            if (in_err) {
                fprintf(stderr,"PNG load error: %d\n",in_err);
                exit(-1);
            }
            auto encoder = CP_create_encoder(width,height,max_strips); // TODO strip buffers
            std::vector<uint8_t> cinep_buffer(CP_get_buffer_size(encoder));
            CP_push_frame(encoder,CP_RGB24,rgb_buffer.data());
            CP_push_frame(encoder,CP_RGB24,nullptr);
            auto cinep_size = CP_pull_frame(encoder,cinep_buffer.data());
            CP_destroy_encoder(encoder);
            cinep_buffer.resize(cinep_size);
            fprintf(stderr,"CineP size is %zu\n",cinep_size);
            auto out_err = lodepng::save_file(cinep_buffer,outfile.value());
            if (out_err) {
                fprintf(stderr,"CineP save error: %d\n",out_err);
                exit(-1);
            }
        } else if (mode == "encraw") {
            unsigned frame_count = 0;
            std::fstream file_in,file_out;
            std::istream *input;
            std::ostream *output;
            if (infile == "-") {
                input = &std::cin;
                #ifdef _WIN32
                    setmode(fileno(stdin),O_BINARY);
                #endif
            } else {
                input = &file_in;
                file_in.open(infile.value(),std::ios_base::in | std::ios_base::binary);
            }
            if (outfile == "-") {
                output = &std::cout;
                #ifdef _WIN32
                    setmode(fileno(stdout),O_BINARY);
                #endif
            } else {
                output = &file_out;
                file_out.open(outfile.value(),std::ios_base::out | std::ios_base::binary);
            }
            SimpleAVIWriter avi(*output);
            auto encoder = CP_create_encoder(width,height,max_strips);
            std::vector<uint8_t> rgb_buffer(width*height*3);
            std::vector<uint8_t> cvid_buffer(CP_get_buffer_size(encoder));
            bool stream_end = false;
            if (makeAvi) avi.begin_simple("cvid"_4cc,width,height,1,rate);
            for (;;) {
                if (!stream_end) {
                    input->read(reinterpret_cast<std::istream::char_type *>(rgb_buffer.data()),rgb_buffer.size());
                    if (input->eof()) {
                        stream_end = true;
                        fprintf(stderr,"WTF EOF\n");
                        CP_push_frame(encoder,CP_RGB24,nullptr);
                    } else {
                        CP_push_frame(encoder,CP_RGB24,rgb_buffer.data());
                    }
                }
                auto size = CP_pull_frame(encoder,cvid_buffer.data());
                if (size > 0) {
                    if (makeAvi) {
                        avi.write_chunk("00dc"_4cc,cvid_buffer.data(),size);
                    } else {
                        output->write(reinterpret_cast<std::istream::char_type *>(cvid_buffer.data()),size);
                    }
                    frame_count++;
                } else if (stream_end) {
                    break;
                }
            }
            fprintf(stderr,"Processed %u frames\n",frame_count);
            if (makeAvi) avi.set_total_frames(frame_count);
            if (makeAvi) avi.end_simple();
            CP_destroy_encoder(encoder);
        }
    } else if (mode == "decstill") {
        std::optional<std::string> infile,outfile;
        uint32_t debugFlags = 0;
        while (!args.empty()) {
            auto arg = get_string(args);
            if (arg == "-crypto") {
                debugFlags |= CP_DECDEBUG_CRYPTOMATTE;
            } else if (!infile) {
                infile = arg;
            } else if (!outfile) {
                outfile = arg;
            } else {
                argFail(argv[0]);
            }
        }
        if (!(infile && outfile)) argFail(argv[0]);
        std::vector<uint8_t> cinep_buffer;
        auto in_err = lodepng::load_file(cinep_buffer,infile.value());
        if (in_err) {
            fprintf(stderr,"CineP load failed: %d\n",in_err);
            exit(-1);
        }
        unsigned width,height;
        if (!CP_peek_dimensions(cinep_buffer.data(),cinep_buffer.size(),&width,&height,nullptr)) {
            fprintf(stderr,"Failed to peek CineP size\n");
            exit(-1);
        }
        std::vector<uint8_t> rgb_buffer(width*height*3);
        auto decoder = CP_create_decoder(width,height);
        CP_set_decoder_debug(decoder,debugFlags);
        CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,rgb_buffer.data());
        CP_destroy_decoder(decoder);
        auto out_err = lodepng::encode(outfile.value(),rgb_buffer,width,height,LCT_RGB);
        if (out_err) {
            fprintf(stderr,"PNG save failed: %d\n",out_err);
            exit(-1);
        }
    } else if (mode == "yuvtest") {
        std::optional<std::string> infile,outfile;
        bool doFast = false;
        while (!args.empty()) {
            auto arg = get_string(args);
            if (arg == "-fast") {
                doFast = true;
            } else if (arg == "-hq") {
                doFast = false;
            } else if (!infile) {
                infile = arg;
            } else if (!outfile) {
                outfile = arg;
            } else {
                argFail(argv[0]);
            }
        }
        if (!(infile && outfile)) argFail(argv[0]);

        std::vector<uint8_t> in_buffer;
        unsigned width,height;
        lodepng::decode(in_buffer,width,height,infile.value(),LCT_RGB);
        std::vector<CPYuvBlock> yuv_buffer(in_buffer.size()/12);

        if (doFast) {
            CP_rgb2yuv_fast(yuv_buffer.data(),in_buffer.data(),width/2,height/2);
        } else {
            CP_rgb2yuv_hq(yuv_buffer.data(),in_buffer.data(),width/2,height/2);
        }
        CP_yuv2rgb(in_buffer.data(),yuv_buffer.data(),width/2,height/2);
        lodepng::encode(outfile.value(),in_buffer,width,height,LCT_RGB);

    } else if (mode == "cvid2avi") {
        std::optional<std::string> infile,outfile;
        uint32_t rate = 30;
        while (!args.empty()) {
            auto arg = get_string(args);
            if (arg == "-r") {
                auto rate_str = get_string(args);
                if (!rate_str) argFail(argv[0]);
                rate = std::stoi(rate_str.value());
            } else if (!infile) {
                infile = arg;
            } else if (!outfile) {
                outfile = arg;
            } else {
                argFail(argv[0]);
            }
        }
        if (!(infile && outfile)) argFail(argv[0]);
        
        std::fstream input(infile.value(),std::ios_base::in | std::ios_base::binary);
        std::fstream output(outfile.value(),std::ios_base::out | std::ios_base::binary);

        uint32_t frame_count = 0;

        std::vector<uint8_t> frame_buffer;

        // Try reading first frame to peek width/height
        frame_buffer.resize(8);
        input.read(reinterpret_cast<std::fstream::char_type *>(frame_buffer.data()),8);
        if (input.fail()) {
            fprintf(stderr,"header read error\n");
            exit(-1);
        }
        unsigned width,height;
        if (!CP_peek_dimensions(frame_buffer.data(),8,&width,&height,nullptr)) {
            fprintf(stderr,"bad header\n");
            exit(-1);
        }

        SimpleAVIWriter avi(output);
        avi.begin_simple("cvid"_4cc,width,height,1,rate);
        for (;;) {
            size_t frame_size;
            if (frame_count != 0) {
                frame_buffer.resize(8);
                input.read(reinterpret_cast<std::fstream::char_type *>(frame_buffer.data()),8);
                if (input.eof()) break;
            }
            if (!CP_peek_dimensions(frame_buffer.data(),8,nullptr,nullptr,&frame_size)) {
                fprintf(stderr,"bad header (frame %u)\n",frame_count);
                exit(-1);
            }
            frame_buffer.resize(frame_size);
            input.read(reinterpret_cast<std::fstream::char_type *>(frame_buffer.data()+8),frame_size-8);
            if (input.eof()) {
                fprintf(stderr,"Frame %u truncated\n",frame_count);
                break;
            }
            if (frame_buffer.size()&1) frame_buffer.push_back(0);
            avi.write_chunk("00dc"_4cc,frame_buffer.data(),frame_buffer.size());
            frame_count++;
        }
        avi.set_total_frames(frame_count);
        avi.end_simple();
        fprintf(stderr,"Processed %u frames\n",frame_count);
        
    
    } else {
        argFail(argv[0]);
    }
    



    /*
    // decode 

    auto decoder = CP_create_decoder(640,480);
    std::vector<uint8_t> cinep_buffer;
    std::vector<uint8_t> out_buffer(640*480*3);
    lodepng::load_file(cinep_buffer,"test/fufu_ffmpeg_max.cinep");
    CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
    lodepng::encode("test/fufu_ffmpeg_max.cinep.png",out_buffer,640,480,LCT_RGB);
    CP_set_decoder_debug(decoder,CP_DECDEBUG_CRYPTOMATTE);
    CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
    lodepng::encode("test/fufu_ffmpeg_max_cryptomatte.cinep.png",out_buffer,640,480,LCT_RGB);
    CP_destroy_decoder(decoder);


    test_still("test/akared");
    test_still("test/psycred");
    test_still("test/kokoro");
    test_still("test/alice");
    test_still("test/fufu");
    test_still("test/fufu_lowres");
    test_still("test/fufu_hueg");
    test_still("test/junko");
    test_still("test/pigge");
    test_still("test/ok_i_guess");
    */
}

