#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include "cinepunk.h"
#include "lodepng.h"


void test_still(std::string basename) {
    printf("Test %s\n",basename.c_str());
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

    CP_yuv_downscale_fast(yuv_buffer2.data(),yuv_buffer.data(),width/4,height/4);
    CP_yuv2rgb(out_buffer.data(),yuv_buffer2.data(),width/4,height/4);
    lodepng::encode(basename+"_downscale.png",out_buffer,width/2,height/2,LCT_RGB);

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
    

}

int main(int argc, char **argv) {
    printf("Henlo wowld\n");

    auto decoder = CP_create_decoder(640,480);
    std::vector<uint8_t> cinep_buffer;
    std::vector<uint8_t> out_buffer(640*480*3);
    lodepng::load_file(cinep_buffer,"test/fufu_ffmpeg.cinep");
    CP_decode_frame(decoder,cinep_buffer.data(),cinep_buffer.size(),CP_RGB24,out_buffer.data());
    lodepng::encode("test/fufu_ffmpeg.cinep.png",out_buffer,640,480,LCT_RGB);
    CP_destroy_decoder(decoder);


    //test_still("test/akared");
    //test_still("test/psycred");
    //test_still("test/kokoro");
    //test_still("test/alice");
    test_still("test/fufu");
    //test_still("test/junko");
    //test_still("test/pigge");
}

