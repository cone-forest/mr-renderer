#include "pch.hpp"
#include <mr-renderer/vulkan_wrappers.hpp>

#include <VkBootstrap.h>
#include <libassert/assert.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace mr {
  namespace {
    bool validation_enabled()
    {
#ifdef MR_RENDERER_ENABLE_VK_VALIDATION
      return true;
#else
      return false;
#endif
    }

    std::expected<vkb::Instance, std::string> create_instance(
      const char* app_name,
      bool headless,
      const std::vector<const char*>& required_extensions)
    {
      vkb::InstanceBuilder builder;
      builder.set_app_name(app_name)
        .set_headless(headless)
        .request_validation_layers(validation_enabled())
        .require_api_version(1, 3, 0);
      if (validation_enabled()) {
        builder.use_default_debug_messenger();
      }
      if (!required_extensions.empty()) {
        builder.enable_extensions(required_extensions.size(), required_extensions.data());
      }
      auto instance_result = builder.build();
      if (!instance_result.has_value()) {
        return std::unexpected("vk-bootstrap failed to create Vulkan instance");
      }
      return instance_result.value();
    }

    bool has_extension(const std::vector<std::string>& extensions, const char* extension_name)
    {
      for (const auto& ext : extensions) {
        if (ext == extension_name) {
          return true;
        }
      }
      return false;
    }

    bool queue_supports_present(
      VkPhysicalDevice physical_device,
      uint32_t queue_family,
      VkSurfaceKHR surface)
    {
      if (surface == VK_NULL_HANDLE) {
        return false;
      }
      VkBool32 supports_present = VK_FALSE;
      const VkResult result =
        vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, queue_family, surface, &supports_present);
      return result == VK_SUCCESS && supports_present == VK_TRUE;
    }

    VulkanFeatureSupport query_feature_support(vkb::PhysicalDevice& physical_device)
    {
      VulkanFeatureSupport support{};

      const auto available_extensions = physical_device.get_available_extensions();
      support.mesh_shader = has_extension(available_extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
      support.task_shader = support.mesh_shader;
      support.draw_indirect_count =
        has_extension(available_extensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);

      vk::PhysicalDeviceFeatures2 core_features{};
      vk::PhysicalDeviceVulkan11Features features_11{};
      vk::PhysicalDeviceVulkan12Features features_12{};
      vk::PhysicalDeviceVulkan13Features features_13{};
      vk::PhysicalDeviceDescriptorIndexingFeatures descriptor_indexing{};
      vk::PhysicalDeviceMeshShaderFeaturesEXT mesh_shader{};

      core_features.pNext = &features_11;
      features_11.pNext = &features_12;
      features_12.pNext = &features_13;
      features_13.pNext = &descriptor_indexing;
      descriptor_indexing.pNext = &mesh_shader;

      vk::PhysicalDevice(physical_device.physical_device).getFeatures2(&core_features);

      support.multi_draw_indirect = core_features.features.multiDrawIndirect == VK_TRUE;
      support.descriptor_indexing = features_12.descriptorIndexing == VK_TRUE;
      support.runtime_descriptor_array =
        features_12.runtimeDescriptorArray == VK_TRUE ||
        descriptor_indexing.runtimeDescriptorArray == VK_TRUE;
      support.descriptor_binding_partially_bound =
        features_12.descriptorBindingPartiallyBound == VK_TRUE ||
        descriptor_indexing.descriptorBindingPartiallyBound == VK_TRUE;
      support.descriptor_binding_variable_descriptor_count =
        features_12.descriptorBindingVariableDescriptorCount == VK_TRUE ||
        descriptor_indexing.descriptorBindingVariableDescriptorCount == VK_TRUE;
      support.descriptor_binding_update_unused_while_pending =
        features_12.descriptorBindingUpdateUnusedWhilePending == VK_TRUE ||
        descriptor_indexing.descriptorBindingUpdateUnusedWhilePending == VK_TRUE;
      support.shader_draw_parameters = features_11.shaderDrawParameters == VK_TRUE;
      support.buffer_device_address = features_12.bufferDeviceAddress == VK_TRUE;
      support.timeline_semaphore = features_12.timelineSemaphore == VK_TRUE;
      support.synchronization2 = features_13.synchronization2 == VK_TRUE;
      support.dynamic_rendering = features_13.dynamicRendering == VK_TRUE;
      support.mesh_shader = support.mesh_shader && mesh_shader.meshShader == VK_TRUE;
      support.task_shader = support.task_shader && mesh_shader.taskShader == VK_TRUE;
      support.draw_indirect_count = support.draw_indirect_count || features_12.drawIndirectCount == VK_TRUE;

      return support;
    }

    void enable_optional_features(vkb::PhysicalDevice& physical_device, const VulkanFeatureSupport& support)
    {
      if (support.draw_indirect_count) {
        static_cast<void>(physical_device.enable_extension_if_present(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME));
      }
      if (support.mesh_shader || support.task_shader) {
        static_cast<void>(physical_device.enable_extension_if_present(VK_EXT_MESH_SHADER_EXTENSION_NAME));
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh_features{};
        mesh_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        mesh_features.meshShader = support.mesh_shader ? VK_TRUE : VK_FALSE;
        mesh_features.taskShader = support.task_shader ? VK_TRUE : VK_FALSE;
        static_cast<void>(physical_device.enable_extension_features_if_present(mesh_features));
      }

      VkPhysicalDeviceFeatures core_features{};
      core_features.multiDrawIndirect = support.multi_draw_indirect ? VK_TRUE : VK_FALSE;
      if (core_features.multiDrawIndirect == VK_TRUE) {
        static_cast<void>(physical_device.enable_features_if_present(core_features));
      }

      VkPhysicalDeviceVulkan12Features features_12{};
      features_12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
      features_12.descriptorIndexing = support.descriptor_indexing ? VK_TRUE : VK_FALSE;
      features_12.runtimeDescriptorArray = support.runtime_descriptor_array ? VK_TRUE : VK_FALSE;
      features_12.descriptorBindingPartiallyBound =
        support.descriptor_binding_partially_bound ? VK_TRUE : VK_FALSE;
      features_12.descriptorBindingVariableDescriptorCount =
        support.descriptor_binding_variable_descriptor_count ? VK_TRUE : VK_FALSE;
      features_12.descriptorBindingUpdateUnusedWhilePending =
        support.descriptor_binding_update_unused_while_pending ? VK_TRUE : VK_FALSE;
      features_12.drawIndirectCount = support.draw_indirect_count ? VK_TRUE : VK_FALSE;
      features_12.bufferDeviceAddress = support.buffer_device_address ? VK_TRUE : VK_FALSE;
      features_12.timelineSemaphore = support.timeline_semaphore ? VK_TRUE : VK_FALSE;
      static_cast<void>(physical_device.enable_extension_features_if_present(features_12));

      VkPhysicalDeviceVulkan11Features features_11{};
      features_11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
      features_11.shaderDrawParameters = support.shader_draw_parameters ? VK_TRUE : VK_FALSE;
      static_cast<void>(physical_device.enable_extension_features_if_present(features_11));

      VkPhysicalDeviceVulkan13Features features_13{};
      features_13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
      features_13.synchronization2 = support.synchronization2 ? VK_TRUE : VK_FALSE;
      features_13.dynamicRendering = support.dynamic_rendering ? VK_TRUE : VK_FALSE;
      static_cast<void>(physical_device.enable_extension_features_if_present(features_13));
    }

    size_t format_byte_size(vk::Format format)
    {
      switch (format) {
      case vk::Format::eR8Unorm:
      case vk::Format::eR8Uint:
      case vk::Format::eR8Srgb:
        return 1;
      case vk::Format::eR8G8Unorm:
      case vk::Format::eR8G8Uint:
      case vk::Format::eR16Sfloat:
        return 2;
      case vk::Format::eR8G8B8A8Unorm:
      case vk::Format::eB8G8R8A8Unorm:
      case vk::Format::eB8G8R8A8Srgb:
      case vk::Format::eR8G8B8A8Srgb:
      case vk::Format::eR32Sfloat:
      case vk::Format::eD32Sfloat:
        return 4;
      case vk::Format::eR16G16B16A16Sfloat:
        return 8;
      case vk::Format::eR32G32B32A32Sfloat:
        return 16;
      default:
        ASSERT(false, "unsupported format for byte-size calculation", static_cast<int>(format));
        return 0;
      }
    }

    std::vector<vk::PipelineColorBlendAttachmentState>
    default_blend_attachments(uint32_t count)
    {
      std::vector<vk::PipelineColorBlendAttachmentState> attachments(count);
      for (auto& attachment : attachments) {
        attachment.blendEnable = false;
        attachment.srcColorBlendFactor = vk::BlendFactor::eOne;
        attachment.dstColorBlendFactor = vk::BlendFactor::eZero;
        attachment.colorBlendOp = vk::BlendOp::eAdd;
        attachment.srcAlphaBlendFactor = vk::BlendFactor::eOne;
        attachment.dstAlphaBlendFactor = vk::BlendFactor::eZero;
        attachment.alphaBlendOp = vk::BlendOp::eAdd;
        attachment.colorWriteMask =
          vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
      }
      return attachments;
    }

    std::vector<std::byte> read_binary_file(const std::string& path)
    {
      if (path.empty()) {
        return {};
      }
      std::ifstream file(path, std::ios::binary);
      if (!file.good()) {
        return {};
      }
      file.seekg(0, std::ios::end);
      const std::streamsize size = file.tellg();
      if (size <= 0) {
        return {};
      }
      file.seekg(0, std::ios::beg);
      std::vector<std::byte> bytes(static_cast<size_t>(size));
      file.read(reinterpret_cast<char*>(bytes.data()), size);
      if (!file.good()) {
        return {};
      }
      return bytes;
    }

    bool write_binary_file(const std::string& path, std::span<const std::byte> bytes)
    {
      if (path.empty()) {
        return false;
      }
      std::error_code ec{};
      const auto parent = std::filesystem::path(path).parent_path();
      if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
          return false;
        }
      }
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      if (!file.good()) {
        return false;
      }
      file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
      return file.good();
    }

    template <typename Fn>
    void immediate_submit(const VulkanContext& context, Fn&& record)
    {
      vk::CommandPoolCreateInfo pool_info{};
      pool_info.queueFamilyIndex = context.graphics_queue_family;
      pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
      const auto pool_rv = context.vk_device().createCommandPool(pool_info);
      ASSERT(pool_rv.result == vk::Result::eSuccess, "createCommandPool failed");
      vk::CommandPool pool = pool_rv.value;

      vk::CommandBufferAllocateInfo alloc_info{};
      alloc_info.commandPool = pool;
      alloc_info.level = vk::CommandBufferLevel::ePrimary;
      alloc_info.commandBufferCount = 1;
      const auto cmd_rv = context.vk_device().allocateCommandBuffers(alloc_info);
      ASSERT(cmd_rv.result == vk::Result::eSuccess, "allocateCommandBuffers failed");
      vk::CommandBuffer cmd = cmd_rv.value[0];

      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(cmd.begin(begin_info) == vk::Result::eSuccess, "command buffer begin failed");
      record(cmd);
      ASSERT(cmd.end() == vk::Result::eSuccess, "command buffer end failed");

      vk::SubmitInfo submit_info{};
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      ASSERT(context.graphics_queue.submit(submit_info, vk::Fence{}) == vk::Result::eSuccess, "queue submit failed");
      ASSERT(context.graphics_queue.waitIdle() == vk::Result::eSuccess, "queue wait idle failed");

      context.vk_device().freeCommandBuffers(pool, cmd);
      context.vk_device().destroyCommandPool(pool);
    }
  } // namespace

  VulkanContext::~VulkanContext()
  {
    flush_pipeline_cache();
    if (pipeline_cache) {
      vk_device().destroyPipelineCache(pipeline_cache);
      pipeline_cache = nullptr;
    }
    if (allocator != VK_NULL_HANDLE) {
      vmaDestroyAllocator(allocator);
      allocator = VK_NULL_HANDLE;
    }
    if (device.device != VK_NULL_HANDLE) {
      vkb::destroy_device(device);
      device = {};
    }
    if (instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(instance);
      instance = {};
    }
  }

  VulkanContext::VulkanContext(VulkanContext&& other) noexcept
    : instance(std::move(other.instance))
    , physical_device(std::move(other.physical_device))
    , device(std::move(other.device))
    , allocator(other.allocator)
    , graphics_queue_family(other.graphics_queue_family)
    , compute_queue_family(other.compute_queue_family)
    , present_queue_family(other.present_queue_family)
    , graphics_queue(other.graphics_queue)
    , compute_queue(other.compute_queue)
    , present_queue(other.present_queue)
    , feature_support(other.feature_support)
    , enabled_extensions(std::move(other.enabled_extensions))
    , pipeline_cache(other.pipeline_cache)
    , pipeline_cache_path(std::move(other.pipeline_cache_path))
  {
    other.instance = {};
    other.device = {};
    other.allocator = VK_NULL_HANDLE;
    other.graphics_queue = nullptr;
    other.compute_queue = nullptr;
    other.present_queue = nullptr;
    other.pipeline_cache = nullptr;
    other.graphics_queue_family = std::numeric_limits<uint32_t>::max();
    other.compute_queue_family = std::numeric_limits<uint32_t>::max();
    other.present_queue_family = std::numeric_limits<uint32_t>::max();
  }

  VulkanContext& VulkanContext::operator=(VulkanContext&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }

    if (allocator != VK_NULL_HANDLE) {
      vmaDestroyAllocator(allocator);
      allocator = VK_NULL_HANDLE;
    }
    if (pipeline_cache) {
      vk_device().destroyPipelineCache(pipeline_cache);
      pipeline_cache = nullptr;
    }
    if (device.device != VK_NULL_HANDLE) {
      vkb::destroy_device(device);
      device = {};
    }
    if (instance.instance != VK_NULL_HANDLE) {
      vkb::destroy_instance(instance);
      instance = {};
    }

    instance = std::move(other.instance);
    physical_device = std::move(other.physical_device);
    device = std::move(other.device);
    allocator = other.allocator;
    graphics_queue_family = other.graphics_queue_family;
    compute_queue_family = other.compute_queue_family;
    present_queue_family = other.present_queue_family;
    graphics_queue = other.graphics_queue;
    compute_queue = other.compute_queue;
    present_queue = other.present_queue;
    feature_support = other.feature_support;
    enabled_extensions = std::move(other.enabled_extensions);
    pipeline_cache = other.pipeline_cache;
    pipeline_cache_path = std::move(other.pipeline_cache_path);

    other.instance = {};
    other.device = {};
    other.allocator = VK_NULL_HANDLE;
    other.graphics_queue = nullptr;
    other.compute_queue = nullptr;
    other.present_queue = nullptr;
    other.pipeline_cache = nullptr;
    other.graphics_queue_family = std::numeric_limits<uint32_t>::max();
    other.compute_queue_family = std::numeric_limits<uint32_t>::max();
    other.present_queue_family = std::numeric_limits<uint32_t>::max();
    return *this;
  }

  vk::Instance VulkanContext::vk_instance() const { return instance.instance; }
  vk::PhysicalDevice VulkanContext::vk_physical_device() const { return physical_device.physical_device; }
  vk::Device VulkanContext::vk_device() const { return device.device; }
  bool VulkanContext::has_present_queue() const
  {
    return present_queue_family != std::numeric_limits<uint32_t>::max() && static_cast<bool>(present_queue);
  }
  vk::PipelineCache VulkanContext::vk_pipeline_cache() const { return pipeline_cache; }

  void VulkanContext::flush_pipeline_cache()
  {
    if (!pipeline_cache || pipeline_cache_path.empty() || device.device == VK_NULL_HANDLE) {
      return;
    }
    const auto rv = vk_device().getPipelineCacheData(pipeline_cache);
    if (rv.result != vk::Result::eSuccess) {
      return;
    }
    std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(rv.value.data()), rv.value.size());
    static_cast<void>(write_binary_file(pipeline_cache_path, bytes));
  }

  std::expected<VulkanContext, std::string> create_vulkan_context(const VulkanContextCreateInfo& create_info)
  {
    auto instance_result = create_instance(create_info.app_name, create_info.headless, {});
    if (!instance_result.has_value()) {
      return std::unexpected(instance_result.error());
    }

    VulkanContext context{};
    context.instance = std::move(*instance_result);

    vkb::PhysicalDeviceSelector selector(context.instance, create_info.surface);
    selector.require_present(create_info.require_present);
    auto physical_result = selector.select();
    if (!physical_result.has_value()) {
      return std::unexpected("vk-bootstrap failed to select a Vulkan physical device");
    }
    context.physical_device = physical_result.value();
    context.feature_support = query_feature_support(context.physical_device);
    enable_optional_features(context.physical_device, context.feature_support);
    context.enabled_extensions = context.physical_device.get_extensions();

    const auto queue_props = context.physical_device.get_queue_families();
    if (queue_props.empty()) {
      return std::unexpected("selected physical device has no queue families");
    }

    auto queue_has_flags = [&](uint32_t family, vk::QueueFlags required_flags) -> bool {
      if (family >= queue_props.size()) {
        return false;
      }
      const vk::QueueFlags flags{queue_props[family].queueFlags};
      return (flags & required_flags) == required_flags;
    };

    for (uint32_t i = 0; i < queue_props.size(); ++i) {
      if (queue_has_flags(
            i,
            vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer)) {
        context.graphics_queue_family = i;
        break;
      }
    }
    if (context.graphics_queue_family == std::numeric_limits<uint32_t>::max()) {
      return std::unexpected("no queue family satisfies graphics+compute+transfer");
    }

    if (create_info.require_present) {
      for (uint32_t i = 0; i < queue_props.size(); ++i) {
        if (queue_supports_present(context.physical_device.physical_device, i, create_info.surface)) {
          context.present_queue_family = i;
          break;
        }
      }
      if (context.present_queue_family == std::numeric_limits<uint32_t>::max()) {
        return std::unexpected("no queue family supports presentation for the provided surface");
      }
    }

    if (create_info.prefer_dedicated_compute_queue) {
      for (uint32_t i = 0; i < queue_props.size(); ++i) {
        const vk::QueueFlags flags{queue_props[i].queueFlags};
        const bool is_compute = (flags & vk::QueueFlagBits::eCompute) == vk::QueueFlagBits::eCompute;
        const bool is_transfer = (flags & vk::QueueFlagBits::eTransfer) == vk::QueueFlagBits::eTransfer;
        const bool is_graphics = (flags & vk::QueueFlagBits::eGraphics) == vk::QueueFlagBits::eGraphics;
        if (is_compute && is_transfer && !is_graphics) {
          context.compute_queue_family = i;
          break;
        }
      }
    }
    if (context.compute_queue_family == std::numeric_limits<uint32_t>::max()) {
      context.compute_queue_family = context.graphics_queue_family;
    }
    if (!queue_has_flags(
          context.compute_queue_family,
          vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer)) {
      return std::unexpected("selected compute queue family does not support compute+transfer");
    }

    std::set<uint32_t> unique_families{};
    unique_families.insert(context.graphics_queue_family);
    unique_families.insert(context.compute_queue_family);
    if (create_info.require_present) {
      unique_families.insert(context.present_queue_family);
    }

    std::vector<vkb::CustomQueueDescription> queue_setup{};
    for (const uint32_t family : unique_families) {
      queue_setup.emplace_back(family, std::vector<float>{1.0f});
    }

    vkb::DeviceBuilder device_builder(context.physical_device);
    device_builder.custom_queue_setup(queue_setup);
    auto device_result = device_builder.build();
    if (!device_result.has_value()) {
      return std::unexpected("vk-bootstrap failed to create Vulkan logical device");
    }
    context.device = device_result.value();

    const vk::Device device = context.vk_device();
    context.graphics_queue = device.getQueue(context.graphics_queue_family, 0);
    context.compute_queue = device.getQueue(context.compute_queue_family, 0);
    if (!context.graphics_queue || !context.compute_queue) {
      return std::unexpected("failed to fetch graphics/compute queue handles");
    }
    if (create_info.require_present) {
      context.present_queue = device.getQueue(context.present_queue_family, 0);
      if (!context.present_queue) {
        return std::unexpected("failed to fetch present queue handle");
      }
    }

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.physicalDevice = context.physical_device.physical_device;
    allocator_info.device = context.device.device;
    allocator_info.instance = context.instance.instance;
    const VkResult allocator_result = vmaCreateAllocator(&allocator_info, &context.allocator);
    if (allocator_result != VK_SUCCESS) {
      return std::unexpected("vmaCreateAllocator failed");
    }

    if (create_info.enable_pipeline_cache) {
      context.pipeline_cache_path = create_info.pipeline_cache_path;
      const auto cache_bytes = read_binary_file(context.pipeline_cache_path);
      vk::PipelineCacheCreateInfo cache_create_info{};
      cache_create_info.initialDataSize = cache_bytes.size();
      cache_create_info.pInitialData = cache_bytes.empty() ? nullptr : cache_bytes.data();
      const auto cache_result = context.vk_device().createPipelineCache(cache_create_info);
      if (cache_result.result != vk::Result::eSuccess) {
        return std::unexpected("createPipelineCache failed");
      }
      context.pipeline_cache = cache_result.value;
    }

    return context;
  }

  std::expected<std::vector<VulkanPhysicalDeviceInfo>, std::string>
  enumerate_vulkan_physical_devices()
  {
    auto instance_ret = create_instance("mr-renderer-lib", true, {});
    if (!instance_ret) {
      return std::unexpected(instance_ret.error());
    }

    vkb::Instance instance = std::move(*instance_ret);

    uint32_t device_count = 0;
    VkResult count_result =
      vkEnumeratePhysicalDevices(instance.instance, &device_count, nullptr);
    if (count_result != VK_SUCCESS) {
      vkb::destroy_instance(instance);
      return std::unexpected("vkEnumeratePhysicalDevices(count) failed");
    }

    if (device_count == 0) {
      vkb::destroy_instance(instance);
      return {};
    }

    std::vector<VkPhysicalDevice> devices(device_count);
    VkResult enum_result =
      vkEnumeratePhysicalDevices(instance.instance, &device_count, devices.data());
    if (enum_result != VK_SUCCESS) {
      vkb::destroy_instance(instance);
      return std::unexpected("vkEnumeratePhysicalDevices(list) failed");
    }

    std::vector<VulkanPhysicalDeviceInfo> out;
    out.reserve(device_count);
    for (uint32_t i = 0; i < device_count; ++i) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(devices[i], &props);
      out.push_back(VulkanPhysicalDeviceInfo{
        .vendor_id = props.vendorID,
        .device_type = props.deviceType,
      });
    }

    vkb::destroy_instance(instance);
    return out;
  }

  Buffer::Buffer(
    const VulkanContext& context,
    vk::DeviceSize byte_size,
    vk::BufferUsageFlags usage_flags,
    vk::MemoryPropertyFlags memory_properties)
    : context_(&context)
    , size_(byte_size)
    , usage_flags_(usage_flags)
  {
    vk::BufferCreateInfo buffer_create_info{};
    buffer_create_info.size = byte_size;
    buffer_create_info.usage = usage_flags;
    buffer_create_info.sharingMode = vk::SharingMode::eExclusive;

    VmaAllocationCreateInfo allocation_create_info{};
    allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_create_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(memory_properties);
    if ((memory_properties & vk::MemoryPropertyFlagBits::eHostVisible) != vk::MemoryPropertyFlags{}) {
      allocation_create_info.flags |= VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }

    const VkResult result = vmaCreateBuffer(
      context.allocator,
      reinterpret_cast<VkBufferCreateInfo*>(&buffer_create_info),
      &allocation_create_info,
      reinterpret_cast<VkBuffer*>(&buffer_),
      &allocation_,
      nullptr);
    ASSERT(result == VK_SUCCESS, "vmaCreateBuffer failed", static_cast<int>(result));
  }

  Buffer::~Buffer() noexcept
  {
    if (buffer_ && context_ != nullptr && context_->allocator != VK_NULL_HANDLE) {
      vmaDestroyBuffer(context_->allocator, static_cast<VkBuffer>(buffer_), allocation_);
      buffer_ = nullptr;
      allocation_ = VK_NULL_HANDLE;
    }
  }

  Buffer::Buffer(Buffer&& other) noexcept { *this = std::move(other); }

  Buffer& Buffer::operator=(Buffer&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    if (buffer_ && context_ != nullptr && context_->allocator != VK_NULL_HANDLE) {
      vmaDestroyBuffer(context_->allocator, static_cast<VkBuffer>(buffer_), allocation_);
    }
    context_ = other.context_;
    size_ = other.size_;
    buffer_ = other.buffer_;
    usage_flags_ = other.usage_flags_;
    allocation_ = other.allocation_;

    other.context_ = nullptr;
    other.size_ = 0;
    other.buffer_ = nullptr;
    other.usage_flags_ = {};
    other.allocation_ = VK_NULL_HANDLE;
    return *this;
  }

  const VulkanContext& Buffer::context() const
  {
    ASSERT(context_ != nullptr, "buffer context is null");
    return *context_;
  }
  vk::Buffer Buffer::buffer() const noexcept { return buffer_; }
  vk::DeviceSize Buffer::byte_size() const noexcept { return size_; }

  HostBuffer::HostBuffer(
    const VulkanContext& context,
    vk::DeviceSize byte_size,
    vk::BufferUsageFlags usage_flags,
    vk::MemoryPropertyFlags memory_properties)
    : Buffer(
        context,
        byte_size,
        usage_flags,
        memory_properties | vk::MemoryPropertyFlagBits::eHostVisible |
          vk::MemoryPropertyFlagBits::eHostCoherent)
  {}

  HostBuffer::~HostBuffer() noexcept
  {
    if (mapped_data_ != nullptr && context_ != nullptr) {
      vmaUnmapMemory(context_->allocator, allocation_);
      mapped_data_ = nullptr;
    }
  }

  HostBuffer::HostBuffer(HostBuffer&& other) noexcept
    : Buffer(std::move(other))
    , mapped_data_(other.mapped_data_)
  {
    other.mapped_data_ = nullptr;
  }

  HostBuffer& HostBuffer::operator=(HostBuffer&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    if (mapped_data_ != nullptr && context_ != nullptr) {
      vmaUnmapMemory(context_->allocator, allocation_);
      mapped_data_ = nullptr;
    }
    Buffer::operator=(std::move(other));
    mapped_data_ = other.mapped_data_;
    other.mapped_data_ = nullptr;
    return *this;
  }

  std::span<const std::byte> HostBuffer::read() noexcept
  {
    if (mapped_data_ == nullptr) {
      const VkResult result = vmaMapMemory(context_->allocator, allocation_, &mapped_data_);
      ASSERT(result == VK_SUCCESS, "vmaMapMemory failed", static_cast<int>(result));
    }
    return std::span(reinterpret_cast<const std::byte*>(mapped_data_), static_cast<size_t>(size_));
  }

  std::vector<std::byte> HostBuffer::copy() noexcept
  {
    std::vector<std::byte> data(static_cast<size_t>(size_));
    auto src = read();
    std::memcpy(data.data(), src.data(), src.size());
    return data;
  }

  HostBuffer& HostBuffer::write(std::span<const std::byte> src)
  {
    ASSERT(src.size() <= static_cast<size_t>(size_), "host buffer overflow write");
    if (mapped_data_ == nullptr) {
      const VkResult result = vmaMapMemory(context_->allocator, allocation_, &mapped_data_);
      ASSERT(result == VK_SUCCESS, "vmaMapMemory failed", static_cast<int>(result));
      std::memcpy(mapped_data_, src.data(), src.size());
      vmaUnmapMemory(context_->allocator, allocation_);
      mapped_data_ = nullptr;
      return *this;
    }
    std::memcpy(mapped_data_, src.data(), src.size());
    return *this;
  }

  DeviceBuffer::DeviceBuffer(
    const VulkanContext& context,
    vk::DeviceSize byte_size,
    vk::BufferUsageFlags usage_flags,
    vk::MemoryPropertyFlags memory_properties)
    : Buffer(context, byte_size, usage_flags, memory_properties | vk::MemoryPropertyFlagBits::eDeviceLocal)
  {}

  DeviceBuffer& DeviceBuffer::resize(vk::CommandBuffer command_buffer, vk::DeviceSize new_size) noexcept
  {
    DeviceBuffer resized(
      *context_,
      new_size,
      usage_flags_,
      vk::MemoryPropertyFlagBits::eDeviceLocal);
    copy_buffer(
      command_buffer,
      BufferRegion{context_, buffer_, 0, std::min(size_, new_size)},
      BufferRegion{context_, resized.buffer_, 0, std::min(size_, new_size)});
    *this = std::move(resized);
    return *this;
  }

  DeviceBuffer& DeviceBuffer::write(
    vk::CommandBuffer command_buffer,
    std::span<const std::byte> src,
    vk::DeviceSize offset)
  {
    ASSERT(offset + src.size() <= size_, "device buffer overflow write");
    HostBuffer staging(*context_, src.size(), vk::BufferUsageFlagBits::eTransferSrc);
    staging.write(src);

    copy_buffer(
      command_buffer,
      BufferRegion{context_, staging.buffer(), 0, static_cast<vk::DeviceSize>(src.size())},
      BufferRegion{context_, buffer_, offset, static_cast<vk::DeviceSize>(src.size())});
    return *this;
  }

  UniformBuffer::UniformBuffer(
    const VulkanContext& context,
    vk::DeviceSize byte_size,
    vk::BufferUsageFlags usage_flags)
    : HostBuffer(context, byte_size, usage_flags | vk::BufferUsageFlagBits::eUniformBuffer)
  {}

  StorageBuffer::StorageBuffer(
    const VulkanContext& context,
    vk::DeviceSize byte_size,
    vk::BufferUsageFlags usage_flags)
    : DeviceBuffer(
        context,
        byte_size,
        usage_flags | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst)
  {}

  VertexBuffer::VertexBuffer(const VulkanContext& context, vk::DeviceSize byte_size)
    : DeviceBuffer(
        context,
        byte_size,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst)
  {}

  IndexBuffer::IndexBuffer(const VulkanContext& context, vk::DeviceSize byte_size, vk::IndexType index_type)
    : DeviceBuffer(
        context,
        byte_size,
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst)
    , index_type_(index_type)
  {}

  VectorBuffer::VectorBuffer(
    const VulkanContext& context,
    vk::BufferUsageFlags usage_flags,
    vk::DeviceSize initial_byte_size)
    : DeviceBuffer(
        context,
        initial_byte_size,
        usage_flags | vk::BufferUsageFlagBits::eTransferSrc,
        vk::MemoryPropertyFlags{})
    , current_size_(0)
  {}

  vk::DeviceSize VectorBuffer::append_range(vk::CommandBuffer command_buffer, std::span<const std::byte> src) noexcept
  {
    const vk::DeviceSize offset = current_size_;
    current_size_ += src.size();
    if (current_size_ > byte_size()) {
      const auto proposed = static_cast<vk::DeviceSize>(static_cast<double>(current_size_) * resize_coefficient);
      const vk::DeviceSize new_capacity = std::max(proposed, current_size_);
      DeviceBuffer::resize(command_buffer, new_capacity);
    }
    write(command_buffer, src, offset);
    return offset;
  }

  VertexVectorBuffer::VertexVectorBuffer(
    const VulkanContext& context,
    vk::DeviceSize initial_byte_size)
    : VectorBuffer(
        context,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        initial_byte_size)
  {}

  IndexVectorBuffer::IndexVectorBuffer(
    const VulkanContext& context,
    vk::DeviceSize initial_byte_size)
    : VectorBuffer(
        context,
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        initial_byte_size)
  {}

  DeviceHeapAllocator::AllocationBlock::AllocationBlock(
    vk::DeviceSize size,
    vk::DeviceSize offset,
    uint32_t block_number) noexcept
    : size_(size)
    , offset_(offset)
    , block_number_(block_number)
  {
    VmaVirtualBlockCreateInfo create_info{};
    create_info.size = size_;
    const VkResult result = vmaCreateVirtualBlock(&create_info, &virtual_block_);
    ASSERT(result == VK_SUCCESS, "vmaCreateVirtualBlock failed", static_cast<int>(result));
  }

  DeviceHeapAllocator::AllocationBlock::~AllocationBlock() noexcept
  {
    if (virtual_block_ != VK_NULL_HANDLE) {
      vmaClearVirtualBlock(virtual_block_);
      vmaDestroyVirtualBlock(virtual_block_);
      virtual_block_ = VK_NULL_HANDLE;
    }
  }

  DeviceHeapAllocator::AllocationBlock::AllocationBlock(AllocationBlock&& other) noexcept
  {
    *this = std::move(other);
  }

  DeviceHeapAllocator::AllocationBlock&
  DeviceHeapAllocator::AllocationBlock::operator=(AllocationBlock&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    virtual_block_ = other.virtual_block_;
    size_ = other.size_;
    offset_ = other.offset_;
    block_number_ = other.block_number_;
    other.virtual_block_ = VK_NULL_HANDLE;
    other.size_ = 0;
    other.offset_ = 0;
    other.block_number_ = 0;
    return *this;
  }

  std::optional<std::pair<vk::DeviceSize, DeviceHeapAllocator::Allocation>>
  DeviceHeapAllocator::AllocationBlock::allocate(vk::DeviceSize allocation_size, uint32_t alignment) noexcept
  {
    Allocation allocation{};
    allocation.byte_size = allocation_size;
    allocation.block_number = block_number_;

    VmaVirtualAllocationCreateInfo alloc_info{};
    alloc_info.size = allocation_size;
    alloc_info.alignment = alignment;

    vk::DeviceSize offset = 0;
    std::lock_guard lock(mutex_);
    const VkResult result = vmaVirtualAllocate(virtual_block_, &alloc_info, &allocation.allocation, &offset);
    if (result != VK_SUCCESS) {
      return std::nullopt;
    }
    return std::make_pair(offset + offset_, allocation);
  }

  void DeviceHeapAllocator::AllocationBlock::deallocate(Allocation& allocation) noexcept
  {
    std::lock_guard lock(mutex_);
    vmaVirtualFree(virtual_block_, allocation.allocation);
    allocation.allocation = VK_NULL_HANDLE;
  }

  DeviceHeapAllocator::DeviceHeapAllocator(vk::DeviceSize start_byte_size, uint32_t alignment)
    : size_(0)
    , alignment_(alignment)
  {
    ASSERT(std::has_single_bit(alignment), "allocator alignment must be power-of-two");
    add_block(start_byte_size);
  }

  DeviceHeapAllocator::DeviceHeapAllocator(DeviceHeapAllocator&& other) noexcept
  {
    *this = std::move(other);
  }

  DeviceHeapAllocator& DeviceHeapAllocator::operator=(DeviceHeapAllocator&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    size_ = other.size_;
    alignment_ = other.alignment_;
    {
      std::lock_guard lock(other.allocations_mutex_);
      allocations_ = std::move(other.allocations_);
    }
    blocks_ = std::move(other.blocks_);
    other.size_ = 0;
    other.alignment_ = 16;
    return *this;
  }

  DeviceHeapAllocator::AllocationBlock&
  DeviceHeapAllocator::add_block(vk::DeviceSize allocation_size) noexcept
  {
    std::lock_guard lock(add_block_mutex_);
    vk::DeviceSize block_size = allocation_size;
    vk::DeviceSize offset = 0;
    if (!blocks_.empty()) {
      const auto& last = blocks_.back();
      block_size = (last.size() + allocation_size) * 2;
      offset = last.offset() + last.size();
    }
    blocks_.emplace_back(block_size, offset, static_cast<uint32_t>(blocks_.size()));
    size_ += block_size;
    return blocks_.back();
  }

  DeviceHeapAllocator::AllocationInfo DeviceHeapAllocator::allocate(vk::DeviceSize request_size) noexcept
  {
    ASSERT(request_size % alignment_ == 0, "heap allocation must satisfy allocator alignment");
    std::pair<vk::DeviceSize, Allocation> allocation{};
    bool reused_existing = false;
    for (auto& block : blocks_) {
      auto result = block.allocate(request_size, alignment_);
      if (result.has_value()) {
        allocation = std::move(*result);
        reused_existing = true;
        break;
      }
    }
    if (!reused_existing) {
      auto result = add_block(request_size).allocate(request_size, alignment_);
      ASSERT(result.has_value(), "allocation after block growth failed");
      allocation = std::move(*result);
    }
    {
      std::lock_guard lock(allocations_mutex_);
      allocations_.insert(allocation);
    }
    return AllocationInfo{
      .offset = allocation.first,
      .resized = !reused_existing,
    };
  }

  void DeviceHeapAllocator::deallocate(vk::DeviceSize offset) noexcept
  {
    std::lock_guard lock(allocations_mutex_);
    const auto it = allocations_.find(offset);
    ASSERT(it != allocations_.end(), "double free or unknown heap offset", offset);
    auto allocation = it->second;
    allocations_.erase(it);
    blocks_[allocation.block_number].deallocate(allocation);
  }

  HeapBuffer::HeapBuffer(
    const VulkanContext& context,
    vk::BufferUsageFlags usage_flags,
    vk::DeviceSize start_byte_size,
    uint32_t alignment)
    : buffer_(context, usage_flags, start_byte_size)
    , heap_(start_byte_size, alignment)
  {}

  vk::DeviceSize HeapBuffer::allocate(vk::DeviceSize size) noexcept
  {
    const auto alloc = heap_.allocate(size);
    if (alloc.resized && heap_.size() > buffer_.capacity()) {
      immediate_submit(buffer_.context(), [&](vk::CommandBuffer cmd) {
        buffer_.resize(cmd, heap_.size());
      });
    }
    return alloc.offset;
  }

  void HeapBuffer::free(vk::DeviceSize offset) noexcept { heap_.deallocate(offset); }

  void HeapBuffer::write(
    vk::CommandBuffer command_buffer,
    std::span<const std::byte> src,
    vk::DeviceSize offset)
  {
    buffer_.write(command_buffer, src, offset);
  }

  vk::DeviceSize HeapBuffer::allocate_and_write(
    vk::CommandBuffer command_buffer,
    std::span<const std::byte> src) noexcept
  {
    const auto offset = allocate(src.size());
    write(command_buffer, src, offset);
    return offset;
  }

  VertexHeapBuffer::VertexHeapBuffer(
    const VulkanContext& context,
    vk::DeviceSize start_byte_size,
    uint32_t alignment)
    : HeapBuffer(
        context,
        vk::BufferUsageFlagBits::eVertexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        start_byte_size,
        alignment)
  {}

  IndexHeapBuffer::IndexHeapBuffer(
    const VulkanContext& context,
    vk::DeviceSize start_byte_size,
    uint32_t alignment)
    : HeapBuffer(
        context,
        vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst,
        start_byte_size,
        alignment)
  {}

  void copy_buffer(vk::CommandBuffer command_buffer, BufferRegion src, BufferRegion dst)
  {
    ASSERT(src.context != nullptr && dst.context != nullptr, "null buffer region context");
    ASSERT(src.context == dst.context, "copy between buffers from different VulkanContext objects");
    ASSERT(dst.size >= src.size, "copy would overflow destination buffer");

    vk::BufferCopy buffer_copy{};
    buffer_copy.srcOffset = src.offset;
    buffer_copy.dstOffset = dst.offset;
    buffer_copy.size = src.size;
    command_buffer.copyBuffer(src.buffer, dst.buffer, buffer_copy);
  }

  Image::Image(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    vk::ImageUsageFlags usage_flags,
    vk::ImageAspectFlags aspect_flags,
    vk::MemoryPropertyFlags memory_properties,
    uint32_t mip_levels,
    bool create_view)
    : context_(&context)
    , extent_(extent)
    , size_(static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height) * format_byte_size(format))
    , format_(format)
    , mip_levels_(mip_levels)
    , layout_(vk::ImageLayout::eUndefined)
    , aspect_flags_(aspect_flags)
  {
    ASSERT(mip_levels > 0, "mip levels must be >= 1");

    vk::ImageCreateInfo image_create_info{};
    image_create_info.imageType = vk::ImageType::e2D;
    image_create_info.format = format_;
    image_create_info.extent = extent_;
    image_create_info.mipLevels = mip_levels_;
    image_create_info.arrayLayers = 1;
    image_create_info.samples = vk::SampleCountFlagBits::e1;
    image_create_info.tiling = vk::ImageTiling::eOptimal;
    image_create_info.usage = usage_flags;
    image_create_info.sharingMode = vk::SharingMode::eExclusive;
    image_create_info.initialLayout = layout_;

    VmaAllocationCreateInfo allocation_create_info{};
    allocation_create_info.usage = VMA_MEMORY_USAGE_AUTO;
    allocation_create_info.requiredFlags = static_cast<VkMemoryPropertyFlags>(memory_properties);
    const VkResult result = vmaCreateImage(
      context.allocator,
      reinterpret_cast<VkImageCreateInfo*>(&image_create_info),
      &allocation_create_info,
      reinterpret_cast<VkImage*>(&image_),
      &allocation_,
      nullptr);
    ASSERT(result == VK_SUCCESS, "vmaCreateImage failed", static_cast<int>(result));

    if (create_view) {
      image_view_ = create_image_view(0, mip_levels_);
      owns_image_view_ = true;
    }
  }

  Image::~Image()
  {
    if (context_ == nullptr) {
      return;
    }
    if (owns_image_view_ && image_view_) {
      context_->vk_device().destroyImageView(image_view_);
      image_view_ = nullptr;
    }
    if (owns_image_ && image_) {
      vmaDestroyImage(context_->allocator, static_cast<VkImage>(image_), allocation_);
      image_ = nullptr;
      allocation_ = VK_NULL_HANDLE;
    }
  }

  Image::Image(Image&& other) noexcept { *this = std::move(other); }

  Image& Image::operator=(Image&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    this->~Image();
    context_ = other.context_;
    image_ = other.image_;
    image_view_ = other.image_view_;
    allocation_ = other.allocation_;
    extent_ = other.extent_;
    size_ = other.size_;
    format_ = other.format_;
    mip_levels_ = other.mip_levels_;
    layout_ = other.layout_;
    aspect_flags_ = other.aspect_flags_;
    owns_image_ = other.owns_image_;
    owns_image_view_ = other.owns_image_view_;

    other.context_ = nullptr;
    other.image_ = nullptr;
    other.image_view_ = nullptr;
    other.allocation_ = VK_NULL_HANDLE;
    other.owns_image_ = false;
    other.owns_image_view_ = false;
    return *this;
  }

  void Image::transition_layout(vk::CommandBuffer command_buffer, vk::ImageLayout new_layout)
  {
    transition_layout(command_buffer, new_layout, 0, mip_levels_, false);
  }

  void Image::transition_layout(
    vk::CommandBuffer command_buffer,
    vk::ImageLayout new_layout,
    uint32_t mip_level,
    uint32_t mip_counts,
    bool ignore_previous_layout)
  {
    if (!ignore_previous_layout && new_layout == layout_) {
      return;
    }

    vk::ImageSubresourceRange range{};
    range.aspectMask = aspect_flags_;
    range.baseMipLevel = mip_level;
    range.levelCount = mip_counts;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = layout_;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.image = image_;
    barrier.subresourceRange = range;

    switch (layout_) {
    case vk::ImageLayout::eUndefined:
      barrier.srcAccessMask = {};
      break;
    case vk::ImageLayout::ePreinitialized:
      barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite;
      break;
    case vk::ImageLayout::eColorAttachmentOptimal:
      barrier.srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
      barrier.srcAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
      break;
    case vk::ImageLayout::eTransferSrcOptimal:
      barrier.srcAccessMask = vk::AccessFlagBits::eTransferRead;
      break;
    case vk::ImageLayout::eTransferDstOptimal:
      barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      break;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead;
      break;
    case vk::ImageLayout::ePresentSrcKHR:
      barrier.srcAccessMask = vk::AccessFlagBits::eNoneKHR;
      break;
    case vk::ImageLayout::eGeneral:
      barrier.srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
      break;
    default:
      barrier.srcAccessMask = {};
      break;
    }

    switch (new_layout) {
    case vk::ImageLayout::eTransferDstOptimal:
      barrier.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
      break;
    case vk::ImageLayout::eTransferSrcOptimal:
      barrier.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      break;
    case vk::ImageLayout::eColorAttachmentOptimal:
      barrier.dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite;
      break;
    case vk::ImageLayout::eDepthStencilAttachmentOptimal:
      barrier.dstAccessMask = vk::AccessFlagBits::eDepthStencilAttachmentWrite;
      break;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      if (barrier.srcAccessMask == vk::AccessFlags{}) {
        barrier.srcAccessMask = vk::AccessFlagBits::eHostWrite | vk::AccessFlagBits::eTransferWrite;
      }
      barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead;
      break;
    case vk::ImageLayout::ePresentSrcKHR:
      barrier.dstAccessMask = vk::AccessFlagBits::eNoneKHR;
      break;
    case vk::ImageLayout::eGeneral:
      barrier.dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
      break;
    default:
      barrier.dstAccessMask = {};
      break;
    }

    command_buffer.pipelineBarrier(
      vk::PipelineStageFlagBits::eAllCommands,
      vk::PipelineStageFlagBits::eAllCommands,
      {},
      {},
      {},
      barrier);
    layout_ = new_layout;
  }

  void Image::write(vk::CommandBuffer command_buffer, std::span<const std::byte> src)
  {
    ASSERT(src.size() <= size_, "image upload size exceeds allocation");
    HostBuffer staging(*context_, src.size(), vk::BufferUsageFlagBits::eTransferSrc);
    staging.write(src);

    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect_flags_;
    region.imageSubresource.mipLevel = mip_levels_ - 1;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D{0, 0, 0};
    region.imageExtent = extent_;
    command_buffer.copyBufferToImage(staging.buffer(), image_, layout_, region);
  }

  HostBuffer Image::read_to_host_buffer(vk::CommandBuffer command_buffer) noexcept
  {
    HostBuffer stage_buffer(*context_, size_, vk::BufferUsageFlagBits::eTransferDst);
    transition_layout(command_buffer, vk::ImageLayout::eTransferSrcOptimal);

    vk::BufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = aspect_flags_;
    region.imageSubresource.mipLevel = mip_levels_ - 1;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = vk::Offset3D{0, 0, 0};
    region.imageExtent = extent_;
    command_buffer.copyImageToBuffer(image_, layout_, stage_buffer.buffer(), region);
    return stage_buffer;
  }

  vk::ImageView Image::create_image_view(uint32_t mip_level, uint32_t mip_levels_count)
  {
    vk::ImageViewCreateInfo create_info{};
    create_info.image = image_;
    create_info.viewType = vk::ImageViewType::e2D;
    create_info.format = format_;
    create_info.components = {
      vk::ComponentSwizzle::eIdentity,
      vk::ComponentSwizzle::eIdentity,
      vk::ComponentSwizzle::eIdentity,
      vk::ComponentSwizzle::eIdentity,
    };
    create_info.subresourceRange.aspectMask = aspect_flags_;
    create_info.subresourceRange.baseMipLevel = mip_level;
    create_info.subresourceRange.levelCount = mip_levels_count;
    create_info.subresourceRange.baseArrayLayer = 0;
    create_info.subresourceRange.layerCount = 1;
    const auto rv = context_->vk_device().createImageView(create_info);
    ASSERT(rv.result == vk::Result::eSuccess, "createImageView failed");
    return rv.value;
  }

  vk::Format Image::find_supported_format(
    const VulkanContext& context,
    std::span<const vk::Format> candidates,
    vk::ImageTiling tiling,
    vk::FormatFeatureFlags features)
  {
    for (auto format : candidates) {
      const vk::FormatProperties props = context.vk_physical_device().getFormatProperties(format);
      if (tiling == vk::ImageTiling::eLinear && (props.linearTilingFeatures & features) == features) {
        return format;
      }
      if (tiling == vk::ImageTiling::eOptimal && (props.optimalTilingFeatures & features) == features) {
        return format;
      }
    }
    ASSERT(false, "failed to find supported image format");
    return vk::Format::eUndefined;
  }

  bool Image::is_image_format_supported(
    const VulkanContext& context,
    vk::Format format,
    vk::ImageType image_type,
    vk::ImageTiling tiling,
    vk::ImageUsageFlags usage)
  {
    vk::PhysicalDeviceImageFormatInfo2 format_info{};
    format_info.format = format;
    format_info.type = image_type;
    format_info.tiling = tiling;
    format_info.usage = usage;
    format_info.flags = {};
    vk::ImageFormatProperties2 format_properties{};
    const vk::Result result = context.vk_physical_device().getImageFormatProperties2(&format_info, &format_properties);
    return result == vk::Result::eSuccess;
  }

  HostImage::HostImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    vk::ImageUsageFlags usage_flags,
    vk::ImageAspectFlags aspect_flags,
    uint32_t mip_levels)
    : Image(
        context,
        extent,
        format,
        usage_flags,
        aspect_flags,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        mip_levels,
        true)
  {}

  DeviceImage::DeviceImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    vk::ImageUsageFlags usage_flags,
    vk::ImageAspectFlags aspect_flags,
    uint32_t mip_levels,
    bool create_image_view)
    : Image(
        context,
        extent,
        format,
        usage_flags,
        aspect_flags,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        mip_levels,
        create_image_view)
  {}

  SwapchainImage::SwapchainImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    vk::Image image,
    vk::ImageView view)
  {
    context_ = &context;
    image_ = image;
    extent_ = extent;
    format_ = format;
    size_ = static_cast<size_t>(extent.width) * static_cast<size_t>(extent.height) * format_byte_size(format);
    aspect_flags_ = vk::ImageAspectFlagBits::eColor;
    mip_levels_ = 1;
    layout_ = vk::ImageLayout::eUndefined;
    owns_image_ = false;
    if (view) {
      image_view_ = view;
      owns_image_view_ = false;
    } else {
      image_view_ = create_image_view(0, 1);
      owns_image_view_ = true;
    }
  }

  SwapchainImage::~SwapchainImage() { owns_image_ = false; }

  TextureImage::TextureImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    vk::ImageUsageFlags usage_flags,
    uint32_t mip_levels)
    : DeviceImage(
        context,
        extent,
        format,
        usage_flags | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled,
        vk::ImageAspectFlagBits::eColor,
        mip_levels,
        true)
  {}

  DepthImage::DepthImage(const VulkanContext& context, vk::Extent3D extent, uint32_t mip_levels)
    : DeviceImage(
        context,
        extent,
        find_supported_format(
          context,
          std::array<vk::Format, 3>{
            vk::Format::eD32Sfloat,
            vk::Format::eD32SfloatS8Uint,
            vk::Format::eD24UnormS8Uint,
          },
          vk::ImageTiling::eOptimal,
          vk::FormatFeatureFlagBits::eDepthStencilAttachment),
        vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
        vk::ImageAspectFlagBits::eDepth,
        mip_levels,
        true)
  {}

  vk::RenderingAttachmentInfo DepthImage::attachment_info() const
  {
    vk::RenderingAttachmentInfo info{};
    info.imageView = image_view_;
    info.imageLayout = layout_;
    info.loadOp = vk::AttachmentLoadOp::eClear;
    info.storeOp = vk::AttachmentStoreOp::eStore;
    info.clearValue = vk::ClearDepthStencilValue(1.0f, 0);
    return info;
  }

  ColorAttachmentImage::ColorAttachmentImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    uint32_t mip_levels)
    : DeviceImage(
        context,
        extent,
        format,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eInputAttachment |
          vk::ImageUsageFlagBits::eTransferSrc,
        vk::ImageAspectFlagBits::eColor,
        mip_levels,
        true)
  {}

  vk::RenderingAttachmentInfo ColorAttachmentImage::attachment_info() const
  {
    vk::RenderingAttachmentInfo info{};
    info.imageView = image_view_;
    info.imageLayout = layout_;
    info.loadOp = vk::AttachmentLoadOp::eClear;
    info.storeOp = vk::AttachmentStoreOp::eStore;
    info.clearValue = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    return info;
  }

  StorageImage::StorageImage(
    const VulkanContext& context,
    vk::Extent3D extent,
    vk::Format format,
    uint32_t mip_levels,
    bool create_image_view)
    : DeviceImage(
        context,
        extent,
        format,
        vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled,
        vk::ImageAspectFlagBits::eColor,
        mip_levels,
        create_image_view)
  {}

  struct FrameRecorder::Impl {
    enum class ResourceKind {
      Buffer,
      Image,
    };

    struct ResourceUsage {
      ResourceKind kind = ResourceKind::Buffer;
      uint64_t resource_key = 0;
      vk::Buffer buffer{};
      vk::Image image{};
      vk::DeviceSize offset = 0;
      vk::DeviceSize size = VK_WHOLE_SIZE;
      vk::ImageSubresourceRange subresource_range{};
      vk::ImageLayout layout = vk::ImageLayout::eUndefined;
      vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eAllCommands;
      vk::AccessFlags2 access = vk::AccessFlagBits2::eMemoryRead;
      bool writes = false;
    };

    struct ResourceState {
      ResourceKind kind = ResourceKind::Buffer;
      QueueTarget queue_target = QueueTarget::Graphics;
      uint32_t queue_family = std::numeric_limits<uint32_t>::max();
      vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eAllCommands;
      vk::AccessFlags2 access = vk::AccessFlagBits2::eMemoryRead;
      vk::ImageLayout layout = vk::ImageLayout::eUndefined;
      bool writes = false;
    };

    struct EnqueuedSubmission {
      RecordedCommandBuffer command_buffer{};
      std::vector<ResourceUsage> usages{};
    };

    struct ThreadPools {
      vk::CommandPool graphics_pool{};
      vk::CommandPool compute_pool{};
    };

    struct FrameState {
      std::vector<ThreadPools> thread_pools{};
      std::unordered_map<std::thread::id, uint32_t> thread_slots{};
      std::vector<EnqueuedSubmission> graphics_submissions{};
      std::vector<EnqueuedSubmission> compute_submissions{};
      uint64_t completion_timeline_value = 0;
      std::mutex mutex{};
    };

    const VulkanContext* context = nullptr;
    CreateInfo create_info;
    std::vector<std::unique_ptr<FrameState>> frames{};
    vk::Semaphore timeline_semaphore{};
    uint64_t timeline_counter = 0;
    uint64_t active_frame_number = 0;
    std::unordered_map<uint64_t, std::vector<ResourceUsage>> pending_usages{};
    std::unordered_map<uint64_t, ResourceState> resource_states{};
    std::mutex resource_mutex{};

    Impl(const VulkanContext& in_context, CreateInfo in_create_info)
      : context(&in_context)
      , create_info(in_create_info)
    {
      if (create_info.frames_in_flight == 0) {
        create_info.frames_in_flight = 1;
      }
      if (create_info.max_recording_threads == 0) {
        create_info.max_recording_threads = 1;
      }
      ASSERT(context->feature_support.synchronization2, "FrameRecorder requires synchronization2 support");
      ASSERT(context->feature_support.timeline_semaphore, "FrameRecorder requires timeline semaphore support");

      vk::SemaphoreTypeCreateInfo type_info{};
      type_info.semaphoreType = vk::SemaphoreType::eTimeline;
      type_info.initialValue = 0;
      vk::SemaphoreCreateInfo semaphore_info{};
      semaphore_info.pNext = &type_info;
      const auto semaphore_rv = context->vk_device().createSemaphore(semaphore_info);
      ASSERT(semaphore_rv.result == vk::Result::eSuccess, "FrameRecorder failed to create timeline semaphore");
      timeline_semaphore = semaphore_rv.value;

      frames.reserve(create_info.frames_in_flight);
      for (uint32_t i = 0; i < create_info.frames_in_flight; ++i) {
        auto frame = std::make_unique<FrameState>();
        frame->thread_pools.resize(create_info.max_recording_threads);
        frames.push_back(std::move(frame));
      }
    }

    ~Impl()
    {
      if (!context) {
        return;
      }
      for (auto& frame : frames) {
        for (auto& pools : frame->thread_pools) {
          if (pools.graphics_pool) {
            context->vk_device().destroyCommandPool(pools.graphics_pool);
            pools.graphics_pool = nullptr;
          }
          if (pools.compute_pool) {
            context->vk_device().destroyCommandPool(pools.compute_pool);
            pools.compute_pool = nullptr;
          }
        }
      }
      if (timeline_semaphore) {
        context->vk_device().destroySemaphore(timeline_semaphore);
        timeline_semaphore = nullptr;
      }
    }

    [[nodiscard]] uint32_t queue_family_for(QueueTarget queue_target) const
    {
      return queue_target == QueueTarget::Graphics ? context->graphics_queue_family : context->compute_queue_family;
    }

    [[nodiscard]] vk::Queue queue_for(QueueTarget queue_target) const
    {
      return queue_target == QueueTarget::Graphics ? context->graphics_queue : context->compute_queue;
    }

    [[nodiscard]] static uint64_t command_key(vk::CommandBuffer command_buffer)
    {
      return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkCommandBuffer>(command_buffer)));
    }

    [[nodiscard]] static uint64_t buffer_key(vk::Buffer buffer)
    {
      return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkBuffer>(buffer)));
    }

    [[nodiscard]] static uint64_t image_key(vk::Image image)
    {
      return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(static_cast<VkImage>(image)));
    }

    uint32_t acquire_thread_slot(FrameState& frame_state)
    {
      std::lock_guard lock(frame_state.mutex);
      const std::thread::id tid = std::this_thread::get_id();
      if (const auto it = frame_state.thread_slots.find(tid); it != frame_state.thread_slots.end()) {
        return it->second;
      }
      const uint32_t slot = static_cast<uint32_t>(frame_state.thread_slots.size() % create_info.max_recording_threads);
      frame_state.thread_slots[tid] = slot;
      return slot;
    }

    vk::CommandPool& command_pool_for(FrameState& frame_state, uint32_t thread_slot, QueueTarget queue_target)
    {
      ThreadPools& pools = frame_state.thread_pools[thread_slot];
      vk::CommandPool& selected_pool =
        queue_target == QueueTarget::Graphics ? pools.graphics_pool : pools.compute_pool;
      if (selected_pool) {
        return selected_pool;
      }
      vk::CommandPoolCreateInfo pool_info{};
      pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
      pool_info.queueFamilyIndex = queue_family_for(queue_target);
      const auto pool_rv = context->vk_device().createCommandPool(pool_info);
      ASSERT(pool_rv.result == vk::Result::eSuccess, "FrameRecorder failed to create command pool");
      selected_pool = pool_rv.value;
      return selected_pool;
    }
  };

  FrameRecorder::FrameRecorder(const VulkanContext& context)
    : FrameRecorder(context, CreateInfo{})
  {}

  FrameRecorder::FrameRecorder(const VulkanContext& context, CreateInfo create_info)
    : impl_(std::make_unique<Impl>(context, create_info))
  {}

  FrameRecorder::~FrameRecorder() = default;
  FrameRecorder::FrameRecorder(FrameRecorder&&) noexcept = default;
  FrameRecorder& FrameRecorder::operator=(FrameRecorder&&) noexcept = default;

  std::expected<void, std::string> FrameRecorder::begin_frame(uint64_t frame_index)
  {
    if (!impl_) {
      return std::unexpected("FrameRecorder is not initialized");
    }
    impl_->active_frame_number = frame_index;
    FrameRecorder::Impl::FrameState& frame_state =
      *impl_->frames[frame_index % impl_->create_info.frames_in_flight];

    if (frame_state.completion_timeline_value > 0) {
      vk::SemaphoreWaitInfo wait_info{};
      wait_info.semaphoreCount = 1;
      wait_info.pSemaphores = &impl_->timeline_semaphore;
      wait_info.pValues = &frame_state.completion_timeline_value;
      const auto wait_result = impl_->context->vk_device().waitSemaphores(wait_info, std::numeric_limits<uint64_t>::max());
      if (wait_result != vk::Result::eSuccess) {
        return std::unexpected("FrameRecorder waitSemaphores failed");
      }
    }

    std::lock_guard lock(frame_state.mutex);
    frame_state.graphics_submissions.clear();
    frame_state.compute_submissions.clear();
    {
      std::lock_guard usage_lock(impl_->resource_mutex);
      impl_->pending_usages.clear();
    }
    for (auto& pools : frame_state.thread_pools) {
      if (pools.graphics_pool) {
        const auto reset_result = impl_->context->vk_device().resetCommandPool(
          pools.graphics_pool,
          vk::CommandPoolResetFlagBits::eReleaseResources);
        if (reset_result != vk::Result::eSuccess) {
          return std::unexpected("FrameRecorder resetCommandPool(graphics) failed");
        }
      }
      if (pools.compute_pool) {
        const auto reset_result = impl_->context->vk_device().resetCommandPool(
          pools.compute_pool,
          vk::CommandPoolResetFlagBits::eReleaseResources);
        if (reset_result != vk::Result::eSuccess) {
          return std::unexpected("FrameRecorder resetCommandPool(compute) failed");
        }
      }
    }
    return {};
  }

  std::expected<FrameRecorder::RecordedCommandBuffer, std::string> FrameRecorder::begin_recording(
    QueueTarget queue_target,
    vk::CommandBufferUsageFlags usage_flags)
  {
    if (!impl_) {
      return std::unexpected("FrameRecorder is not initialized");
    }
    FrameRecorder::Impl::FrameState& frame_state =
      *impl_->frames[impl_->active_frame_number % impl_->create_info.frames_in_flight];
    const uint32_t thread_slot = impl_->acquire_thread_slot(frame_state);
    vk::CommandPool& command_pool = impl_->command_pool_for(frame_state, thread_slot, queue_target);

    vk::CommandBufferAllocateInfo alloc_info{};
    alloc_info.commandPool = command_pool;
    alloc_info.level = vk::CommandBufferLevel::ePrimary;
    alloc_info.commandBufferCount = 1;
    const auto cmd_rv = impl_->context->vk_device().allocateCommandBuffers(alloc_info);
    if (cmd_rv.result != vk::Result::eSuccess) {
      return std::unexpected("FrameRecorder allocateCommandBuffers failed");
    }

    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = usage_flags;
    if (cmd_rv.value[0].begin(begin_info) != vk::Result::eSuccess) {
      return std::unexpected("FrameRecorder command buffer begin failed");
    }

    return RecordedCommandBuffer{
      .handle = cmd_rv.value[0],
      .queue_target = queue_target,
      .queue_family = impl_->queue_family_for(queue_target),
    };
  }

  std::expected<void, std::string>
  FrameRecorder::end_recording(const RecordedCommandBuffer& command_buffer)
  {
    if (!impl_) {
      return std::unexpected("FrameRecorder is not initialized");
    }
    if (!command_buffer.handle) {
      return std::unexpected("FrameRecorder end_recording received null command buffer");
    }
    if (command_buffer.handle.end() != vk::Result::eSuccess) {
      return std::unexpected("FrameRecorder command buffer end failed");
    }
    return {};
  }

  void FrameRecorder::declare_buffer_usage(
    const RecordedCommandBuffer& command_buffer,
    const BufferUsageDesc& usage_desc)
  {
    ASSERT(impl_ != nullptr, "FrameRecorder is not initialized");
    ASSERT(command_buffer.handle, "FrameRecorder::declare_buffer_usage requires a valid command buffer");
    ASSERT(usage_desc.buffer, "FrameRecorder::declare_buffer_usage requires a valid buffer");

    Impl::ResourceUsage usage{};
    usage.kind = Impl::ResourceKind::Buffer;
    usage.resource_key = Impl::buffer_key(usage_desc.buffer);
    usage.buffer = usage_desc.buffer;
    usage.offset = usage_desc.offset;
    usage.size = usage_desc.size;
    usage.stage = usage_desc.stage;
    usage.access = usage_desc.access;
    usage.writes = usage_desc.writes;

    std::lock_guard lock(impl_->resource_mutex);
    impl_->pending_usages[Impl::command_key(command_buffer.handle)].push_back(usage);
  }

  void FrameRecorder::declare_image_usage(
    const RecordedCommandBuffer& command_buffer,
    const ImageUsageDesc& usage_desc)
  {
    ASSERT(impl_ != nullptr, "FrameRecorder is not initialized");
    ASSERT(command_buffer.handle, "FrameRecorder::declare_image_usage requires a valid command buffer");
    ASSERT(usage_desc.image, "FrameRecorder::declare_image_usage requires a valid image");

    Impl::ResourceUsage usage{};
    usage.kind = Impl::ResourceKind::Image;
    usage.resource_key = Impl::image_key(usage_desc.image);
    usage.image = usage_desc.image;
    usage.subresource_range = usage_desc.subresource_range;
    usage.layout = usage_desc.layout;
    usage.stage = usage_desc.stage;
    usage.access = usage_desc.access;
    usage.writes = usage_desc.writes;

    std::lock_guard lock(impl_->resource_mutex);
    impl_->pending_usages[Impl::command_key(command_buffer.handle)].push_back(usage);
  }

  void FrameRecorder::enqueue_for_submit(const RecordedCommandBuffer& command_buffer)
  {
    ASSERT(impl_ != nullptr, "FrameRecorder is not initialized");
    FrameRecorder::Impl::FrameState& frame_state =
      *impl_->frames[impl_->active_frame_number % impl_->create_info.frames_in_flight];
    std::vector<Impl::ResourceUsage> usages{};
    {
      std::lock_guard usage_lock(impl_->resource_mutex);
      const uint64_t command_buffer_key = Impl::command_key(command_buffer.handle);
      if (const auto it = impl_->pending_usages.find(command_buffer_key); it != impl_->pending_usages.end()) {
        usages = std::move(it->second);
        impl_->pending_usages.erase(it);
      }
    }
    std::lock_guard lock(frame_state.mutex);
    Impl::EnqueuedSubmission submission{
      .command_buffer = command_buffer,
      .usages = std::move(usages),
    };
    if (command_buffer.queue_target == QueueTarget::Graphics) {
      frame_state.graphics_submissions.push_back(std::move(submission));
    } else {
      frame_state.compute_submissions.push_back(std::move(submission));
    }
  }

  std::expected<uint64_t, std::string> FrameRecorder::submit_frame()
  {
    if (!impl_) {
      return std::unexpected("FrameRecorder is not initialized");
    }
    FrameRecorder::Impl::FrameState& frame_state =
      *impl_->frames[impl_->active_frame_number % impl_->create_info.frames_in_flight];

    std::vector<Impl::EnqueuedSubmission> graphics_submissions{};
    std::vector<Impl::EnqueuedSubmission> compute_submissions{};
    {
      std::lock_guard lock(frame_state.mutex);
      graphics_submissions = frame_state.graphics_submissions;
      compute_submissions = frame_state.compute_submissions;
    }
    if (graphics_submissions.empty() && compute_submissions.empty()) {
      return impl_->timeline_counter;
    }

    constexpr size_t queue_count = 2;
    const auto queue_index = [](QueueTarget q) -> size_t {
      return q == QueueTarget::Graphics ? 0u : 1u;
    };
    const auto queue_from_index = [](size_t i) -> QueueTarget {
      return i == 0 ? QueueTarget::Graphics : QueueTarget::ComputeTransfer;
    };

    std::array<std::vector<Impl::EnqueuedSubmission>, queue_count> submissions_by_queue{};
    submissions_by_queue[queue_index(QueueTarget::Graphics)] = std::move(graphics_submissions);
    submissions_by_queue[queue_index(QueueTarget::ComputeTransfer)] = std::move(compute_submissions);

    std::array<std::vector<vk::BufferMemoryBarrier2>, queue_count> acquire_buffer_barriers{};
    std::array<std::vector<vk::ImageMemoryBarrier2>, queue_count> acquire_image_barriers{};
    std::array<std::vector<vk::BufferMemoryBarrier2>, queue_count> release_buffer_barriers{};
    std::array<std::vector<vk::ImageMemoryBarrier2>, queue_count> release_image_barriers{};
    std::array<bool, queue_count> queue_wait_from_other{false, false};
    std::array<bool, queue_count> queue_has_work{false, false};

    for (size_t queue_i = 0; queue_i < queue_count; ++queue_i) {
      queue_has_work[queue_i] = !submissions_by_queue[queue_i].empty();
      for (const auto& submission : submissions_by_queue[queue_i]) {
        std::vector<vk::BufferMemoryBarrier2> same_queue_buffer_barriers{};
        std::vector<vk::ImageMemoryBarrier2> same_queue_image_barriers{};
        same_queue_buffer_barriers.reserve(submission.usages.size());
        same_queue_image_barriers.reserve(submission.usages.size());

        {
          std::lock_guard state_lock(impl_->resource_mutex);
          for (const auto& usage : submission.usages) {
            const auto prev_it = impl_->resource_states.find(usage.resource_key);
            const bool has_prev = prev_it != impl_->resource_states.end();
            const bool same_queue_family =
              has_prev && prev_it->second.queue_family == submission.command_buffer.queue_family;
            const bool different_queue_family =
              has_prev && prev_it->second.queue_family != submission.command_buffer.queue_family;

            if (has_prev && same_queue_family && (prev_it->second.writes || usage.writes)) {
              if (usage.kind == Impl::ResourceKind::Buffer) {
                vk::BufferMemoryBarrier2 barrier{};
                barrier.srcStageMask = prev_it->second.stage;
                barrier.srcAccessMask = prev_it->second.access;
                barrier.dstStageMask = usage.stage;
                barrier.dstAccessMask = usage.access;
                barrier.srcQueueFamilyIndex = submission.command_buffer.queue_family;
                barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                barrier.buffer = usage.buffer;
                barrier.offset = usage.offset;
                barrier.size = usage.size;
                same_queue_buffer_barriers.push_back(barrier);
              } else {
                vk::ImageMemoryBarrier2 barrier{};
                barrier.srcStageMask = prev_it->second.stage;
                barrier.srcAccessMask = prev_it->second.access;
                barrier.dstStageMask = usage.stage;
                barrier.dstAccessMask = usage.access;
                barrier.oldLayout = prev_it->second.layout;
                barrier.newLayout = usage.layout;
                barrier.srcQueueFamilyIndex = submission.command_buffer.queue_family;
                barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                barrier.image = usage.image;
                barrier.subresourceRange = usage.subresource_range;
                same_queue_image_barriers.push_back(barrier);
              }
            } else if (has_prev && different_queue_family) {
              const size_t producer_queue_i = queue_index(prev_it->second.queue_target);
              const size_t consumer_queue_i = queue_index(submission.command_buffer.queue_target);
              queue_wait_from_other[consumer_queue_i] = true;
              queue_has_work[producer_queue_i] = true;

              if (usage.kind == Impl::ResourceKind::Buffer) {
                vk::BufferMemoryBarrier2 release_barrier{};
                release_barrier.srcStageMask = prev_it->second.stage;
                release_barrier.srcAccessMask = prev_it->second.access;
                release_barrier.dstStageMask = usage.stage;
                release_barrier.dstAccessMask = {};
                release_barrier.srcQueueFamilyIndex = prev_it->second.queue_family;
                release_barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                release_barrier.buffer = usage.buffer;
                release_barrier.offset = usage.offset;
                release_barrier.size = usage.size;
                release_buffer_barriers[producer_queue_i].push_back(release_barrier);

                vk::BufferMemoryBarrier2 acquire_barrier{};
                acquire_barrier.srcStageMask = prev_it->second.stage;
                acquire_barrier.srcAccessMask = {};
                acquire_barrier.dstStageMask = usage.stage;
                acquire_barrier.dstAccessMask = usage.access;
                acquire_barrier.srcQueueFamilyIndex = prev_it->second.queue_family;
                acquire_barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                acquire_barrier.buffer = usage.buffer;
                acquire_barrier.offset = usage.offset;
                acquire_barrier.size = usage.size;
                acquire_buffer_barriers[consumer_queue_i].push_back(acquire_barrier);
              } else {
                vk::ImageMemoryBarrier2 release_barrier{};
                release_barrier.srcStageMask = prev_it->second.stage;
                release_barrier.srcAccessMask = prev_it->second.access;
                release_barrier.dstStageMask = usage.stage;
                release_barrier.dstAccessMask = {};
                release_barrier.oldLayout = prev_it->second.layout;
                release_barrier.newLayout = usage.layout;
                release_barrier.srcQueueFamilyIndex = prev_it->second.queue_family;
                release_barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                release_barrier.image = usage.image;
                release_barrier.subresourceRange = usage.subresource_range;
                release_image_barriers[producer_queue_i].push_back(release_barrier);

                vk::ImageMemoryBarrier2 acquire_barrier{};
                acquire_barrier.srcStageMask = prev_it->second.stage;
                acquire_barrier.srcAccessMask = {};
                acquire_barrier.dstStageMask = usage.stage;
                acquire_barrier.dstAccessMask = usage.access;
                acquire_barrier.oldLayout = prev_it->second.layout;
                acquire_barrier.newLayout = usage.layout;
                acquire_barrier.srcQueueFamilyIndex = prev_it->second.queue_family;
                acquire_barrier.dstQueueFamilyIndex = submission.command_buffer.queue_family;
                acquire_barrier.image = usage.image;
                acquire_barrier.subresourceRange = usage.subresource_range;
                acquire_image_barriers[consumer_queue_i].push_back(acquire_barrier);
              }
            }

            impl_->resource_states[usage.resource_key] = Impl::ResourceState{
              .kind = usage.kind,
              .queue_target = submission.command_buffer.queue_target,
              .queue_family = submission.command_buffer.queue_family,
              .stage = usage.stage,
              .access = usage.access,
              .layout = usage.layout,
              .writes = usage.writes,
            };
          }
        }

        if (!same_queue_buffer_barriers.empty() || !same_queue_image_barriers.empty()) {
          const uint32_t thread_slot = impl_->acquire_thread_slot(frame_state);
          const QueueTarget queue_target = submission.command_buffer.queue_target;
          vk::CommandPool& command_pool = impl_->command_pool_for(frame_state, thread_slot, queue_target);
          vk::CommandBufferAllocateInfo alloc_info{};
          alloc_info.commandPool = command_pool;
          alloc_info.level = vk::CommandBufferLevel::ePrimary;
          alloc_info.commandBufferCount = 1;
          const auto barrier_cmd_rv = impl_->context->vk_device().allocateCommandBuffers(alloc_info);
          if (barrier_cmd_rv.result != vk::Result::eSuccess) {
            return std::unexpected("FrameRecorder failed to allocate same-queue barrier command buffer");
          }
          vk::CommandBuffer barrier_cmd = barrier_cmd_rv.value[0];
          vk::CommandBufferBeginInfo begin_info{};
          begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
          if (barrier_cmd.begin(begin_info) != vk::Result::eSuccess) {
            return std::unexpected("FrameRecorder failed to begin same-queue barrier command buffer");
          }
          vk::DependencyInfo dependency_info{};
          dependency_info.bufferMemoryBarrierCount = static_cast<uint32_t>(same_queue_buffer_barriers.size());
          dependency_info.pBufferMemoryBarriers = same_queue_buffer_barriers.data();
          dependency_info.imageMemoryBarrierCount = static_cast<uint32_t>(same_queue_image_barriers.size());
          dependency_info.pImageMemoryBarriers = same_queue_image_barriers.data();
          barrier_cmd.pipelineBarrier2(dependency_info);
          if (barrier_cmd.end() != vk::Result::eSuccess) {
            return std::unexpected("FrameRecorder failed to end same-queue barrier command buffer");
          }
          Impl::EnqueuedSubmission barrier_submission{};
          barrier_submission.command_buffer = {
            .handle = barrier_cmd,
            .queue_target = submission.command_buffer.queue_target,
            .queue_family = submission.command_buffer.queue_family,
          };
          submissions_by_queue[queue_i].insert(
            submissions_by_queue[queue_i].begin(),
            std::move(barrier_submission));
        }
      }
    }

    if (queue_wait_from_other[queue_index(QueueTarget::Graphics)] &&
      queue_wait_from_other[queue_index(QueueTarget::ComputeTransfer)]) {
      return std::unexpected("FrameRecorder detected cyclic cross-queue dependencies in a single frame");
    }

    const auto record_queue_barriers = [&](QueueTarget queue_target,
                                       const std::vector<vk::BufferMemoryBarrier2>& buffer_barriers,
                                       const std::vector<vk::ImageMemoryBarrier2>& image_barriers)
      -> std::expected<vk::CommandBuffer, std::string> {
      if (buffer_barriers.empty() && image_barriers.empty()) {
        return vk::CommandBuffer{};
      }
      const uint32_t thread_slot = impl_->acquire_thread_slot(frame_state);
      vk::CommandPool& command_pool = impl_->command_pool_for(frame_state, thread_slot, queue_target);
      vk::CommandBufferAllocateInfo alloc_info{};
      alloc_info.commandPool = command_pool;
      alloc_info.level = vk::CommandBufferLevel::ePrimary;
      alloc_info.commandBufferCount = 1;
      const auto cmd_rv = impl_->context->vk_device().allocateCommandBuffers(alloc_info);
      if (cmd_rv.result != vk::Result::eSuccess) {
        return std::unexpected("FrameRecorder failed to allocate cross-queue barrier command buffer");
      }
      vk::CommandBuffer cmd = cmd_rv.value[0];
      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      if (cmd.begin(begin_info) != vk::Result::eSuccess) {
        return std::unexpected("FrameRecorder failed to begin cross-queue barrier command buffer");
      }
      vk::DependencyInfo dependency_info{};
      dependency_info.bufferMemoryBarrierCount = static_cast<uint32_t>(buffer_barriers.size());
      dependency_info.pBufferMemoryBarriers = buffer_barriers.data();
      dependency_info.imageMemoryBarrierCount = static_cast<uint32_t>(image_barriers.size());
      dependency_info.pImageMemoryBarriers = image_barriers.data();
      cmd.pipelineBarrier2(dependency_info);
      if (cmd.end() != vk::Result::eSuccess) {
        return std::unexpected("FrameRecorder failed to end cross-queue barrier command buffer");
      }
      return cmd;
    };

    for (size_t queue_i = 0; queue_i < queue_count; ++queue_i) {
      const QueueTarget target = queue_from_index(queue_i);
      if (auto barrier_cmd = record_queue_barriers(target, acquire_buffer_barriers[queue_i], acquire_image_barriers[queue_i]);
        barrier_cmd.has_value() && barrier_cmd.value()) {
        Impl::EnqueuedSubmission acquire_submission{};
        acquire_submission.command_buffer = {
          .handle = barrier_cmd.value(),
          .queue_target = target,
          .queue_family = impl_->queue_family_for(target),
        };
        submissions_by_queue[queue_i].insert(submissions_by_queue[queue_i].begin(), std::move(acquire_submission));
        queue_has_work[queue_i] = true;
      } else if (!barrier_cmd.has_value()) {
        return std::unexpected(barrier_cmd.error());
      }

      if (auto barrier_cmd = record_queue_barriers(target, release_buffer_barriers[queue_i], release_image_barriers[queue_i]);
        barrier_cmd.has_value() && barrier_cmd.value()) {
        Impl::EnqueuedSubmission release_submission{};
        release_submission.command_buffer = {
          .handle = barrier_cmd.value(),
          .queue_target = target,
          .queue_family = impl_->queue_family_for(target),
        };
        submissions_by_queue[queue_i].push_back(std::move(release_submission));
        queue_has_work[queue_i] = true;
      } else if (!barrier_cmd.has_value()) {
        return std::unexpected(barrier_cmd.error());
      }
    }

    std::array<uint64_t, queue_count> queue_signal_values{0, 0};
    uint64_t final_timeline_value = impl_->timeline_counter;
    const auto submit_queue = [&](QueueTarget queue_target) -> std::expected<void, std::string> {
      const size_t idx = queue_index(queue_target);
      if (!queue_has_work[idx] || submissions_by_queue[idx].empty()) {
        return {};
      }
      std::vector<vk::CommandBufferSubmitInfo> command_infos{};
      command_infos.reserve(submissions_by_queue[idx].size());
      for (const auto& submission : submissions_by_queue[idx]) {
        vk::CommandBufferSubmitInfo cmd_info{};
        cmd_info.commandBuffer = submission.command_buffer.handle;
        command_infos.push_back(cmd_info);
      }

      std::vector<vk::SemaphoreSubmitInfo> wait_infos{};
      const size_t other_idx = idx == 0 ? 1 : 0;
      if (queue_wait_from_other[idx] && queue_signal_values[other_idx] > 0) {
        vk::SemaphoreSubmitInfo wait_info{};
        wait_info.semaphore = impl_->timeline_semaphore;
        wait_info.value = queue_signal_values[other_idx];
        wait_info.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
        wait_infos.push_back(wait_info);
      }

      const uint64_t signal_value = ++impl_->timeline_counter;
      vk::SemaphoreSubmitInfo signal_info{};
      signal_info.semaphore = impl_->timeline_semaphore;
      signal_info.value = signal_value;
      signal_info.stageMask = vk::PipelineStageFlagBits2::eAllCommands;
      signal_info.deviceIndex = 0;

      vk::SubmitInfo2 submit_info{};
      submit_info.waitSemaphoreInfoCount = static_cast<uint32_t>(wait_infos.size());
      submit_info.pWaitSemaphoreInfos = wait_infos.data();
      submit_info.commandBufferInfoCount = static_cast<uint32_t>(command_infos.size());
      submit_info.pCommandBufferInfos = command_infos.data();
      submit_info.signalSemaphoreInfoCount = 1;
      submit_info.pSignalSemaphoreInfos = &signal_info;
      const auto submit_result = impl_->queue_for(queue_target).submit2(submit_info, vk::Fence{});
      if (submit_result != vk::Result::eSuccess) {
        return std::unexpected("FrameRecorder queue submit2 failed");
      }
      queue_signal_values[idx] = signal_value;
      final_timeline_value = signal_value;
      return {};
    };

    if (queue_wait_from_other[queue_index(QueueTarget::ComputeTransfer)]) {
      if (const auto submit_result = submit_queue(QueueTarget::Graphics); !submit_result.has_value()) {
        return std::unexpected(submit_result.error());
      }
      if (const auto submit_result = submit_queue(QueueTarget::ComputeTransfer); !submit_result.has_value()) {
        return std::unexpected(submit_result.error());
      }
    } else {
      if (const auto submit_result = submit_queue(QueueTarget::ComputeTransfer); !submit_result.has_value()) {
        return std::unexpected(submit_result.error());
      }
      if (const auto submit_result = submit_queue(QueueTarget::Graphics); !submit_result.has_value()) {
        return std::unexpected(submit_result.error());
      }
    }

    frame_state.completion_timeline_value = final_timeline_value;
    return final_timeline_value;
  }

  uint64_t FrameRecorder::last_timeline_value() const noexcept
  {
    if (!impl_) {
      return 0;
    }
    return impl_->timeline_counter;
  }

  vk::Semaphore FrameRecorder::timeline_semaphore() const noexcept
  {
    if (!impl_) {
      return vk::Semaphore{};
    }
    return impl_->timeline_semaphore;
  }

  GraphicsPipeline::~GraphicsPipeline()
  {
    if (!context_) {
      return;
    }
    if (pipeline_) {
      context_->vk_device().destroyPipeline(pipeline_);
      pipeline_ = nullptr;
    }
    if (layout_) {
      context_->vk_device().destroyPipelineLayout(layout_);
      layout_ = nullptr;
    }
  }

  GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
  {
    *this = std::move(other);
  }

  GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept
  {
    if (this == &other) {
      return *this;
    }
    if (context_ && pipeline_) {
      context_->vk_device().destroyPipeline(pipeline_);
    }
    if (context_ && layout_) {
      context_->vk_device().destroyPipelineLayout(layout_);
    }
    context_ = other.context_;
    layout_ = other.layout_;
    pipeline_ = other.pipeline_;

    other.context_ = nullptr;
    other.layout_ = nullptr;
    other.pipeline_ = nullptr;
    return *this;
  }

  void GraphicsPipeline::bind(vk::CommandBuffer command_buffer) const
  {
    ASSERT(pipeline_, "graphics pipeline is not initialized");
    command_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline_);
  }

  std::expected<GraphicsPipeline, std::string>
  build_graphics_pipeline(const VulkanContext& context, const GraphicsPipelineDesc& desc)
  {
    if (desc.shader_stages.empty()) {
      return std::unexpected("graphics pipeline requires at least one shader stage");
    }
    if (desc.color_attachment_formats.empty()) {
      return std::unexpected("graphics pipeline requires at least one color attachment format");
    }

    std::vector<vk::PipelineShaderStageCreateInfo> shader_stages;
    shader_stages.reserve(desc.shader_stages.size());
    for (const auto& stage_desc : desc.shader_stages) {
      if (!stage_desc.module) {
        return std::unexpected("graphics pipeline shader stage has null module");
      }
      vk::PipelineShaderStageCreateInfo stage_info{};
      stage_info.stage = stage_desc.stage;
      stage_info.module = stage_desc.module;
      stage_info.pName = stage_desc.entry_point ? stage_desc.entry_point : "main";
      shader_stages.push_back(stage_info);
    }

    std::vector<vk::PipelineColorBlendAttachmentState> blend_attachments = desc.color_blend_attachments;
    if (blend_attachments.empty()) {
      blend_attachments = default_blend_attachments(static_cast<uint32_t>(desc.color_attachment_formats.size()));
    }
    if (blend_attachments.size() != desc.color_attachment_formats.size()) {
      return std::unexpected("color blend attachment count must match color attachment format count");
    }

    vk::PipelineLayoutCreateInfo layout_info{};
    layout_info.setLayoutCount = static_cast<uint32_t>(desc.descriptor_set_layouts.size());
    layout_info.pSetLayouts = desc.descriptor_set_layouts.data();
    layout_info.pushConstantRangeCount = static_cast<uint32_t>(desc.push_constant_ranges.size());
    layout_info.pPushConstantRanges = desc.push_constant_ranges.data();

    const auto layout_result = context.vk_device().createPipelineLayout(layout_info);
    if (layout_result.result != vk::Result::eSuccess) {
      return std::unexpected("createPipelineLayout failed");
    }
    vk::PipelineLayout pipeline_layout = layout_result.value;

    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.vertexBindingDescriptionCount = static_cast<uint32_t>(desc.vertex_bindings.size());
    vertex_input.pVertexBindingDescriptions = desc.vertex_bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(desc.vertex_attributes.size());
    vertex_input.pVertexAttributeDescriptions = desc.vertex_attributes.data();

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.topology = desc.topology;
    input_assembly.primitiveRestartEnable = desc.primitive_restart_enable ? VK_TRUE : VK_FALSE;

    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    vk::PipelineRasterizationStateCreateInfo rasterization{};
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = desc.polygon_mode;
    rasterization.cullMode = desc.cull_mode;
    rasterization.frontFace = desc.front_face;
    rasterization.depthBiasEnable = VK_FALSE;
    rasterization.lineWidth = desc.line_width;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.rasterizationSamples = desc.samples;
    multisample.sampleShadingEnable = VK_FALSE;

    vk::PipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.depthTestEnable = desc.depth_test_enable ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = desc.depth_write_enable ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = desc.depth_compare_op;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.logicOpEnable = VK_FALSE;
    color_blend.attachmentCount = static_cast<uint32_t>(blend_attachments.size());
    color_blend.pAttachments = blend_attachments.data();

    vk::PipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.dynamicStateCount = static_cast<uint32_t>(desc.dynamic_states.size());
    dynamic_state.pDynamicStates = desc.dynamic_states.data();

    vk::PipelineRenderingCreateInfo rendering_info{};
    rendering_info.colorAttachmentCount = static_cast<uint32_t>(desc.color_attachment_formats.size());
    rendering_info.pColorAttachmentFormats = desc.color_attachment_formats.data();
    rendering_info.depthAttachmentFormat = desc.depth_attachment_format.value_or(vk::Format::eUndefined);

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.stageCount = static_cast<uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.pDynamicState = desc.dynamic_states.empty() ? nullptr : &dynamic_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = nullptr;
    pipeline_info.subpass = 0;
    pipeline_info.pNext = &rendering_info;

    const auto pipeline_result =
      context.vk_device().createGraphicsPipeline(context.vk_pipeline_cache(), pipeline_info);
    if (pipeline_result.result != vk::Result::eSuccess) {
      context.vk_device().destroyPipelineLayout(pipeline_layout);
      return std::unexpected("createGraphicsPipeline failed");
    }

    GraphicsPipeline pipeline{};
    pipeline.context_ = &context;
    pipeline.layout_ = pipeline_layout;
    pipeline.pipeline_ = pipeline_result.value;
    return pipeline;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_bindings(std::span<const vk::VertexInputBindingDescription> bindings)
  {
    desc_.vertex_bindings.assign(bindings.begin(), bindings.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_attributes(std::span<const vk::VertexInputAttributeDescription> attributes)
  {
    desc_.vertex_attributes.assign(attributes.begin(), attributes.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_shader_stages(std::span<const GraphicsShaderStageDesc> stages)
  {
    desc_.shader_stages.assign(stages.begin(), stages.end());
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::add_shader_stage(GraphicsShaderStageDesc stage)
  {
    desc_.shader_stages.push_back(stage);
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_descriptor_set_layouts(std::span<const vk::DescriptorSetLayout> layouts)
  {
    desc_.descriptor_set_layouts.assign(layouts.begin(), layouts.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_push_constants(std::span<const vk::PushConstantRange> ranges)
  {
    desc_.push_constant_ranges.assign(ranges.begin(), ranges.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_dynamic_states(std::span<const vk::DynamicState> states)
  {
    desc_.dynamic_states.assign(states.begin(), states.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_color_attachment_formats(std::span<const vk::Format> formats)
  {
    desc_.color_attachment_formats.assign(formats.begin(), formats.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_color_blend_attachments(
    std::span<const vk::PipelineColorBlendAttachmentState> attachments)
  {
    desc_.color_blend_attachments.assign(attachments.begin(), attachments.end());
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_depth_attachment_format(std::optional<vk::Format> format)
  {
    desc_.depth_attachment_format = format;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_topology(vk::PrimitiveTopology topology)
  {
    desc_.topology = topology;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_cull_mode(vk::CullModeFlags cull_mode)
  {
    desc_.cull_mode = cull_mode;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_front_face(vk::FrontFace front_face)
  {
    desc_.front_face = front_face;
    return *this;
  }

  GraphicsPipelineBuilder&
  GraphicsPipelineBuilder::set_depth_state(bool test_enable, bool write_enable, vk::CompareOp compare_op)
  {
    desc_.depth_test_enable = test_enable;
    desc_.depth_write_enable = write_enable;
    desc_.depth_compare_op = compare_op;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_samples(vk::SampleCountFlagBits samples)
  {
    desc_.samples = samples;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_line_width(float line_width)
  {
    desc_.line_width = line_width;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_primitive_restart(bool enable)
  {
    desc_.primitive_restart_enable = enable;
    return *this;
  }

  GraphicsPipelineBuilder& GraphicsPipelineBuilder::set_polygon_mode(vk::PolygonMode polygon_mode)
  {
    desc_.polygon_mode = polygon_mode;
    return *this;
  }

  std::expected<GraphicsPipeline, std::string> GraphicsPipelineBuilder::build() const
  {
    ASSERT(context_ != nullptr, "GraphicsPipelineBuilder requires a valid VulkanContext");
    return build_graphics_pipeline(*context_, desc_);
  }
} // namespace mr
