/*
 * Copyright (c) 2017-2024 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 */

#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/Application/Interfaces/IScreenshot.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../Common_3/Utilities/RingBuffer.h"

#include "ISPC/test.comp.h"

// Test parameters 
static const uint32_t COMPUTE_TEST_WIDTH = 4096;
static const uint32_t COMPUTE_TEST_HEIGHT = 4096;

struct ComputeTestData {
    uint32_t width;
    uint32_t height;
};

ProfileToken gCpuComputeToken;
ProfileToken gGpuComputeToken;

uint32_t gFontID = 0;

class CPUComputeTest: public IApp 
{
public:
    bool Init() override
    {
        // Initialize renderer with basic settings
        RendererDesc settings = {};
        initGPUConfiguration(settings.pExtendedSettings);
        initRenderer(GetName(), &settings, &pRenderer);
        if (!pRenderer)
        {
            LOGF(LogLevel::eERROR, "Failed to initialize renderer");
            return false;
        }
        ProfilerDesc profiler = {};
        profiler.pRenderer = pRenderer;
        initProfiler(&profiler);

        // Create compute queue...
        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_COMPUTE;
        initQueue(pRenderer, &queueDesc, &pQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &mCmdRing);

        // Create a single command pool and command buffer
        CmdPoolDesc cmdPoolDesc = {};
        cmdPoolDesc.pQueue = pQueue;
        initCmdPool(pRenderer, &cmdPoolDesc, &pCmdPool);

        CmdDesc cmdDesc = {};
        cmdDesc.pPool = pCmdPool;
        initCmd(pRenderer, &cmdDesc, &pCmd);

        initResourceLoaderInterface(pRenderer);

        const uint32_t numElements = COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT;

        // Create output buffer 
        BufferLoadDesc outputDesc = {};
        outputDesc.ppBuffer = &pOutputBuffer;
        outputDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER;
        outputDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_TO_CPU;
        outputDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        outputDesc.mDesc.mSize = sizeof(float) * numElements;
        outputDesc.mDesc.mElementCount = numElements;
        outputDesc.mDesc.mStructStride = sizeof(float);

        SyncToken token = {};
        addResource(&outputDesc, &token);
        waitForToken(&token);

        // Create uniform buffer with test dimensions
        BufferLoadDesc uniformDesc = {};
        uniformDesc.ppBuffer = &pUniformBuffer;  // Add this line
        uniformDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        uniformDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        uniformDesc.mDesc.mSize = sizeof(ComputeTestData); 
        addResource(&uniformDesc, &token);

        // Initialize uniform data
        ComputeTestData* data = (ComputeTestData*)pUniformBuffer->pCpuMappedAddress;
        data->width = COMPUTE_TEST_WIDTH;
        data->height = COMPUTE_TEST_HEIGHT;

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);

        // Load fonts
        FontDesc font = {};
        font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
        fntDefineFonts(&font, 1, &gFontID);

        FontSystemDesc fontRenderDesc = {};
        fontRenderDesc.pRenderer = pRenderer;
        if (!initFontSystem(&fontRenderDesc))
            return false; // report?

        // Initialize Forge User Interface Rendering
        UserInterfaceDesc uiRenderDesc = {};
        uiRenderDesc.pRenderer = pRenderer;
        initUserInterface(&uiRenderDesc);

        initScreenshotCapturer(pRenderer, pQueue, GetName());

        return true;
    }

    void Exit() override
    {
        waitQueueIdle(pQueue);

        exitScreenshotCapturer();

        removeResource(pOutputBuffer);
        removeResource(pUniformBuffer);

        exitProfiler();
        exitUserInterface();
        exitFontSystem();

        exitSemaphore(pRenderer, pImageAcquiredSemaphore);
        exitGpuCmdRing(pRenderer, &mCmdRing);
        exitCmd(pRenderer, pCmd);
        exitCmdPool(pRenderer, pCmdPool);
        exitQueue(pRenderer, pQueue);
        exitResourceLoaderInterface(pRenderer);
        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc) override 
    {
        createShaders();
        createRootSignature();
        createDescriptorSet();
        createPipeline();
        updateDescriptors();
        loadProfilerUI(mSettings.mWidth, mSettings.mHeight);

        UIComponentDesc guiDesc = {};
        guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
        uiAddComponent(GetName(), &guiDesc, &pGuiWindow);

        if (!addSwapChain()) {
            return false;
        }

        UserInterfaceLoadDesc uiLoad = {};
        uiLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        uiLoad.mHeight = mSettings.mHeight;
        uiLoad.mWidth = mSettings.mWidth;
        uiLoad.mLoadType = pReloadDesc->mType;
        loadUserInterface(&uiLoad);

        FontSystemLoadDesc fontLoad = {};
        fontLoad.mColorFormat = pSwapChain->ppRenderTargets[0]->mFormat;
        fontLoad.mHeight = mSettings.mHeight;
        fontLoad.mWidth = mSettings.mWidth;
        fontLoad.mLoadType = pReloadDesc->mType;
        loadFontSystem(&fontLoad);

        return true;
    }

    void Unload(ReloadDesc* pReloadDesc) override
    {
        waitQueueIdle(pQueue);

        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);
        removePipeline(pRenderer, pComputePipeline);
        removeDescriptorSet(pRenderer, pDescriptorSet);
        removeRootSignature(pRenderer, pRootSignature);
        removeShader(pRenderer, pComputeShader);

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);

            uiRemoveComponent(pGuiWindow);
            unloadProfilerUI();
        }
    }

    void Update(float deltaTime) override
    {
        PROFILER_SET_CPU_SCOPE("Cpu Profile", "update", 0x222222);

        if (!uiIsFocused())
        {
            if (inputGetValue(0, CUSTOM_TOGGLE_FULLSCREEN))
            {
                toggleFullscreen(pWindow);
            }
            if (inputGetValue(0, CUSTOM_TOGGLE_UI))
            {
                uiToggleActive();
            }
            if (inputGetValue(0, CUSTOM_DUMP_PROFILE))
            {
                dumpProfileData(GetName());
            }
            if (inputGetValue(0, CUSTOM_EXIT))
            {
                requestShutdown();
            }
        }

        runComputeTest();
        runCPUComputeTest();
    }

    void Draw() override
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        PROFILER_SET_CPU_SCOPE("Cpu Profile", "draw", 0xffffff);

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        GpuCmdRingElement elem = getNextGpuCmdRingElement(&mCmdRing, true, 1);
        FenceStatus       fenceStatus = {};
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);

        resetCmdPool(pRenderer, elem.pCmdPool);

        Cmd* pCmd = elem.pCmds[0];
        beginCmd(pCmd);

        RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];

        BindRenderTargetsDesc bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
        cmdBindRenderTargets(pCmd, &bindRenderTargets);
        cmdSetViewport(pCmd, 0.0f, 0.0f, (float)mSettings.mWidth, (float)mSettings.mHeight, 0.0f, 1.0f);
        cmdSetScissor(pCmd, 0, 0, mSettings.mWidth, mSettings.mHeight);

        cmdBeginDebugMarker(pCmd, 0, 1, 0, "Draw UI");

        FontDrawDesc frameTimeDraw;
        frameTimeDraw.mFontColor = 0xff0080ff;
        frameTimeDraw.mFontSize = 18.0f;
        frameTimeDraw.mFontID = gFontID;
        cmdDrawCpuProfile(pCmd, float2(8.0f, 15.0f), &frameTimeDraw);

        cmdDrawUserInterface(pCmd);
        cmdBindRenderTargets(pCmd, NULL);
        cmdEndDebugMarker(pCmd);

        RenderTargetBarrier presentBarrier = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
        cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, &presentBarrier);

        endCmd(pCmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.mSignalSemaphoreCount = 1;
        submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
        submitDesc.ppCmds = &pCmd;
        submitDesc.ppSignalSemaphores = &elem.pSemaphore;
        submitDesc.ppWaitSemaphores = waitSemaphores;
        submitDesc.pSignalFence = elem.pFence;
        queueSubmit(pQueue, &submitDesc);
        QueuePresentDesc presentDesc = {};
        presentDesc.mIndex = (uint8_t)swapchainImageIndex;
        presentDesc.mWaitSemaphoreCount = 1;
        presentDesc.ppWaitSemaphores = &elem.pSemaphore;
        presentDesc.pSwapChain = pSwapChain;
        presentDesc.mSubmitDone = true;
        queuePresent(pQueue, &presentDesc);
        flipProfiler();
    }

    const char* GetName() override { return "40_CPUComputeShaders"; }

private:
    void createShaders()
    {
        ShaderLoadDesc computeShader = {};
        computeShader.mComp.pFileName = "test.comp";
        addShader(pRenderer, &computeShader, &pComputeShader);
    }

    void createRootSignature()
    {
        RootSignatureDesc rootDesc = {};
        rootDesc.mShaderCount = 1;
        rootDesc.ppShaders = &pComputeShader;
        addRootSignature(pRenderer, &rootDesc, &pRootSignature);
    }

    void createDescriptorSet()
    {
        DescriptorSetDesc setDesc = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
        addDescriptorSet(pRenderer, &setDesc, &pDescriptorSet);
    }

    void createPipeline()
    {
        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_COMPUTE;
        ComputePipelineDesc& computeDesc = desc.mComputeDesc;
        computeDesc.pRootSignature = pRootSignature;
        computeDesc.pShaderProgram = pComputeShader;
        addPipeline(pRenderer, &desc, &pComputePipeline);
    }

    void updateDescriptors()
    {
        DescriptorData params[2] = {};
        params[0].pName = "gOutput";
        params[0].ppBuffers = &pOutputBuffer;
        params[1].pName = "gSettings";  
        params[1].ppBuffers = &pUniformBuffer;
        updateDescriptorSet(pRenderer, 0, pDescriptorSet, 2, params);
    }
    
    void runComputeTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "GPU", 0x222222);
        resetCmdPool(pRenderer, pCmdPool);
        beginCmd(pCmd);

        cmdBindPipeline(pCmd, pComputePipeline);
        cmdBindDescriptorSet(pCmd, 0, pDescriptorSet);

        uint32_t groupSizeX = (COMPUTE_TEST_WIDTH + 15) / 16;  // Assuming 16x16 thread groups
        uint32_t groupSizeY = (COMPUTE_TEST_HEIGHT + 15) / 16;
        cmdDispatch(pCmd, groupSizeX, groupSizeY, 1);

        endCmd(pCmd);

        // Submit work
        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.ppCmds = &pCmd;
        submitDesc.mSubmitDone = true;
        queueSubmit(pQueue, &submitDesc);

        // Wait for work to complete
        waitQueueIdle(pQueue);

        // Print results 
        float* outputData = (float*)pOutputBuffer->pCpuMappedAddress;
        LOGF(LogLevel::eINFO, "Compute Test Results:");
        for (uint32_t i = 0; i < 10 && i < COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT; i++) {
            LOGF(LogLevel::eINFO, "Output[%u] = %f", i, outputData[i]);
        }
    }

    void runCPUComputeTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "CPU", 0x222222);
        uint32_t groupSizeX = (COMPUTE_TEST_WIDTH + 15) / 16;  // Assuming 16x16 thread groups
        uint32_t groupSizeY = (COMPUTE_TEST_HEIGHT + 15) / 16;
        const uint32_t numElements = COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT;
        float *outputBuffer = (float*)malloc(sizeof(float) * numElements);

        const ispc::ComputeTestData settings = { 
            .width = COMPUTE_TEST_WIDTH,
            .height = COMPUTE_TEST_HEIGHT
            };
        // settings.width = COMPUTE_TEST_WIDTH;
        // settings.height = COMPUTE_TEST_HEIGHT;

        ispc::CS_MAIN(outputBuffer, settings, groupSizeX, groupSizeY, 1);
        

        LOGF(LogLevel::eINFO, "CPU Compute Test Results:");
        for (uint32_t i = 0; i < 10 && i < COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT; i++) {
            LOGF(LogLevel::eINFO, "CPU Output[%u] = %f", i, outputBuffer[i]);
        }
        free(outputBuffer);
    }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mColorClearValue = {};
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        swapChainDesc.ppPresentQueues = &pQueue;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
        swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

        return pSwapChain != NULL;
    }

    static const uint32_t gDataBufferCount = 2;
    
    Renderer* pRenderer = NULL;
    Queue* pQueue = NULL;
    GpuCmdRing mCmdRing = {};
    CmdPool* pCmdPool = NULL;
    Cmd* pCmd = NULL;
    SwapChain* pSwapChain = NULL;
    Buffer* pOutputBuffer = NULL;
    Buffer* pUniformBuffer = NULL;
    Shader* pComputeShader = NULL;
    RootSignature* pRootSignature = NULL;
    DescriptorSet* pDescriptorSet = NULL;
    Pipeline* pComputePipeline = NULL;
    Semaphore* pImageAcquiredSemaphore = NULL;
    UIComponent* pGuiWindow = NULL;
};

DEFINE_APPLICATION_MAIN(CPUComputeTest)