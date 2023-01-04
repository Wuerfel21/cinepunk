#include "cinepunk_internal.hpp"
#include <cstdio>

constexpr uint lbg_iterations = 2;
constexpr uint split_iterations = 3;
constexpr uint soca_iterations = 3;
constexpr uint soca_search_len_lower = 256;
//constexpr uint soca_search_len_upper = 16;
constexpr uint soca_sort_len = 32;



static u64 __attribute__((noinline)) voronoi_partition_generic(
    const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
    u64 *code_distortion, std::vector<std::vector<uint>> &partition
) {
        // Do Voronoi Partition
        // i.e. find closest codeword to each vector
        u64 total_distortion = 0;
        for(uint i:applicable_indices) {
            auto vec = data[i];
            u8 best_code = 0; // Doesn't actually need initial value
            u32 lowest_distortion = UINT32_MAX;
            for (uint j=0;j<codebook.size();j++) {
                u32 distortion = blockDistortion(vec,codebook[j]);
                if (distortion<lowest_distortion) {
                    lowest_distortion = distortion;
                    best_code = j;
                }
            }
            lowest_distortion *= vec.weight;
            code_distortion[best_code] += lowest_distortion;
            total_distortion += lowest_distortion;
            partition[best_code].push_back(i);
        }
    return total_distortion;
}

static CPYuvBlock __attribute__((noinline)) calculate_centroid_generic(
    const CPYuvBlock *data, const std::vector<uint> &partition
) {
    // Find centroid of each codeword's partition and move it there
    u32 ytl = 0,ytr = 0,ybl = 0,ybr = 0,u = 0,v = 0;
    u32 total_weight = 0;
    for (uint j:partition) {
        auto block = data[j];
        ytl += block.ytl * block.weight;
        ytr += block.ytr * block.weight;
        ybl += block.ybl * block.weight;
        ybr += block.ybr * block.weight;
        u   += block.u   * block.weight;
        v   += block.v   * block.weight;
        total_weight += block.weight;
    }
    assert(total_weight > 0);
    return {
        .u   = u8((u  +(total_weight>>1))/total_weight),
        .v   = u8((v  +(total_weight>>1))/total_weight),
        .ytl = u8((ytl+(total_weight>>1))/total_weight),
        .ytr = u8((ytr+(total_weight>>1))/total_weight),
        .ybl = u8((ybl+(total_weight>>1))/total_weight),
        .ybr = u8((ybr+(total_weight>>1))/total_weight),
    };
}

#ifdef CINEPUNK_AVX2

static u64 __attribute__((noinline,target("avx2"))) voronoi_partition_AVX2(
    const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
    u64 *code_distortion, std::vector<std::vector<uint>> &partition
) {
    __m256i weights = _mm256_set_epi32(Y_WEIGHT,Y_WEIGHT,Y_WEIGHT,Y_WEIGHT,V_WEIGHT,U_WEIGHT,0,0);
    u64 total_distortion = 0;
    //applicable_indices.reserve((applicable_indices.size()+7)&~7);
    for (uint i=0;i<applicable_indices.size();i+=8) {
        uint use = std::min(8u,uint(applicable_indices.size()-i));

        // Minor hack: remove branches by accessing vector beyond nominal size
        // (see reserve call above)
        #if 0
        __m256i pack10 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+1]]),
            block_packed(data[applicable_indices[i+0]])));
        __m256i pack32 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+3]]),
            block_packed(data[applicable_indices[i+2]])));
        __m256i pack54 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+5]]),
            block_packed(data[applicable_indices[i+4]])));
        __m256i pack76 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+7]]),
            block_packed(data[applicable_indices[i+6]])));
        #else
        __m256i pack10 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  1 ? block_packed(data[applicable_indices[i+1]]) : 0,
                       block_packed(data[applicable_indices[i+0]])    ));
        __m256i pack32 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  3 ? block_packed(data[applicable_indices[i+3]]) : 0,
            use >  2 ? block_packed(data[applicable_indices[i+2]]) : 0));
        __m256i pack54 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  5 ? block_packed(data[applicable_indices[i+5]]) : 0,
            use >  4 ? block_packed(data[applicable_indices[i+4]]) : 0));
        __m256i pack76 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  7 ? block_packed(data[applicable_indices[i+7]]) : 0,
            use >  6 ? block_packed(data[applicable_indices[i+6]]) : 0));
        #endif


        __m256i lowest_distortion = _mm256_set1_epi32(INT32_MAX);
        __m256i best_code = _mm256_set1_epi32(0);

        for (uint j=0;j<codebook.size();j++) {
            __m256i code_pack = _mm256_cvtepu8_epi16(_mm_set1_epi64x(block_packed(codebook[j])));
            __m256i diff10 = _mm256_sub_epi16(code_pack,pack10);
            __m256i diff32 = _mm256_sub_epi16(code_pack,pack32);
            __m256i diff54 = _mm256_sub_epi16(code_pack,pack54);
            __m256i diff76 = _mm256_sub_epi16(code_pack,pack76);
            diff10 = _mm256_mullo_epi16(diff10,diff10);
            diff32 = _mm256_mullo_epi16(diff32,diff32);
            diff54 = _mm256_mullo_epi16(diff54,diff54);
            diff76 = _mm256_mullo_epi16(diff76,diff76);

            #if 1
            assert(Y_WEIGHT == 1 && U_WEIGHT == 2 && V_WEIGHT == 2);
            // Duplicate UV into unused slots
            diff10 = _mm256_shuffle_epi32(diff10,0b11'10'01'01);
            diff32 = _mm256_shuffle_epi32(diff32,0b11'10'01'01);
            diff54 = _mm256_shuffle_epi32(diff54,0b11'10'01'01);
            diff76 = _mm256_shuffle_epi32(diff76,0b11'10'01'01);
            __m256i diff0 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,0));
            __m256i diff1 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,1));
            __m256i diff2 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,0));
            __m256i diff3 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,1));
            __m256i diff4 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,0));
            __m256i diff5 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,1));
            __m256i diff6 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,0));
            __m256i diff7 = _mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,1));
            #else
            __m256i diff0 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,0)),weights);
            __m256i diff1 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,1)),weights);
            __m256i diff2 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,0)),weights);
            __m256i diff3 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,1)),weights);
            __m256i diff4 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,0)),weights);
            __m256i diff5 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,1)),weights);
            __m256i diff6 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,0)),weights);
            __m256i diff7 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,1)),weights);
            #endif

            __m256i sum10 = _mm256_hadd_epi32(diff0,diff1);
            __m256i sum32 = _mm256_hadd_epi32(diff2,diff3);
            __m256i sum54 = _mm256_hadd_epi32(diff4,diff5);
            __m256i sum76 = _mm256_hadd_epi32(diff6,diff7);
            // {0a+0b,0c+0d,1a+1b,1c+1d , 0e+0f,0g+0h,1e+1f,1g+1h}
            __m256i sum3210 = _mm256_hadd_epi32(sum10,sum32);
            __m256i sum7654 = _mm256_hadd_epi32(sum54,sum76);
            // {0a+0b+0c+0d,1a+1b+1c+1d,2a+2b+2c+2d,3a+3b+3c+3d , 0e+0f+0g+0h,1e+1f+1g+1h,2e+2f+2g+2h,3e+3f+3g+3h}
            __m128i sumlow = _mm_add_epi32(_mm256_extractf128_si256(sum3210,1),_mm256_extractf128_si256(sum3210,0));
            __m128i sumhi = _mm_add_epi32(_mm256_extractf128_si256(sum7654,1),_mm256_extractf128_si256(sum7654,0));
            __m256i distortion = _mm256_set_m128i(sumhi,sumlow);

            __m256i better_mask = _mm256_cmpgt_epi32(lowest_distortion,distortion);
            lowest_distortion = _mm256_blendv_epi8(lowest_distortion,distortion,better_mask);
            best_code = _mm256_blendv_epi8(best_code,_mm256_set1_epi32(j),better_mask);
        }

        #pragma GCC unroll 8 // Need to unroll, otherwise extract intrinsic gets busted
        for (uint sub=0;sub<use;sub++) {
            u32 code = _mm256_extract_epi32(best_code,sub);
            auto index = applicable_indices[i+sub];
            auto distortion = _mm256_extract_epi32(lowest_distortion,sub) *
                _mm256_extract_epi16((sub&4) ? ((sub&2) ? pack76 : pack54) : ((sub&2) ? pack32 : pack10) ,(sub&1)*8);
            code_distortion[code] += distortion;
            total_distortion += distortion;
            partition[code].push_back(index);
        }
    }
    return total_distortion;
}

static CPYuvBlock __attribute__((noinline,target("avx2"))) calculate_centroid_AVX2(
    const CPYuvBlock *data, const std::vector<uint> &partition
) {
    // Find centroid of each codeword's partition and move it there
    __m256i total = _mm256_set1_epi32(0);
    for (uint j:partition) {
        auto block = _mm256_cvtepu8_epi32(_mm_set_epi64x(0,block_packed(data[j])));
        auto weight = _mm256_broadcastd_epi32(_mm256_extractf128_si256(block,0));
        block = _mm256_insert_epi32(block,1,0);
        total = _mm256_add_epi32(total,_mm256_mullo_epi32(block,weight));
    }
    int32_t total_weight = _mm256_extract_epi32(total,0);
    assert(total_weight > 0);
    total = _mm256_add_epi32(total,_mm256_srai_epi32(_mm256_broadcastd_epi32(_mm256_extractf128_si256(total,0)),1));
    return {
        .u   = u8(_mm256_extract_epi32(total,2)/total_weight),
        .v   = u8(_mm256_extract_epi32(total,3)/total_weight),
        .ytl = u8(_mm256_extract_epi32(total,4)/total_weight),
        .ytr = u8(_mm256_extract_epi32(total,5)/total_weight),
        .ybl = u8(_mm256_extract_epi32(total,6)/total_weight),
        .ybr = u8(_mm256_extract_epi32(total,7)/total_weight),
    };
}

#endif


u64 voronoi_partition(const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
    u64 *code_distortion, std::vector<std::vector<uint>> &partition
) {
    #ifdef CINEPUNK_AVX2
    if(__builtin_cpu_supports("avx2")) {
        return voronoi_partition_AVX2(codebook,data,applicable_indices,code_distortion,partition);
    }
    #endif
    return voronoi_partition_generic(codebook,data,applicable_indices,code_distortion,partition);
}

static CPYuvBlock __attribute__((noinline)) calculate_centroid(
    const CPYuvBlock *data, const std::vector<uint> &partition
) {
    #ifdef CINEPUNK_AVX2
    if(__builtin_cpu_supports("avx2")) {
        return calculate_centroid_AVX2(data,partition);
    }
    #endif
    return calculate_centroid_generic(data,partition);
}

static std::array<CPYuvBlock,2>  __attribute__((noinline)) bbox_distrib(const CPYuvBlock *data, const std::vector<uint> &indices) {
    // Calculate new codes for SoCAs using bounding box method
    // TODO: since this runs on high-utility codes, maybe make AVX version?
    u8 ytlmin=255,ytrmin=255,yblmin=255,ybrmin=255,umin=255,vmin=255;
    u8 ytlmax=  0,ytrmax=  0,yblmax=  0,ybrmax=  0,umax=  0,vmax=  0;
    for (uint idx : indices) {
        auto block = data[idx];
        ytlmin = std::min(ytlmin,block.ytl);
        ytlmax = std::max(ytlmax,block.ytl);
        ytrmin = std::min(ytrmin,block.ytr);
        ytrmax = std::max(ytrmax,block.ytr);
        yblmin = std::min(yblmin,block.ybl);
        yblmax = std::max(yblmax,block.ybl);
        ybrmin = std::min(ybrmin,block.ybr);
        ybrmax = std::max(ybrmax,block.ybr);
        umin = std::min(umin,block.u);
        umax = std::max(umax,block.u);
        vmin = std::min(vmin,block.v);
        vmax = std::max(vmax,block.v);
    }
    u8 ytloff= (ytlmax-ytlmin)>>2;
    u8 ytroff= (ytrmax-ytrmin)>>2;
    u8 ybloff= (yblmax-yblmin)>>2;
    u8 ybroff= (ybrmax-ybrmin)>>2;
    u8 uoff=   (umax-umin)>>2;
    u8 voff=   (vmax-vmin)>>2;

    CPYuvBlock new1 = {
        .u =   clamp_u8(  umin+uoff),
        .v =   clamp_u8(  vmin+voff),
        .ytl = clamp_u8(ytlmin+ytloff),
        .ytr = clamp_u8(ytrmin+ytroff),
        .ybl = clamp_u8(yblmin+ybloff),
        .ybr = clamp_u8(ybrmin+ybroff),
    };
    CPYuvBlock new2 = {
        .u =   clamp_u8(  umax-uoff),
        .v =   clamp_u8(  vmax-voff),
        .ytl = clamp_u8(ytlmax-ytloff),
        .ytr = clamp_u8(ytrmax-ytroff),
        .ybl = clamp_u8(yblmax-ybloff),
        .ybr = clamp_u8(ybrmax-ybroff),
    };

    return {new1,new2};
}

static bool  __attribute__((noinline)) try_shift(std::vector<CPYuvBlock> &codebook, const CPYuvBlock *data, uint from, uint to, u64 *code_distortion, std::vector<std::vector<uint>> &partition) {

    //printf("try_shift\n");
    if (code_distortion[from] > code_distortion[to]) return false;

    auto target_distortion = code_distortion[from] + code_distortion[to];

    //printf("SoCA %3u %3u\n",from,to);

    // Find codeword to replace from with
    uint replace = 0;
    u32 nearest_distortion = UINT32_MAX;
    for (uint i=0;i<codebook.size();i++) {
        if (i==from || i == to) continue;
        u32 distortion = blockDistortion(codebook[from],codebook[i]);
        if (distortion < nearest_distortion) {
            replace = i;
            nearest_distortion = distortion;
        }
    }
    std::vector<uint> replace_partition;
    replace_partition.reserve(partition[from].size()+partition[replace].size());
    replace_partition.insert(replace_partition.end(),partition[from].begin(),partition[from].end());
    replace_partition.insert(replace_partition.end(),partition[replace].begin(),partition[replace].end());
    auto new_replace = replace_partition.empty() ? codebook[from] : calculate_centroid(data,replace_partition);
    u64 from_distortion = 0;
    for (uint idx : replace_partition) {
        from_distortion += blockDistortion(new_replace,data[idx])*data[idx].weight;
    }

    // Possible early out (TODO profile)
    if (from_distortion > target_distortion) return false;

    auto [new_from,new_to] = bbox_distrib(data,partition[to]);
    // Adjust new vectors
    std::vector<CPYuvBlock> adjust_codes = {new_from,new_to};
    u64 adjust_distortion[2];
    std::vector<std::vector<uint>> adjust_partition(2);
    adjust_partition[0].reserve(partition[to].size());
    adjust_partition[1].reserve(partition[to].size());
    for (uint iter=0;iter<soca_iterations;iter++) {
        adjust_partition[0].clear();
        adjust_partition[1].clear();
        voronoi_partition(adjust_codes,data,partition[to],adjust_distortion,adjust_partition);
        if (adjust_partition[0].empty()||adjust_partition[1].empty()) return false;
        adjust_codes[0] = calculate_centroid(data,adjust_partition[0]);
        adjust_codes[1] = calculate_centroid(data,adjust_partition[1]);
    }
    adjust_partition[0].clear();
    adjust_partition[1].clear();
    adjust_distortion[0] = 0;
    adjust_distortion[1] = 0;
    u64 to_distortion = voronoi_partition(adjust_codes,data,partition[to],adjust_distortion,adjust_partition);

    if (to_distortion + from_distortion > target_distortion) return false;

    printf("SoCA OK! %llu %llu %llu\n",to_distortion,from_distortion,target_distortion);
    // actually do shift
    partition[replace].insert(partition[replace].end(),partition[from].begin(),partition[from].end());
    partition[from] = std::move(adjust_partition[0]);
    code_distortion[from] = adjust_distortion[0];
    partition[to] = std::move(adjust_partition[1]);
    code_distortion[to] = adjust_distortion[1];
    return true;
}

u64 CPEncoderState::vq_elbg(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data, std::vector<uint> &applicable_indices,std::vector<u8> *closest_out) {

    assert(target_codebook_size>=1);
    assert(target_codebook_size<=256);

    uint iteration_left = (codebook.size()*2>target_codebook_size) ? lbg_iterations : split_iterations;

    if (codebook.empty()) {
        // Generate initial codeword. Doesn't matter what it is.
        codebook.push_back({0});
    }

    std::vector<std::vector<uint>> partition;
    u64 code_distortion[256];
    for (;;) {
        partition.resize(codebook.size());
        for (auto p : partition) {
            p.clear();
            p.reserve((applicable_indices.size()/codebook.size())*2);
        }
        std::fill_n(code_distortion,256,0);
        
        u64 total_distortion = voronoi_partition(codebook,data,applicable_indices,code_distortion,partition);

        // ELBG special sauce!
        if (codebook.size() >= 8 && iteration_left > 0) {
            u64 mean_distortion = total_distortion/codebook.size();
            u8 distortion_rank[256];
            u64 shift_max_dist = 0;
            std::iota(distortion_rank,distortion_rank+codebook.size(),0);
            std::sort(distortion_rank,distortion_rank+codebook.size(),[&](u8 i, u8 j){
                return code_distortion[i] < code_distortion[j];
            });
            uint upmost = codebook.size()-1;
            for (uint i=0;i<soca_search_len_lower;i++) {
                if (code_distortion[distortion_rank[i]] > shift_max_dist) break;
                // Documented ELBG does stochastic selection of "to"
                // Randomness is bad, so we just always pick a fight with the top N
                if (i>=upmost) break;
                if (try_shift(codebook,data,distortion_rank[i],distortion_rank[upmost],code_distortion,partition)) {
                    upmost--;
                }
                /*
                for (uint j=codebook.size()-1,left=soca_search_len_upper ; left>0 && j>i && code_distortion[distortion_rank[j]]>=mean_distortion ; j--,left--) {
                    if (try_shift(codebook,data,distortion_rank[i],distortion_rank[j],code_distortion,partition)) {
                        // Re-sort top couple entries
                        std::sort(distortion_rank+std::max(int(i),int(codebook.size()-soca_sort_len)),distortion_rank+codebook.size(),[&](u8 i, u8 j){
                            return code_distortion[i] < code_distortion[j];
                        });
                        break;
                    }
                }*/
            }
        }

        
        for(uint i=0;i<codebook.size();i++) {
            if (partition[i].empty()) continue;
            codebook[i] = calculate_centroid(data,partition[i]);
        }

        if (!--iteration_left) {
            bool codebook_grew = false;
            for (uint split_i = 0,split_max = codebook.size();split_i < split_max && codebook.size() < target_codebook_size;split_i++) {
                // TODO: better pertubation vector ?
                auto code = codebook[split_i];
                codebook.push_back({
                    .u   = clamp_u8(code.u   + 2),
                    .v   = clamp_u8(code.v   + 2),
                    .ytl = clamp_u8(code.ytl + 2),
                    .ytr = clamp_u8(code.ytr + 2),
                    .ybl = clamp_u8(code.ybl + 2),
                    .ybr = clamp_u8(code.ybr + 2),
                });
                codebook[split_i] = {
                    .u   = clamp_u8(code.u   - 2),
                    .v   = clamp_u8(code.v   - 2),
                    .ytl = clamp_u8(code.ytl - 2),
                    .ytr = clamp_u8(code.ytr - 2),
                    .ybl = clamp_u8(code.ybl - 2),
                    .ybr = clamp_u8(code.ybr - 2),
                };
                codebook_grew = true;
            }
            if (!codebook_grew) break; // We're done...
            if (codebook.size() == target_codebook_size) {
                iteration_left = lbg_iterations;
            } else {
                iteration_left = split_iterations;
            }
        }
    }

    // Partition one more time
    for (auto p : partition) {
        p.clear();
    }
    // Note: code_distortion isn't cleared because we DGAS
    u64 distortion_total = voronoi_partition(codebook,data,applicable_indices,code_distortion,partition);
    // Fill closest_out from partition data
    if (closest_out) {
        for (uint i=0;i<partition.size();i++) {
            for (auto idx : partition[i]) {
                (*closest_out)[idx] = u8(i);
            }
        }
    }
    return distortion_total;
}
