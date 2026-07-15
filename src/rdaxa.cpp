/**********************************************************************************************
*
*   rdaxa - Experimental Daxa presentation backend for raylib software rendering
*
*   This backend intentionally keeps raylib's existing software rlgl path intact and uses
*   Daxa only for Vulkan swapchain ownership and presentation. It is a first integration
*   step, not a full Daxa implementation of rlgl's immediate-mode rendering API.
*
**********************************************************************************************/

#include "rdaxa.h"

#include <daxa/daxa.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

struct RaylibDaxaContext
{
    daxa::Instance instance = {};
    daxa::Device device = {};
    daxa::Swapchain swapchain = {};
    daxa::BufferId uploadBuffer = {};
    std::byte *uploadPtr = nullptr;
    std::size_t uploadSize = 0;
    std::vector<std::byte> convertedPixels = {};
    rdaxa_WindowHandle window = nullptr;
    int width = 0;
    int height = 0;
    unsigned int presentedFrameCount = 0;
};

static std::unique_ptr<RaylibDaxaContext> g_daxa;

static daxa::NativeWindowInfo MakeNativeWindowInfo(rdaxa_WindowHandle window)
{
#if defined(_WIN32)
    return daxa::NativeWindowInfoWin32{ window };
#else
    (void)window;
    return {};
#endif
}

static void DestroyUploadBuffer(RaylibDaxaContext &ctx)
{
    if (!ctx.uploadBuffer.is_empty())
    {
        ctx.device.destroy_buffer(ctx.uploadBuffer);
        ctx.uploadBuffer = {};
        ctx.uploadPtr = nullptr;
        ctx.uploadSize = 0;
    }
}

static bool EnsureUploadBuffer(RaylibDaxaContext &ctx, std::size_t requiredSize)
{
    if (!ctx.uploadBuffer.is_empty() && ctx.uploadSize >= requiredSize)
    {
        return true;
    }

    DestroyUploadBuffer(ctx);

    ctx.uploadBuffer = ctx.device.create_buffer({
        .size = requiredSize,
        .memory_flags = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        .name = "raylib software framebuffer upload",
    });

    auto hostAddress = ctx.device.buffer_host_address(ctx.uploadBuffer);
    if (!hostAddress.has_value())
    {
        DestroyUploadBuffer(ctx);
        return false;
    }

    ctx.uploadPtr = hostAddress.value();
    ctx.uploadSize = requiredSize;
    return true;
}

static bool NeedsBgraUpload(daxa::Format format)
{
    return format == daxa::Format::B8G8R8A8_UNORM || format == daxa::Format::B8G8R8A8_SRGB;
}

static void const *PrepareUploadPixels(RaylibDaxaContext &ctx, void const *rgbaPixels, int width, int height)
{
    daxa::Format const format = ctx.swapchain.get_format();
    if (!NeedsBgraUpload(format))
    {
        return rgbaPixels;
    }

    std::size_t const byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    ctx.convertedPixels.resize(byteCount);

    auto const *src = static_cast<unsigned char const *>(rgbaPixels);
    auto *dst = reinterpret_cast<unsigned char *>(ctx.convertedPixels.data());
    for (std::size_t i = 0; i < byteCount; i += 4)
    {
        dst[i + 0] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i + 0];
        dst[i + 3] = src[i + 3];
    }

    return ctx.convertedPixels.data();
}

bool rdaxaInit(rdaxa_WindowHandle window, int width, int height)
{
    if (window == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    try
    {
        g_daxa = std::make_unique<RaylibDaxaContext>();
        RaylibDaxaContext &ctx = *g_daxa;
        ctx.window = window;
        ctx.width = width;
        ctx.height = height;

        ctx.instance = daxa::create_instance({});
        ctx.device = ctx.instance.create_device_2(ctx.instance.choose_device({}, {
            .name = "raylib Daxa device",
        }));

        daxa::NativeWindowInfo nativeWindow = MakeNativeWindowInfo(window);
        ctx.swapchain = ctx.device.create_swapchain({
            .native_window_info = nativeWindow,
            .surface_format = ctx.device.choose_swapchain_surface_format({
                .native_window_info = nativeWindow,
            }),
            .present_mode = daxa::PresentMode::FIFO,
            .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
            .name = "raylib Daxa swapchain",
        });

        return true;
    }
    catch (...)
    {
        g_daxa.reset();
        return false;
    }
}

void rdaxaShutdown(void)
{
    if (!g_daxa)
    {
        return;
    }

    try
    {
        g_daxa->device.wait_idle();
        DestroyUploadBuffer(*g_daxa);
        g_daxa->device.collect_garbage();
    }
    catch (...)
    {
    }

    g_daxa.reset();
}

void rdaxaResize(int width, int height)
{
    if (!g_daxa || width <= 0 || height <= 0)
    {
        return;
    }

    g_daxa->width = width;
    g_daxa->height = height;
    try
    {
        g_daxa->swapchain.resize();
    }
    catch (...)
    {
    }
}

unsigned int rdaxaGetPresentedFrameCount(void)
{
    if (!g_daxa)
    {
        return 0;
    }

    return g_daxa->presentedFrameCount;
}

bool rdaxaPresent(const void *rgbaPixels, int width, int height)
{
    if (!g_daxa || rgbaPixels == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    RaylibDaxaContext &ctx = *g_daxa;
    std::size_t const byteCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;

    try
    {
        if (width != ctx.width || height != ctx.height)
        {
            rdaxaResize(width, height);
        }

        if (!EnsureUploadBuffer(ctx, byteCount))
        {
            return false;
        }

        void const *uploadPixels = PrepareUploadPixels(ctx, rgbaPixels, width, height);
        std::memcpy(ctx.uploadPtr, uploadPixels, byteCount);

        daxa::ImageId swapchainImage = ctx.swapchain.acquire_next_image();
        if (swapchainImage.is_empty())
        {
            return false;
        }

        daxa::CommandRecorder recorder = ctx.device.create_command_recorder({
            .name = "raylib Daxa present recorder",
        });

        recorder.pipeline_image_barrier({
            .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
            .image = swapchainImage,
            .layout_operation = daxa::ImageLayoutOperation::TO_GENERAL,
        });

        recorder.copy_buffer_to_image({
            .src_buffer = ctx.uploadBuffer,
            .buffer_offset = 0,
            .dst_image = swapchainImage,
            .image_slice = { .mip_level = 0, .base_array_layer = 0, .layer_count = 1 },
            .image_offset = { 0, 0, 0 },
            .image_extent = {
                static_cast<daxa::u32>(width),
                static_cast<daxa::u32>(height),
                1,
            },
        });

        recorder.pipeline_image_barrier({
            .src_access = daxa::AccessConsts::TRANSFER_WRITE,
            .image = swapchainImage,
            .layout_operation = daxa::ImageLayoutOperation::TO_PRESENT_SRC,
        });

        daxa::ExecutableCommandList commands = recorder.complete_current_commands();
        auto const &acquireSemaphore = ctx.swapchain.current_acquire_semaphore();
        auto const &presentSemaphore = ctx.swapchain.current_present_semaphore();
        ctx.device.submit_commands(daxa::CommandSubmitInfo{
            .command_lists = std::array{ commands },
            .wait_binary_semaphores = std::array{ acquireSemaphore },
            .signal_binary_semaphores = std::array{ presentSemaphore },
            .signal_timeline_semaphores = std::array{ ctx.swapchain.current_timeline_pair() },
        });
        ctx.device.present_frame({
            .wait_binary_semaphores = std::array{ presentSemaphore },
            .swapchain = ctx.swapchain,
        });
        ctx.device.collect_garbage();
        ctx.presentedFrameCount++;
        return true;
    }
    catch (...)
    {
        return false;
    }
}
