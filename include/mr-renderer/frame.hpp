#pragma once

#include <cstdint>
#include <vector>

namespace mr {
  struct Frame {
    uint32_t index = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> rgba32f;
  };
} // namespace mr
