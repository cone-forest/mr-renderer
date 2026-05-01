#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include <vulkan/vulkan.hpp>

namespace mr {
  struct VulkanContext;

  struct GpuFrame;

  struct CpuFrame {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t presenter_slot_id = UINT32_MAX;
    uint64_t presenter_generation = 0;
    std::vector<float> rgba32f{};

    CpuFrame() = default;
    CpuFrame(uint32_t width, uint32_t height, std::vector<float> rgba32f);
    explicit CpuFrame(const GpuFrame& gpu_frame);
  };

  struct GpuFrame {
    const VulkanContext* context = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    vk::Image image{};
    vk::ImageLayout layout = vk::ImageLayout::eTransferSrcOptimal;
    vk::Format format = vk::Format::eR8G8B8A8Unorm;
    vk::Semaphore ready_semaphore{};
    vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    uint32_t presenter_slot_id = UINT32_MAX;
    uint64_t presenter_generation = 0;
    vk::Semaphore render_finished_semaphore{};

    GpuFrame() = default;
    explicit operator CpuFrame() const;
  };

  using FramePayload = std::variant<CpuFrame, GpuFrame>;

  struct Frame {
    uint32_t index = 0;
    FramePayload payload = CpuFrame{};

    Frame() = default;
    Frame(uint32_t index, CpuFrame cpu_frame);
    Frame(uint32_t index, GpuFrame gpu_frame);

    [[nodiscard]] bool is_cpu() const noexcept;
    [[nodiscard]] bool is_gpu() const noexcept;

    [[nodiscard]] const CpuFrame* cpu() const noexcept;
    [[nodiscard]] const GpuFrame* gpu() const noexcept;

    [[nodiscard]] uint32_t width() const noexcept;
    [[nodiscard]] uint32_t height() const noexcept;
    [[nodiscard]] CpuFrame to_cpu_frame() const;
  };
} // namespace mr
