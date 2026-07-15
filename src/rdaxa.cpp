/**********************************************************************************************
*
*   rdaxa - Experimental Daxa backend for raylib
*
*   This backend owns the Vulkan/Daxa swapchain and provides a native immediate-batch
*   raster path for rlgl, plus a software framebuffer upload fallback used by tests and
*   incomplete rendering features.
*
**********************************************************************************************/

#include "rdaxa.h"

#include <daxa/daxa.hpp>
#include <daxa/utils/pipeline_manager.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#if !defined(DAXA_SHADER_INCLUDE_DIR)
    #define DAXA_SHADER_INCLUDE_DIR "."
#endif

namespace
{
constexpr int RDAXA_RL_LINES = 0x0001;
constexpr int RDAXA_RL_TRIANGLES = 0x0004;
constexpr int RDAXA_RL_QUADS = 0x0007;

struct RdaxaGpuVertex
{
    float position[3];
    float texcoord[2];
    float color[4];
};

struct RdaxaNativeDraw
{
    int mode = 0;
    std::size_t firstVertex = 0;
    std::size_t vertexCount = 0;
};

struct RdaxaPushConstants
{
    float mvp[16];
    daxa::DeviceAddress vertices = {};
};

struct RaylibDaxaContext
{
    daxa::Instance instance = {};
    daxa::Device device = {};
    daxa::Swapchain swapchain = {};
    daxa::PipelineManager pipelineManager = {};
    std::shared_ptr<daxa::RasterPipeline> trianglePipeline = {};
    std::shared_ptr<daxa::RasterPipeline> linePipeline = {};
    daxa::BufferId uploadBuffer = {};
    daxa::BufferId nativeVertexBuffer = {};
    std::byte *uploadPtr = nullptr;
    RdaxaGpuVertex *nativeVertexPtr = nullptr;
    std::size_t uploadSize = 0;
    std::size_t nativeVertexBufferSize = 0;
    std::vector<std::byte> convertedPixels = {};
    std::vector<RdaxaGpuVertex> nativeVertices = {};
    std::vector<RdaxaNativeDraw> nativeDraws = {};
    rdaxa_WindowHandle window = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    unsigned int presentedFrameCount = 0;
};

static std::unique_ptr<RaylibDaxaContext> g_daxa;

static char const *GetNativeVertexShader()
{
    return R"glsl(
#include <daxa/daxa.inl>

struct RdaxaVertex {
    daxa_f32vec3 position;
    daxa_f32vec2 texcoord;
    daxa_f32vec4 color;
};

DAXA_DECL_BUFFER_PTR(RdaxaVertex)

struct RdaxaPushConstants {
    daxa_f32mat4x4 mvp;
    daxa_BufferPtr(RdaxaVertex) vertices;
};

DAXA_DECL_PUSH_CONSTANT(RdaxaPushConstants, rdaxa)

#if DAXA_SHADER_STAGE == DAXA_SHADER_STAGE_VERTEX

layout(location = 0) out daxa_f32vec4 fragColor;

void main()
{
    RdaxaVertex vertex = deref_i(rdaxa.vertices, gl_VertexIndex);
    gl_Position = rdaxa.mvp * daxa_f32vec4(vertex.position, 1.0);
    fragColor = vertex.color;
}

#elif DAXA_SHADER_STAGE == DAXA_SHADER_STAGE_FRAGMENT

layout(location = 0) in daxa_f32vec4 fragColor;
layout(location = 0) out daxa_f32vec4 outColor;

void main()
{
    outColor = fragColor;
}

#endif
)glsl";
}
} // namespace

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

static void DestroyNativeVertexBuffer(RaylibDaxaContext &ctx)
{
    if (!ctx.nativeVertexBuffer.is_empty())
    {
        ctx.device.destroy_buffer(ctx.nativeVertexBuffer);
        ctx.nativeVertexBuffer = {};
        ctx.nativeVertexPtr = nullptr;
        ctx.nativeVertexBufferSize = 0;
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

static bool EnsureNativeVertexBuffer(RaylibDaxaContext &ctx, std::size_t vertexCount)
{
    std::size_t const requiredSize = vertexCount*sizeof(RdaxaGpuVertex);
    if (!ctx.nativeVertexBuffer.is_empty() && ctx.nativeVertexBufferSize >= requiredSize)
    {
        return true;
    }

    DestroyNativeVertexBuffer(ctx);

    ctx.nativeVertexBuffer = ctx.device.create_buffer({
        .size = requiredSize,
        .memory_flags = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
        .name = "raylib Daxa native vertex buffer",
    });

    auto hostAddress = ctx.device.buffer_host_address_as<RdaxaGpuVertex>(ctx.nativeVertexBuffer);
    if (!hostAddress.has_value())
    {
        DestroyNativeVertexBuffer(ctx);
        return false;
    }

    ctx.nativeVertexPtr = hostAddress.value();
    ctx.nativeVertexBufferSize = requiredSize;
    return true;
}

static daxa::BlendInfo MakeAlphaBlend()
{
    return {
        .src_color_blend_factor = daxa::BlendFactor::SRC_ALPHA,
        .dst_color_blend_factor = daxa::BlendFactor::ONE_MINUS_SRC_ALPHA,
        .color_blend_op = daxa::BlendOp::ADD,
        .src_alpha_blend_factor = daxa::BlendFactor::ONE,
        .dst_alpha_blend_factor = daxa::BlendFactor::ONE_MINUS_SRC_ALPHA,
        .alpha_blend_op = daxa::BlendOp::ADD,
    };
}

static bool CreateNativePipeline(
    RaylibDaxaContext &ctx,
    daxa::PrimitiveTopology topology,
    char const *name,
    std::shared_ptr<daxa::RasterPipeline> &outPipeline)
{
    auto result = ctx.pipelineManager.add_raster_pipeline2({
        .vertex_shader_info = daxa::ShaderCompileInfo2{
            .source = daxa::ShaderCode{ GetNativeVertexShader() },
        },
        .fragment_shader_info = daxa::ShaderCompileInfo2{
            .source = daxa::ShaderCode{ GetNativeVertexShader() },
        },
        .color_attachments = {
            daxa::RenderAttachment{
                .format = ctx.swapchain.get_format(),
                .blend = MakeAlphaBlend(),
            },
        },
        .raster = {
            .primitive_topology = topology,
        },
        .push_constant_size = static_cast<daxa::u32>(sizeof(RdaxaPushConstants)),
        .name = name,
    });

    if (result.is_err())
    {
        std::fprintf(stderr, "DAXA: Failed to create %s: %s\n", name, result.message().c_str());
        return false;
    }

    outPipeline = result.value();
    return outPipeline != nullptr;
}

static bool CreateNativePipelines(RaylibDaxaContext &ctx)
{
    ctx.pipelineManager = daxa::PipelineManager({
        .device = ctx.device,
        .root_paths = { DAXA_SHADER_INCLUDE_DIR },
        .default_language = daxa::ShaderLanguage::GLSL,
        .name = "raylib Daxa pipeline manager",
    });

    return CreateNativePipeline(ctx, daxa::PrimitiveTopology::TRIANGLE_LIST, "raylib Daxa triangle pipeline", ctx.trianglePipeline) &&
           CreateNativePipeline(ctx, daxa::PrimitiveTopology::LINE_LIST, "raylib Daxa line pipeline", ctx.linePipeline);
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

static RdaxaGpuVertex ReadBatchVertex(const rdaxa_BatchData &batch, int index)
{
    RdaxaGpuVertex vertex = {};

    vertex.position[0] = batch.vertices[index*3 + 0];
    vertex.position[1] = batch.vertices[index*3 + 1];
    vertex.position[2] = batch.vertices[index*3 + 2];

    if (batch.texcoords != nullptr)
    {
        vertex.texcoord[0] = batch.texcoords[index*2 + 0];
        vertex.texcoord[1] = batch.texcoords[index*2 + 1];
    }

    if (batch.colors != nullptr)
    {
        vertex.color[0] = static_cast<float>(batch.colors[index*4 + 0])/255.0f;
        vertex.color[1] = static_cast<float>(batch.colors[index*4 + 1])/255.0f;
        vertex.color[2] = static_cast<float>(batch.colors[index*4 + 2])/255.0f;
        vertex.color[3] = static_cast<float>(batch.colors[index*4 + 3])/255.0f;
    }
    else
    {
        vertex.color[0] = 1.0f;
        vertex.color[1] = 1.0f;
        vertex.color[2] = 1.0f;
        vertex.color[3] = 1.0f;
    }

    return vertex;
}

static bool BuildNativeVertices(RaylibDaxaContext &ctx, const rdaxa_BatchData &batch)
{
    ctx.nativeVertices.clear();
    ctx.nativeDraws.clear();

    if (batch.vertices == nullptr || batch.draws == nullptr || batch.vertexCounter <= 0 || batch.drawCounter <= 0)
    {
        return false;
    }

    for (int drawIndex = 0, vertexOffset = 0; drawIndex < batch.drawCounter; drawIndex++)
    {
        rdaxa_DrawCall const &draw = batch.draws[drawIndex];
        if (draw.vertexCount <= 0)
        {
            vertexOffset += draw.vertexAlignment;
            continue;
        }

        RdaxaNativeDraw nativeDraw = {
            .mode = draw.mode,
            .firstVertex = ctx.nativeVertices.size(),
            .vertexCount = 0,
        };

        if (draw.mode == RDAXA_RL_LINES)
        {
            int const count = draw.vertexCount - (draw.vertexCount%2);
            for (int i = 0; i < count; i++) ctx.nativeVertices.push_back(ReadBatchVertex(batch, vertexOffset + i));
        }
        else if (draw.mode == RDAXA_RL_TRIANGLES)
        {
            int const count = draw.vertexCount - (draw.vertexCount%3);
            for (int i = 0; i < count; i++) ctx.nativeVertices.push_back(ReadBatchVertex(batch, vertexOffset + i));
        }
        else if (draw.mode == RDAXA_RL_QUADS)
        {
            int const quadCount = draw.vertexCount/4;
            for (int quad = 0; quad < quadCount; quad++)
            {
                int const base = vertexOffset + quad*4;
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 0));
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 1));
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 2));
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 0));
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 2));
                ctx.nativeVertices.push_back(ReadBatchVertex(batch, base + 3));
            }
        }

        nativeDraw.vertexCount = ctx.nativeVertices.size() - nativeDraw.firstVertex;
        if (nativeDraw.vertexCount > 0)
        {
            ctx.nativeDraws.push_back(nativeDraw);
        }

        vertexOffset += draw.vertexCount + draw.vertexAlignment;
    }

    return !ctx.nativeVertices.empty();
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
            .present_mode = daxa::PresentMode::IMMEDIATE,
            .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::COLOR_ATTACHMENT,
            .name = "raylib Daxa swapchain",
        });

        return CreateNativePipelines(ctx);
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
        g_daxa->trianglePipeline.reset();
        g_daxa->linePipeline.reset();
        g_daxa->pipelineManager = {};
        DestroyNativeVertexBuffer(*g_daxa);
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

void rdaxaSetClearColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
    if (!g_daxa)
    {
        return;
    }

    g_daxa->clearColor[0] = static_cast<float>(r)/255.0f;
    g_daxa->clearColor[1] = static_cast<float>(g)/255.0f;
    g_daxa->clearColor[2] = static_cast<float>(b)/255.0f;
    g_daxa->clearColor[3] = static_cast<float>(a)/255.0f;
}

unsigned int rdaxaGetPresentedFrameCount(void)
{
    if (!g_daxa)
    {
        return 0;
    }

    return g_daxa->presentedFrameCount;
}

bool rdaxaDrawBatch(const rdaxa_BatchData *batch)
{
    if (!g_daxa || batch == nullptr || batch->mvp == nullptr)
    {
        return false;
    }

    RaylibDaxaContext &ctx = *g_daxa;

    try
    {
        bool const hasNativeDraws = BuildNativeVertices(ctx, *batch);

        if (hasNativeDraws && !EnsureNativeVertexBuffer(ctx, ctx.nativeVertices.size()))
        {
            return false;
        }

        if (hasNativeDraws)
        {
            std::memcpy(ctx.nativeVertexPtr, ctx.nativeVertices.data(), ctx.nativeVertices.size()*sizeof(RdaxaGpuVertex));
        }

        daxa::ImageId swapchainImage = ctx.swapchain.acquire_next_image();
        if (swapchainImage.is_empty())
        {
            return false;
        }

        daxa::ImageInfo swapchainImageInfo = ctx.device.image_info(swapchainImage).value();
        daxa::CommandRecorder recorder = ctx.device.create_command_recorder({
            .name = "raylib Daxa native draw recorder",
        });

        recorder.pipeline_image_barrier({
            .dst_access = daxa::AccessConsts::COLOR_ATTACHMENT_OUTPUT_READ_WRITE,
            .image = swapchainImage,
            .layout_operation = daxa::ImageLayoutOperation::TO_GENERAL,
        });

        daxa::RenderCommandRecorder renderRecorder = std::move(recorder).begin_renderpass({
            .color_attachments = std::array{
                daxa::RenderAttachmentInfo{
                    .image_view = swapchainImage.default_view(),
                    .load_op = daxa::AttachmentLoadOp::CLEAR,
                    .clear_value = std::array<daxa::f32, 4>{
                        ctx.clearColor[0],
                        ctx.clearColor[1],
                        ctx.clearColor[2],
                        ctx.clearColor[3],
                    },
                },
            },
            .render_area = {
                .width = swapchainImageInfo.size.x,
                .height = swapchainImageInfo.size.y,
            },
        });

        renderRecorder.set_viewport({
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(swapchainImageInfo.size.x),
            .height = static_cast<float>(swapchainImageInfo.size.y),
            .min_depth = 0.0f,
            .max_depth = 1.0f,
        });
        renderRecorder.set_scissor({
            .x = 0,
            .y = 0,
            .width = swapchainImageInfo.size.x,
            .height = swapchainImageInfo.size.y,
        });

        RdaxaPushConstants push = {};
        std::memcpy(push.mvp, batch->mvp, sizeof(push.mvp));
        if (hasNativeDraws)
        {
            push.vertices = ctx.device.device_address(ctx.nativeVertexBuffer).value();
        }

        for (RdaxaNativeDraw const &draw : ctx.nativeDraws)
        {
            if (draw.mode == RDAXA_RL_LINES)
            {
                renderRecorder.set_pipeline(*ctx.linePipeline);
            }
            else
            {
                renderRecorder.set_pipeline(*ctx.trianglePipeline);
            }

            renderRecorder.push_constant(push);
            renderRecorder.draw({
                .vertex_count = static_cast<daxa::u32>(draw.vertexCount),
                .first_vertex = static_cast<daxa::u32>(draw.firstVertex),
            });
        }

        recorder = std::move(renderRecorder).end_renderpass();

        recorder.pipeline_image_barrier({
            .src_access = daxa::AccessConsts::COLOR_ATTACHMENT_OUTPUT_READ_WRITE,
            .image = swapchainImage,
            .layout_operation = daxa::ImageLayoutOperation::TO_PRESENT_SRC,
        });

        daxa::ExecutableCommandList commands = recorder.complete_current_commands();
        ctx.device.submit_commands(daxa::CommandSubmitInfo{
            .command_lists = std::array{ commands },
            .wait_binary_semaphores = std::array{ ctx.swapchain.current_acquire_semaphore() },
            .signal_binary_semaphores = std::array{ ctx.swapchain.current_present_semaphore() },
            .signal_timeline_semaphores = std::array{ ctx.swapchain.current_timeline_pair() },
        });
        ctx.device.present_frame({
            .wait_binary_semaphores = std::array{ ctx.swapchain.current_present_semaphore() },
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
