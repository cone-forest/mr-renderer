#include "pch.hpp"
#include <mr-renderer/file_presenter.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
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
    const auto& img = frame.color;
    ASSERT(img.format == vk::Format::eR32G32B32A32Sfloat, "FilePresenter: expected R32G32B32A32_SFLOAT image data");
    ASSERT(img.width > 0 && img.height > 0 && img.pixels.get() != nullptr, "FilePresenter: invalid image dimensions or pixel buffer");

    const int w = img.width;
    const int h = img.height;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    const auto* px = reinterpret_cast<const float*>(img.pixels.get());

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
