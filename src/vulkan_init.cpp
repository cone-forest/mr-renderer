#include "pch.hpp"
#include "vulkan_init.hpp"

#include <VkBootstrap.h>
#include <array>
#include <optional>
#include <set>

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
        .require_api_version(1, 2, 0);
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
      support.draw_indirect_count = has_extension(available_extensions, VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME);

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
        features_12.runtimeDescriptorArray == VK_TRUE || descriptor_indexing.runtimeDescriptorArray == VK_TRUE;
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
  } // namespace

  VulkanContext::~VulkanContext()
  {
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
    , graphics_queue_family(other.graphics_queue_family)
    , compute_queue_family(other.compute_queue_family)
    , present_queue_family(other.present_queue_family)
    , graphics_queue(other.graphics_queue)
    , compute_queue(other.compute_queue)
    , present_queue(other.present_queue)
    , feature_support(other.feature_support)
    , enabled_extensions(std::move(other.enabled_extensions))
  {
    other.instance = {};
    other.device = {};
    other.graphics_queue = nullptr;
    other.compute_queue = nullptr;
    other.present_queue = nullptr;
    other.graphics_queue_family = std::numeric_limits<uint32_t>::max();
    other.compute_queue_family = std::numeric_limits<uint32_t>::max();
    other.present_queue_family = std::numeric_limits<uint32_t>::max();
  }

  VulkanContext& VulkanContext::operator=(VulkanContext&& other) noexcept
  {
    if (this == &other) {
      return *this;
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
    graphics_queue_family = other.graphics_queue_family;
    compute_queue_family = other.compute_queue_family;
    present_queue_family = other.present_queue_family;
    graphics_queue = other.graphics_queue;
    compute_queue = other.compute_queue;
    present_queue = other.present_queue;
    feature_support = other.feature_support;
    enabled_extensions = std::move(other.enabled_extensions);

    other.instance = {};
    other.device = {};
    other.graphics_queue = nullptr;
    other.compute_queue = nullptr;
    other.present_queue = nullptr;
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
} // namespace mr

