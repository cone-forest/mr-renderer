#include "pch.hpp"
#include <mr-renderer/file_presenter.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <libassert/assert.hpp>
#include <sstream>
#include <string>
#include <vector>

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

  FilePresenter::FilePresenter(std::filesystem::path output_directory)
    : output_directory_(std::move(output_directory))
  {}

  void FilePresenter::present(Frame frame)
  {
    if (frame.is_gpu() && !warned_gpu_fallback_) {
      warned_gpu_fallback_ = true;
      static_cast<void>(std::fprintf(
        stderr,
        "[mr-renderer] FilePresenter warning: consuming GPU frames requires GPU->CPU readback and may be slow.\n"));
    }
    CpuFrame cpu_frame = frame.to_cpu_frame();
    ASSERT(cpu_frame.width > 0 && cpu_frame.height > 0, "FilePresenter: invalid image dimensions");
    const size_t expected_floats =
      static_cast<size_t>(cpu_frame.width) * static_cast<size_t>(cpu_frame.height) * 4u;
    ASSERT(cpu_frame.rgba32f.size() >= expected_floats, "FilePresenter: frame RGBA32F buffer is too small");

    const int w = static_cast<int>(cpu_frame.width);
    const int h = static_cast<int>(cpu_frame.height);
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const auto* px = cpu_frame.rgba32f.data();

    std::vector<uint8_t> rgba8(count * 4u);
    for (size_t i = 0; i < count; ++i) {
      const float r = px[i * 4u + 0u];
      const float g = px[i * 4u + 1u];
      const float b = px[i * 4u + 2u];
      const float a = px[i * 4u + 3u];
      rgba8[i * 4u + 0u] = linear_to_srgb_u8(r);
      rgba8[i * 4u + 1u] = linear_to_srgb_u8(g);
      rgba8[i * 4u + 2u] = linear_to_srgb_u8(b);
      rgba8[i * 4u + 3u] = linear_to_srgb_u8(a);
    }

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
