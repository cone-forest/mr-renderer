#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <vector>

#include <VkBootstrap.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan.h>

namespace mr {
  struct VulkanFeatureSupport {
    bool mesh_shader = false;
    bool task_shader = false;
    bool descriptor_indexing = false;
    bool runtime_descriptor_array = false;
    bool descriptor_binding_partially_bound = false;
    bool descriptor_binding_variable_descriptor_count = false;
    bool descriptor_binding_update_unused_while_pending = false;
    bool draw_indirect_count = false;
    bool multi_draw_indirect = false;
    bool shader_draw_parameters = false;
    bool buffer_device_address = false;
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool dynamic_rendering = false;
  };

  struct VulkanContextCreateInfo {
    const char* app_name = "mr-renderer";
    bool headless = true;
    bool require_present = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool prefer_dedicated_compute_queue = true;
  };

  struct VulkanContext {
    vkb::Instance instance{};
    vkb::PhysicalDevice physical_device{};
    vkb::Device device{};

    uint32_t graphics_queue_family = std::numeric_limits<uint32_t>::max();
    uint32_t compute_queue_family = std::numeric_limits<uint32_t>::max();
    uint32_t present_queue_family = std::numeric_limits<uint32_t>::max();

    vk::Queue graphics_queue{};
    vk::Queue compute_queue{};
    vk::Queue present_queue{};

    VulkanFeatureSupport feature_support{};
    std::vector<std::string> enabled_extensions{};

    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&& other) noexcept;
    VulkanContext& operator=(VulkanContext&& other) noexcept;

    [[nodiscard]] vk::Instance vk_instance() const;
    [[nodiscard]] vk::PhysicalDevice vk_physical_device() const;
    [[nodiscard]] vk::Device vk_device() const;
    [[nodiscard]] bool has_present_queue() const;
  };

  [[nodiscard]] std::expected<VulkanContext, std::string>
  create_vulkan_context(const VulkanContextCreateInfo& create_info);

  struct VulkanPhysicalDeviceInfo {
    uint32_t vendor_id = 0;
    VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  };

  std::expected<std::vector<VulkanPhysicalDeviceInfo>, std::string>
  enumerate_vulkan_physical_devices();
} // namespace mr
