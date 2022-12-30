#include "cinepunk_internal.hpp"

CPEncoderState::CPEncoderState(unsigned frame_width, unsigned frame_height, unsigned max_strips)
: frame_mbWidth{frame_width/4},frame_mbHeight{frame_height/4},max_strips{max_strips} {
    // Set defaults

    assert(frame_width%4 == 0 && frame_height%4 == 0);

    encoder_flags = 0;
    frame_count = 0;
    next_frame = std::make_unique<CPYuvBlock[]>(frame_width*frame_height/4);
    cur_frame  = std::make_unique<CPYuvBlock[]>(frame_width*frame_height/4);
    prev_frame = std::make_unique<CPYuvBlock[]>(frame_width*frame_height/4);
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
        uint distortion = blockDistortion(cur_frame[i],next_frame[i]);
        // TODO: Tune threshold
        cur_frame[i].weight = (distortion < 16) ? 3 : 2;
    }

    // Create low-res copy of frame for V1 encoding
    std::unique_ptr<CPYuvBlock[]> v1_frame(new CPYuvBlock[total_macroblocks()]);
    CP_yuv_downscale_fast(v1_frame.get(),cur_frame.get(),frame_mbWidth,frame_mbHeight);

    auto strip = tryStrip(0,frame_mbHeight,keyframe);
    writeStrip(packet,strip);
    
    
    uint framesize = packet.ptr - frame_header.ptr;
    frame_header.write_u8(CHUNK_FRAME_INTRA);
    frame_header.write_u24(framesize);
    frame_header.write_u16(frame_mbWidth*4);
    frame_header.write_u16(frame_mbHeight*4);
    frame_header.write_u16(1); // TODO strip count

}

CPEncoderState::StripEncoding
CPEncoderState::tryStrip(uint ytop, uint height, bool keyframe) {
    CPEncoderState::StripEncoding strip(frame_mbWidth*height);
    strip.ytop = ytop;
    strip.height = height;
    // Shit algorithm
    strip.strip_type = CPEncoderState::StripEncoding::MB_V4;
    std::vector<uint> all_blk;
    for (uint i=0;i<total_macroblocks();i++) {
        strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V4;
        all_blk.push_back(i*4+0);
        all_blk.push_back(i*4+1);
        all_blk.push_back(i*4+2);
        all_blk.push_back(i*4+3);
    }
    vq_elbg(strip.code_v4,256,cur_frame.get()+blk_index(0,ytop*2),all_blk,strip.blk_v4);

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
    strip_header.write_u16(strip.height*4);
    strip_header.write_u16(frame_mbWidth*4);
}