#include "video_core/renderer_vulkan/conversion_shaders/buffer_to_image_conversions.h"
#include "video_core/renderer_vulkan/conversion_shaders/image_to_buffer_conversions.h"
#include "video_core/renderer_vulkan/vk_convert_and_transfer.h"
#include "video_core/renderer_vulkan/vk_rasterizer_cache.h"

namespace Vulkan {
ConvertaTron5000::ConvertaTron5000(Instance& vk_inst) : vk_inst{vk_inst} {
    using PX = OpenGL::SurfaceParams::PixelFormat;
    struct PXConversion {
        PX src_fmt;
        vk::Format dst_fmt;
        const u32* shader_bin;
        vk::DeviceSize shader_size;
    };
#define PX_CONVERSION(src, dst, shader)                                                            \
    PXConversion{PX::src, vk::Format::dst, BufferToImage::shader, sizeof(BufferToImage::shader)}
    std::initializer_list<PXConversion> buffer_to_image_pipeline_specs{
        PX_CONVERSION(RGBA8, eR8G8B8A8Unorm, rgba8_to_rgba8),
        PX_CONVERSION(RGB8, eR8G8B8A8Unorm, rgb8_to_rgba8),
        PX_CONVERSION(RGB5A1, eR8G8B8A8Unorm, rgb5a1_to_rgba8),
        PX_CONVERSION(RGB565, eR8G8B8A8Unorm, rgb565_to_rgba8),
        PX_CONVERSION(RGBA4, eR8G8B8A8Unorm, rgba4_to_rgba8),
        PX_CONVERSION(IA8, eR8G8Unorm, ia8_to_rg8),
        PX_CONVERSION(RG8, eR8G8Unorm, rg8_to_rg8),
        PX_CONVERSION(I8, eR8Unorm, i8_to_r8),
        PX_CONVERSION(A8, eR8Unorm, a8_to_r8),
        PX_CONVERSION(IA4, eR8G8Unorm, ia4_to_rg8),
        PX_CONVERSION(I4, eR8Unorm, i4_to_r8),
        PX_CONVERSION(A4, eR8Unorm, a4_to_r8),
        PX_CONVERSION(ETC1, eR8G8B8A8Unorm, etc1_to_rgba8),
        PX_CONVERSION(ETC1A4, eR8G8B8A8Unorm, etc1a4_to_rgba8),
    };
#undef PX_CONVERSION

#define PX_CONVERSION(src, dst, shader)                                                            \
    PXConversion{PX::src, vk::Format::dst, ImageToBuffer::shader, sizeof(ImageToBuffer::shader)}
    std::initializer_list<PXConversion> image_to_buffer_pipeline_specs{
        PX_CONVERSION(RGBA8, eR8G8B8A8Unorm, rgba8_to_rgba8),
        PX_CONVERSION(RGB8, eR8G8B8A8Unorm, rgba8_to_rgb8),
        PX_CONVERSION(RGB5A1, eR8G8B8A8Unorm, rgba8_to_rgb5a1),
        PX_CONVERSION(RGB565, eR8G8B8A8Unorm, rgba8_to_rgb565),
        PX_CONVERSION(RGBA4, eR8G8B8A8Unorm, rgba8_to_rgba4)};
#undef PX_CONVERSION

    {
        std::array<vk::DescriptorPoolSize, 2> pool_sizes;
        pool_sizes[0].descriptorCount = 2;
        pool_sizes[0].type = vk::DescriptorType::eStorageBuffer;
        pool_sizes[1].descriptorCount = 1;
        pool_sizes[1].type = vk::DescriptorType::eStorageImage;

        vk::DescriptorPoolCreateInfo descriptor_pool_info;
        descriptor_pool_info.pPoolSizes = pool_sizes.data();
        descriptor_pool_info.poolSizeCount = 2;
        descriptor_pool_info.maxSets = 2;
        descriptor_pool = vk_inst.device->createDescriptorPoolUnique(descriptor_pool_info);
    }
    {
        std::array<vk::DescriptorSetLayoutBinding, 2> descriptors;
        auto& src_buffer_descriptor = descriptors[0];
        src_buffer_descriptor.binding = 0;
        // TODO: some conversion occur because
        // rendering to packed formats (such as RGBA4)
        // is not widely supported but sampling them is possible.
        // In this case we could try copying from a texel buffer
        // instead of doing the conversion ourselves.
        src_buffer_descriptor.descriptorType = vk::DescriptorType::eStorageBuffer;
        src_buffer_descriptor.descriptorCount = 1;
        src_buffer_descriptor.stageFlags = vk::ShaderStageFlagBits::eCompute;

        auto& dst_image_descriptor = descriptors[1];
        dst_image_descriptor.binding = 1;
        dst_image_descriptor.descriptorType = vk::DescriptorType::eStorageImage;
        dst_image_descriptor.descriptorCount = 1;
        dst_image_descriptor.stageFlags = vk::ShaderStageFlagBits::eCompute;

        vk::DescriptorSetLayoutCreateInfo descriptor_layout_info;
        descriptor_layout_info.pBindings = descriptors.data();
        descriptor_layout_info.bindingCount = descriptors.size();
        buffer_to_image_set_layout =
            vk_inst.device->createDescriptorSetLayoutUnique(descriptor_layout_info);

        auto& dst_buffer_descriptor = descriptors[1];
        dst_buffer_descriptor.descriptorType = vk::DescriptorType::eStorageBuffer;
        buffer_to_buffer_set_layout =
            vk_inst.device->createDescriptorSetLayoutUnique(descriptor_layout_info);
    }
    {
        std::array<vk::DescriptorSetLayout, 2> layouts{*buffer_to_image_set_layout,
                                                       *buffer_to_buffer_set_layout};
        vk::DescriptorSetAllocateInfo descriptor_set_allocate_info;
        descriptor_set_allocate_info.descriptorPool = *descriptor_pool;
        descriptor_set_allocate_info.descriptorSetCount = layouts.size();
        descriptor_set_allocate_info.pSetLayouts = layouts.data();
        auto sets = vk_inst.device->allocateDescriptorSetsUnique(descriptor_set_allocate_info);
        buffer_to_image_descriptor_set = std::move(sets[0]);
        buffer_to_buffer_descriptor_set = std::move(sets[1]);
    }
    {
        vk::PushConstantRange push_constant_range;
        push_constant_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
        push_constant_range.offset = 0;
        push_constant_range.size = 8;

        vk::PipelineLayoutCreateInfo pipeline_layout_info;
        pipeline_layout_info.pSetLayouts = &*buffer_to_image_set_layout;
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pPushConstantRanges = &push_constant_range;
        pipeline_layout_info.pushConstantRangeCount = 1;

        buffer_to_image_pipeline_layout =
            vk_inst.device->createPipelineLayoutUnique(pipeline_layout_info);
        pipeline_layout_info.pSetLayouts = &*buffer_to_buffer_set_layout;
        buffer_to_buffer_pipeline_layout =
            vk_inst.device->createPipelineLayoutUnique(pipeline_layout_info);
    }
    const auto BuildPipeline = [&vk_inst](const u32* shader_bin, vk::DeviceSize shader_size,
                                          vk::PipelineLayout layout) {
        vk::ShaderModuleCreateInfo shader_info;
        shader_info.pCode = shader_bin;
        shader_info.codeSize = shader_size;
        auto shader_module = vk_inst.device->createShaderModuleUnique(shader_info);
        vk::ComputePipelineCreateInfo pipeline_info;
        pipeline_info.stage.stage = vk::ShaderStageFlagBits::eCompute;
        pipeline_info.stage.module = *shader_module;
        pipeline_info.stage.pName = "main";
        pipeline_info.layout = layout;
        return vk_inst.device->createComputePipelineUnique({}, pipeline_info);
    };
    for (auto [src_fmt, dst_fmt, shader_bin, shader_size] : buffer_to_image_pipeline_specs) {
        auto pipeline = BuildPipeline(shader_bin, shader_size, *buffer_to_image_pipeline_layout);
        buffer_to_image_pipelines.emplace(src_fmt, std::move(pipeline));
    }
    for (auto [src_fmt, dst_fmt, shader_bin, shader_size] : image_to_buffer_pipeline_specs) {
        auto pipeline = BuildPipeline(shader_bin, shader_size, *buffer_to_image_pipeline_layout);
        image_to_buffer_pipelines.emplace(src_fmt, std::move(pipeline));
    }
    d24s8_buffer_to_image_pipeline =
        BuildPipeline(BufferToImage::d24s8_to_s8, sizeof(BufferToImage::d24s8_to_s8),
                      *buffer_to_buffer_pipeline_layout);
    d24s8_image_to_buffer_pipeline =
        BuildPipeline(ImageToBuffer::s8_to_d24s8, sizeof(ImageToBuffer::s8_to_d24s8),
                      *buffer_to_buffer_pipeline_layout);
    {
        vk::CommandBufferAllocateInfo command_buffer_allocate_info;
        command_buffer_allocate_info.commandBufferCount = 1;
        command_buffer_allocate_info.commandPool = *vk_inst.command_pool;
        command_buffer_allocate_info.level = vk::CommandBufferLevel::ePrimary;
        command_buffer = std::move(
            vk_inst.device->allocateCommandBuffersUnique(command_buffer_allocate_info)[0]);
    }
    {
        vk::BufferCreateInfo temp_info;
        temp_info.sharingMode = vk::SharingMode::eExclusive;
        temp_info.usage =
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;
        // enough for 3DS max surface size
        temp_info.size = 1024 * 1024;
        stencil_temp = vk_inst.device->createBufferUnique(temp_info);
        temp_info.size *= 4;
        depth_temp = vk_inst.device->createBufferUnique(temp_info);

        vk::MemoryAllocateInfo allocation_info;
        auto stencil_memory_requirements =
            vk_inst.device->getBufferMemoryRequirements(*stencil_temp);
        auto depth_memory_requirements = vk_inst.device->getBufferMemoryRequirements(*depth_temp);
        // should check alignment probably, but I doubt it matters here
        allocation_info.allocationSize =
            depth_memory_requirements.size + stencil_memory_requirements.size;
        allocation_info.memoryTypeIndex = vk_inst.getMemoryType(
            depth_memory_requirements.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
        temp_buf_mem = vk_inst.device->allocateMemoryUnique(allocation_info);
        vk_inst.device->bindBufferMemory(*depth_temp, *temp_buf_mem, 0);
        vk_inst.device->bindBufferMemory(*stencil_temp, *temp_buf_mem,
                                         depth_memory_requirements.size);
    }
}

ConvertaTron5000::~ConvertaTron5000() {}

void ConvertaTron5000::ImageFromBuffer(vk::Buffer buffer, vk::Image image,
                                       OpenGL::SurfaceParams::PixelFormat pixel_format,
                                       vk::DeviceSize offset, u32 width, u32 height, u32 stride,
                                       bool tiled) {
    switch (pixel_format) {
    case PX::RGBA8:
    case PX::RGB8:
    case PX::RGBA4:
    case PX::RGB5A1:
    case PX::RGB565:
    case PX::IA8:
    case PX::RG8:
    case PX::IA4:
    case PX::I8:
    case PX::A8:
    case PX::A4:
    case PX::I4:
    case PX::ETC1:
    case PX::ETC1A4: {
        BufferColorConvert(Direction::BufferToImage, buffer, image, pixel_format, offset, width,
                           height, stride, tiled);
    } break;
    case PX::D24S8: {
        D24S8Convert(Direction::BufferToImage, buffer, image, offset, width, height, stride, tiled);
    } break;
    default: {
        vk::CommandBufferBeginInfo command_buffer_begin;
        command_buffer_begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
        command_buffer->begin(command_buffer_begin);

        vk::ImageSubresourceRange image_range;
        image_range.aspectMask = vk::ImageAspectFlagBits::eColor;
        image_range.baseMipLevel = 0;
        image_range.levelCount = 1;
        image_range.baseArrayLayer = 0;
        image_range.layerCount = 1;
        vk::ImageMemoryBarrier barrier;
        barrier.image = image;
        barrier.subresourceRange = image_range;
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

        std::array<u32, 4> orange{0xFF, 0x7F, 0x00, 0xFF};
        command_buffer->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
        command_buffer->clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                                        vk::ClearColorValue{orange}, image_range);

        barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        barrier.newLayout = vk::ImageLayout::eGeneral;
        command_buffer->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                        vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                                        barrier);

        command_buffer->end();
        vk_inst.SubmitCommandBuffers(*command_buffer);

        LOG_ERROR(Render_Vulkan, "Pixel format not implemented: {}",
                  SurfaceParams::PixelFormatAsString(pixel_format));
        vk_inst.queue.waitIdle();
    } break;
    }
    command_buffer->reset({});
}

void ConvertaTron5000::BufferFromImage(vk::Buffer buffer, vk::Image image,
                                       OpenGL::SurfaceParams::PixelFormat pixel_format,
                                       vk::DeviceSize offset, u32 width, u32 height, u32 stride,
                                       bool tiled) {
    switch (pixel_format) {
    case PX::RGBA8:
    case PX::RGB8:
    case PX::RGBA4:
    case PX::RGB5A1:
    case PX::RGB565: {
        BufferColorConvert(Direction::ImageToBuffer, buffer, image, pixel_format, offset, width,
                           height, stride, tiled);
    } break;
    case PX::D24S8: {
        D24S8Convert(Direction::ImageToBuffer, buffer, image, offset, width, height, stride, tiled);
    } break;
    default:
        UNREACHABLE();
    }
    command_buffer->reset({});
}

void ConvertaTron5000::BufferColorConvert(Direction direction, vk::Buffer buffer, vk::Image image,
                                          OpenGL::SurfaceParams::PixelFormat pixel_format,
                                          vk::DeviceSize offset, u32 width, u32 height, u32 stride,
                                          bool tiled) {
    const auto& conversion_piplines = direction == Direction::BufferToImage
                                          ? buffer_to_image_pipelines
                                          : image_to_buffer_pipelines;

    vk::CommandBufferBeginInfo command_buffer_begin;
    command_buffer_begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    command_buffer->begin(command_buffer_begin);
    vk::ImageSubresourceRange image_range;
    image_range.aspectMask = vk::ImageAspectFlagBits::eColor;
    image_range.baseMipLevel = 0;
    image_range.levelCount = 1;
    image_range.baseArrayLayer = 0;
    image_range.layerCount = 1;
    vk::ImageMemoryBarrier barrier;
    barrier.image = image;
    barrier.subresourceRange = image_range;
    barrier.oldLayout = vk::ImageLayout::eUndefined;
    barrier.newLayout = vk::ImageLayout::eGeneral;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    std::array<vk::WriteDescriptorSet, 2> desc_set_writes;

    vk::SamplerCreateInfo sampler_info;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.magFilter = vk::Filter::eNearest;
    sampler_info.minFilter = vk::Filter::eNearest;
    auto sampler = vk_inst.device->createSamplerUnique(sampler_info);
    vk::ImageViewCreateInfo image_view_info;
    image_view_info.viewType = vk::ImageViewType::e2D;
    image_view_info.format = VulkanIntFormat(VulkanPixelFormat(pixel_format));
    image_view_info.image = image;
    image_view_info.subresourceRange = image_range;
    auto image_view = vk_inst.device->createImageViewUnique(image_view_info);
    vk::DescriptorImageInfo image_info;
    image_info.imageLayout = vk::ImageLayout::eGeneral;
    image_info.imageView = *image_view;
    image_info.sampler = *sampler;

    desc_set_writes[0].descriptorCount = 1;
    desc_set_writes[0].descriptorType = vk::DescriptorType::eStorageImage;
    desc_set_writes[0].dstArrayElement = 0;
    desc_set_writes[0].dstBinding = 1;
    desc_set_writes[0].dstSet = *buffer_to_image_descriptor_set;
    desc_set_writes[0].pImageInfo = &image_info;

    vk::DeviceSize size = SurfaceParams::GetFormatBpp(pixel_format) * stride * height / 8;
    vk::DescriptorBufferInfo buffer_info;
    buffer_info.buffer = buffer;
    buffer_info.offset = offset;
    buffer_info.range = size;

    desc_set_writes[1].descriptorCount = 1;
    desc_set_writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    desc_set_writes[1].dstArrayElement = 0;
    desc_set_writes[1].dstBinding = 0;
    desc_set_writes[1].dstSet = *buffer_to_image_descriptor_set;
    desc_set_writes[1].pBufferInfo = &buffer_info;

    vk_inst.device->updateDescriptorSets(desc_set_writes, {});

    command_buffer->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                    vk::PipelineStageFlagBits::eComputeShader, {}, {}, {}, barrier);
    const auto pipeline = *conversion_piplines.at(pixel_format);
    command_buffer->bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    struct {
        u32 stride{};
        u8 pad[3]{};
        bool tiled{};
    } push_values{stride, tiled};
    command_buffer->pushConstants(*buffer_to_image_pipeline_layout,
                                  vk::ShaderStageFlagBits::eCompute, 0, sizeof(push_values),
                                  &push_values);
    command_buffer->bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                       *buffer_to_image_pipeline_layout, 0,
                                       *buffer_to_image_descriptor_set, {});
    u8 div = (pixel_format == PX::ETC1 || pixel_format == PX::ETC1A4) ? 4 : 8;
    command_buffer->dispatch(width / div, height / div, 1);
    command_buffer->end();
    vk_inst.SubmitCommandBuffers(*command_buffer);
    vk_inst.queue.waitIdle();
}

void ConvertaTron5000::D24S8Convert(Direction direction, vk::Buffer buffer, vk::Image image,
                                    vk::DeviceSize offset, u32 width, u32 height, u32 stride,
                                    bool tiled) {
    const auto pipeline = *(direction == Direction::BufferToImage ? d24s8_buffer_to_image_pipeline
                                                                  : d24s8_image_to_buffer_pipeline);

    std::array<vk::WriteDescriptorSet, 2> desc_set_writes;

    vk::DeviceSize size = SurfaceParams::GetFormatBpp(PX::D24S8) * stride * height / 8;
    vk::DescriptorBufferInfo main_buffer_info;
    main_buffer_info.buffer = buffer;
    main_buffer_info.offset = offset;
    main_buffer_info.range = size;

    desc_set_writes[0].descriptorCount = 1;
    desc_set_writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    desc_set_writes[0].dstArrayElement = 0;
    desc_set_writes[0].dstBinding = 0;
    desc_set_writes[0].dstSet = *buffer_to_buffer_descriptor_set;
    desc_set_writes[0].pBufferInfo = &main_buffer_info;

    vk::DescriptorBufferInfo stencil_buffer_info;
    stencil_buffer_info.buffer = *stencil_temp;
    stencil_buffer_info.offset = 0;
    stencil_buffer_info.range = width * height;

    desc_set_writes[1].descriptorCount = 1;
    desc_set_writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    desc_set_writes[1].dstArrayElement = 0;
    desc_set_writes[1].dstBinding = 1;
    desc_set_writes[1].dstSet = *buffer_to_buffer_descriptor_set;
    desc_set_writes[1].pBufferInfo = &stencil_buffer_info;

    vk_inst.device->updateDescriptorSets(desc_set_writes, {});

    vk::ImageSubresourceRange image_range;
    image_range.aspectMask = vk::ImageAspectFlagBits::eDepth;
    image_range.aspectMask |= vk::ImageAspectFlagBits::eStencil;
    image_range.baseMipLevel = 0;
    image_range.levelCount = 1;
    image_range.baseArrayLayer = 0;
    image_range.layerCount = 1;
    vk::ImageMemoryBarrier barrier;
    barrier.image = image;
    barrier.subresourceRange = image_range;
    if (direction == Direction::BufferToImage) {
        barrier.oldLayout = vk::ImageLayout::eUndefined;
        barrier.newLayout = vk::ImageLayout::eTransferDstOptimal;
    } else {
        barrier.oldLayout = vk::ImageLayout::eGeneral;
        barrier.newLayout = vk::ImageLayout::eTransferSrcOptimal;
    }
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

    vk::CommandBufferBeginInfo command_buffer_begin;
    command_buffer_begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    const auto Compute = [&] {
        command_buffer->bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        struct {
            u32 stride{};
            u8 pad[3]{};
            bool tiled{};
        } push_values{stride, tiled};
        command_buffer->pushConstants(*buffer_to_buffer_pipeline_layout,
                                      vk::ShaderStageFlagBits::eCompute, 0, sizeof(push_values),
                                      &push_values);
        command_buffer->bindDescriptorSets(vk::PipelineBindPoint::eCompute,
                                           *buffer_to_buffer_pipeline_layout, 0,
                                           *buffer_to_buffer_descriptor_set, {});
        command_buffer->dispatch(width / 8, height / 8, 1);
    };
    const auto Copy = [&] {
        command_buffer->pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                                        vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
        std::array<vk::BufferImageCopy, 2> copy;
        auto& depth_copy = copy[0];
        depth_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eDepth;
        depth_copy.bufferRowLength = stride;
        depth_copy.bufferOffset = offset;
        depth_copy.imageExtent = vk::Extent3D{width, height, 1};
        depth_copy.imageSubresource.baseArrayLayer = 0;
        depth_copy.imageSubresource.layerCount = 1;
        depth_copy.imageSubresource.mipLevel = 0;
        auto& stencil_copy = copy[1];
        stencil_copy.imageSubresource = depth_copy.imageSubresource;
        stencil_copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eStencil;
        stencil_copy.imageExtent = depth_copy.imageExtent;

        if (direction == Direction::BufferToImage) {
            command_buffer->copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal,
                                              depth_copy);
            command_buffer->copyBufferToImage(*stencil_temp, image,
                                              vk::ImageLayout::eTransferDstOptimal, stencil_copy);
            barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        } else {
            command_buffer->copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                              depth_copy);
            command_buffer->copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal,
                                              *stencil_temp, stencil_copy);
            barrier.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        }
        barrier.newLayout = vk::ImageLayout::eGeneral;

        command_buffer->pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                        vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {},
                                        barrier);
    };
    command_buffer->begin(command_buffer_begin);
    if (direction == Direction::BufferToImage) {
        Compute();
    }
    Copy();
    if (direction == Direction::ImageToBuffer) {
        Compute();
    }
    command_buffer->end();
    vk_inst.SubmitCommandBuffers(*command_buffer);
    vk_inst.queue.waitIdle();
}
} // namespace Vulkan