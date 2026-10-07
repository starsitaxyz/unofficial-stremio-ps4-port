#pragma once

#include <cstddef>
#include <cstdint>

// Reverse-engineered libSceVideodec2 ABI used by Moonlight-PS4 and shadPS4.
// This first Stremio PS4 pass enables AVC/H.264 hardware decoding.

constexpr uint32_t PS4_VD2_CODEC_AVC = 1;
constexpr uint32_t PS4_VD2_RESOURCE_EMBEDDED = 1;
constexpr uint32_t PS4_VD2_PROFILE_HIGH = 100;
constexpr uint32_t PS4_VD2_LEVEL_51 = 51;
constexpr int32_t PS4_VD2_THREAD_PRIO = 700;
constexpr uint64_t PS4_VD2_AFFINITY_ALL = 0x3f;

constexpr size_t PS4_DMEM_ALIGN = 0x4000;
constexpr int32_t PS4_DMEM_ONION = 0;
constexpr int32_t PS4_DMEM_GARLIC = 3;
constexpr int32_t PS4_DMEM_WB_GARLIC = 10;
constexpr int32_t PS4_DMEM_PROT_RW = 0x33;

struct Ps4Vd2ComputeConfigInfo {
    uint64_t thisSize;
    uint16_t computePipeId;
    uint16_t computeQueueId;
    bool checkMemoryType;
    uint8_t reserved0;
    uint16_t reserved1;
};

struct Ps4Vd2ComputeMemoryInfo {
    uint64_t thisSize;
    uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
};

struct Ps4Vd2DecoderConfigInfo {
    uint64_t thisSize;
    uint32_t resourceType;
    uint32_t codecType;
    uint32_t profile;
    uint32_t maxLevel;
    int32_t maxFrameWidth;
    int32_t maxFrameHeight;
    int32_t maxDpbFrameCount;
    uint32_t decodePipelineDepth;
    void* computeQueue;
    uint64_t cpuAffinityMask;
    int32_t cpuThreadPriority;
    bool optimizeProgressiveVideo;
    bool checkMemoryType;
    uint8_t reserved0;
    uint8_t reserved1;
    void* extraConfigInfo;
};

struct Ps4Vd2DecoderMemoryInfo {
    uint64_t thisSize;
    uint64_t cpuMemorySize;
    void* cpuMemory;
    uint64_t gpuMemorySize;
    void* gpuMemory;
    uint64_t cpuGpuMemorySize;
    void* cpuGpuMemory;
    uint64_t maxFrameBufferSize;
    uint32_t frameBufferAlignment;
    uint32_t reserved0;
};

struct Ps4Vd2InputData {
    uint64_t thisSize;
    void* auData;
    uint64_t auSize;
    uint64_t ptsData;
    uint64_t dtsData;
    uint64_t attachedData;
};

struct Ps4Vd2OutputInfo {
    uint64_t thisSize;
    bool isValid;
    bool isErrorFrame;
    uint8_t pictureCount;
    uint32_t codecType;
    uint32_t frameWidth;
    uint32_t framePitch;
    uint32_t frameHeight;
    void* frameBuffer;
    uint64_t frameBufferSize;
    uint32_t frameFormat;
    uint32_t framePitchInBytes;
};

struct Ps4Vd2FrameBuffer {
    uint64_t thisSize;
    void* frameBuffer;
    uint64_t frameBufferSize;
    bool isAccepted;
};

// Fail at compile time if the OpenOrbis compiler lays these reversed ABI
// structures out differently from the console-validated Moonlight-PS4 ABI.
static_assert(sizeof(Ps4Vd2ComputeConfigInfo) == 16, "Videodec2 compute config ABI mismatch");
static_assert(sizeof(Ps4Vd2ComputeMemoryInfo) == 24, "Videodec2 compute memory ABI mismatch");
static_assert(sizeof(Ps4Vd2DecoderConfigInfo) == 72, "Videodec2 decoder config ABI mismatch");
static_assert(sizeof(Ps4Vd2DecoderMemoryInfo) == 72, "Videodec2 decoder memory ABI mismatch");
static_assert(sizeof(Ps4Vd2InputData) == 48, "Videodec2 input ABI mismatch");
static_assert(sizeof(Ps4Vd2OutputInfo) == 56, "Videodec2 output ABI mismatch");
static_assert(sizeof(Ps4Vd2FrameBuffer) == 32, "Videodec2 framebuffer ABI mismatch");

using Ps4Vd2Decoder = void*;
using Ps4Vd2ComputeQueue = void*;

struct Ps4Vd2Api {
    int module = -1;
    Ps4Vd2ComputeQueue queue = nullptr;
    int32_t (*QueryComputeMemoryInfo)(Ps4Vd2ComputeMemoryInfo*) = nullptr;
    int32_t (*AllocateComputeQueue)(const Ps4Vd2ComputeConfigInfo*, const Ps4Vd2ComputeMemoryInfo*, Ps4Vd2ComputeQueue*) = nullptr;
    int32_t (*ReleaseComputeQueue)(Ps4Vd2ComputeQueue) = nullptr;
    int32_t (*QueryDecoderMemoryInfo)(const Ps4Vd2DecoderConfigInfo*, Ps4Vd2DecoderMemoryInfo*) = nullptr;
    int32_t (*CreateDecoder)(const Ps4Vd2DecoderConfigInfo*, const Ps4Vd2DecoderMemoryInfo*, Ps4Vd2Decoder*) = nullptr;
    int32_t (*DeleteDecoder)(Ps4Vd2Decoder) = nullptr;
    int32_t (*Decode)(Ps4Vd2Decoder, const Ps4Vd2InputData*, Ps4Vd2FrameBuffer*, Ps4Vd2OutputInfo*) = nullptr;
    int32_t (*Flush)(Ps4Vd2Decoder, Ps4Vd2FrameBuffer*, Ps4Vd2OutputInfo*) = nullptr;
    int32_t (*Reset)(Ps4Vd2Decoder) = nullptr;
};

bool ps4_vd2_load(Ps4Vd2Api& api);
bool ps4_vd2_ensure_queue(Ps4Vd2Api& api);
