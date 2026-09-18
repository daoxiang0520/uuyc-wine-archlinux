/*
 * uuyc-vkvideo -- does this machine's Vulkan ICD expose Vulkan Video decode?
 *
 * Why this exists
 * ---------------
 * Wine's Direct3D 11 video decoder has exactly one hardware backend: Vulkan
 * Video. wined3d never calls VA-API. So "can UU Remote decode in hardware under
 * Wine" reduces to "does the ICD expose VK_KHR_video_decode_h264 to this
 * process".
 *
 * AMD's RADV exposes it unconditionally. Intel's ANV does not: Mesa gates it
 * behind the ANV_DEBUG=video-decode debug flag --
 *
 *   src/intel/vulkan/anv_physical_device.c:
 *     const bool video_decode_enabled = ANV_DEBUG(VIDEO_DECODE);
 *     .KHR_video_decode_h264 = VIDEO_CODEC_H264DEC && video_decode_enabled,
 *     .KHR_video_decode_h265 = VIDEO_CODEC_H265DEC && video_decode_enabled && device->has_huc,
 *
 *   src/intel/vulkan/anv_instance.c:
 *     { "video-decode", ANV_DEBUG_VIDEO_DECODE },   // hyphen, not underscore
 *
 * so the point of this program is to run the same enumeration twice, with and
 * without that flag, and show the difference.
 *
 * Build (deliberately dependency-free):
 *     gcc -O2 -o uuyc-vkvideo uuyc-vkvideo.c -ldl
 *
 * It does NOT include <vulkan/vulkan.h> and does NOT link against libvulkan:
 * everything is resolved at runtime via dlopen/dlsym, which keeps the tool
 * usable on machines without vulkan-headers or vulkan-tools.
 *
 * ABI note: every struct below is spelled out in full to the exact size the
 * loader expects. An undersized VkPhysicalDeviceProperties makes the ICD write
 * past the end of the buffer -- which shows up as "stack smashing detected",
 * not as an obvious ABI error. vk_assert_sizeof() below guards against that
 * regressing again, so the failure mode is a clear message instead of a crash.
 *
 * SPDX-License-Identifier: 0BSD
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

typedef int32_t VkResult;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkSampleMask;
typedef uint64_t VkDeviceAddress;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef int32_t VkFlags;
typedef uint32_t VkVideoChromaSubsamplingFlagsKHR;
typedef uint32_t VkVideoComponentBitDepthFlagsKHR;
typedef uint32_t VkVideoCapabilityFlagsKHR;
typedef uint32_t VkVideoDecodeCapabilityFlagsKHR;
typedef uint32_t VkVideoCodecOperationFlagBitsKHR;
typedef uint32_t VkVideoDecodeH264PictureLayoutFlagBitsKHR;

#define VK_SUCCESS 0
#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256
#define VK_UUID_SIZE 16
#define VK_LUID_SIZE 8
#define VK_MAX_DRIVER_NAME_SIZE 256
#define VK_MAX_DRIVER_INFO_SIZE 256

typedef struct {
    char     extensionName[256];
    uint32_t specVersion;
} VkExtensionProperties;

typedef struct {
    uint32_t    maxImageDimension1D;
    uint32_t    maxImageDimension2D;
    uint32_t    maxImageDimension3D;
    uint32_t    maxImageDimensionCube;
    uint32_t    maxImageArrayLayers;
    uint32_t    maxTexelBufferElements;
    uint32_t    maxUniformBufferRange;
    uint32_t    maxStorageBufferRange;
    uint32_t    maxPushConstantsSize;
    uint32_t    maxMemoryAllocationCount;
    uint32_t    maxSamplerAllocationCount;
    VkDeviceSize bufferImageGranularity;
    VkDeviceSize sparseAddressSpaceSize;
    uint32_t    maxBoundDescriptorSets;
    uint32_t    maxPerStageDescriptorSamplers;
    uint32_t    maxPerStageDescriptorUniformBuffers;
    uint32_t    maxPerStageDescriptorStorageBuffers;
    uint32_t    maxPerStageDescriptorSampledImages;
    uint32_t    maxPerStageDescriptorStorageImages;
    uint32_t    maxPerStageDescriptorInputAttachments;
    uint32_t    maxPerStageResources;
    uint32_t    maxDescriptorSetSamplers;
    uint32_t    maxDescriptorSetUniformBuffers;
    uint32_t    maxDescriptorSetUniformBuffersDynamic;
    uint32_t    maxDescriptorSetStorageBuffers;
    uint32_t    maxDescriptorSetStorageBuffersDynamic;
    uint32_t    maxDescriptorSetSampledImages;
    uint32_t    maxDescriptorSetStorageImages;
    uint32_t    maxDescriptorSetInputAttachments;
    uint32_t    maxVertexInputAttributes;
    uint32_t    maxVertexInputBindings;
    uint32_t    maxVertexInputAttributeOffset;
    uint32_t    maxVertexInputBindingStride;
    uint32_t    maxVertexOutputComponents;
    uint32_t    maxTessellationGenerationLevel;
    uint32_t    maxTessellationPatchSize;
    uint32_t    maxTessellationControlPerVertexInputComponents;
    uint32_t    maxTessellationControlPerVertexOutputComponents;
    uint32_t    maxTessellationControlPerPatchOutputComponents;
    uint32_t    maxTessellationControlTotalOutputComponents;
    uint32_t    maxTessellationEvaluationInputComponents;
    uint32_t    maxTessellationEvaluationOutputComponents;
    uint32_t    maxGeometryShaderInvocations;
    uint32_t    maxGeometryInputComponents;
    uint32_t    maxGeometryOutputComponents;
    uint32_t    maxGeometryOutputVertices;
    uint32_t    maxGeometryTotalOutputComponents;
    uint32_t    maxFragmentInputComponents;
    uint32_t    maxFragmentOutputAttachments;
    uint32_t    maxFragmentDualSrcAttachments;
    uint32_t    maxFragmentCombinedOutputResources;
    uint32_t    maxComputeSharedMemorySize;
    uint32_t    maxComputeWorkGroupCount[3];
    uint32_t    maxComputeWorkGroupInvocations;
    uint32_t    maxComputeWorkGroupSize[3];
    uint32_t    subPixelPrecisionBits;
    uint32_t    subTexelPrecisionBits;
    uint32_t    mipmapPrecisionBits;
    uint32_t    maxDrawIndexedIndexValue;
    uint32_t    maxDrawIndirectCount;
    float       maxSamplerLodBias;
    float       maxSamplerAnisotropy;
    uint32_t    maxViewports;
    uint32_t    maxViewportDimensions[2];
    float       viewportBoundsRange[2];
    uint32_t    viewportSubPixelBits;
    size_t      minMemoryMapAlignment;
    VkDeviceSize minTexelBufferOffsetAlignment;
    VkDeviceSize minUniformBufferOffsetAlignment;
    VkDeviceSize minStorageBufferOffsetAlignment;
    int32_t     minTexelOffset;
    uint32_t    maxTexelOffset;
    int32_t     minTexelGatherOffset;
    uint32_t    maxTexelGatherOffset;
    float       minInterpolationOffset;
    float       maxInterpolationOffset;
    uint32_t    subPixelInterpolationOffsetBits;
    uint32_t    maxFramebufferWidth;
    uint32_t    maxFramebufferHeight;
    uint32_t    maxFramebufferLayers;
    uint32_t    framebufferColorSampleCounts;
    uint32_t    framebufferDepthSampleCounts;
    uint32_t    framebufferStencilSampleCounts;
    uint32_t    framebufferNoAttachmentsSampleCounts;
    uint32_t    maxColorAttachments;
    uint32_t    sampledImageColorSampleCounts;
    uint32_t    sampledImageIntegerSampleCounts;
    uint32_t    sampledImageDepthSampleCounts;
    uint32_t    sampledImageStencilSampleCounts;
    uint32_t    storageImageSampleCounts;
    uint32_t    maxSampleMaskWords;
    VkBool32    timestampComputeAndGraphics;
    float       timestampPeriod;
    uint32_t    maxClipDistances;
    uint32_t    maxCullDistances;
    uint32_t    maxCombinedClipAndCullDistances;
    uint32_t    discreteQueuePriorities;
    float       pointSizeRange[2];
    float       lineWidthRange[2];
    float       pointSizeGranularity;
    float       lineWidthGranularity;
    VkBool32    strictLines;
    VkBool32    standardSampleLocations;
    VkDeviceSize optimalBufferCopyOffsetAlignment;
    VkDeviceSize optimalBufferCopyRowPitchAlignment;
    VkDeviceSize nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct {
    VkBool32    residencyStandard2DBlockShape;
    VkBool32    residencyStandard2DMultisampleBlockShape;
    VkBool32    residencyStandard3DBlockShape;
    VkBool32    residencyAlignedMipSize;
    VkBool32    residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct {
    uint32_t                       apiVersion;
    uint32_t                       driverVersion;
    uint32_t                       vendorID;
    uint32_t                       deviceID;
    uint32_t                       deviceType;
    char                           deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    uint8_t                        pipelineCacheUUID[VK_UUID_SIZE];
    VkPhysicalDeviceLimits         limits;
    VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct {
    uint32_t    sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t    applicationVersion;
    const char *pEngineName;
    uint32_t    engineVersion;
    uint32_t    apiVersion;
} VkApplicationInfo;

typedef struct { uint32_t width, height; } VkExtent2D;

typedef struct {
    uint32_t                sType;
    const void             *pNext;
    uint32_t                flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t                enabledLayerCount;
    const char *const      *ppEnabledLayerNames;
    uint32_t                enabledExtensionCount;
    const char *const      *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

/* --- Vulkan Video structs. Only the fields wined3d actually reads matter for
 * the verdict, but each struct is still spelled out to its real ABI size. --- */

typedef struct {
    uint32_t    sType;
    const void *pNext;
    VkVideoCodecOperationFlagBitsKHR videoCodecOperation;
    VkVideoChromaSubsamplingFlagsKHR chromaSubsampling;
    VkVideoComponentBitDepthFlagsKHR lumaBitDepth;
    VkVideoComponentBitDepthFlagsKHR chromaBitDepth;
} VkVideoProfileInfoKHR;
/* official: 32 bytes = 4 + 4 pad + 8 + 4*4 */

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t stdProfileIdc;
    VkVideoDecodeH264PictureLayoutFlagBitsKHR pictureLayout;
} VkVideoDecodeH264ProfileInfoKHR;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t maxLevelIdc;
} VkVideoDecodeH264CapabilitiesKHR;

typedef struct {
    uint32_t sType;
    const void *pNext;
    VkVideoDecodeCapabilityFlagsKHR flags;
} VkVideoDecodeCapabilitiesKHR;
/* official: 24 bytes = 4 + 4 pad + 8 + 4 + 4 pad */

/*
 * VkVideoCapabilitiesKHR is 336 bytes in the current headers. It has grown
 * before (stdHeaderVersion was appended) and may grow again, so the tail is
 * reserved generously and canaried: an ICD that writes further than we reserved
 * is reported instead of corrupting the stack.
 *
 *   official layout (offsets):  sType 0, pNext 8, flags 16,
 *                               minBitstreamBufferOffsetAlignment 24,
 *                               minBitstreamBufferSizeAlignment   32,
 *                               ... 336 total
 */
#define VK_VIDEO_CAPS_TAIL 1024
typedef struct {
    uint32_t                     sType;
    const void                  *pNext;
    VkVideoCapabilityFlagsKHR    flags;
    VkDeviceSize                 minBitstreamBufferOffsetAlignment;
    VkDeviceSize                 minBitstreamBufferSizeAlignment;
    uint8_t                      tail[VK_VIDEO_CAPS_TAIL];
} VkVideoCapabilitiesKHR;

typedef VkResult (*PFN_getVideoCaps)(VkPhysicalDevice, const VkVideoProfileInfoKHR *, VkVideoCapabilitiesKHR *);

typedef VkResult (*PFN_enumInstExt)(const char *, uint32_t *, VkExtensionProperties *);
typedef VkResult (*PFN_createInst)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef VkResult (*PFN_enumPhys)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void     (*PFN_getProps)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
typedef VkResult (*PFN_enumDevExt)(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
typedef void    *(*PFN_gipa)(VkInstance, const char *);

/* ABI self-check: the ICD fills the whole struct, so a short definition is a
 * buffer overflow waiting to happen. Fail loudly instead of smashing the stack. */
#define VK_ASSERT_SIZEOF(type, expect) \
    _Static_assert(sizeof(type) == (expect), #type " has the wrong size")

VK_ASSERT_SIZEOF(VkPhysicalDeviceLimits, 504);
VK_ASSERT_SIZEOF(VkPhysicalDeviceSparseProperties, 20);
VK_ASSERT_SIZEOF(VkPhysicalDeviceProperties, 824);
VK_ASSERT_SIZEOF(VkExtensionProperties, 260);
VK_ASSERT_SIZEOF(VkApplicationInfo, 48);
VK_ASSERT_SIZEOF(VkInstanceCreateInfo, 64);

/* Constants from vulkan_core.h, spelled out so this file stays header-free. */
#define STD_VIDEO_H264_PROFILE_IDC_HIGH 100u
#define VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR 0u
#define VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR 0x00000001u
#define VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR 0x00000002u
#define VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR 0x00000001u
#define VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR 1000023000u
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR 1000040003u
#define VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR 1000023001u
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR 1000024001u
#define VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR 1000040000u

static const char *want_exts[] = {
    "VK_KHR_video_queue",
    "VK_KHR_video_decode_queue",
    "VK_KHR_video_decode_h264",
    "VK_KHR_video_decode_h265",
    "VK_KHR_video_decode_vp9",
    "VK_KHR_video_decode_av1",
    "VK_KHR_video_maintenance1",
    NULL,
};

/* Yes/No in a fixed column so the calling script can grep for the verdict. */
static int report_exts(VkPhysicalDevice dev, PFN_enumDevExt enumDevExt)
{
    uint32_t count = 0;
    if (enumDevExt(dev, NULL, &count, NULL) != VK_SUCCESS || count == 0) {
        printf("  device extension count unavailable (count=%u)\n", count);
        return 0;
    }
    VkExtensionProperties *exts = calloc(count, sizeof(*exts));
    if (!exts) { printf("  out of memory listing %u extensions\n", count); return 0; }
    if (enumDevExt(dev, NULL, &count, exts) != VK_SUCCESS) {
        printf("  device extension enumeration failed\n");
        free(exts);
        return 0;
    }
    unsigned have = 0;
    printf("  device extension count = %u\n", count);
    for (int k = 0; want_exts[k]; k++) {
        int found = 0;
        for (uint32_t j = 0; j < count; j++) {
            if (!strcmp(exts[j].extensionName, want_exts[k])) { found = 1; break; }
        }
        printf("  %-28s %s\n", want_exts[k], found ? "YES" : "no");
        if (found) have |= 1u << k;
    }
    free(exts);
    return have;
}

/*
 * This is the check that actually decides hardware decoding under Wine:
 * wined3d_decoder_vk_is_h264_decode_supported() in dlls/wined3d/decoder.c asks
 * the driver for H.264 High profile / progressive / NV12 capabilities, and only
 * then adds DXVA_ModeH264_VLD_NoFGT to the profile list that
 * ID3D11VideoDevice::GetVideoDecoderProfileCount returns. An exposed extension
 * is necessary but not sufficient.
 */
static void report_h264_caps(VkPhysicalDevice dev, PFN_getVideoCaps getVideoCaps)
{
    struct {
        VkVideoCapabilitiesKHR           caps;
        uint64_t                         canary[8];
    } guard;
    VkVideoDecodeCapabilitiesKHR decode_caps;
    VkVideoDecodeH264CapabilitiesKHR h264_caps;
    VkVideoDecodeH264ProfileInfoKHR h264_profile;
    VkVideoProfileInfoKHR profile;

    if (!getVideoCaps) {
        printf("  vkGetPhysicalDeviceVideoCapabilitiesKHR : not resolvable\n");
        return;
    }

    memset(&guard, 0, sizeof(guard));
    for (unsigned c = 0; c < 8; c++) guard.canary[c] = 0x5543594300000000ull + c;
    memset(&decode_caps, 0, sizeof(decode_caps));
    memset(&h264_caps, 0, sizeof(h264_caps));
    memset(&h264_profile, 0, sizeof(h264_profile));
    memset(&profile, 0, sizeof(profile));

    h264_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR;
    h264_profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_HIGH;
    h264_profile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;

    profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
    profile.pNext = &h264_profile;
    profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
    profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;

    /* Mirror wined3d exactly: caps -> decode_caps -> h264_caps. */
    guard.caps.sType = VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR;
    guard.caps.pNext = &decode_caps;
    decode_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR;
    decode_caps.pNext = &h264_caps;
    h264_caps.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR;

    VkResult vr = getVideoCaps(dev, &profile, &guard.caps);

    for (unsigned c = 0; c < 8; c++) {
        if (guard.canary[c] != 0x5543594300000000ull + c) {
            printf("  H.264 capability query                : ABI CHECK FAILED\n");
            printf("        the ICD wrote past our %zu-byte reservation\n", sizeof(guard.caps));
            return;
        }
    }

    printf("  H.264 High/Progressive/NV12 query     : %s\n",
           vr == VK_SUCCESS ? "SUPPORTED" : "NOT SUPPORTED");
    printf("        (vkGetPhysicalDeviceVideoCapabilitiesKHR -> %d)\n", vr);
    if (vr == VK_SUCCESS)
        printf("        ^ this is what makes wined3d report DXVA_ModeH264_VLD_NoFGT\n");
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("dlopen(\"libvulkan.so.1\") failed: %s\n", dlerror());
        printf("install a Vulkan runtime:  sudo pacman -S vulkan-icd-loader\n");
        return 2;
    }
    PFN_gipa gipa = (PFN_gipa)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { printf("libvulkan.so.1 has no vkGetInstanceProcAddr\n"); return 2; }

    PFN_enumInstExt enumInstExt = (PFN_enumInstExt)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    PFN_createInst  createInst  = (PFN_createInst) gipa(NULL, "vkCreateInstance");
    if (!enumInstExt || !createInst) { printf("the loader did not resolve vkCreateInstance\n"); return 2; }

    uint32_t n = 0;
    printf("loader instance extension count = %u\n", (enumInstExt(NULL, &n, NULL), n));

    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = 0; /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
    ai.pApplicationName = "uuyc-vkvideo";
    ai.applicationVersion = 1;
    ai.pEngineName = "uuyc-vkvideo";
    ai.engineVersion = 1;
    ai.apiVersion = (1u << 22) | (3u << 12); /* 1.3 */

    VkInstanceCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = 1; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
    ci.pApplicationInfo = &ai;

    VkInstance inst = NULL;
    VkResult r = createInst(&ci, NULL, &inst);
    printf("vkCreateInstance -> %d\n", r);
    if (r != VK_SUCCESS || !inst) { printf("cannot create a Vulkan instance\n"); return 3; }

    PFN_enumPhys  enumPhys  = (PFN_enumPhys) gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_getProps  getProps  = (PFN_getProps) gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_enumDevExt enumDevExt = (PFN_enumDevExt)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    PFN_getVideoCaps getVideoCaps = (PFN_getVideoCaps)gipa(inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    if (!enumPhys || !getProps || !enumDevExt) { printf("instance functions did not resolve\n"); return 3; }

    uint32_t ndev = 0;
    r = enumPhys(inst, &ndev, NULL);
    printf("vkEnumeratePhysicalDevices -> %d count=%u\n", r, ndev);
    if (r != VK_SUCCESS) {
        printf(">>> ENUMERATION FAILED (VkResult %d)\n", r);
        printf(">>> This usually means the process cannot reach /dev/dri\n");
        return 4;
    }
    if (ndev == 0) {
        printf(">>> NO VULKAN DEVICE VISIBLE to this process\n");
        printf(">>> Check /dev/dri and that your user is in the render group\n");
        return 4;
    }

    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    if (!devs) { printf("out of memory\n"); return 3; }
    if (enumPhys(inst, &ndev, devs) != VK_SUCCESS) { printf("device re-enumeration failed\n"); return 3; }

    for (uint32_t i = 0; i < ndev; i++) {
        /* Canary: the ICD writes sizeof(VkPhysicalDeviceProperties) bytes. If our
         * definition were ever short, this catches it as a clear message rather
         * than as "*** stack smashing detected ***". */
        struct { VkPhysicalDeviceProperties props; uint64_t canary[8]; } guard;
        memset(&guard, 0, sizeof(guard));
        for (unsigned c = 0; c < 8; c++) guard.canary[c] = 0x5543594300000000ull + c;

        getProps(devs[i], &guard.props);
        VkPhysicalDeviceProperties props = guard.props;
        for (unsigned c = 0; c < 8; c++) {
            if (guard.canary[c] != 0x5543594300000000ull + c) {
                printf("dev[%u] ABI CHECK FAILED: the ICD wrote past VkPhysicalDeviceProperties\n", i);
                printf("        (sizeof=%zu). This is a bug in this probe, not in your driver.\n",
                       sizeof(VkPhysicalDeviceProperties));
                return 5;
            }
        }
        printf("dev[%u] %s  api=%u.%u.%u  type=%u\n", i, props.deviceName,
               (props.apiVersion >> 22) & 0x7fu,
               (props.apiVersion >> 12) & 0x3ffu,
               props.apiVersion & 0xfffu,
               props.deviceType);
        unsigned have = report_exts(devs[i], enumDevExt);
        /* index 0 = video_queue, index 2 = video_decode_h264 */
        if ((have & 1u) && (have & (1u << 2))) {
            report_h264_caps(devs[i], getVideoCaps);
        } else {
            printf("  H.264 High/Progressive/NV12 query     : SKIPPED\n");
            printf("        video_queue%s must both be exposed before the driver may\n",
                   (have & 1u) ? "" : " and video_decode_h264");
            printf("        be asked; the loader aborts the process otherwise.\n");
            printf("        wined3d_decoder_vk_is_h264_decode_supported() returns\n");
            printf("        false in this state, so GetVideoDecoderProfileCount is 0.\n");
        }
    }
    free(devs);
    return 0;
}
