#include "pch.hpp"
#include <mr-renderer/window_presenter.hpp>

#define VKFW_NO_EXCEPTIONS
#define VKFW_NO_INCLUDE_VULKAN_HPP
#include <vkfw/vkfw.hpp>

#include <mr-renderer/vulkan_wrappers.hpp>
#include <tracy/TracyVulkan.hpp>

#include <algorithm>
#include <cmath>
#include <span>
#include <cstdint>
#include <libassert/assert.hpp>
#include <limits>
#include <optional>
#include <vector>

namespace mr {
  namespace {
    template <typename T>
    T vk_expect(vk::ResultValue<T> rv, const char* message)
    {
      MR_TRACY_ZONE;
      ASSERT(rv.result == vk::Result::eSuccess, message, static_cast<int>(rv.result));
      return rv.value;
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

    std::unique_ptr<VulkanContext> context{};
    vk::SurfaceKHR surface{};

    vk::SwapchainKHR swapchain{};
    vk::Format swapchain_format = vk::Format::eB8G8R8A8Srgb;
    vk::Extent2D swapchain_extent{.width = 0, .height = 0};
    uint64_t swapchain_generation = 0;
    std::vector<vk::Image> swapchain_images{};
    std::vector<vk::ImageLayout> swapchain_layouts{};
    std::vector<vk::ImageView> swapchain_image_views{};

    vk::Queue queue{};
    uint32_t queue_family = std::numeric_limits<uint32_t>::max();

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

    [[nodiscard]] vk::Device device() const
    {
      MR_TRACY_ZONE_N("WindowPresenter::Impl::device");
      return context->vk_device();
    }

    [[nodiscard]] uint32_t find_memory_type(uint32_t type_filter, vk::MemoryPropertyFlags properties) const
    {
      MR_TRACY_ZONE;
      const vk::PhysicalDeviceMemoryProperties memory_properties =
        context->vk_physical_device().getMemoryProperties();
      for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        const bool type_supported = (type_filter & (1u << i)) != 0;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        const vk::MemoryPropertyFlags mt_flags = memory_properties.memoryTypes[i].propertyFlags;
        const bool has_properties = (mt_flags & properties) == properties;
        if (type_supported && has_properties) {
          return i;
        }
      }
      ASSERT(false, "failed to find suitable Vulkan memory type");
      return 0;
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

      for (vk::ImageView view : swapchain_image_views) {
        if (view) {
          device().destroyImageView(view);
        }
      }
      swapchain_image_views.clear();

      if (swapchain) {
        device().destroySwapchainKHR(swapchain);
        swapchain = nullptr;
      }
      swapchain_images.clear();
      swapchain_layouts.clear();
      swapchain_extent = vk::Extent2D{.width = 0, .height = 0};
    }

    void destroy_vulkan_presentation()
    {
      MR_TRACY_ZONE;
      if (context && context->vk_device()) {
        static_cast<void>(device().waitIdle());
      }
      if (in_flight) {
        static_cast<void>(device().waitForFences(in_flight, VK_TRUE, UINT64_MAX));
      }
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

      if (surface && context && context->vk_instance()) {
        context->vk_instance().destroySurfaceKHR(surface);
        surface = nullptr;
      }
    }

    void shutdown()
    {
      MR_TRACY_ZONE;
      if (context && context->vk_device()) {
        static_cast<void>(device().waitIdle());
      }
      destroy_vulkan_presentation();
      context.reset();

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
      tracy_vk_ctx = TracyVkContext(
        context->physical_device.physical_device,
        context->device.device,
        queue,
        tracy_command_buffer);
#endif
    }

    [[nodiscard]] static vk::SurfaceFormatKHR select_swapchain_format(const std::vector<vk::SurfaceFormatKHR>& formats)
    {
      MR_TRACY_ZONE_N("WindowPresenter::select_swapchain_format");
      const auto chosen_format_it = std::ranges::find_if(formats, [](const vk::SurfaceFormatKHR& fmt) {
        return fmt.format == vk::Format::eR8G8B8A8Srgb || fmt.format == vk::Format::eR8G8B8A8Unorm ||
          fmt.format == vk::Format::eB8G8R8A8Srgb || fmt.format == vk::Format::eB8G8R8A8Unorm;
      });
      return chosen_format_it != formats.end() ? *chosen_format_it : formats.front();
    }

    [[nodiscard]] static vk::Extent2D compute_swapchain_extent(
      uint32_t width,
      uint32_t height,
      const vk::SurfaceCapabilitiesKHR& caps)
    {
      MR_TRACY_ZONE_N("WindowPresenter::compute_swapchain_extent");
      vk::Extent2D extent{.width = width, .height = height};
      if (caps.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
        extent = caps.currentExtent;
      } else {
        extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
      }
      return extent;
    }

    void rebuild_swapchain_image_views()
    {
      MR_TRACY_ZONE_N("WindowPresenter::rebuild_swapchain_image_views");
      for (vk::ImageView view : swapchain_image_views) {
        if (view) {
          device().destroyImageView(view);
        }
      }
      swapchain_image_views.clear();
      swapchain_image_views.reserve(swapchain_images.size());
      for (const vk::Image& img : swapchain_images) {
        vk::ImageViewCreateInfo vi{};
        vi.image = img;
        vi.viewType = vk::ImageViewType::e2D;
        vi.format = swapchain_format;
        vi.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        swapchain_image_views.push_back(vk_expect(device().createImageView(vi), "createImageView(swapchain) failed"));
      }
    }

    void rebuild_swapchain_frame_semaphores()
    {
      MR_TRACY_ZONE_N("WindowPresenter::rebuild_swapchain_frame_semaphores");
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
        image_available_semaphores.push_back(
          vk_expect(device().createSemaphore({}), "createSemaphore(image_available) failed"));
        render_finished_semaphores.push_back(
          vk_expect(device().createSemaphore({}), "createSemaphore(render_finished) failed"));
      }
      acquire_semaphore_cursor = 0;
    }

    void recreate_swapchain(uint32_t width, uint32_t height)
    {
      MR_TRACY_ZONE;
      ASSERT(width > 0 && height > 0, "swapchain dimensions must be non-zero");
      ASSERT(device().waitIdle() == vk::Result::eSuccess, "waitIdle before swapchain recreation failed");

      const vk::SurfaceCapabilitiesKHR caps = vk_expect(
        context->vk_physical_device().getSurfaceCapabilitiesKHR(surface), "getSurfaceCapabilitiesKHR failed");
      const auto formats =
        vk_expect(context->vk_physical_device().getSurfaceFormatsKHR(surface), "getSurfaceFormatsKHR failed");
      ASSERT(!formats.empty(), "surface returned no formats");

      const vk::SurfaceFormatKHR chosen_format = select_swapchain_format(formats);
      swapchain_format = chosen_format.format;

      const vk::Extent2D extent = compute_swapchain_extent(width, height, caps);

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
      create_info.imageUsage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
      create_info.imageSharingMode = vk::SharingMode::eExclusive;
      create_info.preTransform = caps.currentTransform;
      create_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
      create_info.presentMode = vk::PresentModeKHR::eImmediate;
      create_info.clipped = VK_TRUE;
      create_info.oldSwapchain = old_swapchain;
      swapchain = vk_expect(device().createSwapchainKHR(create_info), "createSwapchainKHR failed");

      if (old_swapchain) {
        device().destroySwapchainKHR(old_swapchain);
      }

      swapchain_images = vk_expect(device().getSwapchainImagesKHR(swapchain), "getSwapchainImagesKHR failed");
      ASSERT(!swapchain_images.empty(), "swapchain returned no images");
      swapchain_layouts.assign(swapchain_images.size(), vk::ImageLayout::eUndefined);
      swapchain_extent = extent;

      rebuild_swapchain_image_views();
      rebuild_swapchain_frame_semaphores();

      ensure_staging_buffer(
        static_cast<vk::DeviceSize>(swapchain_extent.width) *
        static_cast<vk::DeviceSize>(swapchain_extent.height) * 4u);

      ++swapchain_generation;
    }

    void initialize(uint32_t width, uint32_t height)
    {
      MR_TRACY_ZONE;
      create_window(width, height);

      const auto required_extensions = vkfw::getRequiredInstanceExtensions();
      std::vector<const char*> ext_ptrs{};
      ext_ptrs.reserve(required_extensions.size());
      for (const char* const ext_name : required_extensions) {
        ext_ptrs.push_back(ext_name);
      }

      auto instance_result = create_vulkan_instance(
        "mr-renderer",
        false,
        std::span<const char* const>(ext_ptrs.data(), ext_ptrs.size()));
      ASSERT(instance_result.has_value(), "create_vulkan_instance failed", instance_result.error());

      surface = vkfw::createWindowSurface(instance_result.value().instance, window, nullptr);
      ASSERT(surface, "vkfw::createWindowSurface failed");

      VulkanContextCreateInfo ci{};
      ci.headless = false;
      ci.require_present = true;
      ci.surface = surface;
      ci.prefer_dedicated_compute_queue = true;

      auto context_result = create_vulkan_context(ci, instance_result.value());
      ASSERT(context_result.has_value(), "create_vulkan_context failed", context_result.error());
      context = std::make_unique<VulkanContext>(std::move(*context_result));

      queue_family = context->has_present_queue() ? context->present_queue_family : context->graphics_queue_family;
      queue = context->has_present_queue() ? context->present_queue : context->graphics_queue;
      ASSERT(static_cast<bool>(queue), "failed to get queue for presentation");

      create_command_resources();
      recreate_swapchain(width, height);
    }

    std::optional<Target> acquire_one_target(uint32_t target_index)
    {
      MR_TRACY_ZONE;
      ASSERT(!image_available_semaphores.empty(), "image_available semaphores are not initialized");
      const vk::Semaphore acquire_semaphore =
        image_available_semaphores.at(acquire_semaphore_cursor % image_available_semaphores.size());
      ++acquire_semaphore_cursor;

      const auto acquire_rv = device().acquireNextImageKHR(swapchain, UINT64_MAX, acquire_semaphore, vk::Fence{});
      if (acquire_rv.result == vk::Result::eErrorOutOfDateKHR) {
        recreate_swapchain(swapchain_extent.width, swapchain_extent.height);
        return std::nullopt;
      }
      if (acquire_rv.result != vk::Result::eSuccess && acquire_rv.result != vk::Result::eSuboptimalKHR) {
        ASSERT(false, "acquireNextImageKHR failed", static_cast<int>(acquire_rv.result));
      }
      const uint32_t image_index = acquire_rv.value;

      GpuTarget gpu{};
      gpu.context = context.get();
      gpu.image = swapchain_images.at(image_index);
      gpu.view = swapchain_image_views.at(image_index);
      gpu.format = swapchain_format;
      gpu.extent = swapchain_extent;
      gpu.slot_id = image_index;
      gpu.generation = swapchain_generation;
      gpu.acquire_semaphore = acquire_semaphore;
      gpu.acquire_stage = vk::PipelineStageFlagBits::eColorAttachmentOutput;
      gpu.render_finished_semaphore = render_finished_semaphores.at(image_index);

      return Target{.index = target_index, .payload = gpu};
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
      const std::span<const float> src = std::span<const float>(frame.rgba32f).subspan(0, expected_floats);
      const std::span<uint8_t> dst = std::span<uint8_t>(
        reinterpret_cast<uint8_t*>(map_rv.value),
        expected_floats); // same byte count as floats for RGBA8
      const size_t pixel_count =
        static_cast<size_t>(swapchain_extent.width) * static_cast<size_t>(swapchain_extent.height);
      const bool bgra =
        swapchain_format == vk::Format::eB8G8R8A8Srgb || swapchain_format == vk::Format::eB8G8R8A8Unorm;
      for (size_t i = 0; i < pixel_count; ++i) {
        const size_t base = i * 4u;
        const uint8_t r = linear_to_srgb_u8(src.at(base + 0u));
        const uint8_t g = linear_to_srgb_u8(src.at(base + 1u));
        const uint8_t b = linear_to_srgb_u8(src.at(base + 2u));
        const uint8_t a = linear_to_srgb_u8(src.at(base + 3u));
        if (bgra) {
          dst.at(base + 0u) = b;
          dst.at(base + 1u) = g;
          dst.at(base + 2u) = r;
          dst.at(base + 3u) = a;
        } else {
          dst.at(base + 0u) = r;
          dst.at(base + 1u) = g;
          dst.at(base + 2u) = b;
          dst.at(base + 3u) = a;
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
      waits.at(0) = acquire_semaphore;
      wait_stages.at(0) = acquire_stage;
      if (extra_wait.has_value() && static_cast<bool>(*extra_wait)) {
        waits.at(1) = *extra_wait;
        wait_stages.at(1) = extra_stage;
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
      handle_present_khr_result(queue.presentKHR(present_info));
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

    void handle_present_khr_result(vk::Result present_result)
    {
      MR_TRACY_ZONE;
      if (present_result == vk::Result::eErrorOutOfDateKHR ||
          present_result == vk::Result::eSuboptimalKHR) {
        recreate_swapchain(swapchain_extent.width, swapchain_extent.height);
      } else {
        ASSERT(present_result == vk::Result::eSuccess, "presentKHR failed", static_cast<int>(present_result));
      }
    }

    [[nodiscard]] bool window_ok_for_present() const
    {
      MR_TRACY_ZONE;
      const auto poll_result = vkfw::pollEvents();
      ASSERT(vkfw::check(poll_result), "vkfw::pollEvents failed");
      if (!window) {
        return false;
      }
      const auto close_result = window.shouldClose();
      ASSERT(vkfw::check(close_result.result), "vkfw::Window::shouldClose failed");
      return !close_result.value;
    }

    [[nodiscard]] bool try_present_generator_gpu_frame(const GpuFrame& gpu_frame)
    {
      MR_TRACY_ZONE;
      if (!gpu_frame.render_finished_semaphore || gpu_frame.presenter_slot_id == UINT32_MAX) {
        return false;
      }

      ASSERT(
        gpu_frame.presenter_generation == swapchain_generation,
        "GpuFrame swapchain generation mismatch (swapchain was recreated)",
        gpu_frame.presenter_generation,
        swapchain_generation);
      ASSERT(
        gpu_frame.presenter_slot_id < swapchain_images.size(),
        "GpuFrame presenter_slot_id out of range",
        gpu_frame.presenter_slot_id,
        swapchain_images.size());

      vk::PresentInfoKHR present_info{};
      present_info.waitSemaphoreCount = 1;
      present_info.pWaitSemaphores = &gpu_frame.render_finished_semaphore;
      present_info.swapchainCount = 1;
      present_info.pSwapchains = &swapchain;
      const uint32_t image_index = gpu_frame.presenter_slot_id;
      present_info.pImageIndices = &image_index;

      handle_present_khr_result(queue.presentKHR(present_info));
      swapchain_layouts.at(image_index) = vk::ImageLayout::ePresentSrcKHR;
      return true;
    }

    void wait_reset_in_flight_fence() const
    {
      MR_TRACY_ZONE;
      ASSERT(device().waitForFences(in_flight, true, UINT64_MAX) == vk::Result::eSuccess, "waitForFences failed");
      ASSERT(device().resetFences(in_flight) == vk::Result::eSuccess, "resetFences failed");
    }

    struct PendingSwapchainAcquire {
      uint32_t image_index{};
      vk::Semaphore acquire_semaphore{};
    };

    [[nodiscard]] std::optional<PendingSwapchainAcquire> acquire_swapchain_for_upload_present()
    {
      MR_TRACY_ZONE;
      wait_reset_in_flight_fence();

      ASSERT(!image_available_semaphores.empty(), "image_available semaphores are not initialized");
      const vk::Semaphore acquire_semaphore =
        image_available_semaphores.at(acquire_semaphore_cursor % image_available_semaphores.size());
      ++acquire_semaphore_cursor;

      const auto acquire_rv =
        device().acquireNextImageKHR(swapchain, UINT64_MAX, acquire_semaphore, vk::Fence{});
      if (acquire_rv.result == vk::Result::eErrorOutOfDateKHR) {
        recreate_swapchain(swapchain_extent.width, swapchain_extent.height);
        return std::nullopt;
      }
      ASSERT(
        acquire_rv.result == vk::Result::eSuccess || acquire_rv.result == vk::Result::eSuboptimalKHR,
        "acquireNextImageKHR failed",
        static_cast<int>(acquire_rv.result));

      return PendingSwapchainAcquire{.image_index = acquire_rv.value, .acquire_semaphore = acquire_semaphore};
    }
  };

  WindowPresenter::WindowPresenter(uint32_t width, uint32_t height)
    : impl_(std::make_unique<Impl>())
  {
    MR_TRACY_ZONE;
    impl_->initialize(width == 0 ? 1u : width, height == 0 ? 1u : height);
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

  const VulkanContext& WindowPresenter::vulkan_context() const
  {
    MR_TRACY_ZONE_N("WindowPresenter::vulkan_context");
    ASSERT(impl_ != nullptr && impl_->context != nullptr, "WindowPresenter has no Vulkan context");
    return *impl_->context;
  }

  coro::generator<Target> WindowPresenter::targets()
  {
    MR_TRACY_ZONE_N("WindowPresenter::targets");
    ASSERT(impl_ != nullptr, "WindowPresenter impl is null");
    uint32_t index = 0;
    while (true) {
      const auto poll_result = vkfw::pollEvents();
      ASSERT(vkfw::check(poll_result), "vkfw::pollEvents failed");

      if (!impl_->window) {
        co_return;
      }
      const auto close_result = impl_->window.shouldClose();
      ASSERT(vkfw::check(close_result.result), "vkfw::Window::shouldClose failed");
      if (close_result.value) {
        co_return;
      }

      auto acquired = impl_->acquire_one_target(index++);
      if (!acquired.has_value()) {
        continue;
      }
      co_yield *acquired;
    }
  }

  void WindowPresenter::present(Frame frame)
  {
    MR_TRACY_ZONE_N("WindowPresenter::present");
    MR_TRACY_FRAME("window_present");
    ASSERT(impl_ != nullptr, "WindowPresenter impl is null");

    if (!impl_->window_ok_for_present()) {
      return;
    }

    const GpuFrame* gpu_frame = frame.gpu();
    if (gpu_frame != nullptr && impl_->try_present_generator_gpu_frame(*gpu_frame)) {
      return;
    }

    const auto acquired = impl_->acquire_swapchain_for_upload_present();
    if (!acquired.has_value()) {
      return;
    }

    if (gpu_frame != nullptr) {
      ASSERT(
        gpu_frame->width == impl_->swapchain_extent.width && gpu_frame->height == impl_->swapchain_extent.height,
        "GpuFrame size must match window size");
      impl_->submit_gpu_copy_and_present(acquired->image_index, *gpu_frame, acquired->acquire_semaphore);
      return;
    }

    const CpuFrame* cpu_frame = frame.cpu();
    ASSERT(cpu_frame != nullptr, "frame has no supported payload");
    impl_->upload_cpu_frame_pixels(*cpu_frame);
    impl_->submit_cpu_copy_and_present(acquired->image_index, acquired->acquire_semaphore);
  }
} // namespace mr
