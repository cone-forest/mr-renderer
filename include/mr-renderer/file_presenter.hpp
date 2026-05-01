#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "presenter.hpp"

namespace mr {
  struct FilePresenter : IPresenter {
    FilePresenter(
      std::filesystem::path output_directory,
      uint32_t width,
      uint32_t height,
      uint32_t parallel_images = 1);

    coro::generator<Target> targets() override;
    void present(Frame frame) override;

  private:
    std::filesystem::path output_directory_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t parallel_images_ = 1;
    std::vector<std::vector<float>> pool_{};
    bool warned_gpu_fallback_ = false;
  };
} // namespace mr
