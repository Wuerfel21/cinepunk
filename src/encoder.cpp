#include "cinepunk_internal.hpp"
#include <cstdio>

CPEncoderState::CPEncoderState(unsigned frame_width, unsigned frame_height, unsigned max_strips)
: frame_mbWidth{frame_width/4},frame_mbHeight{frame_height/4},max_strips{max_strips} {
    // Set defaults

    assert(frame_width%4 == 0 && frame_height%4 == 0);

    encoder_flags = 0;
    frame_count = 0;
    next_frame = std::make_unique<CPYuvBlock[]>(total_blocks());
    cur_frame  = std::make_unique<CPYuvBlock[]>(total_blocks());
    prev_frame = std::make_unique<CPYuvBlock[]>(total_blocks());
    cur_frame_v1 = std::make_unique<CPYuvBlock[]>(total_macroblocks());
    prev_codes_v4.resize(max_strips);
    prev_codes_v1.resize(max_strips);
}


CP_API CPEncoderState *CP_create_encoder(unsigned frame_width, unsigned frame_height, unsigned max_strips) {
    return new CPEncoderState(frame_width,frame_height,max_strips);
}

CP_API void CP_destroy_encoder(CPEncoderState *enc) {
    delete enc;
}

CP_API void CP_set_encflags(CPEncoderState *enc,uint32_t flags) {
    enc->encoder_flags |= flags;
}

CP_API void CP_clear_encflags(CPEncoderState *enc,uint32_t flags) {
    enc->encoder_flags &= ~flags;
}

CP_API bool CP_push_frame(CPEncoderState *enc,CPColorType ctype,const void *data) {
    if (enc->frames_pushed >= 2) return false;
    enc->frames_pushed++;
    std::swap(enc->cur_frame,enc->next_frame);
    if (data) {
        switch(ctype) {
        case CP_RGB24:
            if (enc->encoder_flags & CP_ENCFLAG_RGB2YUV_FAST) {
                CP_rgb2yuv_fast(enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
            } else {
                CP_rgb2yuv_hq  (enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
            }
            break;
        case CP_GRAY:
            CP_gray2yuv(enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
        case CP_YUVBLOCK:
            memcpy(enc->next_frame.get(),data,4*sizeof(CPYuvBlock)*enc->total_macroblocks());
            break;
        default:
            assert(false);
            break;
        }
    } else {
        // Passing nullptr means "end of file"
        // .. in which case we repeat last frame
        // (is usually black, anyways...)
        memcpy(enc->next_frame.get(),enc->cur_frame.get(),4*sizeof(CPYuvBlock)*enc->total_macroblocks());
    }
    return true;
}

CP_API size_t CP_pull_frame(CPEncoderState *enc,uint8_t *buffer) {
    if (enc->frames_pushed < 2) return 0;
    enc->frames_pushed--;
    PacketWriter packet(buffer);
    enc->doFrame(packet);
    assert(packet.get_length() <= CP_BUFFER_SIZE(enc->frame_mbWidth*4,enc->frame_mbHeight*4,enc->max_strips));
    assert(packet.get_length() > 0);
    return packet.get_length();
}



void CPEncoderState::doFrame(PacketWriter &packet) {

    auto frame_header = packet;
    packet.skip(10);
    
    bool keyframe = (inter_frames_left == 0);

    // Assign block weights based on change in lookahead frame
    for(uint i=0;i<total_blocks();i++) {
        //uint distortion = blockDistortion(cur_frame[i],next_frame[i]);
        // TODO: Tune threshold
        cur_frame[i].weight = 3;//(distortion < 16) ? 3 : 2;
    }

    // Create low-res copy of frame for V1 encoding
    CP_yuv_downscale_fast(cur_frame_v1.get(),cur_frame.get(),frame_mbWidth,frame_mbHeight,0);

    uint strips = max_strips;
    uint y1 = 0;
    for (uint i=0;i<strips;i++) {
        auto height = std::min(frame_mbHeight/strips,frame_mbHeight-y1);
        auto strip = tryStrip(y1,height,keyframe);
        writeStrip(packet,strip);
        y1+=height;
    }
    
    
    uint framesize = packet.ptr - frame_header.ptr;
    frame_header.write_u8(CHUNK_FRAME_INTRA);
    frame_header.write_u24(framesize);
    frame_header.write_u16(frame_mbWidth*4);
    frame_header.write_u16(frame_mbHeight*4);
    frame_header.write_u16(strips);

}

[[maybe_unused]]
static void printCBInfo(const std::vector<CPYuvBlock> codebook,const u8 *indices,const CPYuvBlock *frame,uint blocknum) {
        uint histogram[256] = {};
        u64 distortion_total[256] = {};
        for (uint i=0;i<blocknum;i++) {
            auto code = indices[i];
            histogram[code]++;
            distortion_total[code] += blockDistortion(frame[i],codebook[code]);
        }
        for(uint i=0;i<codebook.size();i++) {
            auto code = codebook[i];
            printf("U: %3u V: %3u Y: %3u %3u %3u %3u D: %8llu H: %8u %s\n",code.u,code.v,code.ytl,code.ytr,code.ybl,code.ybr,distortion_total[i],histogram[i],histogram[i]==0?"DEAD!!!":"");
        }

}

CPEncoderState::StripEncoding
CPEncoderState::tryStrip(uint ytop, uint height, bool keyframe) {
    CPEncoderState::StripEncoding strip(frame_mbWidth*height);
    strip.ytop = ytop;
    strip.height = height;
    uint strip_macroblocks = height*frame_mbWidth;

    auto image_v4 = cur_frame.get()+blk_index(0,ytop*2);
    auto image_v1 = cur_frame_v1.get()+mb_index(0,ytop);

    std::vector<uint> v4_idx,v1_idx;
    strip.strip_type = CPEncoderState::StripEncoding::MB_V4; // TODO select dynamic
    for (uint i=0;i<strip_macroblocks;i++) {
        strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V4;
        v1_idx.push_back(i);
        v4_idx.push_back(i*4+0);
        v4_idx.push_back(i*4+1);
        v4_idx.push_back(i*4+2);
        v4_idx.push_back(i*4+3);
    }
    vq_fastpnn(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
    vq_fastpnn(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);
    vq_elbg(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
    vq_elbg(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);

    v4_idx.clear();
    v1_idx.clear();
    for (uint y=0;y<height;y++) {
        for (uint x=0;x<frame_mbWidth;x++) {
            u32 v1_distortion = macroblockV1Distortion(
                image_v4[blk_index(x*2+0,y*2+0)],
                image_v4[blk_index(x*2+1,y*2+0)],
                image_v4[blk_index(x*2+0,y*2+1)],
                image_v4[blk_index(x*2+1,y*2+1)],
                strip.code_v1[strip.mb_v1[mb_index(x,y)]]);
            u32 v4_distortion = blockDistortion(image_v4[blk_index(x*2+0,y*2+0)],strip.code_v4[strip.blk_v4[blk_index(x*2+0,y*2+0)]])
                              + blockDistortion(image_v4[blk_index(x*2+1,y*2+0)],strip.code_v4[strip.blk_v4[blk_index(x*2+1,y*2+0)]])
                              + blockDistortion(image_v4[blk_index(x*2+0,y*2+1)],strip.code_v4[strip.blk_v4[blk_index(x*2+0,y*2+1)]])
                              + blockDistortion(image_v4[blk_index(x*2+1,y*2+1)],strip.code_v4[strip.blk_v4[blk_index(x*2+1,y*2+1)]]);
            
            if (v1_distortion <= v4_distortion) {
                strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_V1;
                v1_idx.push_back(mb_index(x,y));
            } else {
                strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_V4;
                v4_idx.push_back(blk_index(x*2+0,y*2+0));
                v4_idx.push_back(blk_index(x*2+1,y*2+0));
                v4_idx.push_back(blk_index(x*2+0,y*2+1));
                v4_idx.push_back(blk_index(x*2+1,y*2+1));
            }
        }
    }
    printf("V1: %u, V4 : %u\n",uint(v1_idx.size()),uint(v4_idx.size()/4));
    if (v4_idx.empty()) {
        strip.code_v4.clear();
    } else {
        vq_elbg(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
    }
    if (v1_idx.empty()) {
        strip.code_v1.clear();
    } else {
        vq_elbg(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);
    }

    #if 1
    printf("V4 info:\n");
    printCBInfo(strip.code_v4,strip.blk_v4.data(),image_v4,strip_macroblocks*4);
    printf("\nV1 info:\n");
    printCBInfo(strip.code_v4,strip.mb_v1.data(),image_v1,strip_macroblocks);
    printf("\n");
    #endif


    
    return strip;
}

void CPEncoderState::writeCodebook(PacketWriter &packet,std::vector<CPYuvBlock> book,bool isV4) {
    auto header = packet;
    packet.skip(4);
    for (uint i=0;i<book.size();i++) {
        auto code = book[i];
        packet.write_u8(code.ytl);
        packet.write_u8(code.ytr);
        packet.write_u8(code.ybl);
        packet.write_u8(code.ybr);
        packet.write_u8(code.u^128);
        packet.write_u8(code.v^128);
    }
    uint size = packet.ptr - header.ptr;
    header.write_u8(isV4 ? CHUNK_V4_COLOR_FULL : CHUNK_V1_COLOR_FULL);
    header.write_u24(size);
}

void CPEncoderState::writeStrip(PacketWriter &packet,StripEncoding &strip) {
    // Write strip header later
    auto strip_header = packet;
    packet.skip(12);

    writeCodebook(packet,strip.code_v1,false);
    writeCodebook(packet,strip.code_v4,true);

    auto image_header = packet;
    packet.skip(4);
    BitstreamWriter bitstream(packet);
    for(uint y=0;y<strip.height;y++) {
        for(uint x=0;x<frame_mbWidth;x++) {
            switch (strip.mb_types[mb_index(x,y)]) {
            case CPEncoderState::StripEncoding::MB_V1:
                bitstream.put_bit(false);
                bitstream.write_u8(strip.mb_v1[mb_index(x,y)]);
                break;
            case CPEncoderState::StripEncoding::MB_V4:
                bitstream.put_bit(true);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+0,y*2+0)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+1,y*2+0)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+0,y*2+1)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+1,y*2+1)]);
                break;
            default:
                assert(false);
                break;
            }
        }
    }
    bitstream.flush();
    uint imgsize = packet.ptr - image_header.ptr;
    image_header.write_u8(CHUNK_IMAGE_INTRA);
    image_header.write_u24(imgsize);

    uint stripsize = packet.ptr - strip_header.ptr;
    strip_header.write_u8(CHUNK_STRIP_INTRA);
    strip_header.write_u24(stripsize);
    strip_header.write_u16(strip.ytop*4);
    strip_header.write_u16(0);
    strip_header.write_u16((strip.ytop+strip.height)*4);
    strip_header.write_u16(frame_mbWidth*4);
}