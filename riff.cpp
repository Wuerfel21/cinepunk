#include "riff.hpp"


void RIFFWriter::add_size(uint64_t size) {
    for (auto &memo : memo_stack) {
        memo.size += size;
    }
}

void RIFFWriter::write_data(const uint8_t *data, size_t size) {
    stream.write(reinterpret_cast<const std::ostream::char_type *>(data),size);
    add_size(uint64_t(size));
}

void RIFFWriter::replace_uint32(std::ostream::pos_type pos, uint32_t x) {
    auto head = stream.tellp();
    stream.seekp(pos);
    uint8_t bytes[4];
    bytes[0] = (x>> 0)&255;
    bytes[1] = (x>> 8)&255;
    bytes[2] = (x>>16)&255;
    bytes[3] = (x>>24)&255;
    stream.write(reinterpret_cast<const std::ostream::char_type *>(bytes),4);
    stream.seekp(head);
}

void RIFFWriter::write_chunk(FourCC id, const uint8_t *data, size_t size) {
    assert(size <= UINT32_MAX);
    write_fourcc(id);
    write_uint32(uint32_t(size));
    write_data(data,size);
}

void RIFFWriter::begin_chunk(FourCC id) {
    write_fourcc(id);
    auto pos = stream.tellp();
    write_uint32(0xDEADBEEF);
    memo_stack.emplace_back(id,pos);
}

uint64_t RIFFWriter::end_chunk(FourCC id) {
    assert(!memo_stack.empty());
    auto &memo = memo_stack.back();
    assert(memo.id == id);
    auto size = memo.size;
    assert(size <= UINT32_MAX);
    replace_uint32(memo.size_position,uint32_t(size));
    memo_stack.pop_back();
    return size;
}

void RIFFWriter::begin_list(FourCC type) {
    begin_chunk("LIST"_4cc);
    memo_stack.back().id2 = type;
    write_fourcc(type);
}

uint64_t RIFFWriter::end_list(FourCC type) {
    assert(!memo_stack.empty());
    assert(memo_stack.back().id2 == type);
    return end_chunk("LIST"_4cc);
}

void AVIWriter::write_avih(uint32_t width, uint32_t height, uint32_t streams, uint32_t scale, uint32_t rate) {
    begin_chunk("avih"_4cc);
    write_uint32(uint32_t(uint64_t(scale*1000000)/rate)); // Microseconds per frame
    write_uint32(0); // Max bytes per sec (garbo)
    write_uint32(0); // Padding granularity (garbo)
    write_uint32(0); // Flags (garbo)
    total_frames_pos = stream.tellp();
    write_uint32(0); // Total frames
    write_uint32(0); // Initial frames (garbo)
    write_uint32(streams);
    write_uint32(0); // Suggested buffer (garbo)
    write_uint32(width);
    write_uint32(height);
    write_uint32(0); // Reserved
    write_uint32(0); // Reserved
    write_uint32(0); // Reserved
    write_uint32(0); // Reserved
    end_chunk("avih"_4cc);
    this->width = width;
    this->height = height;
}

void AVIWriter::set_total_frames(uint32_t frames) {
    replace_uint32(total_frames_pos,total_frames_pos);
}

void AVIWriter::write_strh(FourCC type, FourCC codec, uint32_t scale, uint32_t rate) {
    begin_chunk("strh"_4cc);
    write_fourcc(type);
    write_fourcc(codec);
    write_uint32(0); // Flags (garbo)
    write_uint16(0); // priority (garbo)
    write_uint16(0); // language (garbo)
    write_uint32(0); // initial frames (garbo)
    write_uint32(scale);
    write_uint32(rate);
    write_uint32(0); // Start time (garbo)
    write_uint32(0); // suggested buffer (garbo)
    write_uint32(0); // length (garbo)
    write_uint32(0); // quality (garbo)
    write_uint32(0); // sample size (garbo)
    write_uint16(0); // rectangle left
    write_uint16(0); // rectangle top
    write_uint16(width); // rectangle right
    write_uint16(height); // rectangle bottom
    end_chunk("strh"_4cc);
}

void AVIWriter::write_strf_video(FourCC codec) {
    begin_chunk("strf"_4cc);
    write_uint32(40); // Structure size??
    write_uint32(width);
    write_uint32(height);
    write_uint16(1); // Planes ????
    write_uint16(24); // bpp???
    write_fourcc(codec); // ????
    write_uint32(0); // ????
    write_uint32(0); // ????
    write_uint32(0); // ????
    write_uint32(0); // ????
    write_uint32(0); // ????
    end_chunk("strf"_4cc);
}

void SimpleAVIWriter::begin_simple(FourCC codec,uint32_t width, uint32_t height, uint32_t scale, uint32_t rate) {
    begin_riff();

    begin_list("hdrl"_4cc);
    write_avih(width,height,1,1,rate);
    begin_list("strl"_4cc);
    write_strh("vids"_4cc,"cvid"_4cc,1,rate);
    write_strf_video("cvid"_4cc);
    end_list("strl"_4cc);
    end_list("hdrl"_4cc);

    begin_list("movi"_4cc);
}

uint64_t SimpleAVIWriter::end_simple() {
    end_list("movi"_4cc);
    return end_riff();
}

