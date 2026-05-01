module;

#include <mr-renderer/file_presenter.hpp>
#include <mr-renderer/frame.hpp>
#include <mr-renderer/presenter.hpp>
#include <mr-renderer/renderer.hpp>
#include <mr-renderer/target.hpp>
#include <mr-renderer/window_presenter.hpp>
#include <mr-renderer/vulkan_wrappers.hpp>

export module mr.renderer.lib;

export namespace mr {
  using ::mr::CpuFrame;
  using ::mr::CpuTarget;
  using ::mr::FilePresenter;
  using ::mr::Frame;
  using ::mr::FramePayload;
  using ::mr::FrameRecorder;
  using ::mr::GraphicsPipeline;
  using ::mr::GraphicsPipelineBuilder;
  using ::mr::GraphicsPipelineDesc;
  using ::mr::GraphicsShaderStageDesc;
  using ::mr::GpuFrame;
  using ::mr::GpuTarget;
  using ::mr::IPresenter;
  using ::mr::IRenderer;
  using ::mr::Buffer;
  using ::mr::BufferRegion;
  using ::mr::ColorAttachmentImage;
  using ::mr::DepthImage;
  using ::mr::DeviceBuffer;
  using ::mr::DeviceImage;
  using ::mr::VulkanContext;
  using ::mr::VulkanContextCreateInfo;
  using ::mr::VulkanFeatureSupport;
  using ::mr::Target;
  using ::mr::TargetPayload;
  using ::mr::VulkanPhysicalDeviceInfo;
  using ::mr::HostBuffer;
  using ::mr::HostImage;
  using ::mr::Image;
  using ::mr::IndexBuffer;
  using ::mr::IndexHeapBuffer;
  using ::mr::IndexVectorBuffer;
  using ::mr::StorageBuffer;
  using ::mr::StorageImage;
  using ::mr::SwapchainImage;
  using ::mr::TextureImage;
  using ::mr::UniformBuffer;
  using ::mr::QueueTarget;
  using ::mr::RgbaFloatRasterView;
  using ::mr::VectorBuffer;
  using ::mr::VertexHeapBuffer;
  using ::mr::VertexVectorBuffer;
  using ::mr::VertexBuffer;
  using ::mr::HeapBuffer;
  using ::mr::DeviceHeapAllocator;
  using ::mr::WindowPresenter;
  using ::mr::copy_buffer;
  using ::mr::build_graphics_pipeline;
  using ::mr::create_vulkan_context;
  using ::mr::create_vulkan_instance;
  using ::mr::enumerate_vulkan_physical_devices;
}
