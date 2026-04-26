#pragma once

#include <cstdint>

#include <mr-importer/importer.hpp>

namespace mr {
  struct Frame {
    uint32_t index = 0;
    mr::importer::ImageData color;
  };
} // namespace mr
