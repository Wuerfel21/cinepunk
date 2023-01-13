#pragma once
#include <cstdint>
#include <cassert>
#include <iostream>
#include <vector>

struct FourCC {
    char chars[4];

    constexpr bool operator == (const FourCC &other) {
        return this->chars[0] == other.chars[0]
            && this->chars[1] == other.chars[1]
            && this->chars[2] == other.chars[2]
            && this->chars[3] == other.chars[3];
    }
};

constexpr FourCC operator "" _4cc(const char *str, size_t size) {
    assert(size == 4);
    return {{str[0],str[1],str[2],str[3]}};
}

class RIFFWriter {

    struct ChunkMemo {
        FourCC id, id2;
        uint64_t size;
        std::ostream::pos_type size_position;

        ChunkMemo(FourCC id,std::ostream::pos_type size_position) : id{id}, id2{"BUGS"_4cc}, size{0}, size_position{size_position} {};
    };

    std::vector<ChunkMemo> memo_stack;

    void add_size(uint64_t size);
protected:
    std::ostream &stream;
    
    void replace_uint32(std::ostream::pos_type pos, uint32_t x);

public:
    
    RIFFWriter(std::ostream &stream) : stream{stream} {};

    void begin_chunk(FourCC id);
    uint64_t end_chunk(FourCC id);
    void write_data(const uint8_t *data, size_t length);
    void write_chunk(FourCC id, const uint8_t *data, size_t length);

    void write_uint8(uint8_t x) {
        write_data(&x,1);
    }

    void write_uint16(uint16_t x) {
        uint8_t bytes[2];
        bytes[0] = (x>> 0)&255;
        bytes[1] = (x>> 8)&255;
        write_data(bytes,2);
    }

    void write_uint32(uint32_t x) {
        uint8_t bytes[4];
        bytes[0] = (x>> 0)&255;
        bytes[1] = (x>> 8)&255;
        bytes[2] = (x>>16)&255;
        bytes[3] = (x>>24)&255;
        write_data(bytes,4);
    }

    void write_fourcc(FourCC id) {
        write_data(reinterpret_cast<const uint8_t *>(id.chars),4);
    }

    void begin_list(FourCC type);
    uint64_t end_list(FourCC type);

};

class AVIWriter : public RIFFWriter {
    
protected:
    std::ostream::pos_type total_frames_pos;
public:
    uint32_t width,height;
    void begin_riff() {
        begin_chunk("RIFF"_4cc);
        write_fourcc("AVI "_4cc);
    };
    uint64_t end_riff() {
        return end_chunk("RIFF"_4cc);
    }

    void write_avih(uint32_t width, uint32_t height, uint32_t streams, uint32_t scale, uint32_t rate);
    void set_total_frames(uint32_t frames);
    void write_strh(FourCC type, FourCC codec, uint32_t scale, uint32_t rate);
    void write_strf_video(FourCC codec);

    using RIFFWriter::RIFFWriter;

};

class SimpleAVIWriter : public AVIWriter {

public:
    using AVIWriter::AVIWriter;

    void begin_simple(FourCC codec,uint32_t width, uint32_t height, uint32_t scale, uint32_t rate);
    uint64_t end_simple();

};