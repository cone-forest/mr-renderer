#include "pch.hpp"
#include "vulkan_init.hpp"

#include <VkBootstrap.h>

namespace mr {
  std::expected<std::vector<VulkanPhysicalDeviceInfo>, std::string>
  enumerate_vulkan_physical_devices()
  {
    vkb::InstanceBuilder builder;
    auto instance_ret = builder.set_app_name("mr-renderer-lib")
                          .request_validation_layers(false)
                          .require_api_version(1, 2, 0)
                          .build();
    if (!instance_ret) {
      return std::unexpected("vk-bootstrap failed to create Vulkan instance");
    }

    vkb::Instance instance = instance_ret.value();

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

