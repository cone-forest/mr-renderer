#include "pch.hpp"
#include "vulkan_init.hpp"
#include <mr-renderer/kompute_graphics_interop_renderer.hpp>

#include <libassert/assert.hpp>
#include <mr-importer/compiler.hpp>

#include <array>
#include <cstring>
#include <filesystem>
#include <ranges>
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
      const auto it = std::ranges::find_if(shaders, [stage](const mr::importer::Shader& shader) -> bool {
        return shader.stage == stage;
      });
      if (it == shaders.end()) {
        return nullptr;
      }
      return &(*it);
    }

    uint32_t find_memory_type(
      const vk::PhysicalDevice physical_device,
      uint32_t type_filter,
      vk::MemoryPropertyFlags properties)
    {
      const vk::PhysicalDeviceMemoryProperties mem_props = physical_device.getMemoryProperties();
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        const bool supported = (type_filter & (1u << i)) != 0;
        const bool has_props = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
        if (supported && has_props) {
          return i;
        }
      }
      ASSERT(false, "no compatible Vulkan memory type found");
      return 0;
    }

    vk::ShaderModule create_shader_module(const vk::Device device, const std::vector<uint32_t>& spirv)
    {
      vk::ShaderModuleCreateInfo shader_info{};
      shader_info.codeSize = spirv.size() * sizeof(uint32_t);
      shader_info.pCode = spirv.data();
      return vk_expect(device.createShaderModule(shader_info), "vk::createShaderModule failed");
    }

    void create_buffer(
      const vk::PhysicalDevice physical_device,
      const vk::Device device,
      vk::DeviceSize size,
      vk::BufferUsageFlags usage,
      vk::MemoryPropertyFlags properties,
      vk::Buffer& out_buffer,
      vk::DeviceMemory& out_memory)
    {
      vk::BufferCreateInfo buffer_info{};
      buffer_info.size = size;
      buffer_info.usage = usage;
      buffer_info.sharingMode = vk::SharingMode::eExclusive;
      out_buffer = vk_expect(device.createBuffer(buffer_info), "vk::createBuffer failed");

      const vk::MemoryRequirements mem_req = device.getBufferMemoryRequirements(out_buffer);

      vk::MemoryAllocateInfo alloc_info{};
      alloc_info.allocationSize = mem_req.size;
      alloc_info.memoryTypeIndex =
        find_memory_type(physical_device, mem_req.memoryTypeBits, properties);

      out_memory = vk_expect(device.allocateMemory(alloc_info), "vk::allocateMemory failed");
      ASSERT(device.bindBufferMemory(out_buffer, out_memory, 0) == vk::Result::eSuccess,
        "vk::bindBufferMemory failed");
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

    vk::Semaphore compute_dispatch_done{};
    vk::Semaphore compute_copy_done{};
    vk::Fence graphics_fence{};

    vk::CommandPool compute_command_pool{};
    vk::CommandBuffer compute_copy_command_buffer{};
    vk::CommandPool graphics_command_pool{};
    vk::CommandBuffer graphics_command_buffer{};

    vk::Buffer graphics_indirect_buffer{};
    vk::DeviceMemory graphics_indirect_memory{};

    vk::Buffer vertex_buffer{};
    vk::DeviceMemory vertex_memory{};
    vk::Buffer index_buffer{};
    vk::DeviceMemory index_memory{};

    vk::Image color_image{};
    vk::DeviceMemory color_memory{};
    vk::ImageView color_image_view{};
    vk::RenderPass render_pass{};
    vk::Framebuffer framebuffer{};

    vk::Buffer readback_buffer{};
    vk::DeviceMemory readback_memory{};

    vk::DescriptorSetLayout descriptor_set_layout{};
    vk::PipelineLayout pipeline_layout{};
    vk::Pipeline graphics_pipeline{};

    ~Impl() { shutdown(); }

    void initialize(uint32_t requested_width, uint32_t requested_height)
    {
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

      create_sync_objects();
      create_command_pools();
      create_static_mesh_buffers();
      create_indirect_buffers();
      create_render_target();
      create_graphics_pipeline(shader_dir / "interop_raster.slang");
      initialize_kompute(shader_dir / "interop_indirect_copy.slang");
      record_compute_copy_command_buffer();
      record_graphics_command_buffer();
    }

    void create_sync_objects()
    {
      vk::SemaphoreCreateInfo sem_info{};
      compute_dispatch_done = vk_expect(vk::Device(device.device).createSemaphore(sem_info), "vk::createSemaphore failed");
      compute_copy_done = vk_expect(vk::Device(device.device).createSemaphore(sem_info), "vk::createSemaphore failed");

      vk::FenceCreateInfo fence_info{};
      graphics_fence = vk_expect(vk::Device(device.device).createFence(fence_info), "vk::createFence failed");
    }

    void create_command_pools()
    {
      vk::CommandPoolCreateInfo compute_pool_info{};
      compute_pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
      compute_pool_info.queueFamilyIndex = kompute_queue_family;
      compute_command_pool = vk_expect(vk::Device(device.device).createCommandPool(compute_pool_info), "vk::createCommandPool failed");

      vk::CommandPoolCreateInfo graphics_pool_info{};
      graphics_pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
      graphics_pool_info.queueFamilyIndex = graphics_queue_family;
      graphics_command_pool = vk_expect(vk::Device(device.device).createCommandPool(graphics_pool_info), "vk::createCommandPool failed");

      vk::CommandBufferAllocateInfo cmd_alloc{};
      cmd_alloc.commandBufferCount = 1;
      cmd_alloc.level = vk::CommandBufferLevel::ePrimary;

      cmd_alloc.commandPool = compute_command_pool;
      compute_copy_command_buffer = vk_expect(vk::Device(device.device).allocateCommandBuffers(cmd_alloc), "vk::allocateCommandBuffers failed")[0];

      cmd_alloc.commandPool = graphics_command_pool;
      graphics_command_buffer = vk_expect(vk::Device(device.device).allocateCommandBuffers(cmd_alloc), "vk::allocateCommandBuffers failed")[0];
    }

    void create_static_mesh_buffers()
    {
      const std::array<Vertex, 3> vertices = {{
        {{-0.7f, -0.7f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.7f}, {0.0f, 1.0f, 0.0f}},
        {{0.7f, -0.7f}, {0.0f, 0.0f, 1.0f}},
      }};
      const std::array<uint32_t, 3> indices = {{0u, 1u, 2u}};

      create_buffer(
        vk::PhysicalDevice(physical_device.physical_device),
        vk::Device(device.device),
        sizeof(vertices),
        vk::BufferUsageFlagBits::eVertexBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        vertex_buffer,
        vertex_memory);
      void* vertex_mapped = vk_expect(vk::Device(device.device).mapMemory(vertex_memory, 0, sizeof(vertices)), "vk::mapMemory(vertex) failed");
      std::memcpy(vertex_mapped, vertices.data(), sizeof(vertices));
      vk::Device(device.device).unmapMemory(vertex_memory);

      create_buffer(
        vk::PhysicalDevice(physical_device.physical_device),
        vk::Device(device.device),
        sizeof(indices),
        vk::BufferUsageFlagBits::eIndexBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        index_buffer,
        index_memory);
      void* index_mapped = vk_expect(vk::Device(device.device).mapMemory(index_memory, 0, sizeof(indices)), "vk::mapMemory(index) failed");
      std::memcpy(index_mapped, indices.data(), sizeof(indices));
      vk::Device(device.device).unmapMemory(index_memory);
    }

    void create_indirect_buffers()
    {
      create_buffer(
        vk::PhysicalDevice(physical_device.physical_device),
        vk::Device(device.device),
        sizeof(DrawIndexedCommandRaw),
        vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eIndirectBuffer,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        graphics_indirect_buffer,
        graphics_indirect_memory);
    }

    void create_render_target()
    {
      vk::ImageCreateInfo image_info{};
      image_info.imageType = vk::ImageType::e2D;
      image_info.extent = vk::Extent3D{width, height, 1u};
      image_info.mipLevels = 1;
      image_info.arrayLayers = 1;
      image_info.format = vk::Format::eR8G8B8A8Unorm;
      image_info.tiling = vk::ImageTiling::eOptimal;
      image_info.initialLayout = vk::ImageLayout::eUndefined;
      image_info.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
      image_info.samples = vk::SampleCountFlagBits::e1;
      image_info.sharingMode = vk::SharingMode::eExclusive;
      color_image = vk_expect(vk::Device(device.device).createImage(image_info), "vk::createImage failed");

      const vk::MemoryRequirements image_mem_req = vk::Device(device.device).getImageMemoryRequirements(color_image);
      vk::MemoryAllocateInfo image_alloc{};
      image_alloc.allocationSize = image_mem_req.size;
      image_alloc.memoryTypeIndex =
        find_memory_type(vk::PhysicalDevice(physical_device.physical_device), image_mem_req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
      color_memory = vk_expect(vk::Device(device.device).allocateMemory(image_alloc), "vk::allocateMemory(image) failed");
      ASSERT(vk::Device(device.device).bindImageMemory(color_image, color_memory, 0) == vk::Result::eSuccess,
        "vk::bindImageMemory failed");

      vk::ImageViewCreateInfo view_info{};
      view_info.image = color_image;
      view_info.viewType = vk::ImageViewType::e2D;
      view_info.format = vk::Format::eR8G8B8A8Unorm;
      view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      view_info.subresourceRange.baseMipLevel = 0;
      view_info.subresourceRange.levelCount = 1;
      view_info.subresourceRange.baseArrayLayer = 0;
      view_info.subresourceRange.layerCount = 1;
      color_image_view = vk_expect(vk::Device(device.device).createImageView(view_info), "vk::createImageView failed");

      vk::AttachmentDescription color_attachment{};
      color_attachment.format = vk::Format::eR8G8B8A8Unorm;
      color_attachment.samples = vk::SampleCountFlagBits::e1;
      color_attachment.loadOp = vk::AttachmentLoadOp::eClear;
      color_attachment.storeOp = vk::AttachmentStoreOp::eStore;
      color_attachment.initialLayout = vk::ImageLayout::eColorAttachmentOptimal;
      color_attachment.finalLayout = vk::ImageLayout::eColorAttachmentOptimal;

      vk::AttachmentReference color_ref{};
      color_ref.attachment = 0;
      color_ref.layout = vk::ImageLayout::eColorAttachmentOptimal;

      vk::SubpassDescription subpass{};
      subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color_ref;

      vk::RenderPassCreateInfo render_pass_info{};
      render_pass_info.attachmentCount = 1;
      render_pass_info.pAttachments = &color_attachment;
      render_pass_info.subpassCount = 1;
      render_pass_info.pSubpasses = &subpass;
      render_pass = vk_expect(vk::Device(device.device).createRenderPass(render_pass_info), "vk::createRenderPass failed");

      vk::FramebufferCreateInfo framebuffer_info{};
      framebuffer_info.renderPass = render_pass;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &color_image_view;
      framebuffer_info.width = width;
      framebuffer_info.height = height;
      framebuffer_info.layers = 1;
      framebuffer = vk_expect(vk::Device(device.device).createFramebuffer(framebuffer_info), "vk::createFramebuffer failed");

      create_buffer(
        vk::PhysicalDevice(physical_device.physical_device),
        vk::Device(device.device),
        static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * 4u,
        vk::BufferUsageFlagBits::eTransferDst,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        readback_buffer,
        readback_memory);
    }

    void create_graphics_pipeline(const std::filesystem::path& shader_path)
    {
      const auto compiled = mr::importer::compile(shader_path);
      ASSERT(compiled, "mr::importer::compile failed for interop_raster.slang");

      const mr::importer::Shader* vs = pick_stage(*compiled, vk::ShaderStageFlagBits::eVertex);
      const mr::importer::Shader* fs = pick_stage(*compiled, vk::ShaderStageFlagBits::eFragment);
      ASSERT(vs != nullptr, "missing vertex shader stage for interop_raster.slang");
      ASSERT(fs != nullptr, "missing fragment shader stage for interop_raster.slang");

      auto vs_spirv = *vs;
      auto fs_spirv = *fs;
      vk::ShaderModule vs_module = create_shader_module(vk::Device(device.device), vs_spirv);
      vk::ShaderModule fs_module = create_shader_module(vk::Device(device.device), fs_spirv);

      std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages{};
      shader_stages[0].stage = vk::ShaderStageFlagBits::eVertex;
      shader_stages[0].module = vs_module;
      shader_stages[0].pName = "main";
      shader_stages[1].stage = vk::ShaderStageFlagBits::eFragment;
      shader_stages[1].module = fs_module;
      shader_stages[1].pName = "main";

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

      vk::PipelineVertexInputStateCreateInfo vertex_input{};
      vertex_input.vertexBindingDescriptionCount = 1;
      vertex_input.pVertexBindingDescriptions = &binding_desc;
      vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
      vertex_input.pVertexAttributeDescriptions = attributes.data();

      vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
      input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

      vk::Viewport viewport{};
      viewport.width = static_cast<float>(width);
      viewport.height = static_cast<float>(height);
      viewport.maxDepth = 1.0f;
      vk::Rect2D scissor{};
      scissor.extent = vk::Extent2D{width, height};

      vk::PipelineViewportStateCreateInfo viewport_state{};
      viewport_state.viewportCount = 1;
      viewport_state.pViewports = &viewport;
      viewport_state.scissorCount = 1;
      viewport_state.pScissors = &scissor;

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

      vk::PipelineColorBlendStateCreateInfo color_blend{};
      color_blend.attachmentCount = 1;
      color_blend.pAttachments = &color_blend_attachment;

      vk::PipelineLayoutCreateInfo pipeline_layout_info{};
      pipeline_layout = vk_expect(vk::Device(device.device).createPipelineLayout(pipeline_layout_info), "vk::createPipelineLayout failed");

      vk::GraphicsPipelineCreateInfo pipeline_info{};
      pipeline_info.stageCount = 2;
      pipeline_info.pStages = shader_stages.data();
      pipeline_info.pVertexInputState = &vertex_input;
      pipeline_info.pInputAssemblyState = &input_assembly;
      pipeline_info.pViewportState = &viewport_state;
      pipeline_info.pRasterizationState = &rasterizer;
      pipeline_info.pMultisampleState = &multisampling;
      pipeline_info.pColorBlendState = &color_blend;
      pipeline_info.layout = pipeline_layout;
      pipeline_info.renderPass = render_pass;
      pipeline_info.subpass = 0;

      graphics_pipeline = vk_expect(
        vk::Device(device.device).createGraphicsPipeline(vk::PipelineCache{}, pipeline_info),
        "vk::createGraphicsPipeline failed");

      vk::Device(device.device).destroyShaderModule(fs_module);
      vk::Device(device.device).destroyShaderModule(vs_module);
    }

    void initialize_kompute(const std::filesystem::path& shader_path)
    {
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
    }

    void record_compute_copy_command_buffer()
    {
      ASSERT(compute_copy_command_buffer.reset(vk::CommandBufferResetFlags{}) == vk::Result::eSuccess,
        "vk::CommandBuffer::reset(compute) failed");

      vk::CommandBufferBeginInfo begin_info{};
      ASSERT(compute_copy_command_buffer.begin(begin_info) == vk::Result::eSuccess,
        "vk::CommandBuffer::begin(compute) failed");

      const vk::Buffer source_buffer = *destination_indirect_tensor->getPrimaryBuffer();
      vk::BufferCopy copy_region{};
      copy_region.size = sizeof(DrawIndexedCommandRaw);
      compute_copy_command_buffer.copyBuffer(source_buffer, graphics_indirect_buffer, copy_region);
      ASSERT(compute_copy_command_buffer.end() == vk::Result::eSuccess,
        "vk::CommandBuffer::end(compute) failed");
    }

    void record_graphics_command_buffer()
    {
      ASSERT(graphics_command_buffer.reset(vk::CommandBufferResetFlags{}) == vk::Result::eSuccess,
        "vk::CommandBuffer::reset(graphics) failed");

      vk::CommandBufferBeginInfo begin_info{};
      ASSERT(graphics_command_buffer.begin(begin_info) == vk::Result::eSuccess,
        "vk::CommandBuffer::begin(graphics) failed");

      vk::ImageMemoryBarrier to_color_attachment{};
      to_color_attachment.oldLayout = vk::ImageLayout::eUndefined;
      to_color_attachment.newLayout = vk::ImageLayout::eColorAttachmentOptimal;
      to_color_attachment.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_color_attachment.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_color_attachment.image = color_image;
      to_color_attachment.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_color_attachment.subresourceRange.levelCount = 1;
      to_color_attachment.subresourceRange.layerCount = 1;
      to_color_attachment.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      graphics_command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::DependencyFlags{},
        {},
        {},
        to_color_attachment);

      vk::BufferMemoryBarrier indirect_barrier{};
      indirect_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      indirect_barrier.dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead;
      indirect_barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      indirect_barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      indirect_barrier.buffer = graphics_indirect_buffer;
      indirect_barrier.offset = 0;
      indirect_barrier.size = sizeof(DrawIndexedCommandRaw);
      graphics_command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eDrawIndirect,
        vk::DependencyFlags{},
        {},
        indirect_barrier,
        {});

      vk::ClearValue clear_color{};
      clear_color.color = vk::ClearColorValue(std::array<float, 4>{0.05f, 0.05f, 0.1f, 1.0f});
      vk::RenderPassBeginInfo rp_begin{};
      rp_begin.renderPass = render_pass;
      rp_begin.framebuffer = framebuffer;
      rp_begin.renderArea.extent = vk::Extent2D{width, height};
      rp_begin.clearValueCount = 1;
      rp_begin.pClearValues = &clear_color;
      graphics_command_buffer.beginRenderPass(rp_begin, vk::SubpassContents::eInline);
      graphics_command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, graphics_pipeline);

      vk::DeviceSize vb_offset = 0;
      graphics_command_buffer.bindVertexBuffers(0, vertex_buffer, vb_offset);
      graphics_command_buffer.bindIndexBuffer(index_buffer, 0, vk::IndexType::eUint32);
      graphics_command_buffer.drawIndexedIndirect(
        graphics_indirect_buffer,
        0,
        1,
        sizeof(vk::DrawIndexedIndirectCommand));
      graphics_command_buffer.endRenderPass();

      vk::ImageMemoryBarrier to_transfer_src{};
      to_transfer_src.oldLayout = vk::ImageLayout::eColorAttachmentOptimal;
      to_transfer_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      to_transfer_src.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.image = color_image;
      to_transfer_src.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_transfer_src.subresourceRange.levelCount = 1;
      to_transfer_src.subresourceRange.layerCount = 1;
      to_transfer_src.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      to_transfer_src.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      graphics_command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},
        {},
        {},
        to_transfer_src);

      vk::BufferImageCopy copy_region{};
      copy_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageExtent = vk::Extent3D{width, height, 1u};
      graphics_command_buffer.copyImageToBuffer(
        color_image,
        vk::ImageLayout::eTransferSrcOptimal,
        readback_buffer,
        copy_region);
      ASSERT(graphics_command_buffer.end() == vk::Result::eSuccess,
        "vk::CommandBuffer::end(graphics) failed");
    }

    Frame render_frame(uint32_t frame_index)
    {
      if (compute_sequence->isRunning()) {
        compute_sequence->evalAwait();
      }

      compute_sequence->evalAsync(
        std::vector<vk::Semaphore>{},
        std::vector<vk::PipelineStageFlags>{},
        std::vector<vk::Semaphore>{compute_dispatch_done});

      vk::PipelineStageFlags compute_wait_stage = vk::PipelineStageFlagBits::eTransfer;
      vk::SubmitInfo compute_copy_submit{};
      compute_copy_submit.waitSemaphoreCount = 1;
      compute_copy_submit.pWaitSemaphores = &compute_dispatch_done;
      compute_copy_submit.pWaitDstStageMask = &compute_wait_stage;
      compute_copy_submit.commandBufferCount = 1;
      compute_copy_submit.pCommandBuffers = &compute_copy_command_buffer;
      compute_copy_submit.signalSemaphoreCount = 1;
      compute_copy_submit.pSignalSemaphores = &compute_copy_done;
      ASSERT(kompute_queue.submit(compute_copy_submit, vk::Fence{}) == vk::Result::eSuccess,
        "vk::Queue::submit(compute) failed");

      ASSERT(vk::Device(device.device).resetFences(graphics_fence) == vk::Result::eSuccess,
        "vk::Device::resetFences failed");
      vk::PipelineStageFlags graphics_wait_stage = vk::PipelineStageFlagBits::eDrawIndirect;
      vk::SubmitInfo graphics_submit{};
      graphics_submit.waitSemaphoreCount = 1;
      graphics_submit.pWaitSemaphores = &compute_copy_done;
      graphics_submit.pWaitDstStageMask = &graphics_wait_stage;
      graphics_submit.commandBufferCount = 1;
      graphics_submit.pCommandBuffers = &graphics_command_buffer;
      ASSERT(graphics_queue.submit(graphics_submit, graphics_fence) == vk::Result::eSuccess,
        "vk::Queue::submit(graphics) failed");
      ASSERT(vk::Device(device.device).waitForFences(graphics_fence, true, UINT64_MAX) == vk::Result::eSuccess,
        "vk::Device::waitForFences failed");

      const size_t byte_count = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
      void* mapped = vk_expect(
        vk::Device(device.device).mapMemory(readback_memory, 0, byte_count),
        "vk::mapMemory(readback) failed");
      const uint8_t* rgba8 = reinterpret_cast<const uint8_t*>(mapped);

      Frame frame;
      frame.index = frame_index;
      frame.width = width;
      frame.height = height;
      frame.rgba32f.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
      for (size_t i = 0; i < static_cast<size_t>(width) * static_cast<size_t>(height); ++i) {
        frame.rgba32f[i * 4u + 0u] = static_cast<float>(rgba8[i * 4u + 0u]) / 255.0f;
        frame.rgba32f[i * 4u + 1u] = static_cast<float>(rgba8[i * 4u + 1u]) / 255.0f;
        frame.rgba32f[i * 4u + 2u] = static_cast<float>(rgba8[i * 4u + 2u]) / 255.0f;
        frame.rgba32f[i * 4u + 3u] = static_cast<float>(rgba8[i * 4u + 3u]) / 255.0f;
      }
      vk::Device(device.device).unmapMemory(readback_memory);
      return frame;
    }

    void shutdown()
    {
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

      if (graphics_pipeline) {
        vk::Device(device.device).destroyPipeline(graphics_pipeline);
        graphics_pipeline = nullptr;
      }
      if (pipeline_layout) {
        vk::Device(device.device).destroyPipelineLayout(pipeline_layout);
        pipeline_layout = nullptr;
      }
      if (descriptor_set_layout) {
        vk::Device(device.device).destroyDescriptorSetLayout(descriptor_set_layout);
        descriptor_set_layout = nullptr;
      }
      if (framebuffer) {
        vk::Device(device.device).destroyFramebuffer(framebuffer);
        framebuffer = nullptr;
      }
      if (render_pass) {
        vk::Device(device.device).destroyRenderPass(render_pass);
        render_pass = nullptr;
      }
      if (color_image_view) {
        vk::Device(device.device).destroyImageView(color_image_view);
        color_image_view = nullptr;
      }
      if (color_image) {
        vk::Device(device.device).destroyImage(color_image);
        color_image = nullptr;
      }
      if (color_memory) {
        vk::Device(device.device).freeMemory(color_memory);
        color_memory = nullptr;
      }
      if (vertex_buffer) {
        vk::Device(device.device).destroyBuffer(vertex_buffer);
        vertex_buffer = nullptr;
      }
      if (vertex_memory) {
        vk::Device(device.device).freeMemory(vertex_memory);
        vertex_memory = nullptr;
      }
      if (index_buffer) {
        vk::Device(device.device).destroyBuffer(index_buffer);
        index_buffer = nullptr;
      }
      if (index_memory) {
        vk::Device(device.device).freeMemory(index_memory);
        index_memory = nullptr;
      }
      if (graphics_indirect_buffer) {
        vk::Device(device.device).destroyBuffer(graphics_indirect_buffer);
        graphics_indirect_buffer = nullptr;
      }
      if (graphics_indirect_memory) {
        vk::Device(device.device).freeMemory(graphics_indirect_memory);
        graphics_indirect_memory = nullptr;
      }
      if (readback_buffer) {
        vk::Device(device.device).destroyBuffer(readback_buffer);
        readback_buffer = nullptr;
      }
      if (readback_memory) {
        vk::Device(device.device).freeMemory(readback_memory);
        readback_memory = nullptr;
      }
      if (compute_dispatch_done) {
        vk::Device(device.device).destroySemaphore(compute_dispatch_done);
        compute_dispatch_done = nullptr;
      }
      if (compute_copy_done) {
        vk::Device(device.device).destroySemaphore(compute_copy_done);
        compute_copy_done = nullptr;
      }
      if (graphics_fence) {
        vk::Device(device.device).destroyFence(graphics_fence);
        graphics_fence = nullptr;
      }
      if (compute_command_pool) {
        vk::Device(device.device).destroyCommandPool(compute_command_pool);
        compute_command_pool = nullptr;
      }
      if (graphics_command_pool) {
        vk::Device(device.device).destroyCommandPool(graphics_command_pool);
        graphics_command_pool = nullptr;
      }

      device = {};
      physical_device = {};
      instance = {};
      vulkan_context.reset();
    }
  };

  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>())
  {
    impl_->initialize(width, height);
  }

  KomputeGraphicsInteropRenderer::~KomputeGraphicsInteropRenderer() = default;
  KomputeGraphicsInteropRenderer::KomputeGraphicsInteropRenderer(KomputeGraphicsInteropRenderer&&) noexcept =
    default;
  KomputeGraphicsInteropRenderer&
  KomputeGraphicsInteropRenderer::operator=(KomputeGraphicsInteropRenderer&&) noexcept = default;

  coro::generator<Frame> KomputeGraphicsInteropRenderer::frames()
  {
    uint32_t index = 0;
    while (true) {
      co_yield impl_->render_frame(index++);
    }
  }
} // namespace mr
