#include "pch.hpp"
#include <mr-renderer/window_presenter.hpp>

#define VKFW_NO_EXCEPTIONS
#define VKFW_NO_INCLUDE_VULKAN_HPP
#include <vkfw/vkfw.hpp>

#include <VkBootstrap.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mr {
  namespace {
    uint8_t linear_to_srgb_u8(float x)
    {
      x = std::clamp(x, 0.f, 1.f);
      const float s =
        x <= 0.0031308f ? (12.92f * x) : (1.055f * std::pow(x, 1.f / 2.4f) - 0.055f);
      const auto out = static_cast<long>(std::lround(s * 255.f));
      return static_cast<uint8_t>(std::clamp(out, 0L, 255L));
    }
  } // namespace

  struct WindowPresenter::Impl {
    vkfw::Window window{};
    bool vkfw_initialized = false;

    vkb::Instance vkb_instance{};
    vkb::PhysicalDevice vkb_physical_device{};
    vkb::Device vkb_device{};
    vkb::Swapchain vkb_swapchain{};

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;

    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkDeviceSize staging_size = 0;

    VkExtent2D swapchain_extent{0, 0};
    std::vector<VkImage> swapchain_images{};

    void destroy_staging()
    {
      if (staging_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(vkb_device.device, staging_buffer, nullptr);
        staging_buffer = VK_NULL_HANDLE;
      }
      if (staging_memory != VK_NULL_HANDLE) {
        vkFreeMemory(vkb_device.device, staging_memory, nullptr);
        staging_memory = VK_NULL_HANDLE;
      }
      staging_size = 0;
    }

    void destroy_swapchain()
    {
      if (vkb_swapchain.swapchain != VK_NULL_HANDLE) {
        vkb::destroy_swapchain(vkb_swapchain);
        vkb_swapchain = {};
      }
      swapchain_images.clear();
      swapchain_extent = {0, 0};
    }

    void shutdown()
    {
      if (vkb_device.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vkb_device.device);
      }

      destroy_staging();

      if (in_flight != VK_NULL_HANDLE) {
        vkDestroyFence(vkb_device.device, in_flight, nullptr);
        in_flight = VK_NULL_HANDLE;
      }
      if (render_finished != VK_NULL_HANDLE) {
        vkDestroySemaphore(vkb_device.device, render_finished, nullptr);
        render_finished = VK_NULL_HANDLE;
      }
      if (image_available != VK_NULL_HANDLE) {
        vkDestroySemaphore(vkb_device.device, image_available, nullptr);
        image_available = VK_NULL_HANDLE;
      }
      if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(vkb_device.device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
      }

      destroy_swapchain();

      if (vkb_device.device != VK_NULL_HANDLE) {
        vkb::destroy_device(vkb_device);
        vkb_device = {};
      }
      if (surface != VK_NULL_HANDLE) {
        vkb::destroy_surface(vkb_instance, surface);
        surface = VK_NULL_HANDLE;
      }
      if (vkb_instance.instance != VK_NULL_HANDLE) {
        vkb::destroy_instance(vkb_instance);
        vkb_instance = {};
      }

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

    uint32_t find_memory_type(uint32_t type_filter, VkMemoryPropertyFlags properties) const
    {
      VkPhysicalDeviceMemoryProperties memory_properties{};
      vkGetPhysicalDeviceMemoryProperties(vkb_physical_device.physical_device, &memory_properties);
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

    void ensure_staging_buffer(VkDeviceSize required_size)
    {
      if (required_size <= staging_size && staging_buffer != VK_NULL_HANDLE) {
        return;
      }

      destroy_staging();

      VkBufferCreateInfo buffer_info{};
      buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      buffer_info.size = required_size;
      buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      const VkResult buffer_result =
        vkCreateBuffer(vkb_device.device, &buffer_info, nullptr, &staging_buffer);
      ASSERT(buffer_result == VK_SUCCESS, "vkCreateBuffer for staging failed");

      VkMemoryRequirements mem_req{};
      vkGetBufferMemoryRequirements(vkb_device.device, staging_buffer, &mem_req);

      VkMemoryAllocateInfo alloc_info{};
      alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc_info.allocationSize = mem_req.size;
      alloc_info.memoryTypeIndex = find_memory_type(
        mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

      const VkResult alloc_result =
        vkAllocateMemory(vkb_device.device, &alloc_info, nullptr, &staging_memory);
      ASSERT(alloc_result == VK_SUCCESS, "vkAllocateMemory for staging failed");

      const VkResult bind_result =
        vkBindBufferMemory(vkb_device.device, staging_buffer, staging_memory, 0);
      ASSERT(bind_result == VK_SUCCESS, "vkBindBufferMemory for staging failed");

      staging_size = required_size;
    }

    void recreate_swapchain(uint32_t width, uint32_t height)
    {
      ASSERT(width > 0 && height > 0, "swapchain dimensions must be non-zero");

      vkb::Swapchain old_swapchain = vkb_swapchain;
      vkb::SwapchainBuilder swapchain_builder(vkb_device, surface);
      swapchain_builder
        .set_desired_extent(width, height)
        .set_desired_format(VkSurfaceFormatKHR{
          VK_FORMAT_B8G8R8A8_SRGB,
          VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        })
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT);

      if (old_swapchain.swapchain != VK_NULL_HANDLE) {
        swapchain_builder.set_old_swapchain(old_swapchain);
      }

      const auto swapchain_result = swapchain_builder.build();
      ASSERT(swapchain_result.has_value(), "failed to build swapchain");
      vkb_swapchain = swapchain_result.value();

      if (old_swapchain.swapchain != VK_NULL_HANDLE) {
        vkb::destroy_swapchain(old_swapchain);
      }

      swapchain_extent = vkb_swapchain.extent;
      const auto images_result = vkb_swapchain.get_images();
      ASSERT(images_result.has_value(), "failed to get swapchain images");
      swapchain_images = images_result.value();

      ensure_staging_buffer(static_cast<VkDeviceSize>(swapchain_extent.width) *
                            static_cast<VkDeviceSize>(swapchain_extent.height) * 4u);
    }

    void initialize(const Frame& frame)
    {
      ASSERT(frame.color.width > 0 && frame.color.height > 0, "invalid frame size for window presenter");
      ASSERT(frame.color.format == vk::Format::eR32G32B32A32Sfloat,
        "WindowPresenter expects RGBA32F frame input");

      const vkfw::Result init_result = vkfw::init();
      ASSERT(vkfw::check(init_result), "vkfw::init failed");
      vkfw_initialized = true;

      glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
      glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
      vkfw::WindowHints hints{};
      hints.clientAPI = vkfw::ClientAPI::eNone;
      hints.resizable = false;
      const auto window_result =
        vkfw::createWindow(frame.color.width, frame.color.height, "mr-renderer", hints);
      ASSERT(vkfw::check(window_result.result), "vkfw::createWindow failed");
      ASSERT(window_result.value, "vkfw::createWindow returned null window");
      window = window_result.value;

      const auto required_extensions = vkfw::getRequiredInstanceExtensions();
      vkb::InstanceBuilder instance_builder;
      instance_builder
        .set_app_name("mr-renderer")
        .request_validation_layers(false)
        .require_api_version(1, 2, 0)
        .enable_extensions(required_extensions.size(), required_extensions.data());

      const auto instance_result = instance_builder.build();
      ASSERT(instance_result.has_value(), "vk-bootstrap instance creation failed");
      vkb_instance = instance_result.value();

      surface = vkfw::createWindowSurface(vkb_instance.instance, window, nullptr);
      ASSERT(surface != VK_NULL_HANDLE, "vkfw::createWindowSurface failed");

      vkb::PhysicalDeviceSelector selector(vkb_instance, surface);
      const auto physical_result = selector.select();
      ASSERT(physical_result.has_value(), "vk-bootstrap physical device selection failed");
      vkb_physical_device = physical_result.value();

      vkb::DeviceBuilder device_builder(vkb_physical_device);
      const auto device_result = device_builder.build();
      ASSERT(device_result.has_value(), "vk-bootstrap logical device creation failed");
      vkb_device = device_result.value();

      const auto queue_result = vkb_device.get_queue(vkb::QueueType::graphics);
      const auto queue_index_result = vkb_device.get_queue_index(vkb::QueueType::graphics);
      const auto present_queue_index_result = vkb_device.get_queue_index(vkb::QueueType::present);
      ASSERT(queue_result.has_value(), "failed to get graphics queue");
      ASSERT(queue_index_result.has_value(), "failed to get graphics queue family index");
      ASSERT(present_queue_index_result.has_value(), "failed to get present queue family index");
      ASSERT(queue_index_result.value() == present_queue_index_result.value(),
        "WindowPresenter currently requires graphics and present queue families to match");

      queue = queue_result.value();
      queue_family = queue_index_result.value();

      VkCommandPoolCreateInfo pool_info{};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.queueFamilyIndex = queue_family;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      const VkResult pool_result =
        vkCreateCommandPool(vkb_device.device, &pool_info, nullptr, &command_pool);
      ASSERT(pool_result == VK_SUCCESS, "vkCreateCommandPool failed");

      VkCommandBufferAllocateInfo cmd_alloc_info{};
      cmd_alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      cmd_alloc_info.commandPool = command_pool;
      cmd_alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      cmd_alloc_info.commandBufferCount = 1;
      const VkResult cmd_alloc_result =
        vkAllocateCommandBuffers(vkb_device.device, &cmd_alloc_info, &command_buffer);
      ASSERT(cmd_alloc_result == VK_SUCCESS, "vkAllocateCommandBuffers failed");

      VkSemaphoreCreateInfo sem_info{};
      sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      ASSERT(vkCreateSemaphore(vkb_device.device, &sem_info, nullptr, &image_available) == VK_SUCCESS,
        "vkCreateSemaphore(image_available) failed");
      ASSERT(vkCreateSemaphore(vkb_device.device, &sem_info, nullptr, &render_finished) == VK_SUCCESS,
        "vkCreateSemaphore(render_finished) failed");

      VkFenceCreateInfo fence_info{};
      fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      ASSERT(vkCreateFence(vkb_device.device, &fence_info, nullptr, &in_flight) == VK_SUCCESS,
        "vkCreateFence failed");

      recreate_swapchain(frame.color.width, frame.color.height);
    }

    void upload_frame_pixels(const Frame& frame)
    {
      ASSERT(frame.color.width == static_cast<int32_t>(swapchain_extent.width),
        "frame width must match swapchain width");
      ASSERT(frame.color.height == static_cast<int32_t>(swapchain_extent.height),
        "frame height must match swapchain height");

      const auto* src = reinterpret_cast<const float*>(frame.color.pixels.get());
      ASSERT(src != nullptr, "frame pixel buffer is null");

      void* mapped = nullptr;
      ASSERT(vkMapMemory(vkb_device.device, staging_memory, 0, staging_size, 0, &mapped) == VK_SUCCESS,
        "vkMapMemory failed for staging buffer");

      auto* dst = reinterpret_cast<uint8_t*>(mapped);
      const size_t pixel_count =
        static_cast<size_t>(swapchain_extent.width) * static_cast<size_t>(swapchain_extent.height);
      for (size_t i = 0; i < pixel_count; ++i) {
        const float r = src[i * 4u + 0u];
        const float g = src[i * 4u + 1u];
        const float b = src[i * 4u + 2u];
        const float a = src[i * 4u + 3u];
        dst[i * 4u + 0u] = linear_to_srgb_u8(b);
        dst[i * 4u + 1u] = linear_to_srgb_u8(g);
        dst[i * 4u + 2u] = linear_to_srgb_u8(r);
        dst[i * 4u + 3u] = linear_to_srgb_u8(a);
      }

      vkUnmapMemory(vkb_device.device, staging_memory);
    }

    void submit_copy_and_present(uint32_t image_index)
    {
      ASSERT(image_index < swapchain_images.size(), "swapchain image index out of range");
      const VkImage image = swapchain_images[image_index];

      ASSERT(vkResetCommandBuffer(command_buffer, 0) == VK_SUCCESS, "vkResetCommandBuffer failed");

      VkCommandBufferBeginInfo begin_info{};
      begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ASSERT(vkBeginCommandBuffer(command_buffer, &begin_info) == VK_SUCCESS,
        "vkBeginCommandBuffer failed");

      VkImageMemoryBarrier to_transfer{};
      to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.image = image;
      to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      to_transfer.subresourceRange.baseMipLevel = 0;
      to_transfer.subresourceRange.levelCount = 1;
      to_transfer.subresourceRange.baseArrayLayer = 0;
      to_transfer.subresourceRange.layerCount = 1;
      to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

      vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_transfer);

      VkBufferImageCopy copy_region{};
      copy_region.bufferOffset = 0;
      copy_region.bufferRowLength = 0;
      copy_region.bufferImageHeight = 0;
      copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy_region.imageSubresource.mipLevel = 0;
      copy_region.imageSubresource.baseArrayLayer = 0;
      copy_region.imageSubresource.layerCount = 1;
      copy_region.imageExtent = {
        swapchain_extent.width,
        swapchain_extent.height,
        1u,
      };

      vkCmdCopyBufferToImage(
        command_buffer,
        staging_buffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &copy_region);

      VkImageMemoryBarrier to_present{};
      to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_present.image = image;
      to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      to_present.subresourceRange.baseMipLevel = 0;
      to_present.subresourceRange.levelCount = 1;
      to_present.subresourceRange.baseArrayLayer = 0;
      to_present.subresourceRange.layerCount = 1;
      to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

      vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &to_present);

      ASSERT(vkEndCommandBuffer(command_buffer) == VK_SUCCESS, "vkEndCommandBuffer failed");

      VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
      VkSubmitInfo submit_info{};
      submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      submit_info.waitSemaphoreCount = 1;
      submit_info.pWaitSemaphores = &image_available;
      submit_info.pWaitDstStageMask = &wait_stage;
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &command_buffer;
      submit_info.signalSemaphoreCount = 1;
      submit_info.pSignalSemaphores = &render_finished;

      ASSERT(vkQueueSubmit(queue, 1, &submit_info, in_flight) == VK_SUCCESS, "vkQueueSubmit failed");

      VkPresentInfoKHR present_info{};
      present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
      present_info.waitSemaphoreCount = 1;
      present_info.pWaitSemaphores = &render_finished;
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &vkb_swapchain.swapchain;
      present_info.pImageIndices = &image_index;
      const VkResult present_result = vkQueuePresentKHR(queue, &present_info);
      ASSERT(
        present_result == VK_SUCCESS || present_result == VK_SUBOPTIMAL_KHR || present_result == VK_ERROR_OUT_OF_DATE_KHR,
        "vkQueuePresentKHR failed");
      if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain(swapchain_extent.width, swapchain_extent.height);
      }
    }
  };

  WindowPresenter::WindowPresenter()
    : impl_(std::make_unique<Impl>())
  {}

  WindowPresenter::~WindowPresenter()
  {
    if (impl_) {
      impl_->shutdown();
    }
  }

  WindowPresenter::WindowPresenter(WindowPresenter&&) noexcept = default;
  WindowPresenter& WindowPresenter::operator=(WindowPresenter&&) noexcept = default;

  void WindowPresenter::present(Frame frame)
  {
    ASSERT(impl_ != nullptr, "WindowPresenter impl is null");

    if (!impl_->vkfw_initialized) {
      impl_->initialize(frame);
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

    ASSERT(vkWaitForFences(impl_->vkb_device.device, 1, &impl_->in_flight, VK_TRUE, UINT64_MAX) == VK_SUCCESS,
      "vkWaitForFences failed");
    ASSERT(vkResetFences(impl_->vkb_device.device, 1, &impl_->in_flight) == VK_SUCCESS,
      "vkResetFences failed");

    uint32_t image_index = 0;
    const VkResult acquire_result = vkAcquireNextImageKHR(
      impl_->vkb_device.device,
      impl_->vkb_swapchain.swapchain,
      UINT64_MAX,
      impl_->image_available,
      VK_NULL_HANDLE,
      &image_index);
    ASSERT(
      acquire_result == VK_SUCCESS || acquire_result == VK_SUBOPTIMAL_KHR || acquire_result == VK_ERROR_OUT_OF_DATE_KHR,
      "vkAcquireNextImageKHR failed");
    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
      impl_->recreate_swapchain(impl_->swapchain_extent.width, impl_->swapchain_extent.height);
      return;
    }

    impl_->upload_frame_pixels(frame);
    impl_->submit_copy_and_present(image_index);
  }
}
