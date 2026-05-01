#include "pch.hpp"

#include <mr-renderer/frame.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>

#include <libassert/assert.hpp>

#include <bit>
#include <cstddef>
#include <limits>
#include <span>

namespace mr {
  namespace {
    uint32_t queue_family_for_context(const VulkanContext& context)
    {
      MR_TRACY_ZONE_N("queue_family_for_context");
      if (context.graphics_queue_family != std::numeric_limits<uint32_t>::max()) {
        return context.graphics_queue_family;
      }
      return context.compute_queue_family;
    }

    [[nodiscard]] vk::CommandPool make_transient_command_pool(const VulkanContext& context)
    {
      MR_TRACY_ZONE_N("make_transient_command_pool");
      vk::CommandPoolCreateInfo pool_info{};
      pool_info.queueFamilyIndex = queue_family_for_context(context);
      pool_info.flags = vk::CommandPoolCreateFlagBits::eTransient;
      const auto pool_rv = context.vk_device().createCommandPool(pool_info);
      ASSERT(pool_rv.result == vk::Result::eSuccess, "GpuFrame conversion createCommandPool failed");
      return pool_rv.value;
    }

    [[nodiscard]] vk::CommandBuffer allocate_primary_command_buffer(const VulkanContext& context, vk::CommandPool pool)
    {
      MR_TRACY_ZONE_N("allocate_primary_command_buffer");
      vk::CommandBufferAllocateInfo alloc_info{};
      alloc_info.commandPool = pool;
      alloc_info.level = vk::CommandBufferLevel::ePrimary;
      alloc_info.commandBufferCount = 1;
      const auto cmd_rv = context.vk_device().allocateCommandBuffers(alloc_info);
      ASSERT(cmd_rv.result == vk::Result::eSuccess, "GpuFrame conversion allocateCommandBuffers failed");
      return cmd_rv.value.at(0);
    }

    void record_gpu_frame_to_host_copy(vk::CommandBuffer cmd, const GpuFrame& gpu_frame, const HostBuffer& readback)
    {
      MR_TRACY_ZONE_N("record_gpu_frame_to_host_copy");
      vk::ImageMemoryBarrier to_transfer_src{};
      to_transfer_src.oldLayout = gpu_frame.layout;
      to_transfer_src.newLayout = vk::ImageLayout::eTransferSrcOptimal;
      to_transfer_src.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
      to_transfer_src.image = gpu_frame.image;
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
      copy_region.imageExtent = vk::Extent3D{.width = gpu_frame.width, .height = gpu_frame.height, .depth = 1u};
      cmd.copyImageToBuffer(
        gpu_frame.image,
        vk::ImageLayout::eTransferSrcOptimal,
        readback.buffer(),
        copy_region);

      if (gpu_frame.layout != vk::ImageLayout::eTransferSrcOptimal) {
        vk::ImageMemoryBarrier restore_layout{};
        restore_layout.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        restore_layout.newLayout = gpu_frame.layout;
        restore_layout.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_layout.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
        restore_layout.image = gpu_frame.image;
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
    }

    void submit_graphics_cmd_and_wait(const VulkanContext& context, vk::CommandBuffer cmd, const GpuFrame& gpu_frame)
    {
      MR_TRACY_ZONE_N("submit_graphics_cmd_and_wait");
      vk::SubmitInfo submit_info{};
      if (gpu_frame.ready_semaphore) {
        submit_info.waitSemaphoreCount = 1;
        submit_info.pWaitSemaphores = &gpu_frame.ready_semaphore;
        submit_info.pWaitDstStageMask = &gpu_frame.wait_stage;
      }
      submit_info.commandBufferCount = 1;
      submit_info.pCommandBuffers = &cmd;
      ASSERT(
        context.graphics_queue.submit(submit_info, vk::Fence{}) == vk::Result::eSuccess,
        "GpuFrame conversion queue submit failed");
      ASSERT(
        context.graphics_queue.waitIdle() == vk::Result::eSuccess,
        "GpuFrame conversion queue wait idle failed");
    }

    void unpack_rgba8_bytes_to_linear_float(std::vector<float>& rgba32f, std::span<const std::byte> bytes)
    {
      MR_TRACY_ZONE_N("unpack_rgba8_bytes_to_linear_float");
      ASSERT(rgba32f.size() == bytes.size(), "GpuFrame conversion byte size mismatch");
      for (size_t i = 0; i < bytes.size(); ++i) {
        rgba32f.at(i) = static_cast<float>(std::to_integer<uint8_t>(bytes.at(i))) / 255.0f;
      }
    }
  } // namespace

  CpuFrame::CpuFrame(uint32_t width, uint32_t height, std::vector<float> rgba32f)
    : width(width)
    , height(height)
    , rgba32f(std::move(rgba32f))
  {
    MR_TRACY_ZONE_N("CpuFrame::CpuFrame(linear)");
  }

  CpuFrame::CpuFrame(const GpuFrame& gpu_frame)
  {
    MR_TRACY_ZONE_N("CpuFrame::CpuFrame(GpuFrame)");
    ASSERT(gpu_frame.context != nullptr, "GpuFrame conversion requires a valid VulkanContext");
    ASSERT(gpu_frame.image, "GpuFrame conversion requires a valid vk::Image");
    ASSERT(gpu_frame.width > 0 && gpu_frame.height > 0, "GpuFrame conversion requires non-zero dimensions");
    ASSERT(
      gpu_frame.format == vk::Format::eR8G8B8A8Unorm || gpu_frame.format == vk::Format::eR8G8B8A8Srgb,
      "GpuFrame conversion currently supports only R8G8B8A8 formats");

    const VulkanContext& context = *gpu_frame.context;
    const vk::DeviceSize byte_size =
      static_cast<vk::DeviceSize>(gpu_frame.width) * static_cast<vk::DeviceSize>(gpu_frame.height) * 4u;

    HostBuffer readback(
      context,
      byte_size,
      vk::BufferUsageFlagBits::eTransferDst,
      vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);

    const vk::CommandPool pool = make_transient_command_pool(context);
    const vk::CommandBuffer cmd = allocate_primary_command_buffer(context, pool);

    vk::CommandBufferBeginInfo begin_info{};
    begin_info.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    ASSERT(cmd.begin(begin_info) == vk::Result::eSuccess, "GpuFrame conversion begin command buffer failed");

    record_gpu_frame_to_host_copy(cmd, gpu_frame, readback);

    ASSERT(cmd.end() == vk::Result::eSuccess, "GpuFrame conversion end command buffer failed");

    submit_graphics_cmd_and_wait(context, cmd, gpu_frame);

    context.vk_device().freeCommandBuffers(pool, cmd);
    context.vk_device().destroyCommandPool(pool);

    const std::span<const std::byte> bytes = readback.read();
    width = gpu_frame.width;
    height = gpu_frame.height;
    rgba32f.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    unpack_rgba8_bytes_to_linear_float(rgba32f, bytes);
  }

  GpuFrame::operator CpuFrame() const
  {
    MR_TRACY_ZONE;
    return CpuFrame(*this);
  }

  Frame::Frame(uint32_t index, CpuFrame cpu_frame)
    : index(index)
    , payload(std::move(cpu_frame))
  {
    MR_TRACY_ZONE_N("Frame::Frame(CpuFrame)");
  }

  Frame::Frame(uint32_t index, GpuFrame gpu_frame)
    : index(index)
    , payload(gpu_frame)
  {
    MR_TRACY_ZONE_N("Frame::Frame(GpuFrame)");
  }

  bool Frame::is_cpu() const noexcept
  {
    MR_TRACY_ZONE;
    return std::holds_alternative<CpuFrame>(payload);
  }

  bool Frame::is_gpu() const noexcept
  {
    MR_TRACY_ZONE;
    return std::holds_alternative<GpuFrame>(payload);
  }

  const CpuFrame* Frame::cpu() const noexcept
  {
    MR_TRACY_ZONE;
    return std::get_if<CpuFrame>(&payload);
  }

  const GpuFrame* Frame::gpu() const noexcept
  {
    MR_TRACY_ZONE;
    return std::get_if<GpuFrame>(&payload);
  }

  uint32_t Frame::width() const noexcept
  {
    MR_TRACY_ZONE;
    if (const auto* cpu_frame = cpu()) {
      return cpu_frame->width;
    }
    if (const auto* gpu_frame = gpu()) {
      return gpu_frame->width;
    }
    return 0;
  }

  uint32_t Frame::height() const noexcept
  {
    MR_TRACY_ZONE;
    if (const auto* cpu_frame = cpu()) {
      return cpu_frame->height;
    }
    if (const auto* gpu_frame = gpu()) {
      return gpu_frame->height;
    }
    return 0;
  }

  CpuFrame Frame::to_cpu_frame() const
  {
    MR_TRACY_ZONE;
    if (const auto* cpu_frame = cpu()) {
      return *cpu_frame;
    }
    ASSERT(gpu() != nullptr, "Frame payload has no known representation");
    return CpuFrame(*gpu());
  }
} // namespace mr
