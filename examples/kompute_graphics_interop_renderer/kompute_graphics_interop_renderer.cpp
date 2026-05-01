#include "pch.hpp"
#include "kompute_graphics_interop_renderer.hpp"

#include <libassert/assert.hpp>
#include <mr-renderer/target.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>
#include <mr-importer/compiler.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <exception>
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
      std::array<float, 2> position{};
      std::array<float, 3> color{};
    };

    struct DrawIndexedCommandRaw {
      uint32_t index_count;
      uint32_t instance_count;
      uint32_t first_index;
      int32_t vertex_offset;
      uint32_t first_instance;
    };

    uint32_t queue_family_for_upload(const VulkanContext& context)
    {
      if (context.graphics_queue_family != std::numeric_limits<uint32_t>::max()) {
        return context.graphics_queue_family;
      }
      return context.compute_queue_family;
    }

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
      vk::ShaderModuleCreateInfo shader_info{};
      shader_info.codeSize = spirv.size() * sizeof(uint32_t);
      shader_info.pCode = spirv.data();
      return vk_expect(device.createShaderModule(shader_info), "vk::createShaderModule failed");
    }

    void unpack_rgba8_bytes_to_float_raster(
      std::span<const std::byte> bytes,
      uint32_t width,
      uint32_t height,
      RgbaFloatRasterView out_pixels)
    {
      const auto byte_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
      ASSERT(bytes.size() >= byte_size, "readback byte size too small");
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const size_t p =
            ((static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)) * 4u;
          out_pixels[static_cast<std::size_t>(y), static_cast<std::size_t>(x), 0zu] =
            static_cast<float>(std::to_integer<uint8_t>(bytes.at(p + 0u))) / 255.0f;
          out_pixels[static_cast<std::size_t>(y), static_cast<std::size_t>(x), 1zu] =
            static_cast<float>(std::to_integer<uint8_t>(bytes.at(p + 1u))) / 255.0f;
          out_pixels[static_cast<std::size_t>(y), static_cast<std::size_t>(x), 2zu] =
            static_cast<float>(std::to_integer<uint8_t>(bytes.at(p + 2u))) / 255.0f;
          out_pixels[static_cast<std::size_t>(y), static_cast<std::size_t>(x), 3zu] =
            static_cast<float>(std::to_integer<uint8_t>(bytes.at(p + 3u))) / 255.0f;
        }
      }
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    void readback_rgba8_unorm_image_to_raster(
      const VulkanContext& context,
      vk::Image image,
      vk::ImageLayout layout,
      uint32_t width,
      uint32_t height,
      RgbaFloatRasterView out_pixels)
    {
      MR_TRACY_ZONE;
      ASSERT(out_pixels.extent(0) == static_cast<std::size_t>(height), "raster height mismatch");
      ASSERT(out_pixels.extent(1) == static_cast<std::size_t>(width), "raster width mismatch");
      ASSERT(out_pixels.extent(2) == 4zu, "raster channels mismatch");

      const vk::DeviceSize byte_size =
        static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4u;

      HostBuffer readback(
        context,
        byte_size,
        vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

      vk::CommandPoolCreateInfo pool_info{};
      pool_info.queueFamilyIndex = queue_family_for_upload(context);
      pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
      auto pool_rv = context.vk_device().createCommandPool(pool_info);
      ASSERT(pool_rv.result == vk::Result::eSuccess, "readback createCommandPool failed");
      vk::CommandPool pool = pool_rv.value;

      vk::CommandBufferAllocateInfo alloc_info{};
      alloc_info.commandPool = pool;
      alloc_info.level = vk::CommandBufferLevel::ePrimary;
      alloc_info.commandBufferCount = 1;
      auto cmd_rv = context.vk_device().allocateCommandBuffers(alloc_info);
      ASSERT(cmd_rv.result == vk::Result::eSuccess, "readback allocateCommandBuffers failed");
      vk::CommandBuffer cmd = cmd_rv.value.at(0);

      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(cmd.begin(begin_info) == vk::Result::eSuccess, "readback begin failed");

      vk::ImageMemoryBarrier to_transfer_src{};
      to_transfer_src.oldLayout = layout;
      to_transfer_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      to_transfer_src.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.image = image;
      to_transfer_src.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_transfer_src.subresourceRange.baseMipLevel = 0;
      to_transfer_src.subresourceRange.levelCount = 1;
      to_transfer_src.subresourceRange.baseArrayLayer = 0;
      to_transfer_src.subresourceRange.layerCount = 1;
      to_transfer_src.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
      to_transfer_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},
        {},
        {},
        to_transfer_src);

      vk::BufferImageCopy copy_region{};
      copy_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy_region.imageSubresource.mipLevel = 0;
      copy_region.imageSubresource.baseArrayLayer = 0;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageExtent = vk::Extent3D{.width = width, .height = height, .depth = 1u};
      cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, readback.buffer(), copy_region);

      if (layout != vk::ImageLayout::eTransferSrcOptimal) {
        vk::ImageMemoryBarrier restore_layout{};
        restore_layout.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        restore_layout.newLayout = layout;
        restore_layout.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_layout.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_layout.image = image;
        restore_layout.subresourceRange = to_transfer_src.subresourceRange;
        restore_layout.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        restore_layout.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
        cmd.pipelineBarrier(
          vk::PipelineStageFlagBits::eTransfer,
          vk::PipelineStageFlagBits::eAllCommands,
          vk::DependencyFlags{},
          {},
          {},
          restore_layout);
      }

      ASSERT(cmd.end() == vk::Result::eSuccess, "readback end failed");

      vk::SubmitInfo submit_info{};
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      ASSERT(
        context.graphics_queue.submit(submit_info, vk::Fence{}) == vk::Result::eSuccess,
        "readback queue submit failed");
      ASSERT(context.graphics_queue.waitIdle() == vk::Result::eSuccess, "readback waitIdle failed");

      context.vk_device().freeCommandBuffers(pool, cmd);
      context.vk_device().destroyCommandPool(pool);

      unpack_rgba8_bytes_to_float_raster(readback.read(), width, height, out_pixels);
    }
  } // namespace

  struct KomputeGraphicsInteropRenderer::Impl {
    uint32_t width = 1;
    uint32_t height = 1;

    std::unique_ptr<VulkanContext> owned_vulkan_context{};
    VulkanContext* active_context = nullptr;

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
    vk::Format graphics_pipeline_format = vk::Format::eUndefined;

    vk::CommandPool gfx_transient_pool{};

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;
    ~Impl() noexcept
    {
      try {
        shutdown();
      } catch (...) {
        std::terminate();
      }
    }

    [[nodiscard]] const VulkanContext& vulkan_ctx() const
    {
      ASSERT(active_context != nullptr, "no Vulkan context");
      return *active_context;
    }

    [[nodiscard]] VulkanContext& vulkan_ctx_mut()
    {
      ASSERT(active_context != nullptr, "no Vulkan context");
      return *active_context;
    }

    void ensure_dimensions(uint32_t w, uint32_t h)
    {
      width = w == 0 ? 1u : w;
      height = h == 0 ? 1u : h;
    }

    void wire_context_pointers()
    {
      const VulkanContext& ctx = vulkan_ctx();
      instance = ctx.instance;
      physical_device = ctx.physical_device;
      device = ctx.device;
      graphics_queue_family = ctx.graphics_queue_family;
      kompute_queue_family = ctx.compute_queue_family;
      graphics_queue = ctx.graphics_queue;
      kompute_queue = ctx.compute_queue;
      feature_support = ctx.feature_support;
    }

    void create_static_mesh_buffers()
    {
      MR_TRACY_ZONE_N("KomputeInterop::create_static_mesh_buffers");
      VulkanContext& ctx = vulkan_ctx_mut();
      const std::array<Vertex, 3> vertices = {{
        {.position = {-0.7f, -0.7f}, .color = {1.0f, 0.0f, 0.0f}},
        {.position = {0.0f, 0.7f}, .color = {0.0f, 1.0f, 0.0f}},
        {.position = {0.7f, -0.7f}, .color = {0.0f, 0.0f, 1.0f}},
      }};
      const std::array<uint32_t, 3> indices = {{0u, 1u, 2u}};
      vertex_buffer = VertexBuffer(ctx, sizeof(vertices));
      index_buffer = IndexBuffer(ctx, sizeof(indices), vk::IndexType::eUint32);
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
      auto upload_cmds = vk_expect(
        vk::Device(device.device).allocateCommandBuffers(upload_cmd_alloc),
        "vk::allocateCommandBuffers(upload) failed");
      auto upload_cmd = upload_cmds.at(0);

      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(upload_cmd.begin(begin_info) == vk::Result::eSuccess, "vk::CommandBuffer::begin(upload) failed");
      HostBuffer vertex_staging(
        ctx,
        sizeof(vertices),
        vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
      HostBuffer index_staging(
        ctx,
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

    void create_or_resize_color_image()
    {
      MR_TRACY_ZONE;
      color_image.emplace(
        vulkan_ctx_mut(),
        vk::Extent3D{.width = width, .height = height, .depth = 1u},
        vk::Format::eR8G8B8A8Unorm);
    }

    void ensure_graphics_pipeline_for_format(
      const std::filesystem::path& shader_path,
      vk::Format color_format)
    {
      MR_TRACY_ZONE_N("KomputeInterop::ensure_graphics_pipeline_for_format");
      if (graphics_pipeline_format == color_format && graphics_pipeline.valid()) {
        return;
      }
      graphics_pipeline = {};
      graphics_pipeline_format = color_format;

      const auto compiled = mr::importer::compile(shader_path);
      ASSERT(compiled.has_value(), "mr::importer::compile failed for interop_raster.slang");
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT(has_value)
      const auto& shaders = compiled.value();

      const mr::importer::Shader* vs = pick_stage(shaders, vk::ShaderStageFlagBits::eVertex);
      const mr::importer::Shader* fs = pick_stage(shaders, vk::ShaderStageFlagBits::eFragment);
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
      attributes.at(0).location = 0;
      attributes.at(0).binding = 0;
      attributes.at(0).format = vk::Format::eR32G32Sfloat;
      attributes.at(0).offset = offsetof(Vertex, position);
      attributes.at(1).location = 1;
      attributes.at(1).binding = 0;
      attributes.at(1).format = vk::Format::eR32G32B32Sfloat;
      attributes.at(1).offset = offsetof(Vertex, color);

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
      const std::array<vk::Format, 1> color_formats{{color_format}};
      const std::array<vk::PipelineColorBlendAttachmentState, 1> blend_descs{{color_blend_attachment}};

      auto pipeline_result =
        GraphicsPipelineBuilder(vulkan_ctx_mut())
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
      ASSERT(compiled.has_value(), "mr::importer::compile failed for interop_indirect_copy.slang");
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT(has_value)
      const mr::importer::Shader* cs = pick_stage(compiled.value(), vk::ShaderStageFlagBits::eCompute);
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
        vulkan_ctx_mut(),
        sizeof(DrawIndexedCommandRaw),
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    }

    void record_indirect_copy_and_raster(
      vk::CommandBuffer command_buffer,
      vk::RenderingAttachmentInfo color_attachment_info,
      vk::Extent2D extent)
    {
      MR_TRACY_ZONE_N("KomputeInterop::record_indirect_copy_and_raster");
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

      vk::RenderingInfo rendering_info{};
      rendering_info.renderArea.offset = vk::Offset2D{.x = 0, .y = 0};
      rendering_info.renderArea.extent = extent;
      rendering_info.layerCount = 1;
      rendering_info.colorAttachmentCount = 1;
      rendering_info.pColorAttachments = &color_attachment_info;
      command_buffer.beginRendering(rendering_info);
      vk::Viewport viewport{};
      viewport.x = 0.0f;
      viewport.y = 0.0f;
      viewport.width = static_cast<float>(extent.width);
      viewport.height = static_cast<float>(extent.height);
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      command_buffer.setViewport(0, viewport);
      vk::Rect2D scissor{};
      scissor.offset = vk::Offset2D{.x = 0, .y = 0};
      scissor.extent = extent;
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
    }

    void record_offscreen_raster(vk::CommandBuffer command_buffer)
    {
      MR_TRACY_ZONE_N("KomputeInterop::record_offscreen_raster");
      ASSERT(color_image.has_value(), "color attachment image is not initialized");
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT(has_value)
      auto& color = *color_image;
      color.transition_layout(command_buffer, vk::ImageLayout::eColorAttachmentOptimal);

      vk::RenderingAttachmentInfo color_attachment_info = color.attachment_info();
      color_attachment_info.clearValue = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.05f, 0.1f, 1.0f});
      record_indirect_copy_and_raster(
        command_buffer,
        color_attachment_info,
        vk::Extent2D{.width = width, .height = height});

      color.transition_layout(command_buffer, vk::ImageLayout::eTransferSrcOptimal);
    }

    void initialize_owned(uint32_t requested_width, uint32_t requested_height)
    {
      MR_TRACY_ZONE_N("KomputeInterop::initialize_owned");
      ensure_dimensions(requested_width, requested_height);

      auto context_result = create_vulkan_context(VulkanContextCreateInfo{
        .app_name = "mr-renderer",
        .headless = true,
        .require_present = false,
        .surface = VK_NULL_HANDLE,
        .prefer_dedicated_compute_queue = true,
      });
      ASSERT(context_result.has_value(), "vulkan context creation failed", context_result.error());
      owned_vulkan_context = std::make_unique<VulkanContext>(std::move(*context_result));
      active_context = owned_vulkan_context.get();

      initialize_after_context(std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR));
    }

    void initialize_shared_context(const VulkanContext& ctx, uint32_t requested_width, uint32_t requested_height)
    {
      MR_TRACY_ZONE_N("KomputeInterop::initialize_shared_context");
      owned_vulkan_context.reset();
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): WindowPresenter exposes const VulkanContext&
      active_context = const_cast<VulkanContext*>(&ctx);
      ensure_dimensions(requested_width, requested_height);

      ASSERT(ctx.feature_support.dynamic_rendering, "dynamic rendering is required for this example");
      if (ctx.has_present_queue()) {
        ASSERT(
          ctx.present_queue_family == ctx.graphics_queue_family,
          "Kompute swapchain path requires present and graphics on the same queue family");
      }

      initialize_after_context(std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR));
    }

    void initialize_after_context(const std::filesystem::path& shader_dir)
    {
      MR_TRACY_ZONE_N("KomputeInterop::initialize_after_context");
      wire_context_pointers();

      ASSERT(static_cast<bool>(graphics_queue), "failed to acquire graphics queue handle");
      ASSERT(static_cast<bool>(kompute_queue), "failed to acquire compute queue handle");

      kp_instance = std::shared_ptr<vk::Instance>(new vk::Instance(instance.instance), [](vk::Instance*) {});
      kp_physical_device = std::shared_ptr<vk::PhysicalDevice>(
        new vk::PhysicalDevice(physical_device.physical_device),
        [](vk::PhysicalDevice*) {});
      kp_device = std::shared_ptr<vk::Device>(new vk::Device(device.device), [](vk::Device*) {});
      kp_kompute_queue = std::shared_ptr<vk::Queue>(new vk::Queue(kompute_queue), [](vk::Queue*) {});

      frame_recorder = std::make_unique<FrameRecorder>(vulkan_ctx_mut());

      vk::CommandPoolCreateInfo transient_info{};
      transient_info.queueFamilyIndex = graphics_queue_family;
      transient_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
      gfx_transient_pool =
        vk_expect(vk::Device(device.device).createCommandPool(transient_info), "createCommandPool(transient gfx) failed");

      create_static_mesh_buffers();
      create_or_resize_color_image();
      ensure_graphics_pipeline_for_format(shader_dir / "interop_raster.slang", vk::Format::eR8G8B8A8Unorm);
      initialize_kompute(shader_dir / "interop_indirect_copy.slang");
    }

    Frame render_cpu_target(uint32_t frame_index, CpuTarget cpu_target)
    {
      MR_TRACY_ZONE_N("KomputeInterop::render_cpu_target");
      MR_TRACY_FRAME("kompute_interop_frame_cpu");
      const auto w = static_cast<uint32_t>(cpu_target.pixels.extent(1));
      const auto h = static_cast<uint32_t>(cpu_target.pixels.extent(0));
      ensure_dimensions(w, h);
      color_image.emplace(
        vulkan_ctx_mut(),
        vk::Extent3D{.width = width, .height = height, .depth = 1u},
        vk::Format::eR8G8B8A8Unorm);
      ensure_graphics_pipeline_for_format(
        std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR) / "interop_raster.slang",
        vk::Format::eR8G8B8A8Unorm);

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
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access): guarded by ASSERT(has_value)
      const auto& color = *color_image;
      vk::ImageSubresourceRange color_range{};
      color_range.aspectMask = vk::ImageAspectFlagBits::eColor;
      color_range.baseMipLevel = 0;
      color_range.levelCount = 1;
      color_range.baseArrayLayer = 0;
      color_range.layerCount = 1;
      frame_recorder->declare_image_usage(recorded, FrameRecorder::ImageUsageDesc{
        .image = color.image(),
        .subresource_range = color_range,
        .layout = vk::ImageLayout::eColorAttachmentOptimal,
        .stage = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .access = vk::AccessFlagBits2::eColorAttachmentWrite,
        .writes = true,
      });

      record_offscreen_raster(recorded.handle);
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

      readback_rgba8_unorm_image_to_raster(
        vulkan_ctx_mut(),
        color.image(),
        vk::ImageLayout::eTransferSrcOptimal,
        width,
        height,
        cpu_target.pixels);

      CpuFrame cpu_frame{};
      cpu_frame.width = width;
      cpu_frame.height = height;
      cpu_frame.presenter_slot_id = cpu_target.presenter_slot_id;
      cpu_frame.presenter_generation = cpu_target.presenter_generation;
      return Frame{frame_index, std::move(cpu_frame)};
    }

    // NOLINTNEXTLINE(readability-function-cognitive-complexity)
    Frame render_gpu_target(uint32_t frame_index, GpuTarget gt)
    {
      MR_TRACY_ZONE_N("KomputeInterop::render_gpu_target");
      MR_TRACY_FRAME("kompute_interop_frame_gpu");
      ASSERT(gt.context == &vulkan_ctx(), "GpuTarget VulkanContext mismatch");
      ASSERT(gt.image, "GpuTarget image is null");
      ASSERT(gt.view, "GpuTarget view is null");
      ASSERT(static_cast<bool>(gt.acquire_semaphore), "GpuTarget acquire semaphore is null");
      ASSERT(static_cast<bool>(gt.render_finished_semaphore), "GpuTarget render_finished semaphore is null");

      ensure_dimensions(gt.extent.width, gt.extent.height);

      const std::filesystem::path shader_path =
        std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR) / "interop_raster.slang";
      ensure_graphics_pipeline_for_format(shader_path, gt.format);

      if (compute_sequence->isRunning()) {
        compute_sequence->evalAwait();
      }
      compute_sequence->eval();

      ASSERT(kompute_queue.waitIdle() == vk::Result::eSuccess, "kompute queue waitIdle failed");

      ASSERT(
        vk::Device(device.device).resetCommandPool(gfx_transient_pool) == vk::Result::eSuccess,
        "resetCommandPool(transient) failed");

      vk::CommandBufferAllocateInfo alloc_info{};
      alloc_info.commandPool = gfx_transient_pool;
      alloc_info.level = vk::CommandBufferLevel::ePrimary;
      alloc_info.commandBufferCount = 1;
      vk::CommandBuffer cmd =
        vk_expect(vk::Device(device.device).allocateCommandBuffers(alloc_info), "allocate transient cmd failed").front();

      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(cmd.begin(begin_info) == vk::Result::eSuccess, "transient cmd begin failed");

      vk::ImageMemoryBarrier to_attachment{};
      to_attachment.oldLayout = vk::ImageLayout::eUndefined;
      to_attachment.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
      to_attachment.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_attachment.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_attachment.image = gt.image;
      to_attachment.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_attachment.subresourceRange.levelCount = 1;
      to_attachment.subresourceRange.layerCount = 1;
      to_attachment.srcAccessMask = {};
      to_attachment.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::DependencyFlags{},
        {},
        {},
        to_attachment);

      vk::RenderingAttachmentInfo attach{};
      attach.imageView = gt.view;
      attach.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
      attach.loadOp = vk::AttachmentLoadOp::eClear;
      attach.storeOp = vk::AttachmentStoreOp::eStore;
      attach.clearValue = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.05f, 0.1f, 1.0f});

      record_indirect_copy_and_raster(cmd, attach, gt.extent);

      vk::ImageMemoryBarrier to_present{};
      to_present.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
      to_present.newLayout = vk::ImageLayout::ePresentSrcKHR;
      to_present.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_present.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_present.image = gt.image;
      to_present.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_present.subresourceRange.levelCount = 1;
      to_present.subresourceRange.layerCount = 1;
      to_present.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
      cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eBottomOfPipe,
        vk::DependencyFlags{},
        {},
        {},
        to_present);

      ASSERT(cmd.end() == vk::Result::eSuccess, "transient cmd end failed");

      vk::SubmitInfo submit_info{};
      submit_info.waitSemaphoreCount = 1;
      submit_info.pWaitSemaphores = &gt.acquire_semaphore;
      submit_info.pWaitDstStageMask = &gt.acquire_stage;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &gt.render_finished_semaphore;
      ASSERT(graphics_queue.submit(submit_info, vk::Fence{}) == vk::Result::eSuccess, "graphics submit(swapchain) failed");

      ASSERT(graphics_queue.waitIdle() == vk::Result::eSuccess, "graphics queue must be idle before freeing submit cmd");

      vk::Device(device.device).freeCommandBuffers(gfx_transient_pool, cmd);

      GpuFrame gpu_frame{};
      gpu_frame.context = gt.context;
      gpu_frame.width = gt.extent.width;
      gpu_frame.height = gt.extent.height;
      gpu_frame.image = gt.image;
      gpu_frame.layout = vk::ImageLayout::ePresentSrcKHR;
      gpu_frame.format = gt.format;
      gpu_frame.presenter_slot_id = gt.slot_id;
      gpu_frame.presenter_generation = gt.generation;
      gpu_frame.render_finished_semaphore = gt.render_finished_semaphore;
      return Frame{frame_index, gpu_frame};
    }

    void shutdown()
    {
      MR_TRACY_ZONE_N("KomputeInterop::shutdown");
      if (owned_vulkan_context && device.device != VK_NULL_HANDLE) {
        ASSERT(vk::Device(device.device).waitIdle() == vk::Result::eSuccess, "vk::Device::waitIdle failed");
      }
      if (!owned_vulkan_context && device.device != VK_NULL_HANDLE) {
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

      if (gfx_transient_pool && device.device != VK_NULL_HANDLE) {
        vk::Device(device.device).destroyCommandPool(gfx_transient_pool);
      }
      gfx_transient_pool = nullptr;

      graphics_pipeline = {};
      graphics_pipeline_format = vk::Format::eUndefined;
      color_image.reset();
      index_buffer = {};
      vertex_buffer = {};
      indirect_draw_buffer = {};
      indirect_destination_buffer = nullptr;

      device = {};
      physical_device = {};
      instance = {};
      owned_vulkan_context.reset();
      active_context = nullptr;
    }
  };

  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>())
  {
    MR_TRACY_ZONE;
    impl_->initialize_owned(width, height);
  }

  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(
    const VulkanContext& shared_context,
    uint32_t width,
    uint32_t height)
    : impl_(std::make_unique<Impl>())
  {
    MR_TRACY_ZONE;
    impl_->initialize_shared_context(shared_context, width, height);
  }

  KomputeGraphicsInteropRenderer::~KomputeGraphicsInteropRenderer() = default;
  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(KomputeGraphicsInteropRenderer&&) noexcept = default;
  KomputeGraphicsInteropRenderer&
  KomputeGraphicsInteropRenderer::operator=(KomputeGraphicsInteropRenderer&&) noexcept = default;

  const VulkanContext& KomputeGraphicsInteropRenderer::vulkan_context() const
  {
    ASSERT(impl_ != nullptr, "KomputeGraphicsInteropRenderer impl is null");
    return impl_->vulkan_ctx();
  }

  coro::generator<Frame> KomputeGraphicsInteropRenderer::frames(coro::generator<Target> targets)
  {
    MR_TRACY_ZONE_N("KomputeInterop::frames");
    ASSERT(impl_ != nullptr, "KomputeGraphicsInteropRenderer impl is null");
    for (Target t : targets) {
      co_yield std::visit(
        [&](auto&& payload) -> Frame {
          using U = std::decay_t<decltype(payload)>;
          if constexpr (std::is_same_v<U, CpuTarget>) {
            return impl_->render_cpu_target(t.index, payload);
          } else {
            return impl_->render_gpu_target(t.index, payload);
          }
        },
        t.payload);
    }
  }
} // namespace mr
