#include "cinepunk_internal.hpp"
#include <cstdio>
#include <iterator>

constexpr uint LEAF_SIZE = 8;


template<typename T,typename F>
static T *partition(T *begin, T *end, F valuate) {
    // Note: end is inclusive

    // Pick pivot using median-of-three strategy
    T *middle = begin + ((end-begin)>>1);
    assert(end >= begin);
    assert(end > begin);
    assert(middle >= begin);
    assert(middle <= end);
    T *pivot;
    auto begin_val  = valuate(begin );
    auto middle_val = valuate(middle);
    auto end_val    = valuate(end   );
    if      ((begin_val > middle_val) ^ (begin_val > end_val)) pivot = begin;
    else if ((begin_val > middle_val) ^ (end_val > begin_val)) pivot = middle;
    else                                                       pivot = end;

    // Partition 
    auto pivot_val = valuate(pivot);
    auto store = begin;
    auto read = begin;
    // swap pivot to end of list
    std::swap(*pivot,*end);
    for(;read<end;read++) {
        if (valuate(read) <= pivot_val) std::swap(*read,*store++);
    }
    std::swap(*end,*store);
    return store;
}

// Fast median partitioning
template<typename T,typename F>
static T *quickselect(T *begin, T *end, uint k, F valuate) {
    // We need inclusive end
    end--;
    assert(end >= begin);
    for (;;) {
        // Exit if called with size=1
        if (begin == end) return begin;
        assert(end > begin);
        auto pivot = partition(begin,end,valuate);
        assert(pivot >= begin);
        assert(pivot <= end);
        // Choose top or bottom half
        uint pivot_index = pivot-begin;
        if (pivot_index == k) {
            return pivot;
        } else if (k < pivot_index) {
            end = pivot - 1;
            assert(end >= begin);
        } else {
            k -= pivot_index+1;
            begin = pivot + 1;
            assert(end >= begin);
        }
    }
}

template<typename T,typename F>
static void quicksort(T *begin, T *end, F valuate) {
    // Exit if called with size=1 or size=0
    if (begin == end-1 || begin == end) return;
    auto pivot = partition(begin,end-1,valuate);
    quicksort(begin,pivot,valuate);
    quicksort(pivot+1,end,valuate);
}


static std::pair<uint,u8> max_extent(CPYuvBlock *begin,CPYuvBlock *end) {
    u8 ytlmin=255,ytrmin=255,yblmin=255,ybrmin=255,umin=255,vmin=255;
    u8 ytlmax=  0,ytrmax=  0,yblmax=  0,ybrmax=  0,umax=  0,vmax=  0;
    for (;begin!=end;begin++) {
        auto block = *begin;
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
    u8   uext=     (umax-umin);
    u8   vext=     (vmax-vmin);
    u8 ytlext= (ytlmax-ytlmin);
    u8 ytrext= (ytrmax-ytrmin);
    u8 yblext= (yblmax-yblmin);
    u8 ybrext= (ybrmax-ybrmin);
    // Very poor way of choosing max extent.
    std::array<u8,6> values = {uext,vext,ytlext,ytrext,yblext,ybrext};
    auto max_element = std::max_element(values.begin(),values.end());
    return {std::distance(values.begin(),max_element) + offsetof(CPYuvBlock,u),*max_element};
}

struct KDnode {
    int8_t axis_or_fill; // if positive, offset of split element, if negative, amount of leaf data
    u8 threshold;
    union {
        struct {
            KDnode *lower,*upper;
        };
        CPYuvBlock *leaf_data;
    };
    inline bool is_leaf() {return axis_or_fill < 0;};
    // Default non-constructor
    KDnode() : axis_or_fill{0},lower{nullptr},upper{nullptr} {};
    // leaf constructor
    KDnode(CPYuvBlock *leaf_data,uint fill) : axis_or_fill{int8_t(-int(fill))},leaf_data{leaf_data} {};
    // branch constructor
    KDnode(KDnode *lower, KDnode *upper,u8 threshold,uint axis) : axis_or_fill{int8_t(axis)},threshold{threshold},lower{lower},upper{upper} {};
    // move constructor
    KDnode(KDnode &&src) : axis_or_fill{src.axis_or_fill},threshold{src.threshold} {
        if (is_leaf()) {
            leaf_data = src.leaf_data;
        } else {
            upper = src.upper;
            lower = src.lower;
            src.upper = nullptr;
            src.lower = nullptr;
        }
    }
    // I hate C++
    KDnode &operator=(KDnode &&other) {
        if (this != &other) {
            // Is construct-in-place correct here?
            new(this) KDnode(std::move(other));
        }
        return *this;
    }
    // Destructor
    ~KDnode() {
        if (!is_leaf()) {
            delete lower;
            delete upper;
        }
    }

};

// Tuple returned is leaf and vector counts
static std::pair<uint,uint> build_kdtree(CPYuvBlock *begin, CPYuvBlock *end, KDnode *emplace) {

    uint count = end-begin;
    assert(count > 0);
    if (count <= LEAF_SIZE) {
        new(emplace) KDnode(begin,count);
        return {1,count};
    } else {
        uint median_idx = std::distance(begin,end) / 2;
        auto [axis,extent] = max_extent(begin,end);
        if (extent == 0 && false) {
            // Can squash entire thing
            // But TODO this actually results in worse everything, so don't
            u16 total_weight = 0;
            for (auto blk=begin;blk!=end;blk++) {
                total_weight = clamp_u16(total_weight+blk->weight);
            }
            begin->weight = total_weight;
            new(emplace) KDnode(begin,1);
            return {1,1};
        } else {
            auto pivot = quickselect(begin,end,median_idx,[axis](CPYuvBlock *block){return block_byte(*block,axis);});
            auto lower_node = (KDnode*)::operator new(sizeof(KDnode));
            auto lower = build_kdtree(begin,pivot,lower_node);
            auto upper_node = (KDnode*)::operator new(sizeof(KDnode));
            auto upper = build_kdtree(pivot,end,upper_node);
            new(emplace) KDnode(lower_node,upper_node,block_byte(*pivot,axis),axis);
            return {lower.first+upper.first,lower.second+upper.second};
        }
    }    
}


static CPYuvBlock *tree_flatten(CPYuvBlock* dst,KDnode *node) {
    if (node->is_leaf()) {
        auto leaf_data = node->leaf_data;
        if (leaf_data != dst) std::copy(
            leaf_data,
            leaf_data - node->axis_or_fill,
            dst
        );
        dst -= node->axis_or_fill;
    } else {
        dst = tree_flatten(dst,node->lower);
        dst = tree_flatten(dst,node->upper);
    }
    return dst;
}

constexpr uint REBLANCE_RATIO = 2;

static uint rebalance_kdtree(KDnode *node) {
    // very approximate "rebalancing".
    // really just progressively deconstructing the tree
    if (node->is_leaf()) return 0-node->axis_or_fill;
    uint lower_size = rebalance_kdtree(node->lower);
    uint upper_size = rebalance_kdtree(node->upper);
    if (node->lower->is_leaf() && node->upper->is_leaf() && lower_size+upper_size <= LEAF_SIZE) {
        std::copy(
            node->upper->leaf_data,
            node->upper->leaf_data + upper_size,
            node->lower->leaf_data + lower_size
        );
        auto leaf_data = node->lower->leaf_data;
        node->axis_or_fill = int8_t(0-(lower_size+upper_size));
        delete node->lower;
        delete node->upper;
        node->leaf_data = leaf_data;
        return lower_size+upper_size;
    } else if (lower_size > REBLANCE_RATIO*upper_size || upper_size > REBLANCE_RATIO*lower_size) {
        auto lowest_lower = node->lower;
        while (!lowest_lower->is_leaf()) lowest_lower = lowest_lower->lower;
        auto data = lowest_lower->leaf_data;
        auto end = tree_flatten(data,node);
        node->~KDnode(); // Manually destroy so we can emplace new node over same memory
        auto [leaves,vectors] = build_kdtree(data,end,node);
        //printf("Node Rebuilt!\n");
        //printf("Should rebuild node, but shit's busted %u %u\n",lower_size,upper_size);
        return vectors;
    } else {
        return lower_size+upper_size;
    }
}

[[maybe_unused]]
static uint count_leaves(KDnode *node) {
    if (node->is_leaf()) return 1;
    else return count_leaves(node->lower) + count_leaves(node->upper);
}

struct MergeInfo {
    KDnode *node;
    u32 distortion;
    std::pair<u8,u8> pair;
    u8 inter_weight;
};

static MergeInfo *gen_leaf_merge(KDnode *node,MergeInfo *merge_dst) {

    // Find lowest merge distortion inside leaf bucket
    u64 lowest_distortion = UINT64_MAX;
    std::pair<u8,u8> best_pair(0,0);
    u8 fill = u8(0-node->axis_or_fill);
    if (fill<2) return merge_dst;
    for (u8 i=0;i<fill-1;i++) {
        auto i_block = node->leaf_data[i];
        for (u8 j=i+1;j<fill;j++) {
            auto j_block = node->leaf_data[j];
            u64 distortion = blockDistortion(i_block,j_block);
            distortion *= (i_block.weight*j_block.weight) / (i_block.weight + j_block.weight);
            if (distortion < lowest_distortion) {
                lowest_distortion = distortion;
                best_pair = {i,j};
            }
        }
    }
    auto weight1 = node->leaf_data[best_pair.first].weight;
    auto weight2 = node->leaf_data[best_pair.second].weight;
    u8 inter_weight = clamp_u8((511*weight1+weight2)/(2*(weight1+weight2)));

    *merge_dst++ = {
        .node = node,
        .distortion = (u32)std::min(lowest_distortion,(u64)UINT32_MAX),
        .pair = best_pair,
        .inter_weight = inter_weight,
    };
    return merge_dst;
}

static MergeInfo *gen_merges(KDnode *node,MergeInfo *merge_dst) {
    if (node->is_leaf()) {
        merge_dst = gen_leaf_merge(node,merge_dst);
    } else {
        merge_dst = gen_merges(node->lower,merge_dst);
        merge_dst = gen_merges(node->upper,merge_dst);
    }

    return merge_dst;
}

static void do_merge(MergeInfo merge) {
    assert(merge.pair.first < merge.pair.second);
    u8 aw = merge.inter_weight;
    u8 bw = aw^255;
    CPYuvBlock a = merge.node->leaf_data[merge.pair.first];
    CPYuvBlock b = merge.node->leaf_data[merge.pair.second];
    merge.node->leaf_data[merge.pair.first] = {
        .weight = clamp_u16(a.weight+b.weight),
        .u   = u8((a.u  *aw + b.u  *bw + 255)/256),
        .v   = u8((a.v  *aw + b.v  *bw + 255)/256),
        .ytl = u8((a.ytl*aw + b.ytl*bw + 255)/256),
        .ytr = u8((a.ytr*aw + b.ytr*bw + 255)/256),
        .ybl = u8((a.ybl*aw + b.ybl*bw + 255)/256),
        .ybr = u8((a.ybr*aw + b.ybr*bw + 255)/256),
    };

    if (0-merge.pair.second != 1+merge.node->axis_or_fill) std::copy(
        merge.node->leaf_data + merge.pair.second + 1, // Source begin
        merge.node->leaf_data - merge.node->axis_or_fill, // Source end (remember, fill is negative)
        merge.node->leaf_data + merge.pair.second // Destination begin
    );
    merge.node->axis_or_fill++;
}

u64 CPEncoderState::vq_fastpnn(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data, std::vector<uint> &applicable_indices,std::vector<u8> *closest_out) {

    // PNN is not iterative
    codebook.clear();
    // Init data structures
    auto vectors = std::make_unique<CPYuvBlock[]>(applicable_indices.size());
    auto end = vectors.get();
    // copy blocks into buffer that we can then partition, etc
    for (uint idx : applicable_indices) {
        *end++ = data[idx];
    }

    KDnode kd_root;
    auto kd_build_result = build_kdtree(vectors.get(),end,&kd_root);
    auto kd_leaves = kd_build_result.first;
    u32 vector_count = kd_build_result.second;
    vector_count = rebalance_kdtree(&kd_root);
    const uint mergelist_size = kd_leaves+kd_leaves/2; // Sometimes rebalancing grows the tree
    auto merges = std::make_unique<MergeInfo[]>(mergelist_size);
    printf("Tree built! %u leaves %u vectors\n",kd_leaves,vector_count);

    u64 approx_distortion = 0;

    while (vector_count > target_codebook_size) {
        printf("Leaf count: %u\n",count_leaves(&kd_root));
        auto merge_end = gen_merges(&kd_root,merges.get());
        uint merge_count = merge_end - merges.get();
        assert(merge_count <= mergelist_size);
        assert(merge_count > 0);
        if (vector_count-merge_count/2 < target_codebook_size) {
            // Final iteration, need to fully sort candidates
            quicksort(merges.get(),merge_end,[](MergeInfo *i){return i->distortion;});
        } else {
            // Not final iteration, partition is enough
            auto median = quickselect(merges.get(),merge_end,merge_count/2,[](MergeInfo *i){return i->distortion;});
            assert(median->distortion >= merges[0].distortion);
            merge_end = 1+median;
        }
        for (auto merge_ptr = merges.get();merge_ptr!=merge_end;merge_ptr++) {
            do_merge(*merge_ptr);
            approx_distortion += merge_ptr->distortion;
            if (--vector_count == target_codebook_size) goto done;
        }
        printf("Merge iterated! %u vectors\n",vector_count);
        vector_count = rebalance_kdtree(&kd_root);
        printf("Rebalance iterated!\n");
    }
    done:
    codebook.resize(vector_count);
    tree_flatten(codebook.data(),&kd_root);

    if (closest_out) {
        std::vector<std::vector<uint>> partition(codebook.size());
        u64 code_distortion[256];
        approx_distortion = voronoi_partition(codebook,data,applicable_indices,code_distortion,partition);
        // Fill closest_out from partition data
        for (uint i=0;i<partition.size();i++) {
            for (auto idx : partition[i]) {
                (*closest_out)[idx] = u8(i);
            }
        }
    }
    return approx_distortion;
}