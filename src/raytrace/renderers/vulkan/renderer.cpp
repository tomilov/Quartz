/*
 * Copyright (C) 2018-2019 Michał Siejak
 * This file is part of Quartz - a raytracing aspect for Qt3D.
 * See LICENSE file for licensing information.
 */

#include <renderers/vulkan/renderer.h>
#include <renderers/vulkan/vkcommon.h>
#include <renderers/vulkan/commandbuffer.h>
#include <renderers/vulkan/initializers.h>
#include <renderers/vulkan/shadermodule.h>
#include <renderers/vulkan/pipeline/graphicspipeline.h>
#include <renderers/vulkan/pipeline/computepipeline.h>
#include <renderers/vulkan/pipeline/raytracingpipeline.h>

#include <renderers/vulkan/jobs/buildscenetlasjob.h>
#include <renderers/vulkan/jobs/buildgeometryjob.h>
#include <renderers/vulkan/jobs/updateinstancebufferjob.h>
#include <renderers/vulkan/jobs/updatematerialsjob.h>
#include <renderers/vulkan/jobs/uploadtexturejob.h>

#include <renderers/vulkan/shaders/lib/bindings.glsl>

#include <backend/managers_p.h>
#include <backend/rendersettings_p.h>

#include <QVulkanInstance>
#include <QWindow>
#include <QThread>
#include <QTimer>
#include <QElapsedTimer>

static void initializeResources()
{
    Q_INIT_RESOURCE(vulkan_shaders);
}

namespace Qt3DRaytrace {
namespace Vulkan {

namespace Config {

constexpr bool     EnableVsync = false;
constexpr VkFormat RenderBufferFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr uint32_t DescriptorPoolCapacity = 1024;
constexpr uint32_t GlobalMaxRecursionDepth = 16;

} // Config

Q_LOGGING_CATEGORY(logVulkan, "raytrace.vulkan")

Renderer::Renderer(QObject *parent)
    : QObject(parent)
    , m_renderFrameTimer(new QTimer(this))
    , m_cameraManager(new CameraManager)
    , m_frameAdvanceService(new FrameAdvanceService)
    , m_updateWorldTransformJob(new Raytrace::UpdateWorldTransformJob)
    , m_destroyExpiredResourcesJob(new DestroyExpiredResourcesJob(this))
    , m_updateRenderParametersJob(new UpdateRenderParametersJob(this))
    , m_updateInstanceBufferJob(new UpdateInstanceBufferJob(this))
    , m_updateEmittersJob(new UpdateEmittersJob(this))
{
    initializeResources();
    QObject::connect(m_renderFrameTimer, &QTimer::timeout, this, &Renderer::renderFrame);
}

bool Renderer::initialize()
{
    static const QByteArrayList RequiredDeviceExtensions {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_NV_RAY_TRACING_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
    };

    if(!m_window) {
        qCCritical(logVulkan) << "Cannot initialize renderer: no surface set";
        return false;
    }
    if(!m_window->vulkanInstance()) {
        qCCritical(logVulkan) << "Cannot initialize renderer: no Vulkan instance set";
        return false;
    }

    if(VKFAILED(volkInitialize())) {
        qCCritical(logVulkan) << "Failed to initialize Vulkan function loader";
        return false;
    }

    m_instance = m_window->vulkanInstance();
    volkLoadInstance(m_instance->vkInstance());

    uint32_t queueFamilyIndex;
    VkPhysicalDevice physicalDevice = choosePhysicalDevice(RequiredDeviceExtensions, queueFamilyIndex);
    if(physicalDevice == VK_NULL_HANDLE) {
        qCCritical(logVulkan) << "No suitable Vulkan physical device found";
        return false;
    }

    m_device = QSharedPointer<Device>(Device::create(physicalDevice, queueFamilyIndex, RequiredDeviceExtensions));
    if(!m_device) {
        return false;
    }
    vkGetDeviceQueue(*m_device, queueFamilyIndex, 0, &m_graphicsQueue);

    int numConcurrentFrames;
    if(!querySwapchainProperties(physicalDevice, m_swapchainFormat, numConcurrentFrames)) {
        return false;
    }
    if(!querySwapchainPresentModes(physicalDevice, Config::EnableVsync, m_swapchainPresentMode)) {
        return false;
    }
    m_frameResources.resize(numConcurrentFrames);

    m_commandBufferManager.reset(new CommandBufferManager(this));
    m_descriptorManager.reset(new DescriptorManager(this));
    m_sceneManager.reset(new SceneManager(this));

    if(!createResources()) {
        return false;
    }

    m_renderFrameTimer->start();
    m_frameAdvanceService->proceedToNextFrame();
    return true;
}

void Renderer::shutdown()
{
    m_renderFrameTimer->stop();

    if(m_device) {
        m_device->waitIdle();

        if(m_swapchain) {
            releaseSwapchainResources();
            m_device->destroySwapchain(m_swapchain);
        }

        releaseRenderBufferResources();
        releaseResources();

        m_sceneManager.reset();
        m_descriptorManager.reset();
        m_commandBufferManager.reset();

        m_device.reset();
    }

    m_swapchain = {};
    m_graphicsQueue = VK_NULL_HANDLE;
}

QVector<Qt3DCore::QAspectJobPtr> Renderer::createGeometryJobs()
{
    QVector<Qt3DCore::QAspectJobPtr> geometryJobs;

    auto *geometryManager = &m_nodeManagers->geometryManager;
    auto dirtyGeometry = geometryManager->acquireDirtyComponents();

    QVector<Qt3DCore::QAspectJobPtr> buildGeometryJobs;
    buildGeometryJobs.reserve(dirtyGeometry.size());
    for(const Qt3DCore::QNodeId &geometryId : dirtyGeometry) {
        Raytrace::HGeometry handle = geometryManager->lookupHandle(geometryId);
        if(!handle.isNull()) {
            auto job = BuildGeometryJobPtr::create(this, handle);
            buildGeometryJobs.append(job);
        }
    }

    geometryJobs.append(buildGeometryJobs);
    return geometryJobs;
}

QVector<Qt3DCore::QAspectJobPtr> Renderer::createTextureJobs()
{
    QVector<Qt3DCore::QAspectJobPtr> textureJobs;

    auto *textureImageManager = &m_nodeManagers->textureImageManager;
    auto dirtyTextureImages = textureImageManager->acquireDirtyComponents();

    QVector<Qt3DCore::QAspectJobPtr> uploadTextureJobs;
    uploadTextureJobs.reserve(dirtyTextureImages.size());
    for(const Qt3DCore::QNodeId &textureImageId : dirtyTextureImages) {
        Raytrace::HTextureImage handle = textureImageManager->lookupHandle(textureImageId);
        if(!handle.isNull()) {
            auto job = UploadTextureJobPtr::create(this, handle);
            uploadTextureJobs.append(job);
        }
    }

    textureJobs.append(uploadTextureJobs);
    return textureJobs;
}

QVector<Qt3DCore::QAspectJobPtr> Renderer::createMaterialJobs(bool forceAllDirty)
{
    auto *materialManager = &m_nodeManagers->materialManager;

    QVector<Raytrace::HMaterial> dirtyMaterialHandles;
    if(forceAllDirty) {
        dirtyMaterialHandles = materialManager->activeHandles();
        materialManager->clearDirtyComponents();
    }
    else {
        auto dirtyMaterials = materialManager->acquireDirtyComponents();
        dirtyMaterialHandles.reserve(dirtyMaterials.size());
        for(const Qt3DCore::QNodeId &materialId : dirtyMaterials) {
            Raytrace::HMaterial handle = materialManager->lookupHandle(materialId);
            if(!handle.isNull()) {
                dirtyMaterialHandles.append(handle);
            }
        }
    }

    auto updateMaterialsJob = UpdateMaterialsJobPtr::create(this, &m_nodeManagers->textureManager);
    updateMaterialsJob->setDirtyMaterialHandles(dirtyMaterialHandles);
    return { updateMaterialsJob };
}


bool Renderer::createResources()
{
    if(!m_descriptorManager->createDescriptorPool(ResourceClass::AttributeBuffer, Config::DescriptorPoolCapacity)) {
        qCCritical(logVulkan) << "Failed to create attribute buffer descriptor pool";
        return false;
    }
    if(!m_descriptorManager->createDescriptorPool(ResourceClass::IndexBuffer, Config::DescriptorPoolCapacity)) {
        qCCritical(logVulkan) << "Failed to create index buffer descriptor pool";
        return false;
    }
    if(!m_descriptorManager->createDescriptorPool(ResourceClass::TextureImage, Config::DescriptorPoolCapacity)) {
        qCCritical(logVulkan) << "Failed to create texture image descriptor pool";
        return false;
    }

    m_renderingFinishedSemaphore = m_device->createSemaphore();
    m_presentationFinishedSemaphore = m_device->createSemaphore();

    m_frameCommandPool = m_device->createCommandPool(VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
    {
        QVector<CommandBuffer> frameCommandBuffers = m_device->allocateCommandBuffers({m_frameCommandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, uint32_t(numConcurrentFrames())});
        for(int i=0; i<int(numConcurrentFrames()); ++i) {
            m_frameResources[i].commandBuffer = frameCommandBuffers[i];
            m_frameResources[i].commandBuffersExecutedFence = m_device->createFence(VK_FENCE_CREATE_SIGNALED_BIT);
        }
    }

    {
        const QVector<VkDescriptorPoolSize> descriptorPoolSizes = {
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV, numConcurrentFrames() }, // Scene TLAS
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, numConcurrentFrames() }, // Display buffer
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, numConcurrentFrames() }, // Render buffer
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, numConcurrentFrames() }, // Instance buffer
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, numConcurrentFrames() }, // Material buffer
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, numConcurrentFrames() }, // Emitter buffer
        };
        const uint32_t descriptorPoolCapacity = uint32_t(descriptorPoolSizes.size()) * numConcurrentFrames();
        m_frameDescriptorPool = m_device->createDescriptorPool({ descriptorPoolCapacity, descriptorPoolSizes});
    }

    m_defaultQueryPool = m_device->createQueryPool({VK_QUERY_TYPE_TIMESTAMP, 2 * numConcurrentFrames()});

    m_displaySampler = m_device->createSampler({VK_FILTER_NEAREST});
    m_textureSampler = m_device->createSampler({VK_FILTER_LINEAR});

    m_displayRenderPass = createDisplayRenderPass(m_swapchainFormat.format);
    m_displayPipeline = GraphicsPipelineBuilder(m_device.get(), m_displayRenderPass)
            .shaders({"display.vert", "display.frag"})
            .defaultSampler(m_displaySampler)
            .build();

    m_renderPipeline = RayTracingPipelineBuilder(m_device.get())
            .shaders({"pathtrace.rgen", "pathtrace.rmiss", "pathtrace.rchit"})
            .shaders({"queryemission.rchit", "queryemission.rmiss"})
            .shaders({"queryvisibility.rchit", "queryvisibility.rmiss"})
            .defaultSampler(m_textureSampler)
            .descriptorBindingManager(DS_AttributeBuffer, 0, m_descriptorManager.get(), ResourceClass::AttributeBuffer)
            .descriptorBindingManager(DS_IndexBuffer, 0, m_descriptorManager.get(), ResourceClass::IndexBuffer)
            .descriptorBindingManager(DS_TextureImage, 0, m_descriptorManager.get(), ResourceClass::TextureImage)
            .maxRecursionDepth(Config::GlobalMaxRecursionDepth)
            .build();

    for(auto &frame : m_frameResources) {
        const QVector<VkDescriptorSetLayout> descriptorSetLayouts = {
            m_displayPipeline.descriptorSetLayouts[DS_Display],
            m_renderPipeline.descriptorSetLayouts[DS_Render],
        };
        auto descriptorSets = m_device->allocateDescriptorSets({m_frameDescriptorPool, descriptorSetLayouts});
        frame.displayDescriptorSet = descriptorSets[0];
        frame.renderDescriptorSet = descriptorSets[1];
    }

    return true;
}

void Renderer::releaseResources()
{
    m_device->destroySemaphore(m_renderingFinishedSemaphore);
    m_device->destroySemaphore(m_presentationFinishedSemaphore);

    m_device->destroyCommandPool(m_frameCommandPool);
    m_device->destroyDescriptorPool(m_frameDescriptorPool);
    m_device->destroyQueryPool(m_defaultQueryPool);

    m_device->destroySampler(m_displaySampler);
    m_device->destroySampler(m_textureSampler);

    m_device->destroyRenderPass(m_displayRenderPass);
    m_device->destroyPipeline(m_displayPipeline);
    m_device->destroyPipeline(m_renderPipeline);

    for(auto &frame : m_frameResources) {
        m_device->destroyFence(frame.commandBuffersExecutedFence);
    }

    m_sceneManager->destroyResources();
    m_descriptorManager->destroyAllDescriptorPools();
}

bool Renderer::createSwapchainResources(const QSize &size)
{
    Result result;

    const uint32_t swapchainWidth = uint32_t(size.width());
    const uint32_t swapchainHeight = uint32_t(size.height());

    QVector<VkImage> swapchainImages;
    uint32_t numSwapchainImages = 0;
    vkGetSwapchainImagesKHR(*m_device, m_swapchain, &numSwapchainImages, nullptr);
    swapchainImages.resize(int(numSwapchainImages));
    if(VKFAILED(result = vkGetSwapchainImagesKHR(*m_device, m_swapchain, &numSwapchainImages, swapchainImages.data()))) {
        qCWarning(logVulkan) << "Failed to obtain swapchain image handles:" << result.toString();
        return false;
    }

    m_swapchainAttachments.resize(int(numSwapchainImages));
    for(uint32_t imageIndex=0; imageIndex < numSwapchainImages; ++imageIndex) {
        auto &attachment = m_swapchainAttachments[int(imageIndex)];
        attachment.image.handle = swapchainImages[int(imageIndex)];
        attachment.image.view = m_device->createImageView({attachment.image, VK_IMAGE_VIEW_TYPE_2D, m_swapchainFormat.format});
        attachment.framebuffer = m_device->createFramebuffer({m_displayRenderPass, { attachment.image.view }, swapchainWidth, swapchainHeight});
    }

    m_swapchainSize = size;
    return true;
}

void Renderer::releaseSwapchainResources()
{
    for(auto &attachment : m_swapchainAttachments) {
        m_device->destroyImageView(attachment.image.view);
        m_device->destroyFramebuffer(attachment.framebuffer);
    }
    m_swapchainAttachments.clear();
    m_lastSwapchainImage = nullptr;
    m_swapchainSize = QSize();
}

bool Renderer::createRenderBufferResources(const QSize &size, VkFormat format)
{
    for(auto &frame : m_frameResources) {
        ImageCreateInfo renderBufferCreateInfo{VK_IMAGE_TYPE_2D, format, size};
        renderBufferCreateInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if(!(frame.renderBuffer = m_device->createImage(renderBufferCreateInfo, VMA_MEMORY_USAGE_GPU_ONLY))) {
            qCCritical(logVulkan) << "Failed to create render buffer";
            return false;
        }
    }

    auto *previousFrame = &m_frameResources[int(numConcurrentFrames()-1)];
    for(auto &frame : m_frameResources) {
        m_device->writeDescriptors({
            { frame.displayDescriptorSet, Binding_DisplayBuffer, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DescriptorImageInfo(frame.renderBuffer.view, ImageState::ShaderRead) },
            { frame.renderDescriptorSet, Binding_RenderBuffer, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DescriptorImageInfo(frame.renderBuffer.view, ImageState::ShaderReadWrite) },
            { frame.renderDescriptorSet, Binding_PrevRenderBuffer, 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, DescriptorImageInfo(previousFrame->renderBuffer.view, ImageState::ShaderReadWrite) },
        });
        previousFrame = &frame;
    }

    m_renderBufferSize = size;
    resetRenderProgress();
    return true;
}

void Renderer::releaseRenderBufferResources()
{
    for(auto &frame : m_frameResources) {
        m_device->destroyImage(frame.renderBuffer);
    }
    m_renderBufferSize   = QSize();
    m_renderBuffersReady = false;
    m_lastRenderBuffer   = nullptr;
}

void Renderer::beginRenderIteration()
{
    if(m_settings) {
        m_renderParams.numPrimarySamples = m_settings->primarySamples();
        m_renderParams.numSecondarySamples = m_settings->secondarySamples();
        m_renderParams.minDepth = m_settings->minDepth();
        m_renderParams.maxDepth = m_settings->maxDepth();
        m_renderParams.directRadianceClamp = m_settings->directRadianceClamp();
        m_renderParams.indirectRadianceClamp = m_settings->indirectRadianceClamp();
    }

    m_renderParams.frameNumber = ++m_frameNumber;
    m_renderParams.numEmitters = m_sceneManager->numEmitters();
}

void Renderer::releaseWindowSurface()
{
    if(m_device && m_window) {
        m_device->waitIdle();
        if(m_swapchain) {
            releaseSwapchainResources();
            m_device->destroySwapchain(m_swapchain);
        }
    }
    m_window = nullptr;
}

void Renderer::resetRenderProgress()
{
    m_clearPreviousRenderBuffer = true;
    m_frameNumber = 0;

    if(m_frameElapsedTimer.isValid()) {
        m_frameElapsedTimer.restart();
    }
    else {
        m_frameElapsedTimer.start();
    }
}

void Renderer::updateActiveCamera()
{
    Q_ASSERT(m_cameraManager);
    if(m_settings) {
        Raytrace::Entity *cameraEntity = m_nodeManagers->entityManager.lookupResource(m_settings->cameraId());
        if(cameraEntity && cameraEntity->isCamera()) {
            m_cameraManager->setActiveCamera(cameraEntity);
        }
    }
}

bool Renderer::querySwapchainProperties(VkPhysicalDevice physicalDevice, VkSurfaceFormatKHR &surfaceFormat, int &minImageCount) const
{
    Result result;
    VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(m_window);

    VkSurfaceCapabilitiesKHR surfaceCaps;
    if(VKFAILED(result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &surfaceCaps))) {
        qCCritical(logVulkan) << "Failed to query physical device surface capabilities" << result.toString();
        return false;
    }

    QVector<VkSurfaceFormatKHR> surfaceFormats;
    uint32_t surfaceFormatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, nullptr);
    surfaceFormats.resize(int(surfaceFormatCount));
    if(VKFAILED(result = vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &surfaceFormatCount, surfaceFormats.data()))) {
        qCCritical(logVulkan) << "Failed to enumerate physical device surface formats:" << result.toString();
        return false;
    }

    surfaceFormat = surfaceFormats[0];
    minImageCount = int(surfaceCaps.minImageCount);
    return true;
}

bool Renderer::querySwapchainPresentModes(VkPhysicalDevice physicalDevice, bool vsync, VkPresentModeKHR &presentMode) const
{
    if(vsync) {
        // This mode is guaranteed to be supported.
        presentMode = VK_PRESENT_MODE_FIFO_KHR;
        return true;
    }

    Result result;
    VkSurfaceKHR surface = QVulkanInstance::surfaceForWindow(m_window);

    QVector<VkPresentModeKHR> presentModes;
    uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, nullptr);
    presentModes.resize(int(presentModeCount));
    if(VKFAILED(result = vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount, presentModes.data()))) {
        qCCritical(logVulkan) << "Failed to enumerate physical device surface present modes:" << result.toString();
        return false;
    }

    VkPresentModeKHR selectedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for(VkPresentModeKHR mode : presentModes) {
        if(mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            selectedPresentMode = mode;
            break;
        }
        if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            selectedPresentMode = mode;
        }
    }

    presentMode = selectedPresentMode;
    return true;
}

void Renderer::resizeSwapchain()
{
    Q_ASSERT(m_device);
    Q_ASSERT(m_window);

    QSize currentSwapchainSize;
    if(!m_device->querySwapchainSize(m_window, currentSwapchainSize)) {
        qCWarning(logVulkan) << "Failed to retrieve current swapchain size";
        return;
    }

    if(currentSwapchainSize != m_swapchainSize) {
        m_device->waitIdle();

        if(m_swapchain) {
            releaseSwapchainResources();
        }
        if(!currentSwapchainSize.isNull()) {
            Swapchain newSwapchain = m_device->createSwapchain(m_window, m_swapchainFormat, m_swapchainPresentMode, uint32_t(numConcurrentFrames()), m_swapchain);
            if(newSwapchain) {
                m_device->destroySwapchain(m_swapchain);
                m_swapchain = newSwapchain;

                createSwapchainResources(currentSwapchainSize);
                if(m_swapchainSize != m_renderBufferSize) {
                    releaseRenderBufferResources();
                    createRenderBufferResources(m_swapchainSize, Config::RenderBufferFormat);
                }
            }
            else {
                qCWarning(logVulkan) << "Failed to resize swapchain";
            }
        }
        else {
            m_device->destroySwapchain(m_swapchain);
        }
    }
}

bool Renderer::acquireNextSwapchainImage(uint32_t &imageIndex) const
{
    Result result;
    if(VKFAILED(result = vkAcquireNextImageKHR(*m_device, m_swapchain, UINT64_MAX, m_presentationFinishedSemaphore, VK_NULL_HANDLE, &imageIndex))) {
        if(result != VK_SUBOPTIMAL_KHR) {
            qCCritical(logVulkan) << "Failed to acquire next swapchain image:" << result.toString();
            return false;
        }
    }
    return true;
}

bool Renderer::presentSwapchainImage(uint32_t imageIndex)
{
    Q_ASSERT(m_swapchain);
    Q_ASSERT(m_swapchainSize.isValid());

    Result result;

    VkPresentInfoKHR presentInfo = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &m_renderingFinishedSemaphore.handle;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &m_swapchain.handle;
    presentInfo.pImageIndices = &imageIndex;
    result = vkQueuePresentKHR(m_graphicsQueue, &presentInfo);
    if(VKSUCCEEDED(result) || result == VK_SUBOPTIMAL_KHR) {
        Q_ASSERT(m_instance);
        m_instance->presentQueued(m_window);
    }
    else if(result != VK_ERROR_OUT_OF_DATE_KHR) {
        qCCritical(logVulkan) << "Failed to queue swapchain image for presentation:" << result.toString();
        return false;
    }

    return true;
}

bool Renderer::submitFrameCommands()
{
    const FrameResources &frame = m_frameResources[m_frameIndex];

    Result result;

    VkSubmitInfo submitInfo = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.commandBuffer.handle;
    if(m_swapchainSize.isValid()) {
        VkPipelineStageFlags submitWaitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &m_presentationFinishedSemaphore.handle;
        submitInfo.pWaitDstStageMask = &submitWaitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &m_renderingFinishedSemaphore.handle;
    }
    if(VKFAILED(result = vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.commandBuffersExecutedFence))) {
        qCCritical(logVulkan) << "Failed to submit frame commands to the graphics queue:" << result.toString();
        return false;
    }

    m_frameIndex = (m_frameIndex + 1) % int(numConcurrentFrames());

    return true;
}

void Renderer::renderFrame()
{
    Q_ASSERT(m_device);

    QReadLocker lock(&m_windowSurfaceLock);
    if(!m_window) {
        // Window surface has already been released. Bail out.
        return;
    }

    resizeSwapchain();

    QElapsedTimer frameTimer;
    frameTimer.start();

    const QRect renderRect = { {0, 0}, m_renderBufferSize };
    const uint32_t currentFrameQueryIndex = uint32_t(currentFrameIndex() * 2);
    const uint32_t previousFrameQueryIndex = uint32_t(previousFrameIndex() * 2);

    FrameResources &currentFrame = m_frameResources[currentFrameIndex()];
    FrameResources &previousFrame = m_frameResources[previousFrameIndex()];

    m_device->waitForFence(currentFrame.commandBuffersExecutedFence);
    m_device->resetFence(currentFrame.commandBuffersExecutedFence);

    m_sceneManager->updateRetiredResources();

    const bool readyToRender = m_sceneManager->isReadyToRender();
    if(readyToRender) {
        beginRenderIteration();

        m_cameraManager->applyRenderParameters(m_renderParams);
        m_cameraManager->applyDisplayPrameters(m_displayParams);

        m_device->writeDescriptor({ currentFrame.renderDescriptorSet, Binding_TLAS, 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_NV }, m_sceneManager->sceneTLAS());
        m_device->writeDescriptors({
            { currentFrame.renderDescriptorSet, Binding_Instances, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DescriptorBufferInfo(m_sceneManager->instanceBuffer()) },
            { currentFrame.renderDescriptorSet, Binding_Materials, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DescriptorBufferInfo(m_sceneManager->materialBuffer()) },
            { currentFrame.renderDescriptorSet, Binding_Emitters,  0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, DescriptorBufferInfo(m_sceneManager->emitterBuffer()) },
        });
    }

    m_commandBufferManager->submitCommandBuffers(m_graphicsQueue);

    uint32_t swapchainImageIndex = 0;

    CommandBuffer &commandBuffer = currentFrame.commandBuffer;
    commandBuffer.begin(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    {
        commandBuffer.resetQueryPool(m_defaultQueryPool, currentFrameQueryIndex, 2);
        commandBuffer.writeTimestamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_defaultQueryPool, currentFrameQueryIndex);

        if(!m_renderBuffersReady) {
            QVector<ImageTransition> transitions{int(numConcurrentFrames())};
            for(int index=0; index < transitions.size(); ++index) {
                if(m_clearPreviousRenderBuffer && index == previousFrameIndex()) {
                    transitions[index] = { m_frameResources[index].renderBuffer, ImageState::Undefined, ImageState::CopyDest };
                }
                else {
                    transitions[index] = { m_frameResources[index].renderBuffer, ImageState::Undefined, ImageState::ShaderReadWrite };
                }
            }
            commandBuffer.resourceBarrier(transitions);
        }
        if(m_clearPreviousRenderBuffer) {
            if(m_renderBuffersReady) {
                commandBuffer.resourceBarrier({previousFrame.renderBuffer, ImageState::Undefined, ImageState::CopyDest});
            }
            commandBuffer.clearColorImage(previousFrame.renderBuffer, ImageState::CopyDest);
            commandBuffer.resourceBarrier({previousFrame.renderBuffer, ImageState::CopyDest, ImageState::ShaderReadWrite});
        }
        m_renderBuffersReady = true;
        m_clearPreviousRenderBuffer = false;

        if(readyToRender) {
            const QVector<VkDescriptorSet> descriptorSets = {
                currentFrame.renderDescriptorSet,
                m_descriptorManager->descriptorSet(ResourceClass::AttributeBuffer),
                m_descriptorManager->descriptorSet(ResourceClass::IndexBuffer),
                m_descriptorManager->descriptorSet(ResourceClass::TextureImage)
            };

            commandBuffer.bindPipeline(m_renderPipeline);
            commandBuffer.bindDescriptorSets(m_renderPipeline, 0, descriptorSets);
            commandBuffer.pushConstants(m_renderPipeline, 0, &m_renderParams);
            commandBuffer.traceRays(m_renderPipeline, uint32_t(renderRect.width()), uint32_t(renderRect.height()));
            m_lastRenderBuffer = &currentFrame.renderBuffer;
        }

        commandBuffer.resourceBarrier({currentFrame.renderBuffer, ImageState::ShaderReadWrite, ImageState::ShaderRead});

        if(m_swapchainSize.isValid() && acquireNextSwapchainImage(swapchainImageIndex)) {
            const auto &attachment = m_swapchainAttachments[int(swapchainImageIndex)];
            commandBuffer.beginRenderPass({m_displayRenderPass, attachment.framebuffer, renderRect}, VK_SUBPASS_CONTENTS_INLINE);
            commandBuffer.bindPipeline(m_displayPipeline);
            commandBuffer.bindDescriptorSets(m_displayPipeline, 0, {currentFrame.displayDescriptorSet});
            commandBuffer.pushConstants(m_displayPipeline, 0, &m_displayParams);
            commandBuffer.setViewport(renderRect);
            commandBuffer.setScissor(renderRect);
            commandBuffer.draw(3, 1);
            commandBuffer.endRenderPass();
            m_lastSwapchainImage = &attachment.image;
        }

        commandBuffer.resourceBarrier({currentFrame.renderBuffer, ImageState::ShaderRead, ImageState::ShaderReadWrite});
        commandBuffer.writeTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_defaultQueryPool, currentFrameQueryIndex+1);
    }
    commandBuffer.end();

    if(submitFrameCommands()) {
        if(m_swapchainSize.isValid()) {
            presentSwapchainImage(swapchainImageIndex);
        }
    }
    else {
        qCWarning(logVulkan) << "Failed to submit frame commands to the graphics queue";
    }

    m_commandBufferManager->proceedToNextFrame();
    m_frameAdvanceService->proceedToNextFrame();

    // TODO: Don't wait on previous frame query availability (though in practice it doesn't seem to reduce performance).
    double previousDeviceTime = -1.0;
    m_device->queryTimeElapsed(m_defaultQueryPool, previousFrameQueryIndex, previousDeviceTime, VK_QUERY_RESULT_WAIT_BIT);
    updateFrameTimings(frameTimer.nsecsElapsed() * 1e-6, previousDeviceTime);
}

VkPhysicalDevice Renderer::choosePhysicalDevice(const QByteArrayList &requiredExtensions, uint32_t &queueFamilyIndex) const
{
    Q_ASSERT(m_instance);

    VkPhysicalDevice selectedPhysicalDevice = VK_NULL_HANDLE;
    uint32_t selectedQueueFamilyIndex = uint32_t(-1);

    QVector<VkPhysicalDevice> physicalDevices;
    uint32_t physicalDeviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance->vkInstance(), &physicalDeviceCount, nullptr);
    if(physicalDeviceCount > 0) {
        physicalDevices.resize(int(physicalDeviceCount));
        if(VKFAILED(vkEnumeratePhysicalDevices(m_instance->vkInstance(), &physicalDeviceCount, physicalDevices.data()))) {
            qCWarning(logVulkan) << "Failed to enumerate available physical devices";
            return VK_NULL_HANDLE;
        }
    }
    else {
        qCWarning(logVulkan) << "No Vulkan capable physical devices found";
        return VK_NULL_HANDLE;
    }

    for(VkPhysicalDevice physicalDevice : physicalDevices) {
        QVector<VkQueueFamilyProperties> queueFamilies;
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        if(queueFamilyCount == 0) {
            continue;
        }
        queueFamilies.resize(int(queueFamilyCount));
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

        selectedQueueFamilyIndex = uint32_t(-1);
        for(uint32_t index=0; index < queueFamilyCount; ++index) {
            const auto &queueFamily = queueFamilies[int(index)];

            constexpr VkQueueFlags RequiredQueueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if((queueFamily.queueFlags & RequiredQueueFlags) != RequiredQueueFlags) {
                continue;
            }
            if(!m_instance->supportsPresent(physicalDevice, index, m_window)) {
                continue;
            }
            selectedQueueFamilyIndex = index;
            break;
        }
        if(selectedQueueFamilyIndex == uint32_t(-1)) {
            continue;
        }

        QVector<VkExtensionProperties> extensions;
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
        if(extensionCount == 0) {
            continue;
        }
        extensions.resize(int(extensionCount));
        if(VKFAILED(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()))) {
            qCWarning(logVulkan) << "Failed to enumerate device extensions for physical device:" << physicalDevice;
            continue;
        }

        bool allRequiredExtensionsFound = true;
        for(const QByteArray &requiredExtension : requiredExtensions) {
            bool extensionFound = false;
            for(const VkExtensionProperties &extension : extensions) {
                if(requiredExtension == extension.extensionName) {
                    extensionFound = true;
                    break;
                }
            }
            if(!extensionFound) {
                allRequiredExtensionsFound = false;
                break;
            }
        }
        if(!allRequiredExtensionsFound) {
            continue;
        }

        selectedPhysicalDevice = physicalDevice;
        break;
    }
    if(selectedPhysicalDevice == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkPhysicalDeviceProperties selectedDeviceProperties;
    vkGetPhysicalDeviceProperties(selectedPhysicalDevice, &selectedDeviceProperties);
    qCInfo(logVulkan) << "Selected physical device:" << selectedDeviceProperties.deviceName;

    queueFamilyIndex = selectedQueueFamilyIndex;
    return selectedPhysicalDevice;
}

RenderPass Renderer::createDisplayRenderPass(VkFormat swapchainFormat) const
{
    VkAttachmentDescription colorAttachment = {};
    colorAttachment.format = swapchainFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    RenderPassCreateInfo createInfo;
    createInfo.attachmentCount = 1;
    createInfo.pAttachments = &colorAttachment;
    createInfo.subpassCount = 1;
    createInfo.pSubpasses = &subpass;

    RenderPass renderPass;
    if(!(renderPass = m_device->createRenderPass(createInfo))) {
        qCCritical(logVulkan) << "Could not create display render pass";
        return VK_NULL_HANDLE;
    }
    return renderPass;
}

void Renderer::updateFrameTimings(double cpuFrameTime, double gpuFrameTime)
{
    QWriteLocker lock(&m_frameTimingsLock);
    m_hostTimeAverage.add(cpuFrameTime);
    if(gpuFrameTime > 0.0) {
        m_deviceTimeAverage.add(gpuFrameTime);
    }
}

QImageData Renderer::grabImage(const Image *image, ImageState imageState, uint32_t width, uint32_t height, VkFormat format)
{
    Q_ASSERT(image);
    Q_ASSERT(width > 0 && height > 0);

    QImageData output = {};
    output.width    = int(width);
    output.height   = int(height);
    output.channels = 4;

    switch(format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
        output.type   = QImageData::ValueType::UInt8;
        output.format = QImageData::Format::RGBA;
        break;
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
        output.type   = QImageData::ValueType::UInt8;
        output.format = QImageData::Format::BGRA;
        break;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        output.type   = QImageData::ValueType::Float32;
        output.format = QImageData::Format::RGBA;
        break;
    default:
        Q_ASSERT_X(0, Q_FUNC_INFO, "Unsupported image format");
    }

    const VkDeviceSize pixelSize = uint32_t(output.channels * static_cast<int>(output.type));
    const VkDeviceSize stagingBufferSize = VkDeviceSize(width * height * pixelSize);

    Buffer stagingBuffer = m_device->createStagingBuffer(stagingBufferSize);
    if(!stagingBuffer || !stagingBuffer.isHostAccessible()) {
        qCCritical(logVulkan) << "Failed to grab image: staging buffer creation failed";
        return output;
    }

    TransientCommandBuffer commandBuffer = m_commandBufferManager->acquireCommandBuffer();
    {
        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = VkExtent3D{ width, height, 1 };

        commandBuffer->resourceBarrier(ImageTransition{ image->handle, imageState, ImageState::CopySource });
        commandBuffer->copyImageToBuffer(image->handle, ImageState::CopySource, stagingBuffer, region);
        commandBuffer->resourceBarrier(ImageTransition{ image->handle, ImageState::CopySource, imageState });
        commandBuffer->pipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    }
    if(!m_commandBufferManager->executeCommandBufferImmediate(m_graphicsQueue, commandBuffer)) {
        qCCritical(logVulkan) << "Failed to grab image: tansfer operation failed";
        return output;
    }

    output.data = QByteArray(stagingBuffer.memory<const char>(), int(stagingBufferSize));
    m_device->destroyBuffer(stagingBuffer);

    return output;
}

int Renderer::currentFrameIndex() const
{
    return m_frameIndex;
}

int Renderer::previousFrameIndex() const
{
    return (m_frameIndex > 0) ? (m_frameIndex - 1) : (int(numConcurrentFrames()) - 1);
}

QSurface *Renderer::surface() const
{
    QReadLocker lock(&m_windowSurfaceLock);
    return m_window;
}

void Renderer::setSurface(QObject *surfaceObject)
{
    QWriteLocker lock(&m_windowSurfaceLock);

    if(m_window) {
        releaseWindowSurface();
    }
    if(surfaceObject) {
        if(QWindow *window = qobject_cast<QWindow*>(surfaceObject)) {
            m_window = window;
        }
        else {
            qCWarning(logVulkan) << "Incompatible surface object: expected QWindow instance";
        }
    }
}

Device *Renderer::device() const
{
    Q_ASSERT(m_device);
    return m_device.get();
}

void Renderer::markDirty(DirtySet changes, Raytrace::BackendNode *node)
{
    Q_UNUSED(node);
    m_dirtySet |= changes;
}

Raytrace::Entity *Renderer::sceneRoot() const
{
    return m_sceneRoot;
}

void Renderer::setSceneRoot(Raytrace::Entity *rootEntity)
{
    m_sceneRoot = rootEntity;
    m_updateWorldTransformJob->setRoot(m_sceneRoot);
}

Raytrace::RenderSettings *Renderer::settings() const
{
    return m_settings;
}

QRenderStatistics Renderer::statistics() const
{
    QReadLocker lock(&m_frameTimingsLock);

    QRenderStatistics stats;
    stats.cpuFrameTime = m_hostTimeAverage.average();
    stats.gpuFrameTime = m_deviceTimeAverage.average();
    stats.totalRenderTime = m_frameElapsedTimer.elapsed() * 1e-3;
    stats.numFramesRendered = m_frameNumber;
    return stats;
}

void Renderer::setSettings(Raytrace::RenderSettings *settings)
{
    m_settings = settings;
    updateActiveCamera();
}

void Renderer::setNodeManagers(Raytrace::NodeManagers *nodeManagers)
{
    Q_ASSERT(nodeManagers);
    m_nodeManagers = nodeManagers;
    m_updateEmittersJob->setTextureManager(&m_nodeManagers->textureManager);
}

Qt3DCore::QAbstractFrameAdvanceService *Renderer::frameAdvanceService() const
{
    return m_frameAdvanceService.get();
}

CommandBufferManager *Renderer::commandBufferManager() const
{
    return m_commandBufferManager.get();
}

DescriptorManager *Renderer::descriptorManager() const
{
    return m_descriptorManager.get();
}

SceneManager *Renderer::sceneManager() const
{
    return m_sceneManager.get();
}

CameraManager *Renderer::cameraManager() const
{
    return m_cameraManager.get();
}

QVector<Qt3DCore::QAspectJobPtr> Renderer::jobsToExecute(qint64 time)
{
    QVector<Qt3DCore::QAspectJobPtr> jobs;

    bool shouldUpdateRenderParameters = false;
    bool shouldUpdateInstanceBuffer = false;
    bool shouldUpdateEmitters = false;
    bool shouldUpdateTLAS = false;
    bool sceneEntitiesDirty = false;

    m_updateRenderParametersJob->removeDependency(m_updateWorldTransformJob);

    m_updateInstanceBufferJob->removeDependency(m_updateWorldTransformJob);
    m_updateInstanceBufferJob->removeDependency(Qt3DCore::QAspectJobPtr());

    m_updateEmittersJob->removeDependency(m_updateWorldTransformJob);
    m_updateEmittersJob->removeDependency(Qt3DCore::QAspectJobPtr());

    jobs.append(m_destroyExpiredResourcesJob);

    if(m_dirtySet != DirtyFlag::NoneDirty) {
        resetRenderProgress();
    }

    if(m_dirtySet & DirtyFlag::EntityDirty || m_dirtySet & DirtyFlag::GeometryDirty) {
        shouldUpdateInstanceBuffer = true;
        shouldUpdateEmitters = true;
        shouldUpdateTLAS = true;
        sceneEntitiesDirty = true;
    }
    if(m_dirtySet & DirtyFlag::LightDirty) {
        shouldUpdateEmitters = true;
        sceneEntitiesDirty = true;
    }

    if(m_dirtySet & DirtyFlag::TransformDirty) {
        jobs.append(m_updateWorldTransformJob);
        m_updateRenderParametersJob->addDependency(m_updateWorldTransformJob);
        m_updateInstanceBufferJob->addDependency(m_updateWorldTransformJob);
        m_updateEmittersJob->addDependency(m_updateWorldTransformJob);
        shouldUpdateTLAS = true;
        shouldUpdateRenderParameters = true;
        shouldUpdateInstanceBuffer = true;
        shouldUpdateEmitters = true;
    }

    QVector<Qt3DCore::QAspectJobPtr> geometryJobs;
    if(m_dirtySet & DirtyFlag::GeometryDirty) {
        geometryJobs = createGeometryJobs();
        jobs.append(geometryJobs);
        shouldUpdateTLAS = true;
        shouldUpdateInstanceBuffer = true;
        shouldUpdateEmitters = true;
    }

    QVector<Qt3DCore::QAspectJobPtr> textureJobs;
    if(m_dirtySet & DirtyFlag::TextureDirty) {
        textureJobs = createTextureJobs();
        jobs.append(textureJobs);
        shouldUpdateEmitters = true;
    }

    QVector<Qt3DCore::QAspectJobPtr> materialJobs;
    if(m_dirtySet & DirtyFlag::MaterialDirty || m_dirtySet & DirtyFlag::TextureDirty) {
        bool forceUpdateAllMaterials = (m_dirtySet & DirtyFlag::TextureDirty);
        materialJobs = createMaterialJobs(forceUpdateAllMaterials);
        jobs.append(materialJobs);
        for(const auto &materialJob : materialJobs) {
            for(const auto &textureJob : textureJobs) {
                materialJob->addDependency(textureJob);
            }
        }
        shouldUpdateInstanceBuffer = true;
        shouldUpdateEmitters = true;
    }

    if(m_dirtySet & DirtyFlag::CameraDirty) {
        updateActiveCamera();
        shouldUpdateRenderParameters = true;
    }

    m_dirtySet = DirtyFlag::NoneDirty;

    if(shouldUpdateRenderParameters) {
        jobs.append(m_updateRenderParametersJob);
    }

    if(sceneEntitiesDirty) {
        m_sceneManager->gatherEntities(&m_nodeManagers->entityManager);
    }
    if(m_sceneManager->renderables().size() == 0) {
        return jobs;
    }

    if(shouldUpdateTLAS) {
        Qt3DCore::QAspectJobPtr buildSceneTLASJob = BuildSceneTopLevelAccelerationStructureJobPtr::create(this);
        buildSceneTLASJob->addDependency(m_updateWorldTransformJob);
        for(const auto &job : geometryJobs) {
            buildSceneTLASJob->addDependency(job);
        }
        jobs.append(buildSceneTLASJob);
    }
    if(shouldUpdateInstanceBuffer) {
        for(const auto &job : geometryJobs) {
            m_updateInstanceBufferJob->addDependency(job);
        }
        for(const auto &job : materialJobs) {
            m_updateInstanceBufferJob->addDependency(job);
        }
        jobs.append(m_updateInstanceBufferJob);
    }
    if(shouldUpdateEmitters) {
        for(const auto &job : geometryJobs) {
            m_updateEmittersJob->addDependency(job);
        }
        for(const auto &job : materialJobs) {
            m_updateEmittersJob->addDependency(job);
        }
        for(const auto &job : textureJobs) {
            m_updateEmittersJob->addDependency(job);
        }
        jobs.append(m_updateEmittersJob);
    }

    return jobs;
}

uint32_t Renderer::numConcurrentFrames() const
{
    return uint32_t(m_frameResources.size());
}

QImageData Renderer::grabImage(QRenderImage type)
{
    uint32_t width, height;
    switch(type) {
    case QRenderImage::HDR:
        width  = uint32_t(m_renderBufferSize.width());
        height = uint32_t(m_renderBufferSize.height());
        if(!m_lastRenderBuffer) {
            qCWarning(logVulkan) << "Cannot grab render buffer: image not ready";
            return QImageData{};
        }
        return grabImage(m_lastRenderBuffer, ImageState::ShaderReadWrite, width, height, Config::RenderBufferFormat);
    case QRenderImage::FinalLDR:
        width  = uint32_t(m_swapchainSize.width());
        height = uint32_t(m_swapchainSize.height());
        if(!m_lastSwapchainImage) {
            qCWarning(logVulkan) << "Cannot grab swapchain: image not ready";
            return QImageData{};
        }
        return grabImage(m_lastSwapchainImage, ImageState::PresentSource, width, height, m_swapchainFormat.format);
    }
    return QImageData{};
}

} // Vulkan
} // Qt3DRaytrace
