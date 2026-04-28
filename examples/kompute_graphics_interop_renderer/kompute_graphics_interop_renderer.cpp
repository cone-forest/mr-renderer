#include "pch.hpp"
#include "kompute_graphics_interop_renderer.hpp"

#include <libassert/assert.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>
#include <mr-importer/compiler.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

#ifndef MR_RENDERER_LIB_SHADER_DIR
#define MR_RENDERER_LIB_SHADER_DIR "shaders"
#endif

namespace mr {
  namespace {
    template <typename T>
    T vk_expect(vk::ResultValue<T> rv, const char* message)
    {
      ASSERT(rv.result == vk::Result::eSuccess, message, static_cast<int>(rv.result));
      return rv.value;
    }

    struct Vertex {
      float position[2];
      float color[3];
    };

    struct DrawIndexedCommandRaw {
      uint32_t index_count;
      uint32_t instance_count;
      uint32_t first_index;
      int32_t vertex_offset;
      uint32_t first_instance;
    };

    std::vector<uint32_t> spirv_words(const mr::importer::Shader& shader)
    {
      MR_TRACY_ZONE;
      const size_t nbytes = shader.spirv.size();
      ASSERT(nbytes % 4 == 0, "SPIR-V size is not a multiple of 4");
      std::vector<uint32_t> words(nbytes / 4);
      std::memcpy(words.data(), shader.spirv.get(), nbytes);
      return words;
    }

    const mr::importer::Shader* pick_stage(
      const std::vector<mr::importer::Shader>& shaders,
      vk::ShaderStageFlagBits stage)
    {
      MR_TRACY_ZONE;
      const auto it = std::ranges::find_if(shaders, [stage](const mr::importer::Shader& shader) -> bool {
        return shader.stage == stage;
      });
      if (it == shaders.end()) {
        return nullptr;
      }
      return &(*it);
    }

    vk::ShaderModule create_shader_module(const vk::Device device, const std::vector<uint32_t>& spirv)
    {
      MR_TRACY_ZONE;
      vk::ShaderModuleCreateInfo shader_info{};
      shader_info.codeSize = spirv.size() * sizeof(uint32_t);
      shader_info.pCode = spirv.data();
      return vk_expect(device.createShaderModule(shader_info), "vk::createShaderModule failed");
    }
  } // namespace

  struct KomputeGraphicsInteropRenderer::Impl {
    uint32_t width = 1;
    uint32_t height = 1;
    std::unique_ptr<VulkanContext> vulkan_context{};

    vkb::Instance instance{};
    vkb::PhysicalDevice physical_device{};
    vkb::Device device{};

    uint32_t graphics_queue_family = 0;
    uint32_t kompute_queue_family = 0;
    vk::Queue graphics_queue{};
    vk::Queue kompute_queue{};
    VulkanFeatureSupport feature_support{};

    std::shared_ptr<vk::Instance> kp_instance{};
    std::shared_ptr<vk::PhysicalDevice> kp_physical_device{};
    std::shared_ptr<vk::Device> kp_device{};
    std::shared_ptr<vk::Queue> kp_kompute_queue{};

    std::shared_ptr<kp::TensorT<uint32_t>> source_indirect_tensor{};
    std::shared_ptr<kp::TensorT<uint32_t>> destination_indirect_tensor{};
    std::shared_ptr<kp::Algorithm> compute_algorithm{};
    std::shared_ptr<kp::Sequence> compute_sequence{};
    std::unique_ptr<FrameRecorder> frame_recorder{};

    vk::Buffer indirect_destination_buffer{};
    DeviceBuffer indirect_draw_buffer{};
    VertexBuffer vertex_buffer{};
    IndexBuffer index_buffer{};
    std::optional<ColorAttachmentImage> color_image{};

    GraphicsPipeline graphics_pipeline{};

    ~Impl() { shutdown(); }

    void initialize(uint32_t requested_width, uint32_t requested_height)
    {
      MR_TRACY_ZONE_N("KomputeInterop::initialize");
      width = requested_width == 0 ? 1u : requested_width;
      height = requested_height == 0 ? 1u : requested_height;

      auto context_result = create_vulkan_context(VulkanContextCreateInfo{
        .app_name = "mr-renderer",
        .headless = true,
        .require_present = false,
        .surface = VK_NULL_HANDLE,
        .prefer_dedicated_compute_queue = true,
      });
      ASSERT(context_result.has_value(), "vulkan context creation failed", context_result.error());
      vulkan_context = std::make_unique<VulkanContext>(std::move(*context_result));

      instance = vulkan_context->instance;
      physical_device = vulkan_context->physical_device;
      device = vulkan_context->device;
      graphics_queue_family = vulkan_context->graphics_queue_family;
      kompute_queue_family = vulkan_context->compute_queue_family;
      graphics_queue = vulkan_context->graphics_queue;
      kompute_queue = vulkan_context->compute_queue;
      feature_support = vulkan_context->feature_support;
      ASSERT(feature_support.dynamic_rendering, "dynamic rendering is required for this example");

      ASSERT(static_cast<bool>(graphics_queue), "failed to acquire graphics queue handle");
      ASSERT(static_cast<bool>(kompute_queue), "failed to acquire compute queue handle");

      const auto queue_props = physical_device.get_queue_families();
      const vk::QueueFlags graphics_flags{queue_props[graphics_queue_family].queueFlags};
      const vk::QueueFlags kompute_flags{queue_props[kompute_queue_family].queueFlags};
      ASSERT((graphics_flags & vk::QueueFlagBits::eGraphics) != vk::QueueFlags{}, "graphics queue must support graphics");
      ASSERT((graphics_flags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{}, "graphics queue must support compute");
      ASSERT((graphics_flags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{}, "graphics queue must support transfer");
      ASSERT((kompute_flags & vk::QueueFlagBits::eCompute) != vk::QueueFlags{}, "kompute queue must support compute");
      ASSERT((kompute_flags & vk::QueueFlagBits::eTransfer) != vk::QueueFlags{}, "kompute queue must support transfer");
      ASSERT(
        kompute_queue_family == graphics_queue_family ||
          (kompute_flags & vk::QueueFlagBits::eGraphics) == vk::QueueFlags{},
        "kompute queue should be compute/transfer-only when a dedicated family is available");

      kp_instance = std::shared_ptr<vk::Instance>(new vk::Instance(instance.instance), [](vk::Instance*) {});
      kp_physical_device = std::shared_ptr<vk::PhysicalDevice>(
        new vk::PhysicalDevice(physical_device.physical_device),
        [](vk::PhysicalDevice*) {});
      kp_device = std::shared_ptr<vk::Device>(new vk::Device(device.device), [](vk::Device*) {});
      kp_kompute_queue =
        std::shared_ptr<vk::Queue>(new vk::Queue(kompute_queue), [](vk::Queue*) {});

      const std::filesystem::path shader_dir = std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR);

      frame_recorder = std::make_unique<FrameRecorder>(*vulkan_context);
      create_static_mesh_buffers();
      create_render_target();
      create_graphics_pipeline(shader_dir / "interop_raster.slang");
      initialize_kompute(shader_dir / "interop_indirect_copy.slang");
    }

    void create_static_mesh_buffers()
    {
      MR_TRACY_ZONE_N("KomputeInterop::create_static_mesh_buffers");
      const std::array<Vertex, 3> vertices = {{
        {{-0.7f, -0.7f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.7f}, {0.0f, 1.0f, 0.0f}},
        {{0.7f, -0.7f}, {0.0f, 0.0f, 1.0f}},
      }};
      const std::array<uint32_t, 3> indices = {{0u, 1u, 2u}};
      vertex_buffer = VertexBuffer(*vulkan_context, sizeof(vertices));
      index_buffer = IndexBuffer(*vulkan_context, sizeof(indices), vk::IndexType::eUint32);
      index_buffer.set_element_count(indices.size());

      vk::CommandPoolCreateInfo upload_pool_info{};
      upload_pool_info.queueFamilyIndex = graphics_queue_family;
      upload_pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
      auto upload_pool =
        vk_expect(vk::Device(device.device).createCommandPool(upload_pool_info), "vk::createCommandPool(upload) failed");

      vk::CommandBufferAllocateInfo upload_cmd_alloc{};
      upload_cmd_alloc.commandPool = upload_pool;
      upload_cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
      upload_cmd_alloc.commandBufferCount = 1;
      auto upload_cmd = vk_expect(vk::Device(device.device).allocateCommandBuffers(upload_cmd_alloc),
        "vk::allocateCommandBuffers(upload) failed")[0];

      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(upload_cmd.begin(begin_info) == vk::Result::eSuccess, "vk::CommandBuffer::begin(upload) failed");
      HostBuffer vertex_staging(
        *vulkan_context,
        sizeof(vertices),
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
      HostBuffer index_staging(
        *vulkan_context,
        sizeof(indices),
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
      vertex_staging.write(std::as_bytes(std::span{vertices}));
      index_staging.write(std::as_bytes(std::span{indices}));

      vk::BufferCopy vertex_copy{};
      vertex_copy.srcOffset = 0;
      vertex_copy.dstOffset = 0;
      vertex_copy.size = sizeof(vertices);
      upload_cmd.copyBuffer(vertex_staging.buffer(), vertex_buffer.buffer(), vertex_copy);

      vk::BufferCopy index_copy{};
      index_copy.srcOffset = 0;
      index_copy.dstOffset = 0;
      index_copy.size = sizeof(indices);
      upload_cmd.copyBuffer(index_staging.buffer(), index_buffer.buffer(), index_copy);
      ASSERT(upload_cmd.end() == vk::Result::eSuccess, "vk::CommandBuffer::end(upload) failed");

      vk::SubmitInfo upload_submit{};
      upload_submit.commandBufferCount = 1;
      upload_submit.pCommandBuffers = &upload_cmd;
      ASSERT(graphics_queue.submit(upload_submit, vk::Fence{}) == vk::Result::eSuccess, "vk::Queue::submit(upload) failed");
      ASSERT(graphics_queue.waitIdle() == vk::Result::eSuccess, "vk::Queue::waitIdle(upload) failed");
      vk::Device(device.device).freeCommandBuffers(upload_pool, upload_cmd);
      vk::Device(device.device).destroyCommandPool(upload_pool);
    }

    void create_render_target()
    {
      MR_TRACY_ZONE;
      color_image.emplace(
        *vulkan_context,
        vk::Extent3D{width, height, 1u},
        vk::Format::eR8G8B8A8Unorm);
    }

    void create_graphics_pipeline(const std::filesystem::path& shader_path)
    {
      MR_TRACY_ZONE_N("KomputeInterop::create_graphics_pipeline");
      const auto compiled = mr::importer::compile(shader_path);
      ASSERT(compiled, "mr::importer::compile failed for interop_raster.slang");

      const mr::importer::Shader* vs = pick_stage(*compiled, vk::ShaderStageFlagBits::eVertex);
      const mr::importer::Shader* fs = pick_stage(*compiled, vk::ShaderStageFlagBits::eFragment);
      ASSERT(vs != nullptr, "missing vertex shader stage for interop_raster.slang");
      ASSERT(fs != nullptr, "missing fragment shader stage for interop_raster.slang");

      const std::vector<uint32_t> vs_spirv = spirv_words(*vs);
      const std::vector<uint32_t> fs_spirv = spirv_words(*fs);
      vk::ShaderModule vs_module = create_shader_module(vk::Device(device.device), vs_spirv);
      vk::ShaderModule fs_module = create_shader_module(vk::Device(device.device), fs_spirv);

      vk::VertexInputBindingDescription binding_desc{};
      binding_desc.binding = 0;
      binding_desc.stride = sizeof(Vertex);
      binding_desc.inputRate = vk::VertexInputRate::eVertex;

      std::array<vk::VertexInputAttributeDescription, 2> attributes{};
      attributes[0].location = 0;
      attributes[0].binding = 0;
      attributes[0].format = vk::Format::eR32G32Sfloat;
      attributes[0].offset = offsetof(Vertex, position);
      attributes[1].location = 1;
      attributes[1].binding = 0;
      attributes[1].format = vk::Format::eR32G32B32Sfloat;
      attributes[1].offset = offsetof(Vertex, color);

      vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
      input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

      vk::PipelineRasterizationStateCreateInfo rasterizer{};
      rasterizer.polygonMode = vk::PolygonMode::eFill;
      rasterizer.cullMode = vk::CullModeFlagBits::eNone;
      rasterizer.frontFace = vk::FrontFace::eCounterClockwise;
      rasterizer.lineWidth = 1.0f;

      vk::PipelineMultisampleStateCreateInfo multisampling{};
      multisampling.rasterizationSamples = vk::SampleCountFlagBits::e1;

      vk::PipelineColorBlendAttachmentState color_blend_attachment{};
      color_blend_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
      const std::array<GraphicsShaderStageDesc, 2> stage_descs{{
        {.stage = vk::ShaderStageFlagBits::eVertex, .module = vs_module, .entry_point = "main"},
        {.stage = vk::ShaderStageFlagBits::eFragment, .module = fs_module, .entry_point = "main"},
      }};
      const std::array<vk::VertexInputBindingDescription, 1> binding_descs{{binding_desc}};
      const std::array<vk::Format, 1> color_formats{{vk::Format::eR8G8B8A8Unorm}};
      const std::array<vk::PipelineColorBlendAttachmentState, 1> blend_descs{{color_blend_attachment}};

      auto pipeline_result =
        GraphicsPipelineBuilder(*vulkan_context)
          .set_shader_stages(stage_descs)
          .set_bindings(binding_descs)
          .set_attributes(attributes)
          .set_color_attachment_formats(color_formats)
          .set_color_blend_attachments(blend_descs)
          .set_topology(input_assembly.topology)
          .set_polygon_mode(rasterizer.polygonMode)
          .set_cull_mode(rasterizer.cullMode)
          .set_front_face(rasterizer.frontFace)
          .set_line_width(rasterizer.lineWidth)
          .set_samples(multisampling.rasterizationSamples)
          .build();
      ASSERT(pipeline_result.has_value(), "build_graphics_pipeline failed", pipeline_result.error());
      graphics_pipeline = std::move(*pipeline_result);

      vk::Device(device.device).destroyShaderModule(fs_module);
      vk::Device(device.device).destroyShaderModule(vs_module);
    }

    void initialize_kompute(const std::filesystem::path& shader_path)
    {
      MR_TRACY_ZONE_N("KomputeInterop::initialize_kompute");
      const auto compiled = mr::importer::compile(shader_path);
      ASSERT(compiled, "mr::importer::compile failed for interop_indirect_copy.slang");
      const mr::importer::Shader* cs = pick_stage(*compiled, vk::ShaderStageFlagBits::eCompute);
      ASSERT(cs != nullptr, "missing compute shader stage for interop_indirect_copy.slang");
      const std::vector<uint32_t> cs_spirv = spirv_words(*cs);

      const DrawIndexedCommandRaw cmd{
        .index_count = 3u,
        .instance_count = 1u,
        .first_index = 0u,
        .vertex_offset = 0,
        .first_instance = 0u,
      };
      std::array<uint32_t, 5> source_words{};
      std::memcpy(source_words.data(), &cmd, sizeof(cmd));
      std::array<uint32_t, 5> destination_words{};

      source_indirect_tensor = std::make_shared<kp::TensorT<uint32_t>>(
        kp_physical_device,
        kp_device,
        std::vector<uint32_t>(source_words.begin(), source_words.end()),
        kp::Memory::MemoryTypes::eDevice);
      destination_indirect_tensor = std::make_shared<kp::TensorT<uint32_t>>(
        kp_physical_device,
        kp_device,
        std::vector<uint32_t>(destination_words.begin(), destination_words.end()),
        kp::Memory::MemoryTypes::eDevice);

      std::vector<std::shared_ptr<kp::Memory>> mem_objects = {
        source_indirect_tensor,
        destination_indirect_tensor,
      };
      compute_algorithm = std::make_shared<kp::Algorithm>(
        kp_device,
        mem_objects,
        cs_spirv,
        kp::Workgroup{1u, 1u, 1u},
        std::vector<float>{},
        std::vector<float>{});

      compute_sequence = std::make_shared<kp::Sequence>(
        kp_physical_device,
        kp_device,
        kp_kompute_queue,
        kompute_queue_family,
        0);
      compute_sequence
        ->record<kp::OpSyncDevice>(mem_objects)
        ->record<kp::OpAlgoDispatch>(compute_algorithm);
      indirect_destination_buffer = *destination_indirect_tensor->getPrimaryBuffer();
      ASSERT(static_cast<bool>(indirect_destination_buffer), "Kompute destination indirect buffer is null");
      indirect_draw_buffer = DeviceBuffer(
        *vulkan_context,
        sizeof(DrawIndexedCommandRaw),
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    }

    void record_graphics_command_buffer(vk::CommandBuffer command_buffer)
    {
      MR_TRACY_ZONE_N("KomputeInterop::record_graphics_command_buffer");
      ASSERT(color_image.has_value(), "color attachment image is not initialized");
      color_image->transition_layout(command_buffer, vk::ImageLayout::eColorAttachmentOptimal);

      vk::BufferMemoryBarrier indirect_barrier{};
      vk::BufferCopy indirect_copy{};
      indirect_copy.srcOffset = 0;
      indirect_copy.dstOffset = 0;
      indirect_copy.size = sizeof(DrawIndexedCommandRaw);
      command_buffer.copyBuffer(indirect_destination_buffer, indirect_draw_buffer.buffer(), indirect_copy);
      indirect_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      indirect_barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
      indirect_barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      indirect_barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      indirect_barrier.buffer = indirect_draw_buffer.buffer();
      indirect_barrier.offset = 0;
      indirect_barrier.size = sizeof(DrawIndexedCommandRaw);
      command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eDrawIndirect,
        vk::DependencyFlags{},
        {},
        indirect_barrier,
        {});

      vk::RenderingAttachmentInfo color_attachment_info = color_image->attachment_info();
      color_attachment_info.clearValue.color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.05f, 0.1f, 1.0f});
      vk::RenderingInfo rendering_info{};
      rendering_info.renderArea.offset = vk::Offset2D{0, 0};
      rendering_info.renderArea.extent = vk::Extent2D{width, height};
      rendering_info.layerCount = 1;
      rendering_info.colorAttachmentCount = 1;
      rendering_info.pColorAttachments = &color_attachment_info;
      command_buffer.beginRendering(rendering_info);
      vk::Viewport viewport{};
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = static_cast<float>(width);
      viewport.height = static_cast<float>(height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      command_buffer.setViewport(0, viewport);
      vk::Rect2D scissor{};
      scissor.offset = vk::Offset2D{0, 0};
      scissor.extent = vk::Extent2D{width, height};
      command_buffer.setScissor(0, scissor);
      graphics_pipeline.bind(command_buffer);

      vk::DeviceSize vb_offset = 0;
      command_buffer.bindVertexBuffers(0, vertex_buffer.buffer(), vb_offset);
      command_buffer.bindIndexBuffer(index_buffer.buffer(), 0, index_buffer.index_type());
      command_buffer.drawIndexedIndirect(
        indirect_draw_buffer.buffer(),
        0,
        1,
        sizeof(vk::DrawIndexedIndirectCommand));
      command_buffer.endRendering();

      color_image->transition_layout(command_buffer, vk::ImageLayout::eTransferSrcOptimal);
    }

    Frame render_frame(uint32_t frame_index)
    {
      MR_TRACY_ZONE_N("KomputeInterop::render_frame");
      MR_TRACY_FRAME("kompute_interop_frame");
      if (compute_sequence->isRunning()) {
        compute_sequence->evalAwait();
      }
      compute_sequence->eval();

      ASSERT(frame_recorder != nullptr, "frame recorder is not initialized");
      auto begin_frame_result = frame_recorder->begin_frame(frame_index);
      ASSERT(begin_frame_result.has_value(), "FrameRecorder::begin_frame failed", begin_frame_result.error());

      auto record_result = frame_recorder->begin_recording(QueueTarget::Graphics);
      ASSERT(record_result.has_value(), "FrameRecorder::begin_recording failed", record_result.error());
      auto recorded = *record_result;
      frame_recorder->declare_buffer_usage(recorded, FrameRecorder::BufferUsageDesc{
        .buffer = indirect_draw_buffer.buffer(),
        .offset = 0,
        .size = sizeof(DrawIndexedCommandRaw),
        .stage = vk::PipelineStageFlagBits2::eDrawIndirect,
        .access = vk::AccessFlagBits2::eIndirectCommandRead,
        .writes = false,
      });
      ASSERT(color_image.has_value(), "color attachment image is not initialized");
      vk::ImageSubresourceRange color_range{};
      color_range.aspectMask = vk::ImageAspectFlagBits::eColor;
      color_range.baseMipLevel = 0;
      color_range.levelCount = 1;
      color_range.baseArrayLayer = 0;
      color_range.layerCount = 1;
      frame_recorder->declare_image_usage(recorded, FrameRecorder::ImageUsageDesc{
        .image = color_image->image(),
        .subresource_range = color_range,
        .layout = vk::ImageLayout::eColorAttachmentOptimal,
        .stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .access = vk::AccessFlagBits2::eColorAttachmentWrite,
        .writes = true,
      });

      record_graphics_command_buffer(recorded.handle);
      auto end_result = frame_recorder->end_recording(recorded);
      ASSERT(end_result.has_value(), "FrameRecorder::end_recording failed", end_result.error());
      frame_recorder->enqueue_for_submit(recorded);

      auto submit_result = frame_recorder->submit_frame();
      ASSERT(submit_result.has_value(), "FrameRecorder::submit_frame failed", submit_result.error());
      const uint64_t completion_value = *submit_result;
      if (completion_value > 0) {
        vk::SemaphoreWaitInfo wait_info{};
        const vk::Semaphore timeline = frame_recorder->timeline_semaphore();
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline;
        wait_info.pValues = &completion_value;
        ASSERT(vk::Device(device.device).waitSemaphores(wait_info, UINT64_MAX) == vk::Result::eSuccess,
          "waitSemaphores(frame recorder completion) failed");
      }
      ASSERT(color_image.has_value(), "color attachment image is not initialized");
      GpuFrame gpu_frame{};
      gpu_frame.context = vulkan_context.get();
      gpu_frame.width = width;
      gpu_frame.height = height;
      gpu_frame.image = color_image->image();
      gpu_frame.layout = vk::ImageLayout::eTransferSrcOptimal;
      gpu_frame.format = vk::Format::eR8G8B8A8Unorm;
      return Frame{frame_index, std::move(gpu_frame)};
    }

    void shutdown()
    {
      MR_TRACY_ZONE_N("KomputeInterop::shutdown");
      if (vulkan_context && device.device != VK_NULL_HANDLE) {
        ASSERT(vk::Device(device.device).waitIdle() == vk::Result::eSuccess, "vk::Device::waitIdle failed");
      }
      if (compute_sequence && compute_sequence->isRunning()) {
        compute_sequence->evalAwait();
      }

      compute_sequence.reset();
      compute_algorithm.reset();
      destination_indirect_tensor.reset();
      source_indirect_tensor.reset();
      kp_kompute_queue.reset();
      kp_device.reset();
      kp_physical_device.reset();
      kp_instance.reset();
      frame_recorder.reset();

      graphics_pipeline = {};
      color_image.reset();
      index_buffer = {};
      vertex_buffer = {};
      indirect_draw_buffer = {};
      indirect_destination_buffer = nullptr;

      device = {};
      physical_device = {};
      instance = {};
      vulkan_context.reset();
    }
  };

  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>())
  {
    MR_TRACY_ZONE;
    impl_->initialize(width, height);
  }

  KomputeGraphicsInteropRenderer::~KomputeGraphicsInteropRenderer() = default;
  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(KomputeGraphicsInteropRenderer&&) noexcept =
    default;
  KomputeGraphicsInteropRenderer&
  KomputeGraphicsInteropRenderer::operator=(KomputeGraphicsInteropRenderer&&) noexcept = default;

  coro::generator<Frame> KomputeGraphicsInteropRenderer::frames()
  {
    MR_TRACY_ZONE_N("KomputeInterop::frames");
    uint32_t index = 0;
    while (true) {
      co_yield impl_->render_frame(index++);
    }
  }
} // namespace mr
