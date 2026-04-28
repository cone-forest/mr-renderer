#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>

namespace mr {
  struct VulkanFeatureSupport {
    bool mesh_shader = false;
    bool task_shader = false;
    bool descriptor_indexing = false;
    bool runtime_descriptor_array = false;
    bool descriptor_binding_partially_bound = false;
    bool descriptor_binding_variable_descriptor_count = false;
    bool descriptor_binding_update_unused_while_pending = false;
    bool draw_indirect_count = false;
    bool multi_draw_indirect = false;
    bool shader_draw_parameters = false;
    bool buffer_device_address = false;
    bool timeline_semaphore = false;
    bool synchronization2 = false;
    bool dynamic_rendering = false;
  };

  struct VulkanContextCreateInfo {
    const char* app_name = "mr-renderer";
    bool headless = true;
    bool require_present = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool prefer_dedicated_compute_queue = true;
    bool enable_pipeline_cache = true;
    std::string pipeline_cache_path = ".mr-renderer-pipeline-cache.bin";
  };

  struct VulkanContext {
    vkb::Instance instance{};
    vkb::PhysicalDevice physical_device{};
    vkb::Device device{};
    VmaAllocator allocator = VK_NULL_HANDLE;

    uint32_t graphics_queue_family = std::numeric_limits<uint32_t>::max();
    uint32_t compute_queue_family = std::numeric_limits<uint32_t>::max();
    uint32_t present_queue_family = std::numeric_limits<uint32_t>::max();

    vk::Queue graphics_queue{};
    vk::Queue compute_queue{};
    vk::Queue present_queue{};

    VulkanFeatureSupport feature_support{};
    std::vector<std::string> enabled_extensions{};
    vk::PipelineCache pipeline_cache{};
    std::string pipeline_cache_path{};

    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&& other) noexcept;
    VulkanContext& operator=(VulkanContext&& other) noexcept;

    [[nodiscard]] vk::Instance vk_instance() const;
    [[nodiscard]] vk::PhysicalDevice vk_physical_device() const;
    [[nodiscard]] vk::Device vk_device() const;
    [[nodiscard]] bool has_present_queue() const;
    [[nodiscard]] vk::PipelineCache vk_pipeline_cache() const;
    void flush_pipeline_cache();
  };

  [[nodiscard]] std::expected<VulkanContext, std::string>
  create_vulkan_context(const VulkanContextCreateInfo& create_info);

  struct VulkanPhysicalDeviceInfo {
    uint32_t vendor_id = 0;
    VkPhysicalDeviceType device_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
  };

  std::expected<std::vector<VulkanPhysicalDeviceInfo>, std::string>
  enumerate_vulkan_physical_devices();

  enum class QueueTarget {
    Graphics,
    ComputeTransfer,
  };

  class FrameRecorder {
  public:
    struct CreateInfo {
      uint32_t frames_in_flight = 3;
      uint32_t max_recording_threads = 8;
    };

    struct RecordedCommandBuffer {
      vk::CommandBuffer handle{};
      QueueTarget queue_target = QueueTarget::Graphics;
      uint32_t queue_family = std::numeric_limits<uint32_t>::max();
    };

    struct BufferUsageDesc {
      vk::Buffer buffer{};
      vk::DeviceSize offset = 0;
      vk::DeviceSize size = VK_WHOLE_SIZE;
      vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eAllCommands;
      vk::AccessFlags2 access = vk::AccessFlagBits2::eMemoryRead;
      bool writes = false;
    };

    struct ImageUsageDesc {
      vk::Image image{};
      vk::ImageSubresourceRange subresource_range{};
      vk::ImageLayout layout = vk::ImageLayout::eGeneral;
      vk::PipelineStageFlags2 stage = vk::PipelineStageFlagBits2::eAllCommands;
      vk::AccessFlags2 access = vk::AccessFlagBits2::eMemoryRead;
      bool writes = false;
    };

    explicit FrameRecorder(const VulkanContext& context);
    FrameRecorder(const VulkanContext& context, CreateInfo create_info);
    ~FrameRecorder();

    FrameRecorder(const FrameRecorder&) = delete;
    FrameRecorder& operator=(const FrameRecorder&) = delete;
    FrameRecorder(FrameRecorder&&) noexcept;
    FrameRecorder& operator=(FrameRecorder&&) noexcept;

    std::expected<void, std::string> begin_frame(uint64_t frame_index);
    std::expected<RecordedCommandBuffer, std::string> begin_recording(
      QueueTarget queue_target,
      vk::CommandBufferUsageFlags usage_flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    std::expected<void, std::string> end_recording(const RecordedCommandBuffer& command_buffer);
    void declare_buffer_usage(
      const RecordedCommandBuffer& command_buffer,
      const BufferUsageDesc& usage_desc);
    void declare_image_usage(
      const RecordedCommandBuffer& command_buffer,
      const ImageUsageDesc& usage_desc);
    void enqueue_for_submit(const RecordedCommandBuffer& command_buffer);
    std::expected<uint64_t, std::string> submit_frame();

    [[nodiscard]] uint64_t last_timeline_value() const noexcept;
    [[nodiscard]] vk::Semaphore timeline_semaphore() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
  };

  struct BufferRegion {
    const VulkanContext* context = nullptr;
    vk::Buffer buffer{};
    vk::DeviceSize offset = 0;
    vk::DeviceSize size = 0;
  };

  class Buffer {
  public:
    Buffer() = default;
    Buffer(
      const VulkanContext& context,
      vk::DeviceSize byte_size,
      vk::BufferUsageFlags usage_flags,
      vk::MemoryPropertyFlags memory_properties);
    virtual ~Buffer() noexcept;

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&& other) noexcept;
    Buffer& operator=(Buffer&& other) noexcept;

    [[nodiscard]] const VulkanContext& context() const;
    [[nodiscard]] vk::Buffer buffer() const noexcept;
    [[nodiscard]] vk::DeviceSize byte_size() const noexcept;

  protected:
    const VulkanContext* context_ = nullptr;
    vk::DeviceSize size_ = 0;
    vk::Buffer buffer_{};
    vk::BufferUsageFlags usage_flags_{};
    VmaAllocation allocation_ = VK_NULL_HANDLE;
  };

  class HostBuffer : public Buffer {
  public:
    HostBuffer() = default;
    HostBuffer(
      const VulkanContext& context,
      vk::DeviceSize byte_size,
      vk::BufferUsageFlags usage_flags,
      vk::MemoryPropertyFlags memory_properties = {});
    ~HostBuffer() noexcept override;

    HostBuffer(const HostBuffer&) = delete;
    HostBuffer& operator=(const HostBuffer&) = delete;
    HostBuffer(HostBuffer&& other) noexcept;
    HostBuffer& operator=(HostBuffer&& other) noexcept;

    std::span<const std::byte> read() noexcept;
    std::vector<std::byte> copy() noexcept;
    HostBuffer& write(std::span<const std::byte> src);

    template <typename T>
    HostBuffer& write(std::span<T> src)
    {
      return write(std::as_bytes(src));
    }

  private:
    void* mapped_data_ = nullptr;
  };

  class DeviceBuffer : public Buffer {
  public:
    DeviceBuffer() = default;
    DeviceBuffer(
      const VulkanContext& context,
      vk::DeviceSize byte_size,
      vk::BufferUsageFlags usage_flags,
      vk::MemoryPropertyFlags memory_properties = {});

    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    DeviceBuffer(DeviceBuffer&&) noexcept = default;
    DeviceBuffer& operator=(DeviceBuffer&&) noexcept = default;

    DeviceBuffer& resize(vk::CommandBuffer command_buffer, vk::DeviceSize new_size) noexcept;
    DeviceBuffer& write(
      vk::CommandBuffer command_buffer,
      std::span<const std::byte> src,
      vk::DeviceSize offset = 0);

    template <typename T>
    DeviceBuffer& write(vk::CommandBuffer command_buffer, std::span<T> src, vk::DeviceSize offset = 0)
    {
      return write(command_buffer, std::as_bytes(src), offset);
    }
  };

  class VectorBuffer : public DeviceBuffer {
  public:
    static constexpr float resize_coefficient = 1.5f;
    static constexpr vk::DeviceSize default_initial_byte_size = 1 << 20;

    VectorBuffer() = default;
    VectorBuffer(
      const VulkanContext& context,
      vk::BufferUsageFlags usage_flags,
      vk::DeviceSize initial_byte_size = default_initial_byte_size);

    [[nodiscard]] vk::DeviceSize capacity() const noexcept { return byte_size(); }
    [[nodiscard]] vk::DeviceSize size() const noexcept { return current_size_; }
    void set_size(vk::DeviceSize new_size) noexcept { current_size_ = new_size; }

    vk::DeviceSize append_range(vk::CommandBuffer command_buffer, std::span<const std::byte> src) noexcept;

    template <typename T>
    vk::DeviceSize append_range(vk::CommandBuffer command_buffer, std::span<T> src) noexcept
    {
      return append_range(command_buffer, std::as_bytes(src));
    }

  private:
    vk::DeviceSize current_size_ = 0;
  };

  class VertexVectorBuffer : public VectorBuffer {
  public:
    VertexVectorBuffer() = default;
    explicit VertexVectorBuffer(
      const VulkanContext& context,
      vk::DeviceSize initial_byte_size = default_initial_byte_size);
  };

  class IndexVectorBuffer : public VectorBuffer {
  public:
    IndexVectorBuffer() = default;
    explicit IndexVectorBuffer(
      const VulkanContext& context,
      vk::DeviceSize initial_byte_size = default_initial_byte_size);
  };

  class DeviceHeapAllocator {
  private:
    struct Allocation {
      VmaVirtualAllocation allocation = VK_NULL_HANDLE;
      vk::DeviceSize byte_size = 0;
      uint32_t block_number = 0;
    };

    class AllocationBlock {
    public:
      AllocationBlock(vk::DeviceSize size, vk::DeviceSize offset, uint32_t block_number) noexcept;
      ~AllocationBlock() noexcept;

      AllocationBlock(const AllocationBlock&) = delete;
      AllocationBlock& operator=(const AllocationBlock&) = delete;
      AllocationBlock(AllocationBlock&& other) noexcept;
      AllocationBlock& operator=(AllocationBlock&& other) noexcept;

      std::optional<std::pair<vk::DeviceSize, Allocation>> allocate(
        vk::DeviceSize allocation_size,
        uint32_t alignment) noexcept;
      void deallocate(Allocation& allocation) noexcept;
      [[nodiscard]] vk::DeviceSize size() const noexcept { return size_; }
      [[nodiscard]] vk::DeviceSize offset() const noexcept { return offset_; }

    private:
      VmaVirtualBlock virtual_block_ = VK_NULL_HANDLE;
      vk::DeviceSize size_ = 0;
      vk::DeviceSize offset_ = 0;
      uint32_t block_number_ = 0;
      std::mutex mutex_;
    };

  public:
    struct AllocationInfo {
      vk::DeviceSize offset = 0;
      bool resized = false;
    };

    explicit DeviceHeapAllocator(
      vk::DeviceSize start_byte_size = VectorBuffer::default_initial_byte_size,
      uint32_t alignment = 16);

    DeviceHeapAllocator(const DeviceHeapAllocator&) = delete;
    DeviceHeapAllocator& operator=(const DeviceHeapAllocator&) = delete;
    DeviceHeapAllocator(DeviceHeapAllocator&& other) noexcept;
    DeviceHeapAllocator& operator=(DeviceHeapAllocator&& other) noexcept;

    AllocationInfo allocate(vk::DeviceSize size) noexcept;
    void deallocate(vk::DeviceSize offset) noexcept;

    [[nodiscard]] vk::DeviceSize size() const noexcept { return size_; }
    [[nodiscard]] uint32_t alignment() const noexcept { return alignment_; }

  private:
    AllocationBlock& add_block(vk::DeviceSize allocation_size = 0) noexcept;

    vk::DeviceSize size_ = 0;
    uint32_t alignment_ = 16;
    std::unordered_map<vk::DeviceSize, Allocation> allocations_{};
    std::mutex allocations_mutex_{};
    std::mutex add_block_mutex_{};
    std::vector<AllocationBlock> blocks_{};
  };

  class HeapBuffer {
  public:
    static constexpr auto default_initial_byte_size = VectorBuffer::default_initial_byte_size;
    static constexpr uint32_t default_alignment = 16;

    HeapBuffer() = default;
    HeapBuffer(
      const VulkanContext& context,
      vk::BufferUsageFlags usage_flags,
      vk::DeviceSize start_byte_size = default_initial_byte_size,
      uint32_t alignment = default_alignment);

    vk::DeviceSize allocate(vk::DeviceSize size) noexcept;
    void free(vk::DeviceSize offset) noexcept;

    void write(vk::CommandBuffer command_buffer, std::span<const std::byte> src, vk::DeviceSize offset = 0);

    template <typename T>
    void write(vk::CommandBuffer command_buffer, std::span<T> src, vk::DeviceSize offset = 0)
    {
      write(command_buffer, std::as_bytes(src), offset);
    }

    vk::DeviceSize allocate_and_write(vk::CommandBuffer command_buffer, std::span<const std::byte> src) noexcept;

    template <typename T>
    vk::DeviceSize allocate_and_write(vk::CommandBuffer command_buffer, std::span<T> src) noexcept
    {
      return allocate_and_write(command_buffer, std::as_bytes(src));
    }

    [[nodiscard]] vk::DeviceSize byte_size() const noexcept { return buffer_.byte_size(); }
    [[nodiscard]] vk::Buffer buffer() const noexcept { return buffer_.buffer(); }

  private:
    VectorBuffer buffer_{};
    DeviceHeapAllocator heap_{};
  };

  class VertexHeapBuffer : public HeapBuffer {
  public:
    VertexHeapBuffer() = default;
    VertexHeapBuffer(
      const VulkanContext& context,
      vk::DeviceSize start_byte_size = default_initial_byte_size,
      uint32_t alignment = default_alignment);
  };

  class IndexHeapBuffer : public HeapBuffer {
  public:
    IndexHeapBuffer() = default;
    IndexHeapBuffer(
      const VulkanContext& context,
      vk::DeviceSize start_byte_size = default_initial_byte_size,
      uint32_t alignment = default_alignment);
  };

  class UniformBuffer : public HostBuffer {
  public:
    UniformBuffer() = default;
    UniformBuffer(const VulkanContext& context, vk::DeviceSize byte_size, vk::BufferUsageFlags usage_flags = {});
  };

  class StorageBuffer : public DeviceBuffer {
  public:
    StorageBuffer() = default;
    StorageBuffer(const VulkanContext& context, vk::DeviceSize byte_size, vk::BufferUsageFlags usage_flags = {});
  };

  class VertexBuffer : public DeviceBuffer {
  public:
    VertexBuffer() = default;
    VertexBuffer(const VulkanContext& context, vk::DeviceSize byte_size);
  };

  class IndexBuffer : public DeviceBuffer {
  public:
    IndexBuffer() = default;
    IndexBuffer(const VulkanContext& context, vk::DeviceSize byte_size, vk::IndexType index_type = vk::IndexType::eUint32);

    [[nodiscard]] vk::IndexType index_type() const noexcept { return index_type_; }
    [[nodiscard]] size_t element_count() const noexcept { return element_count_; }
    void set_element_count(size_t count) noexcept { element_count_ = count; }

  private:
    vk::IndexType index_type_ = vk::IndexType::eUint32;
    size_t element_count_ = 0;
  };

  void copy_buffer(vk::CommandBuffer command_buffer, BufferRegion src, BufferRegion dst);

  class Image {
  public:
    Image() = default;
    Image(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      vk::ImageUsageFlags usage_flags,
      vk::ImageAspectFlags aspect_flags,
      vk::MemoryPropertyFlags memory_properties,
      uint32_t mip_levels = 1,
      bool create_image_view = true);
    virtual ~Image();

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept;
    Image& operator=(Image&& other) noexcept;

    void transition_layout(vk::CommandBuffer command_buffer, vk::ImageLayout new_layout);
    void transition_layout(
      vk::CommandBuffer command_buffer,
      vk::ImageLayout new_layout,
      uint32_t mip_level,
      uint32_t mip_counts,
      bool ignore_previous_layout = false);

    void write(vk::CommandBuffer command_buffer, std::span<const std::byte> src);
    HostBuffer read_to_host_buffer(vk::CommandBuffer command_buffer) noexcept;

    [[nodiscard]] vk::Image image() const noexcept { return image_; }
    [[nodiscard]] vk::ImageView image_view() const noexcept { return image_view_; }
    [[nodiscard]] vk::Format format() const noexcept { return format_; }
    [[nodiscard]] vk::Extent3D extent() const noexcept { return extent_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] uint32_t mip_levels() const noexcept { return mip_levels_; }
    [[nodiscard]] vk::ImageLayout layout() const noexcept { return layout_; }

    static vk::Format find_supported_format(
      const VulkanContext& context,
      std::span<const vk::Format> candidates,
      vk::ImageTiling tiling,
      vk::FormatFeatureFlags features);

    static bool is_image_format_supported(
      const VulkanContext& context,
      vk::Format format,
      vk::ImageType image_type,
      vk::ImageTiling tiling,
      vk::ImageUsageFlags usage);

  protected:
    vk::ImageView create_image_view(uint32_t mip_level, uint32_t mip_levels_count);

    const VulkanContext* context_ = nullptr;
    vk::Image image_{};
    vk::ImageView image_view_{};
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    vk::Extent3D extent_{};
    size_t size_ = 0;
    vk::Format format_ = vk::Format::eUndefined;
    uint32_t mip_levels_ = 1;
    vk::ImageLayout layout_ = vk::ImageLayout::eUndefined;
    vk::ImageAspectFlags aspect_flags_{};
    bool owns_image_ = true;
    bool owns_image_view_ = true;
  };

  class HostImage : public Image {
  public:
    HostImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      vk::ImageUsageFlags usage_flags,
      vk::ImageAspectFlags aspect_flags,
      uint32_t mip_levels = 1);
  };

  class DeviceImage : public Image {
  public:
    DeviceImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      vk::ImageUsageFlags usage_flags,
      vk::ImageAspectFlags aspect_flags,
      uint32_t mip_levels = 1,
      bool create_image_view = true);
  };

  class SwapchainImage : public Image {
  public:
    SwapchainImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      vk::Image image,
      vk::ImageView view = {});
    ~SwapchainImage() override;
  };

  class TextureImage : public DeviceImage {
  public:
    TextureImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      vk::ImageUsageFlags usage_flags = {},
      uint32_t mip_levels = 1);
  };

  class DepthImage : public DeviceImage {
  public:
    explicit DepthImage(const VulkanContext& context, vk::Extent3D extent, uint32_t mip_levels = 1);
    vk::RenderingAttachmentInfo attachment_info() const;
  };

  class ColorAttachmentImage : public DeviceImage {
  public:
    ColorAttachmentImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      uint32_t mip_levels = 1);
    vk::RenderingAttachmentInfo attachment_info() const;
  };

  class StorageImage : public DeviceImage {
  public:
    StorageImage(
      const VulkanContext& context,
      vk::Extent3D extent,
      vk::Format format,
      uint32_t mip_levels = 1,
      bool create_image_view = false);
  };

  struct GraphicsShaderStageDesc {
    vk::ShaderStageFlagBits stage = vk::ShaderStageFlagBits::eVertex;
    vk::ShaderModule module{};
    const char* entry_point = "main";
  };

  struct GraphicsPipelineDesc {
    std::vector<GraphicsShaderStageDesc> shader_stages{};
    std::vector<vk::VertexInputBindingDescription> vertex_bindings{};
    std::vector<vk::VertexInputAttributeDescription> vertex_attributes{};
    std::vector<vk::DescriptorSetLayout> descriptor_set_layouts{};
    std::vector<vk::PushConstantRange> push_constant_ranges{};
    std::vector<vk::DynamicState> dynamic_states{
      vk::DynamicState::eViewport,
      vk::DynamicState::eScissor,
    };
    std::vector<vk::PipelineColorBlendAttachmentState> color_blend_attachments{};
    std::vector<vk::Format> color_attachment_formats{};
    std::optional<vk::Format> depth_attachment_format{};

    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    bool primitive_restart_enable = false;
    vk::PolygonMode polygon_mode = vk::PolygonMode::eFill;
    vk::CullModeFlags cull_mode = vk::CullModeFlagBits::eNone;
    vk::FrontFace front_face = vk::FrontFace::eCounterClockwise;
    float line_width = 1.0f;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;

    bool depth_test_enable = false;
    bool depth_write_enable = false;
    vk::CompareOp depth_compare_op = vk::CompareOp::eLessOrEqual;
  };

  class GraphicsPipeline {
  public:
    GraphicsPipeline() = default;
    ~GraphicsPipeline();

    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(pipeline_); }
    [[nodiscard]] vk::Pipeline vk_pipeline() const noexcept { return pipeline_; }
    [[nodiscard]] vk::PipelineLayout vk_pipeline_layout() const noexcept { return layout_; }

    void bind(vk::CommandBuffer command_buffer) const;

  private:
    friend std::expected<GraphicsPipeline, std::string>
    build_graphics_pipeline(const VulkanContext& context, const GraphicsPipelineDesc& desc);

    const VulkanContext* context_ = nullptr;
    vk::PipelineLayout layout_{};
    vk::Pipeline pipeline_{};
  };

  std::expected<GraphicsPipeline, std::string>
  build_graphics_pipeline(const VulkanContext& context, const GraphicsPipelineDesc& desc);

  class GraphicsPipelineBuilder {
  public:
    explicit GraphicsPipelineBuilder(const VulkanContext& context)
      : context_(&context)
    {}

    GraphicsPipelineBuilder& set_bindings(std::span<const vk::VertexInputBindingDescription> bindings);
    GraphicsPipelineBuilder& set_attributes(std::span<const vk::VertexInputAttributeDescription> attributes);
    GraphicsPipelineBuilder& set_shader_stages(std::span<const GraphicsShaderStageDesc> stages);
    GraphicsPipelineBuilder& add_shader_stage(GraphicsShaderStageDesc stage);
    GraphicsPipelineBuilder& set_descriptor_set_layouts(std::span<const vk::DescriptorSetLayout> layouts);
    GraphicsPipelineBuilder& set_push_constants(std::span<const vk::PushConstantRange> ranges);
    GraphicsPipelineBuilder& set_dynamic_states(std::span<const vk::DynamicState> states);
    GraphicsPipelineBuilder& set_color_attachment_formats(std::span<const vk::Format> formats);
    GraphicsPipelineBuilder&
    set_color_blend_attachments(std::span<const vk::PipelineColorBlendAttachmentState> attachments);
    GraphicsPipelineBuilder& set_depth_attachment_format(std::optional<vk::Format> format);
    GraphicsPipelineBuilder& set_topology(vk::PrimitiveTopology topology);
    GraphicsPipelineBuilder& set_cull_mode(vk::CullModeFlags cull_mode);
    GraphicsPipelineBuilder& set_front_face(vk::FrontFace front_face);
    GraphicsPipelineBuilder& set_depth_state(bool test_enable, bool write_enable, vk::CompareOp compare_op);
    GraphicsPipelineBuilder& set_samples(vk::SampleCountFlagBits samples);
    GraphicsPipelineBuilder& set_line_width(float line_width);
    GraphicsPipelineBuilder& set_primitive_restart(bool enable);
    GraphicsPipelineBuilder& set_polygon_mode(vk::PolygonMode polygon_mode);

    [[nodiscard]] GraphicsPipelineDesc& desc() noexcept { return desc_; }
    [[nodiscard]] const GraphicsPipelineDesc& desc() const noexcept { return desc_; }

    std::expected<GraphicsPipeline, std::string> build() const;

  private:
    const VulkanContext* context_ = nullptr;
    GraphicsPipelineDesc desc_{};
  };
} // namespace mr
