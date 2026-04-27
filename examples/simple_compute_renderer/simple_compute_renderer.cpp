#include "pch.hpp"
#include "simple_compute_renderer.hpp"

#include <algorithm>
#include <libassert/assert.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>
#include <mr-importer/compiler.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <vector>

#ifndef MR_RENDERER_LIB_SHADER_DIR
#define MR_RENDERER_LIB_SHADER_DIR "shaders"
#endif

namespace mr {
  namespace {

    std::vector<uint32_t> spirv_words(const mr::importer::Shader& shader)
    {
      const size_t nbytes = shader.spirv.size();
      ASSERT(nbytes % 4 == 0, "SPIR-V size is not a multiple of 4");
      std::vector<uint32_t> words(nbytes / 4);
      std::memcpy(words.data(), shader.spirv.get(), nbytes);
      return words;
    }

    const mr::importer::Shader* pick_compute(const std::vector<mr::importer::Shader>& shaders)
    {
      const auto it = std::ranges::find_if(shaders, [](const mr::importer::Shader& shader) -> bool {
        return shader.stage == vk::ShaderStageFlagBits::eCompute;
      });
      if (it == shaders.end()) {
        return nullptr;
      }
      return &(*it);
    }

    void fill_frame_pixels(
      CpuFrame& dst,
      const std::vector<float>& linear_rgba,
      uint32_t w,
      uint32_t h)
    {
      const size_t expected_floats = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
      ASSERT(linear_rgba.size() >= expected_floats, "tensor data smaller than expected RGBA float image");

      dst.width = w;
      dst.height = h;
      dst.rgba32f.assign(linear_rgba.begin(), linear_rgba.begin() + expected_floats);
    }

    const char *physical_device_index_env()
    {
      if (const char *var = std::getenv("MR_VK_PHYSICAL_DEVICE_INDEX")) {
        return var;
      }
      return std::getenv("VK_PHYSICAL_DEVICE_INDEX");
    }

    /**
     * Physical device index for Kompute. Override with MR_VK_PHYSICAL_DEVICE_INDEX
     * or VK_PHYSICAL_DEVICE_INDEX (decimal, must be in range). Otherwise prefers a
     * discrete NVIDIA GPU, then any discrete GPU, then index 0.
     */
    uint32_t kompute_physical_device_index()
    {
      const auto device_props_result = enumerate_vulkan_physical_devices();
      ASSERT(device_props_result.has_value(),
        "failed to enumerate Vulkan physical devices: ",
        device_props_result.error());
      const std::vector<VulkanPhysicalDeviceInfo>& device_props = *device_props_result;
      if (device_props.empty()) {
        return 0;
      }

      if (const char *const env = physical_device_index_env()) {
        if (env[0] != '\0') {
          char *end = nullptr;
          const unsigned long v = std::strtoul(env, &end, 10);
          if (end != env) {
            const auto idx = static_cast<uint32_t>(v);
            ASSERT(idx < device_props.size(),
                "physical device index is out of range;"
                " check MR_VK_PHYSICAL_DEVICE_INDEX / VK_PHYSICAL_DEVICE_INDEX",
                idx, device_props.size());
            return idx;
          }
        }
      }

      constexpr uint32_t kVendorNvidia = 0x10DEu;

      uint32_t first_discrete = 0;
      bool have_discrete = false;
      uint32_t first_discrete_nvidia = 0;
      bool have_nv_discrete = false;

      for (uint32_t i = 0; i < device_props.size(); ++i) {
        const VulkanPhysicalDeviceInfo &props = device_props[i];
        if (props.device_type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          if (!have_discrete) {
            have_discrete = true;
            first_discrete = i;
          }
          if (props.vendor_id == kVendorNvidia && !have_nv_discrete) {
            have_nv_discrete = true;
            first_discrete_nvidia = i;
          }
        }
      }

      if (have_nv_discrete) {
        return first_discrete_nvidia;
      }
      if (have_discrete) {
        return first_discrete;
      }
      return 0;
    }

  } // namespace

  SimpleComputeRenderer::SimpleComputeRenderer(uint32_t width, uint32_t height)
    : width_(width == 0 ? 1u : width)
    , height_(height == 0 ? 1u : height)
  {}

  coro::generator<Frame> SimpleComputeRenderer::frames()
  {
    const std::filesystem::path shader_path =
      std::filesystem::path(MR_RENDERER_LIB_SHADER_DIR) / "gradient.slang";

    const auto compiled = mr::importer::compile(shader_path);
    ASSERT(compiled, "mr::importer::compile failed for gradient.slang");
    const mr::importer::Shader* cs = pick_compute(*compiled);
    ASSERT(cs != nullptr, "no compute shader stage in compiled gradient.slang");
    const std::vector<uint32_t> spirv = spirv_words(*cs);

    const uint32_t physical_device_index = kompute_physical_device_index();
    kp::Manager mgr(physical_device_index);

    const size_t pixel_count = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    std::vector<float> zero_rgba(pixel_count * 4u, 0.f);

    std::vector<uint32_t> dims = {width_, height_, 0u, 0u};
    auto dims_buf = mgr.tensorT<uint32_t>(dims);
    auto output = mgr.tensorT<float>(zero_rgba);
    std::vector<std::shared_ptr<kp::Memory>> algo_tensors = {dims_buf, output};

    const kp::Workgroup workgroup{
      (width_ + 7u) / 8u,
      (height_ + 7u) / 8u,
      1u,
    };

    auto algorithm = mgr.algorithm(
      algo_tensors,
      spirv,
      workgroup,
      std::vector<float>{},
      std::vector<float>{});

    std::vector<std::shared_ptr<kp::Memory>> sync_to_device = {dims_buf, output};
    std::vector<std::shared_ptr<kp::Memory>> sync_from_device = {output};

    auto seq = mgr.sequence()
        ->record<kp::OpSyncDevice>(sync_to_device)
        ->record<kp::OpAlgoDispatch>(algorithm)
        ->record<kp::OpSyncLocal>(sync_from_device);

    int idx = 0;
    while (true) {
      seq->eval();

      CpuFrame cpu_frame{};
      fill_frame_pixels(cpu_frame, output->vector(), width_, height_);
      co_yield Frame{static_cast<uint32_t>(idx++), std::move(cpu_frame)};
    }
  }

} // namespace mr
