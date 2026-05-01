#include "pch.hpp"
#include <mr-renderer/file_presenter.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <libassert/assert.hpp>
#include <sstream>
#include <span>
#include <string>
#include <vector>

namespace mr {
  namespace {
    uint8_t linear_to_srgb_u8(float x)
    {
      MR_TRACY_ZONE;
      x = std::clamp(x, 0.f, 1.f);
      const float s =
        x <= 0.0031308f ? (12.92f * x) : ((1.055f * std::pow(x, 1.f / 2.4f)) - 0.055f);
      const auto out = std::lround(s * 255.f);
      return static_cast<uint8_t>(std::clamp(out, 0L, 255L));
    }

    void warn_file_presenter_gpu_path(bool& warned_flag)
    {
      MR_TRACY_ZONE;
      if (warned_flag) {
        return;
      }
      warned_flag = true;
      static_cast<void>(std::fprintf(
        stderr,
        "[mr-renderer] FilePresenter warning: consuming GPU frames requires GPU->CPU readback and may be slow.\n"));
    }

    [[nodiscard]] std::span<const float> cpu_frame_linear_span(const CpuFrame& cpu_frame, const std::vector<std::vector<float>>& pool)
    {
      MR_TRACY_ZONE;
      if (cpu_frame.presenter_slot_id != UINT32_MAX) {
        ASSERT(cpu_frame.presenter_slot_id < pool.size(), "FilePresenter: invalid presenter slot id");
        const auto& slot_buf = pool.at(cpu_frame.presenter_slot_id);
        ASSERT(
          slot_buf.size() >= static_cast<size_t>(cpu_frame.width) * static_cast<size_t>(cpu_frame.height) * 4u,
          "FilePresenter: pooled buffer too small for frame dimensions");
        return std::span<const float>{slot_buf.data(), slot_buf.size()};
      }
      const size_t expected_floats =
        static_cast<size_t>(cpu_frame.width) * static_cast<size_t>(cpu_frame.height) * 4u;
      ASSERT(cpu_frame.rgba32f.size() >= expected_floats, "FilePresenter: frame RGBA32F buffer is too small");
      return std::span<const float>{cpu_frame.rgba32f.data(), expected_floats};
    }

    void floats_linear_to_rgba8_bytes(std::span<const float> px, std::span<uint8_t> rgba8_out)
    {
      MR_TRACY_ZONE;
      ASSERT(px.size() >= rgba8_out.size(), "pixel span mismatch");
      const size_t count = rgba8_out.size() / 4u;
      for (size_t i = 0; i < count; ++i) {
        const size_t base = i * 4u;
        rgba8_out.at(base + 0u) = linear_to_srgb_u8(px.at(base + 0u));
        rgba8_out.at(base + 1u) = linear_to_srgb_u8(px.at(base + 1u));
        rgba8_out.at(base + 2u) = linear_to_srgb_u8(px.at(base + 2u));
        rgba8_out.at(base + 3u) = linear_to_srgb_u8(px.at(base + 3u));
      }
    }
  } // namespace

  FilePresenter::FilePresenter(
    std::filesystem::path output_directory,
    uint32_t width,
    uint32_t height,
    uint32_t parallel_images)
    : output_directory_(std::move(output_directory))
    , width_(width == 0 ? 1u : width)
    , height_(height == 0 ? 1u : height)
    , parallel_images_(parallel_images == 0 ? 1u : parallel_images)
  {
    MR_TRACY_ZONE_N("FilePresenter::FilePresenter");
    pool_.resize(parallel_images_);
    const size_t floats_per_image =
      static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;
    for (auto& slot : pool_) {
      slot.resize(floats_per_image);
    }
  }

  coro::generator<Target> FilePresenter::targets()
  {
    MR_TRACY_ZONE_N("FilePresenter::targets");
    uint32_t index = 0;
    uint32_t slot_cursor = 0;
    while (true) {
      const uint32_t slot = slot_cursor++ % parallel_images_;
      CpuTarget cpu{};
      cpu.presenter_slot_id = slot;
      cpu.presenter_generation = 0;
      cpu.pixels = RgbaFloatRasterView{
        .data = pool_.at(slot).data(),
        .height = static_cast<std::size_t>(height_),
        .width = static_cast<std::size_t>(width_),
      };
      co_yield Target{.index = index++, .payload = cpu};
    }
  }

  void FilePresenter::present(Frame frame)
  {
    MR_TRACY_ZONE_N("FilePresenter::present");
    MR_TRACY_FRAME("file_present");
    if (frame.is_gpu()) {
      warn_file_presenter_gpu_path(warned_gpu_fallback_);
    }
    CpuFrame cpu_frame = frame.to_cpu_frame();
    ASSERT(cpu_frame.width > 0 && cpu_frame.height > 0, "FilePresenter: invalid image dimensions");

    const int w = static_cast<int>(cpu_frame.width);
    const int h = static_cast<int>(cpu_frame.height);
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t float_count = count * 4u;
    const std::span<const float> px_full = cpu_frame_linear_span(cpu_frame, pool_);
    ASSERT(px_full.size() >= float_count, "FilePresenter: linear RGBA buffer shorter than frame dimensions");
    const std::span<const float> px = px_full.subspan(0, float_count);

    std::vector<uint8_t> rgba8(float_count);
    floats_linear_to_rgba8_bytes(px, rgba8);

    std::ostringstream name;
    name << "frame_" << frame.index << ".png";
    const std::filesystem::path out_path = output_directory_ / name.str();

    if (!std::filesystem::exists(output_directory_)) {
      std::filesystem::create_directories(output_directory_);
    }

    const int stride = w * 4;
    ASSERT(stbi_write_png(out_path.string().c_str(), w, h, 4, rgba8.data(), stride) != 0, "stbi_write_png failed for " + out_path.string());
  }

} // namespace mr
