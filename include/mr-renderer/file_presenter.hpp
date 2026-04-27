#pragma once

#include <filesystem>

#include "presenter.hpp"

namespace mr {
  struct FilePresenter : IPresenter {
    explicit FilePresenter(std::filesystem::path output_directory);

    void present(Frame frame) override;

  private:
    std::filesystem::path output_directory_;
    bool warned_gpu_fallback_ = false;
  };
} // namespace mr
