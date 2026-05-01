#include "pch.hpp"
#include <mr-renderer/window_presenter.hpp>

#define VKFW_NO_EXCEPTIONS
#define VKFW_NO_INCLUDE_VULKAN_HPP
#include <vkfw/vkfw.hpp>

#include <VkBootstrap.h>
#include <mr-renderer/vulkan_wrappers.hpp>
#include <tracy/TracyVulkan.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <libassert/assert.hpp>
#include <optional>

namespace mr {
  namespace {
    template <typename T>
    T vk_expect(vk::ResultValue<T> rv, const char* message)
    {
      MR_TRACY_ZONE;
      ASSERT(rv.result == vk::Result::eSuccess, message, static_cast<int>(rv.result));
      return rv.value;
    }

    bool has_extension(const std::vector<std::string>& extensions, const char* extension_name)
    {
      MR_TRACY_ZONE;
      return std::ranges::any_of(extensions, [extension_name](const std::string& ext) {
        return ext == extension_name;
      });
    }

    uint8_t linear_to_srgb_u8(float x)
    {
      MR_TRACY_ZONE;
      x = std::clamp(x, 0.f, 1.f);
      const float s =
        x <= 0.0031308f ? (12.92f * x) : ((1.055f * std::pow(x, 1.f / 2.4f)) - 0.055f);
      const auto out = std::lround(s * 255.f);
      return static_cast<uint8_t>(std::clamp(out, 0L, 255L));
    }
  } // namespace

  struct WindowPresenter::Impl {
    vkfw::Window window{};
    bool vkfw_initialized = false;
    bool owns_vulkan_context = false;
    bool initialized = false;

    const VulkanContext* external_context = nullptr;
    vkb::Instance vkb_instance{};
    vkb::PhysicalDevice vkb_physical_device{};
    vkb::Device vkb_device{};

    vk::SurfaceKHR surface{};
    vk::Queue queue{};
    uint32_t queue_family = std::numeric_limits<uint32_t>::max();

    vk::SwapchainKHR swapchain{};
    vk::Format swapchain_format = vk::Format::eB8G8R8A8Srgb;
    vk::Extent2D swapchain_extent{.width = 0, .height = 0};
    std::vector<vk::Image> swapchain_images{};
    std::vector<vk::ImageLayout> swapchain_layouts{};

    vk::CommandPool command_pool{};
    vk::CommandBuffer command_buffer{};
    vk::Fence in_flight{};

    std::vector<vk::Semaphore> image_available_semaphores{};
    std::vector<vk::Semaphore> render_finished_semaphores{};
    uint32_t acquire_semaphore_cursor = 0;

    vk::Buffer staging_buffer{};
    vk::DeviceMemory staging_memory{};
    vk::DeviceSize staging_size = 0;

#ifdef TRACY_ENABLE
    TracyVkCtx tracy_vk_ctx = nullptr;
    vk::CommandPool tracy_command_pool{};
    vk::CommandBuffer tracy_command_buffer{};
#endif

    [[nodiscard]] vk::Device device() const { return vk::Device(vkb_device.device); }
    [[nodiscard]] vk::PhysicalDevice physical_device() const { return vk::PhysicalDevice(vkb_physical_device.physical_device); }
    [[nodiscard]] vk::Instance instance() const { return vk::Instance(vkb_instance.instance); }

    void destroy_window()
    {
      MR_TRACY_ZONE;
      if (window) {
        const auto destroy_result = window.destroy();
        ASSERT(vkfw::check(destroy_result), "vkfw::Window::destroy failed");
        window = nullptr;
      }
      if (vkfw_initialized) {
        static_cast<void>(vkfw::terminate());
        vkfw_initialized = false;
      }
    }

    void destroy_staging()
    {
      MR_TRACY_ZONE;
      if (staging_buffer) {
        device().destroyBuffer(staging_buffer);
        staging_buffer = nullptr;
      }
      if (staging_memory) {
        device().freeMemory(staging_memory);
        staging_memory = nullptr;
      }
      staging_size = 0;
    }

    void destroy_swapchain()
    {
      MR_TRACY_ZONE;
      for (const vk::Semaphore semaphore : image_available_semaphores) {
        if (semaphore) {
          device().destroySemaphore(semaphore);
        }
      }
      for (const vk::Semaphore semaphore : render_finished_semaphores) {
        if (semaphore) {
          device().destroySemaphore(semaphore);
        }
      }
      image_available_semaphores.clear();
      render_finished_semaphores.clear();
      acquire_semaphore_cursor = 0;

      if (swapchain) {
        device().destroySwapchainKHR(swapchain);
        swapchain = nullptr;
      }
      swapchain_images.clear();
      swapchain_layouts.clear();
      swapchain_extent = {.width = 0, .height = 0};
    }

    void destroy_vulkan_handles()
    {
      MR_TRACY_ZONE;
#ifdef TRACY_ENABLE
      if (tracy_vk_ctx != nullptr) {
        TracyVkDestroy(tracy_vk_ctx);
        tracy_vk_ctx = nullptr;
      }
      if (tracy_command_buffer && tracy_command_pool) {
        device().freeCommandBuffers(tracy_command_pool, tracy_command_buffer);
      }
      tracy_command_buffer = nullptr;
      if (tracy_command_pool) {
        device().destroyCommandPool(tracy_command_pool);
      }
      tracy_command_pool = nullptr;
#endif

      destroy_staging();

      if (in_flight) {
        device().destroyFence(in_flight);
        in_flight = nullptr;
      }
      if (command_pool) {
        device().destroyCommandPool(command_pool);
        command_pool = nullptr;
      }

      destroy_swapchain();

      if (surface) {
        instance().destroySurfaceKHR(surface);
        surface = nullptr;
      }

      if (owns_vulkan_context) {
        if (vkb_device.device != VK_NULL_HANDLE) {
          device().destroy();
        }
        if (vkb_instance.debug_messenger != VK_NULL_HANDLE) {
          const auto destroy_debug_utils = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(vkb_instance.instance, "vkDestroyDebugUtilsMessengerEXT"));
          if (destroy_debug_utils != nullptr) {
            destroy_debug_utils(vkb_instance.instance, vkb_instance.debug_messenger, nullptr);
          }
          vkb_instance.debug_messenger = VK_NULL_HANDLE;
        }
        if (vkb_instance.instance != VK_NULL_HANDLE) {
          instance().destroy();
        }
      }

      external_context = nullptr;
      vkb_device = {};
      vkb_physical_device = {};
      vkb_instance = {};
      queue = nullptr;
      queue_family = std::numeric_limits<uint32_t>::max();
      owns_vulkan_context = false;
      initialized = false;
    }

    void shutdown()
    {
      MR_TRACY_ZONE;
      if (vkb_device.device != VK_NULL_HANDLE) {
        static_cast<void>(device().waitIdle());
      }
      destroy_vulkan_handles();
      destroy_window();
    }

    [[nodiscard]] uint32_t find_memory_type(uint32_t type_filter, vk::MemoryPropertyFlags properties) const
    {
      MR_TRACY_ZONE;
      const vk::PhysicalDeviceMemoryProperties memory_properties = physical_device().getMemoryProperties();
      for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const bool type_supported = (type_filter & (1u << i)) != 0;
        const bool has_properties =
          (memory_properties.memoryTypes[i].propertyFlags & properties) == properties;
        if (type_supported && has_properties) {
          return i;
        }
      }
      ASSERT(false, "failed to find suitable Vulkan memory type");
      return 0;
    }

    void ensure_staging_buffer(vk::DeviceSize required_size)
    {
      MR_TRACY_ZONE;
      if (required_size <= staging_size && staging_buffer) {
        return;
      }

      destroy_staging();

      vk::BufferCreateInfo buffer_info{};
      buffer_info.size = required_size;
      buffer_info.usage = vk::BufferUsageFlagBits::eTransferSrc;
      buffer_info.sharingMode = vk::SharingMode::eExclusive;
      staging_buffer = vk_expect(device().createBuffer(buffer_info), "createBuffer(staging) failed");

      const vk::MemoryRequirements mem_req = device().getBufferMemoryRequirements(staging_buffer);
      vk::MemoryAllocateInfo alloc_info{};
      alloc_info.allocationSize = mem_req.size;
      alloc_info.memoryTypeIndex = find_memory_type(
        mem_req.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
      staging_memory = vk_expect(device().allocateMemory(alloc_info), "allocateMemory(staging) failed");
      ASSERT(device().bindBufferMemory(staging_buffer, staging_memory, 0) == vk::Result::eSuccess,
        "bindBufferMemory(staging) failed");
      staging_size = required_size;
    }

    void create_window(uint32_t width, uint32_t height)
    {
      MR_TRACY_ZONE;
      const vkfw::Result init_result = vkfw::init();
      ASSERT(vkfw::check(init_result), "vkfw::init failed");
      vkfw_initialized = true;

      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
      vkfw::WindowHints hints{};
      hints.clientAPI = vkfw::ClientAPI::eNone;
      hints.resizable = false;
      const auto window_result =
        vkfw::createWindow(static_cast<int>(width), static_cast<int>(height), "mr-renderer", hints);
      ASSERT(vkfw::check(window_result.result), "vkfw::createWindow failed");
      ASSERT(window_result.value, "vkfw::createWindow returned null window");
      window = window_result.value;
    }

    void create_command_resources()
    {
      MR_TRACY_ZONE;
      vk::CommandPoolCreateInfo pool_info{};
      pool_info.queueFamilyIndex = queue_family;
      pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
      command_pool = vk_expect(device().createCommandPool(pool_info), "createCommandPool failed");

      vk::CommandBufferAllocateInfo cmd_alloc_info{};
      cmd_alloc_info.commandPool = command_pool;
      cmd_alloc_info.level = vk::CommandBufferLevel::ePrimary;
      cmd_alloc_info.commandBufferCount = 1;
      command_buffer = vk_expect(device().allocateCommandBuffers(cmd_alloc_info), "allocateCommandBuffers failed").at(0);

      vk::FenceCreateInfo fence_info{};
      fence_info.flags = vk::FenceCreateFlagBits::eSignaled;
      in_flight = vk_expect(device().createFence(fence_info), "createFence failed");

#ifdef TRACY_ENABLE
      vk::CommandPoolCreateInfo tracy_pool_info{};
      tracy_pool_info.queueFamilyIndex = queue_family;
      tracy_pool_info.flags =
        vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
      tracy_command_pool = vk_expect(device().createCommandPool(tracy_pool_info), "createCommandPool(tracy) failed");
      vk::CommandBufferAllocateInfo tracy_alloc_info{};
      tracy_alloc_info.commandPool = tracy_command_pool;
      tracy_alloc_info.level = vk::CommandBufferLevel::ePrimary;
      tracy_alloc_info.commandBufferCount = 1;
      tracy_command_buffer =
        vk_expect(device().allocateCommandBuffers(tracy_alloc_info), "allocateCommandBuffers(tracy) failed").at(0);
      tracy_vk_ctx = TracyVkContext(vkb_physical_device.physical_device, vkb_device.device, queue, tracy_command_buffer);
#endif
    }

    void recreate_swapchain(uint32_t width, uint32_t height)
    {
      MR_TRACY_ZONE;
      ASSERT(width > 0 && height > 0, "swapchain dimensions must be non-zero");
      ASSERT(device().waitIdle() == vk::Result::eSuccess, "waitIdle before swapchain recreation failed");

      const vk::SurfaceCapabilitiesKHR caps =
        vk_expect(physical_device().getSurfaceCapabilitiesKHR(surface), "getSurfaceCapabilitiesKHR failed");
      const auto formats = vk_expect(physical_device().getSurfaceFormatsKHR(surface), "getSurfaceFormatsKHR failed");
      ASSERT(!formats.empty(), "surface returned no formats");

      const auto chosen_format_it = std::ranges::find_if(formats, [](const vk::SurfaceFormatKHR& fmt) {
        return fmt.format == vk::Format::eR8G8B8A8Srgb || fmt.format == vk::Format::eR8G8B8A8Unorm;
      });
      const vk::SurfaceFormatKHR chosen_format =
        chosen_format_it != formats.end() ? *chosen_format_it : formats.front();
      swapchain_format = chosen_format.format;

      vk::Extent2D extent{.width = width, .height = height};
      if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = caps.currentExtent;
      } else {
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
      }

      uint32_t image_count = std::max(caps.minImageCount, 2u);
      if (caps.maxImageCount > 0) {
        image_count = std::min(image_count, caps.maxImageCount);
      }

      const vk::SwapchainKHR old_swapchain = swapchain;
      vk::SwapchainCreateInfoKHR create_info{};
      create_info.surface = surface;
      create_info.minImageCount = image_count;
      create_info.imageFormat = chosen_format.format;
      create_info.imageColorSpace = chosen_format.colorSpace;
      create_info.imageExtent = extent;
      create_info.imageArrayLayers = 1;
      create_info.imageUsage = vk::ImageUsageFlagBits::eTransferDst;
      create_info.imageSharingMode = vk::SharingMode::eExclusive;
      create_info.preTransform = caps.currentTransform;
      create_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
      create_info.presentMode = vk::PresentModeKHR::eFifo;
      create_info.clipped = true;
      create_info.oldSwapchain = old_swapchain;
      swapchain = vk_expect(device().createSwapchainKHR(create_info), "createSwapchainKHR failed");

      if (old_swapchain) {
        device().destroySwapchainKHR(old_swapchain);
      }

      swapchain_images = vk_expect(device().getSwapchainImagesKHR(swapchain), "getSwapchainImagesKHR failed");
      ASSERT(!swapchain_images.empty(), "swapchain returned no images");
      swapchain_layouts.assign(swapchain_images.size(), vk::ImageLayout::eUndefined);
      swapchain_extent = extent;

      for (const vk::Semaphore sem : image_available_semaphores) {
        if (sem) {
          device().destroySemaphore(sem);
        }
      }
      for (const vk::Semaphore sem : render_finished_semaphores) {
        if (sem) {
          device().destroySemaphore(sem);
        }
      }
      image_available_semaphores.clear();
      render_finished_semaphores.clear();
      image_available_semaphores.reserve(swapchain_images.size());
      render_finished_semaphores.reserve(swapchain_images.size());
      for (size_t i = 0; i < swapchain_images.size(); ++i) {
        image_available_semaphores.push_back(vk_expect(device().createSemaphore({}), "createSemaphore(image_available) failed"));
        render_finished_semaphores.push_back(vk_expect(device().createSemaphore({}), "createSemaphore(render_finished) failed"));
      }
      acquire_semaphore_cursor = 0;

      ensure_staging_buffer(
        static_cast<vk::DeviceSize>(swapchain_extent.width) *
        static_cast<vk::DeviceSize>(swapchain_extent.height) * 4u);
    }

    void initialize_from_cpu(const CpuFrame& frame)
    {
      MR_TRACY_ZONE;
      create_window(frame.width, frame.height);

      const auto required_extensions = vkfw::getRequiredInstanceExtensions();
      const bool enable_validation =
#ifdef MR_RENDERER_ENABLE_VK_VALIDATION
        true;
#else
        false;
#endif
      vkb::InstanceBuilder instance_builder;
      instance_builder
        .set_app_name("mr-renderer")
        .request_validation_layers(enable_validation)
        .require_api_version(1, 3, 0)
        .enable_extensions(required_extensions.size(), required_extensions.data());
      if (enable_validation) {
        instance_builder.use_default_debug_messenger();
      }

      const auto instance_result = instance_builder.build();
      ASSERT(instance_result.has_value(), "vk-bootstrap instance creation failed");
      vkb_instance = instance_result.value();
      owns_vulkan_context = true;

      surface = vkfw::createWindowSurface(vkb_instance.instance, window, nullptr);
      ASSERT(surface, "vkfw::createWindowSurface failed");

      vkb::PhysicalDeviceSelector selector(vkb_instance);
      selector.require_present(false);
      selector.add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      const auto physical_result = selector.select();
      ASSERT(physical_result.has_value(), "vk-bootstrap physical device selection failed");
      vkb_physical_device = physical_result.value();

      vkb::DeviceBuilder device_builder(vkb_physical_device);
      const auto device_result = device_builder.build();
      ASSERT(device_result.has_value(), "vk-bootstrap logical device creation failed");
      vkb_device = device_result.value();

      const auto queue_index_result = vkb_device.get_queue_index(vkb::QueueType::graphics);
      ASSERT(queue_index_result.has_value(), "failed to get graphics queue family index");
      queue_family = queue_index_result.value();
      const auto surface_support =
        vk_expect(physical_device().getSurfaceSupportKHR(queue_family, surface), "getSurfaceSupportKHR failed");
      ASSERT(surface_support == VK_TRUE, "graphics queue family does not support present for this surface");
      queue = device().getQueue(queue_family, 0);
      ASSERT(static_cast<bool>(queue), "failed to get graphics queue");

      create_command_resources();
      recreate_swapchain(frame.width, frame.height);
      initialized = true;
    }

    void initialize_from_gpu(const GpuFrame& frame)
    {
      MR_TRACY_ZONE;
      ASSERT(frame.context != nullptr, "GpuFrame context is null");
      external_context = frame.context;
      vkb_instance = external_context->instance;
      vkb_physical_device = external_context->physical_device;
      vkb_device = external_context->device;
      owns_vulkan_context = false;

      ASSERT(
        has_extension(external_context->enabled_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
        "GpuFrame context device was created without VK_KHR_swapchain extension");

      create_window(frame.width, frame.height);

      surface = vkfw::createWindowSurface(vkb_instance.instance, window, nullptr);
      ASSERT(surface, "vkfw::createWindowSurface failed");

      uint32_t present_family = external_context->present_queue_family;
      if (present_family == std::numeric_limits<uint32_t>::max()) {
        present_family = external_context->graphics_queue_family;
      }
      ASSERT(
        present_family != std::numeric_limits<uint32_t>::max(),
        "GpuFrame context has no usable queue family for presentation");
      const auto surface_support =
        vk_expect(physical_device().getSurfaceSupportKHR(present_family, surface), "getSurfaceSupportKHR failed");
      ASSERT(surface_support == VK_TRUE, "selected queue family does not support present for this surface");
      queue_family = present_family;
      queue = device().getQueue(queue_family, 0);
      ASSERT(static_cast<bool>(queue), "failed to get present queue");

      create_command_resources();
      recreate_swapchain(frame.width, frame.height);
      initialized = true;
    }

    void upload_cpu_frame_pixels(const CpuFrame& frame)
    {
      MR_TRACY_ZONE;
      ASSERT(frame.width == swapchain_extent.width && frame.height == swapchain_extent.height,
        "frame size must match swapchain size");
      const size_t expected_floats =
        static_cast<size_t>(swapchain_extent.width) * static_cast<size_t>(swapchain_extent.height) * 4u;
      ASSERT(frame.rgba32f.size() >= expected_floats, "frame pixel buffer is too small");

      auto map_rv = device().mapMemory(staging_memory, 0, staging_size);
      ASSERT(map_rv.result == vk::Result::eSuccess, "mapMemory(staging) failed");
      auto* dst = reinterpret_cast<uint8_t*>(map_rv.value);
      const float* src = frame.rgba32f.data();
      const size_t pixel_count =
        static_cast<size_t>(swapchain_extent.width) * static_cast<size_t>(swapchain_extent.height);
      const bool bgra =
        swapchain_format == vk::Format::eB8G8R8A8Srgb || swapchain_format == vk::Format::eB8G8R8A8Unorm;
      for (size_t i = 0; i < pixel_count; ++i) {
        const uint8_t r = linear_to_srgb_u8(src[i * 4u + 0u]);
        const uint8_t g = linear_to_srgb_u8(src[i * 4u + 1u]);
        const uint8_t b = linear_to_srgb_u8(src[i * 4u + 2u]);
        const uint8_t a = linear_to_srgb_u8(src[i * 4u + 3u]);
        if (bgra) {
          dst[i * 4u + 0u] = b;
          dst[i * 4u + 1u] = g;
          dst[i * 4u + 2u] = r;
          dst[i * 4u + 3u] = a;
        } else {
          dst[i * 4u + 0u] = r;
          dst[i * 4u + 1u] = g;
          dst[i * 4u + 2u] = b;
          dst[i * 4u + 3u] = a;
        }
      }
      device().unmapMemory(staging_memory);
    }

    void record_swapchain_transition_to_transfer(vk::CommandBuffer cmd, uint32_t image_index)
    {
      MR_TRACY_ZONE;
      vk::ImageMemoryBarrier to_transfer{};
      to_transfer.oldLayout = swapchain_layouts.at(image_index);
      to_transfer.newLayout = vk::ImageLayout::eTransferDstOptimal;
      to_transfer.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer.image = swapchain_images.at(image_index);
      to_transfer.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_transfer.subresourceRange.levelCount = 1;
      to_transfer.subresourceRange.layerCount = 1;
      to_transfer.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
      to_transfer.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
      cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},
        {},
        {},
        to_transfer);
    }

    void record_swapchain_transition_to_present(vk::CommandBuffer cmd, uint32_t image_index)
    {
      MR_TRACY_ZONE;
      vk::ImageMemoryBarrier to_present{};
      to_present.oldLayout = vk::ImageLayout::eTransferDstOptimal;
      to_present.newLayout = vk::ImageLayout::ePresentSrcKHR;
      to_present.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_present.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_present.image = swapchain_images.at(image_index);
      to_present.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      to_present.subresourceRange.levelCount = 1;
      to_present.subresourceRange.layerCount = 1;
      to_present.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      to_present.dstAccessMask = vk::AccessFlagBits::eMemoryRead;
      cmd.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eBottomOfPipe,
        vk::DependencyFlags{},
        {},
        {},
        to_present);
      swapchain_layouts.at(image_index) = vk::ImageLayout::ePresentSrcKHR;
    }

    void submit_and_present(
      uint32_t image_index,
      vk::Semaphore acquire_semaphore,
      vk::PipelineStageFlags acquire_stage,
      std::optional<vk::Semaphore> extra_wait = std::nullopt,
      vk::PipelineStageFlags extra_stage = vk::PipelineStageFlagBits::eTransfer)
    {
      MR_TRACY_ZONE;
      ASSERT(image_index < render_finished_semaphores.size(), "present semaphore index out of range");
      const vk::Semaphore signal_semaphore = render_finished_semaphores.at(image_index);

      std::array<vk::Semaphore, 2> waits{};
      std::array<vk::PipelineStageFlags, 2> wait_stages{};
      uint32_t wait_count = 1;
      waits[0] = acquire_semaphore;
      wait_stages[0] = acquire_stage;
      if (extra_wait.has_value() && static_cast<bool>(*extra_wait)) {
        waits[1] = *extra_wait;
        wait_stages[1] = extra_stage;
        wait_count = 2;
      }

      vk::SubmitInfo submit_info{};
      submit_info.waitSemaphoreCount = wait_count;
      submit_info.pWaitSemaphores = waits.data();
      submit_info.pWaitDstStageMask = wait_stages.data();
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &command_buffer;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &signal_semaphore;
      ASSERT(queue.submit(submit_info, in_flight) == vk::Result::eSuccess, "queue submit failed");

#ifdef TRACY_ENABLE
      if (tracy_vk_ctx != nullptr && static_cast<bool>(tracy_command_buffer)) {
        TracyVkCollect(tracy_vk_ctx, tracy_command_buffer);
      }
#endif

      vk::PresentInfoKHR present_info{};
      present_info.waitSemaphoreCount = 1;
      present_info.pWaitSemaphores = &signal_semaphore;
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &swapchain;
      present_info.pImageIndices = &image_index;
      const vk::Result present_result = queue.presentKHR(present_info);
      if (present_result == vk::Result::eErrorOutOfDateKHR || present_result == vk::Result::eSuboptimalKHR) {
        recreate_swapchain(swapchain_extent.width, swapchain_extent.height);
      } else {
        ASSERT(present_result == vk::Result::eSuccess, "presentKHR failed", static_cast<int>(present_result));
      }
    }

    void submit_cpu_copy_and_present(uint32_t image_index, vk::Semaphore acquire_semaphore)
    {
      MR_TRACY_ZONE;
      ASSERT(command_buffer.reset() == vk::Result::eSuccess, "command buffer reset failed");
      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(command_buffer.begin(begin_info) == vk::Result::eSuccess, "command buffer begin failed");

      record_swapchain_transition_to_transfer(command_buffer, image_index);

      vk::BufferImageCopy copy_region{};
      copy_region.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageExtent = vk::Extent3D{.width = swapchain_extent.width, .height = swapchain_extent.height, .depth = 1u};
      command_buffer.copyBufferToImage(
        staging_buffer,
        swapchain_images.at(image_index),
        vk::ImageLayout::eTransferDstOptimal,
        copy_region);

      record_swapchain_transition_to_present(command_buffer, image_index);
      ASSERT(command_buffer.end() == vk::Result::eSuccess, "command buffer end failed");

      submit_and_present(
        image_index,
        acquire_semaphore,
        vk::PipelineStageFlagBits::eTransfer);
    }

    void submit_gpu_copy_and_present(uint32_t image_index, const GpuFrame& frame, vk::Semaphore acquire_semaphore)
    {
      MR_TRACY_ZONE;
      ASSERT(command_buffer.reset() == vk::Result::eSuccess, "command buffer reset failed");
      vk::CommandBufferBeginInfo begin_info{};
      begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
      ASSERT(command_buffer.begin(begin_info) == vk::Result::eSuccess, "command buffer begin failed");

      record_swapchain_transition_to_transfer(command_buffer, image_index);

      vk::ImageMemoryBarrier src_to_copy{};
      src_to_copy.oldLayout = frame.layout;
      src_to_copy.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      src_to_copy.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      src_to_copy.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      src_to_copy.image = frame.image;
      src_to_copy.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
      src_to_copy.subresourceRange.levelCount = 1;
      src_to_copy.subresourceRange.layerCount = 1;
      src_to_copy.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
      src_to_copy.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      command_buffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags{},
        {},
        {},
        src_to_copy);

      vk::ImageCopy copy_region{};
      copy_region.srcSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy_region.srcSubresource.layerCount = 1;
      copy_region.dstSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy_region.dstSubresource.layerCount = 1;
      copy_region.extent = vk::Extent3D{.width = frame.width, .height = frame.height, .depth = 1u};
      command_buffer.copyImage(
        frame.image,
        vk::ImageLayout::eTransferSrcOptimal,
        swapchain_images.at(image_index),
        vk::ImageLayout::eTransferDstOptimal,
        copy_region);

      if (frame.layout != vk::ImageLayout::eTransferSrcOptimal) {
        vk::ImageMemoryBarrier restore_src{};
        restore_src.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        restore_src.newLayout = frame.layout;
        restore_src.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_src.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_src.image = frame.image;
        restore_src.subresourceRange = src_to_copy.subresourceRange;
        restore_src.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        restore_src.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
        command_buffer.pipelineBarrier(
          vk::PipelineStageFlagBits::eTransfer,
          vk::PipelineStageFlagBits::eAllCommands,
          vk::DependencyFlags{},
          {},
          {},
          restore_src);
      }

      record_swapchain_transition_to_present(command_buffer, image_index);
      ASSERT(command_buffer.end() == vk::Result::eSuccess, "command buffer end failed");

      submit_and_present(
        image_index,
        acquire_semaphore,
        vk::PipelineStageFlagBits::eTransfer,
        frame.ready_semaphore ? std::optional<vk::Semaphore>{frame.ready_semaphore} : std::nullopt,
        frame.wait_stage);
    }
  };

  WindowPresenter::WindowPresenter()
    : impl_(std::make_unique<Impl>())
  {
    MR_TRACY_ZONE;
  }

  WindowPresenter::~WindowPresenter()
  {
    MR_TRACY_ZONE;
    if (impl_) {
      impl_->shutdown();
    }
  }

  WindowPresenter::WindowPresenter(WindowPresenter&&) noexcept = default;
  WindowPresenter& WindowPresenter::operator=(WindowPresenter&&) noexcept = default;

  void WindowPresenter::present(Frame frame)
  {
    MR_TRACY_ZONE_N("WindowPresenter::present");
    MR_TRACY_FRAME("window_present");
    ASSERT(impl_ != nullptr, "WindowPresenter impl is null");

    const GpuFrame* gpu_frame = frame.gpu();
    const CpuFrame* cpu_frame = frame.cpu();

    if (!impl_->initialized) {
      if (gpu_frame != nullptr) {
        impl_->initialize_from_gpu(*gpu_frame);
      } else {
        ASSERT(cpu_frame != nullptr, "frame has no supported payload");
        impl_->initialize_from_cpu(*cpu_frame);
      }
    }

    const auto poll_result = vkfw::pollEvents();
    ASSERT(vkfw::check(poll_result), "vkfw::pollEvents failed");

    if (!impl_->window) {
      return;
    }
    const auto close_result = impl_->window.shouldClose();
    ASSERT(vkfw::check(close_result.result), "vkfw::Window::shouldClose failed");
    if (close_result.value) {
      return;
    }

    ASSERT(impl_->device().waitForFences(impl_->in_flight, true, UINT64_MAX) == vk::Result::eSuccess,
      "waitForFences failed");
    ASSERT(impl_->device().resetFences(impl_->in_flight) == vk::Result::eSuccess, "resetFences failed");

    ASSERT(!impl_->image_available_semaphores.empty(), "image_available semaphores are not initialized");
    const vk::Semaphore acquire_semaphore =
      impl_->image_available_semaphores.at(impl_->acquire_semaphore_cursor % impl_->image_available_semaphores.size());
    ++impl_->acquire_semaphore_cursor;

    const auto acquire_rv = impl_->device().acquireNextImageKHR(
      impl_->swapchain,
      UINT64_MAX,
      acquire_semaphore,
      vk::Fence{});
    if (acquire_rv.result == vk::Result::eErrorOutOfDateKHR) {
      impl_->recreate_swapchain(impl_->swapchain_extent.width, impl_->swapchain_extent.height);
      return;
    }
    ASSERT(
      acquire_rv.result == vk::Result::eSuccess || acquire_rv.result == vk::Result::eSuboptimalKHR,
      "acquireNextImageKHR failed",
      static_cast<int>(acquire_rv.result));
    const uint32_t image_index = acquire_rv.value;

    if (gpu_frame != nullptr) {
      ASSERT(gpu_frame->width == impl_->swapchain_extent.width && gpu_frame->height == impl_->swapchain_extent.height,
        "GpuFrame size must match window size");
      impl_->submit_gpu_copy_and_present(image_index, *gpu_frame, acquire_semaphore);
      return;
    }

    ASSERT(cpu_frame != nullptr, "frame has no supported payload");
    impl_->upload_cpu_frame_pixels(*cpu_frame);
    impl_->submit_cpu_copy_and_present(image_index, acquire_semaphore);
  }
} // namespace mr
