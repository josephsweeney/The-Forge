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
#include "ISPC/distfield_init.comp.h"
#include "ISPC/distfield_flood.comp.h"
// NOTE(joesweeney): Right now these share the same namespace and define global variables to match shader semantics
// within the ISPC code. This means they can't have any name collisions for resources and for their entry function name.
// This is handled in the generator, but something to keep in mind when adding new shaders here if you get linker errors.

static const uint32_t COMPUTE_TEST_WIDTH = 512;
static const uint32_t COMPUTE_TEST_HEIGHT = 512;

ispc::DistanceFieldParams gDistfieldParams = {
    .width = COMPUTE_TEST_WIDTH,
    .height = COMPUTE_TEST_HEIGHT,
    .threshold = 0.5f,
};

ProfileToken gCpuComputeToken;
ProfileToken gGpuComputeToken;

uint32_t gFontID = 0;
uint64_t gTestCounter = 0;
bool gRunTests = true;
uint32_t gDistfieldFloodPushConstantIndex = 5;

class CPUComputeTest: public IApp 
{
public:
    bool Init() override
    {
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

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_COMPUTE;
        initQueue(pRenderer, &queueDesc, &pQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &mCmdRing);

        CmdPoolDesc cmdPoolDesc = {};
        cmdPoolDesc.pQueue = pQueue;
        initCmdPool(pRenderer, &cmdPoolDesc, &pCmdPool);

        CmdDesc cmdDesc = {};
        cmdDesc.pPool = pCmdPool;
        initCmd(pRenderer, &cmdDesc, &pCmd);

        initResourceLoaderInterface(pRenderer);

        const uint32_t numElements = COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT;

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

        BufferLoadDesc uniformDesc = {};
        uniformDesc.ppBuffer = &pUniformBuffer;
        uniformDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        uniformDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        uniformDesc.mDesc.mSize = sizeof(ispc::ComputeTestData); 
        addResource(&uniformDesc, &token);

        ispc::ComputeTestData* data = (ispc::ComputeTestData*)pUniformBuffer->pCpuMappedAddress;
        data->width = COMPUTE_TEST_WIDTH;
        data->height = COMPUTE_TEST_HEIGHT;
    
        BufferLoadDesc distfieldInputDesc = {};
        distfieldInputDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
        distfieldInputDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        distfieldInputDesc.mDesc.mSize = sizeof(float) * numElements;
        distfieldInputDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        distfieldInputDesc.mDesc.mElementCount = numElements;
        distfieldInputDesc.mDesc.mStructStride = sizeof(float);
        distfieldInputDesc.ppBuffer = &pDistfieldBufferInput;
        pDistfieldGpuInputBuffer = (float*)malloc(sizeof(float) * numElements);
        initializeDistanceFieldInput(pDistfieldGpuInputBuffer, COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT);
        distfieldInputDesc.pData = pDistfieldGpuInputBuffer;
        addResource(&distfieldInputDesc, &token);

        BufferLoadDesc distfieldOutputDesc = {};
        distfieldOutputDesc.ppBuffer = &pDistfieldBufferOutput;
        distfieldOutputDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER;
        distfieldOutputDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_TO_CPU;
        distfieldOutputDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        distfieldOutputDesc.mDesc.mSize = sizeof(float) * numElements;
        distfieldOutputDesc.mDesc.mElementCount = numElements;
        distfieldOutputDesc.mDesc.mStructStride = sizeof(float);
        addResource(&distfieldOutputDesc, &token);

        for (int i = 0; i < 2; ++i) 
        {
            BufferLoadDesc distfieldSeedDesc = {};
            distfieldSeedDesc.ppBuffer = &pDistfieldBufferSeed[i];
            distfieldSeedDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER;
            distfieldSeedDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_TO_CPU;
            distfieldSeedDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
            distfieldSeedDesc.mDesc.mSize = sizeof(int32_t) * numElements * 2;
            distfieldSeedDesc.mDesc.mElementCount = numElements;
            distfieldSeedDesc.mDesc.mStructStride = sizeof(int32_t);
            addResource(&distfieldSeedDesc, &token);
        }

        BufferLoadDesc paramsDesc = {};
        paramsDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        paramsDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        paramsDesc.mDesc.mSize = sizeof(ispc::DistanceFieldParams);
        paramsDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        paramsDesc.ppBuffer = &pDistfieldBufferParams;
        
        paramsDesc.pData = &gDistfieldParams;
        addResource(&paramsDesc, &token);

        pBasicOutputBuffer = (float*)malloc(sizeof(float) * numElements);
        pDistfieldCpuInputBuffer = (float*)malloc(sizeof(float) * numElements);
        initializeDistanceFieldInput(pDistfieldCpuInputBuffer, COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT);
        pDistfieldCpuOutputBuffer = (float*)malloc(sizeof(float) * numElements);
        pDistfieldCpuSeedBuffer[0] = (int32_t*)malloc(sizeof(int32_t) * numElements * 2);
        pDistfieldCpuSeedBuffer[1] = (int32_t*)malloc(sizeof(int32_t) * numElements * 2);

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);
        initDebugTextures(&token);

        FontDesc font = {};
        font.pFontPath = "TitilliumText/TitilliumText-Bold.otf";
        fntDefineFonts(&font, 1, &gFontID);

        FontSystemDesc fontRenderDesc = {};
        fontRenderDesc.pRenderer = pRenderer;
        if (!initFontSystem(&fontRenderDesc))
            return false;

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
        removeResource(pDistfieldBufferInput);
        removeResource(pDistfieldBufferOutput);
        removeResource(pDistfieldBufferParams);
        removeResource(pDistfieldBufferSeed[0]);
        removeResource(pDistfieldBufferSeed[1]);
        free(pBasicOutputBuffer);
        free(pDistfieldGpuInputBuffer);
        free(pDistfieldCpuInputBuffer);
        free(pDistfieldCpuOutputBuffer);
        free(pDistfieldCpuSeedBuffer[0]);
        free(pDistfieldCpuSeedBuffer[1]);
        exitDebugTextures();

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
        toggleProfilerUI(true);
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

        SetupDebugWindow();

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
        removeShader(pRenderer, pDistFieldInitShader);
        removeShader(pRenderer, pDistFieldFloodShader);
        removeRootSignature(pRenderer, pDistFieldInitRootSignature);
        removeRootSignature(pRenderer, pDistFieldFloodRootSignature);
        removeDescriptorSet(pRenderer, pDistFieldInitDescriptorSet);
        removeDescriptorSet(pRenderer, pDistFieldFloodDescriptorSet[0]);
        removeDescriptorSet(pRenderer, pDistFieldFloodDescriptorSet[1]);
        removePipeline(pRenderer, pDistFieldInitPipeline);
        removePipeline(pRenderer, pDistFieldFloodPipeline);

        if (pDebugWindow)
        {
            uiRemoveComponent(pDebugWindow);
            pDebugWindow = NULL;
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);
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

        if (gTestCounter++ % 60 == 0 && gRunTests)
        {
            runComputeTest();
            runCPUComputeTest();
            checkBasicTestOutputs();
            runDistanceFieldTest();
            runCPUDistanceFieldTest();
            checkDistanceFieldOutputs();

            loadTextureFromFloatBuffer(pBasicCpuOutput, pBasicOutputBuffer);
            loadTextureFromFloatBuffer(pBasicGpuOutput, (float*)pOutputBuffer->pCpuMappedAddress);
            loadTextureFromFloatBuffer(pDistfieldCpuInput, pDistfieldCpuInputBuffer);
            loadTextureFromFloatBuffer(pDistfieldGpuInput, (float*)pDistfieldBufferInput->pCpuMappedAddress);
            loadTextureFromDistanceFieldHeatMap(pDistfieldCpuOutput, pDistfieldCpuOutputBuffer);
            loadTextureFromDistanceFieldHeatMap(pDistfieldGpuOutput, (float*)pDistfieldBufferOutput->pCpuMappedAddress);
        }
        
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
        ShaderLoadDesc distFieldInitShader = {};
        distFieldInitShader.mComp.pFileName = "distfield_init.comp";
        addShader(pRenderer, &distFieldInitShader, &pDistFieldInitShader);
        ShaderLoadDesc distFieldFloodShader = {};
        distFieldFloodShader.mComp.pFileName = "distfield_flood.comp";
        addShader(pRenderer, &distFieldFloodShader, &pDistFieldFloodShader);
    }

    void createRootSignature()
    {
        RootSignatureDesc rootDesc = {};
        rootDesc.mShaderCount = 1;
        rootDesc.ppShaders = &pComputeShader;
        addRootSignature(pRenderer, &rootDesc, &pRootSignature);
        RootSignatureDesc distFieldRootDesc = {};
        distFieldRootDesc.mShaderCount = 1;
        distFieldRootDesc.ppShaders = &pDistFieldInitShader;
        addRootSignature(pRenderer, &distFieldRootDesc, &pDistFieldInitRootSignature);
        RootSignatureDesc distFieldFloodRootDesc = {};
        distFieldFloodRootDesc.mShaderCount = 1;
        distFieldFloodRootDesc.ppShaders = &pDistFieldFloodShader;
        addRootSignature(pRenderer, &distFieldFloodRootDesc, &pDistFieldFloodRootSignature);
    }

    void createDescriptorSet()
    {
        DescriptorSetDesc setDesc = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
        addDescriptorSet(pRenderer, &setDesc, &pDescriptorSet);

        DescriptorSetDesc distFieldInitSetDesc = { pDistFieldInitRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
        addDescriptorSet(pRenderer, &distFieldInitSetDesc, &pDistFieldInitDescriptorSet);

        DescriptorSetDesc distFieldFloodSetDesc = { pDistFieldFloodRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
        addDescriptorSet(pRenderer, &distFieldFloodSetDesc, &pDistFieldFloodDescriptorSet[0]);
        addDescriptorSet(pRenderer, &distFieldFloodSetDesc, &pDistFieldFloodDescriptorSet[1]);
    }

    void createPipeline()
    {
        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_COMPUTE;
        ComputePipelineDesc& computeDesc = desc.mComputeDesc;
        computeDesc.pRootSignature = pRootSignature;
        computeDesc.pShaderProgram = pComputeShader;
        addPipeline(pRenderer, &desc, &pComputePipeline);

        PipelineDesc distfieldInitDesc = {};
        distfieldInitDesc.mType = PIPELINE_TYPE_COMPUTE;
        ComputePipelineDesc& distFieldInitComputeDesc = distfieldInitDesc.mComputeDesc;
        distFieldInitComputeDesc.pRootSignature = pDistFieldInitRootSignature;
        distFieldInitComputeDesc.pShaderProgram = pDistFieldInitShader;
        addPipeline(pRenderer, &distfieldInitDesc, &pDistFieldInitPipeline);

        PipelineDesc distfieldFloodDesc = {};
        distfieldFloodDesc.mType = PIPELINE_TYPE_COMPUTE;
        ComputePipelineDesc& distfieldFloodComputeDesc = distfieldFloodDesc.mComputeDesc;
        distfieldFloodComputeDesc.pRootSignature = pDistFieldFloodRootSignature;
        distfieldFloodComputeDesc.pShaderProgram = pDistFieldFloodShader;
        addPipeline(pRenderer, &distfieldFloodDesc, &pDistFieldFloodPipeline);
    }

    void updateDescriptors()
    {
        DescriptorData params[2] = {};
        params[0].pName = "gOutput";
        params[0].ppBuffers = &pOutputBuffer;
        params[1].pName = "gSettings";  
        params[1].ppBuffers = &pUniformBuffer;
        updateDescriptorSet(pRenderer, 0, pDescriptorSet, 2, params);
        DescriptorData distfieldParams[3] = {};
        distfieldParams[0].pName = "gInputBuffer";
        distfieldParams[0].ppBuffers = &pDistfieldBufferInput;
        distfieldParams[1].pName = "gSeedBuffer";
        distfieldParams[1].ppBuffers = &pDistfieldBufferSeed[0];
        distfieldParams[2].pName = "gParams";
        distfieldParams[2].ppBuffers = &pDistfieldBufferParams;
        updateDescriptorSet(pRenderer, 0, pDistFieldInitDescriptorSet, 3, distfieldParams);
        for (int i = 0; i < 2; ++i)
        {
            DescriptorData distfieldFloodParams[5] = {};
            distfieldFloodParams[0].pName = "gFloodInputBuffer";
            distfieldFloodParams[0].ppBuffers = &pDistfieldBufferInput;
            distfieldFloodParams[1].pName = "gFloodOutputBuffer";
            distfieldFloodParams[1].ppBuffers = &pDistfieldBufferOutput;
            distfieldFloodParams[2].pName = "gFloodSeedBufferIn";
            distfieldFloodParams[2].ppBuffers = &pDistfieldBufferSeed[i];
            distfieldFloodParams[3].pName = "gFloodSeedBufferOut";
            distfieldFloodParams[3].ppBuffers = &pDistfieldBufferSeed[1-i];
            distfieldFloodParams[4].pName = "gFloodParams";
            distfieldFloodParams[4].ppBuffers = &pDistfieldBufferParams;
            updateDescriptorSet(pRenderer, 0, pDistFieldFloodDescriptorSet[i], 5, distfieldFloodParams);
        }
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

    void initDebugTextures(SyncToken* token)
    {
        TextureDesc texDesc = {};
        texDesc.mWidth = COMPUTE_TEST_WIDTH;
        texDesc.mHeight = COMPUTE_TEST_HEIGHT;
        texDesc.mDepth = 1;
        texDesc.mArraySize = 1;
        texDesc.mMipLevels = 1;
        texDesc.mSampleCount = SAMPLE_COUNT_1;
        texDesc.mFormat = TinyImageFormat_R32_SFLOAT;
        texDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
        texDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        TextureLoadDesc basicCpuDesc = {};
        basicCpuDesc.ppTexture = &pBasicCpuOutput;
        basicCpuDesc.pDesc = &texDesc;
        addResource(&basicCpuDesc, token);
        TextureLoadDesc basicGpuDesc = {};
        basicGpuDesc.ppTexture = &pBasicGpuOutput;
        basicGpuDesc.pDesc = &texDesc;
        addResource(&basicGpuDesc, token);


        TextureLoadDesc distfieldCpuDesc = {};
        distfieldCpuDesc.ppTexture = &pDistfieldCpuInput;
        distfieldCpuDesc.pDesc = &texDesc;
        addResource(&distfieldCpuDesc, token);
        TextureLoadDesc distfieldGpuDesc = {};
        distfieldGpuDesc.ppTexture = &pDistfieldGpuInput;
        distfieldGpuDesc.pDesc = &texDesc;
        addResource(&distfieldGpuDesc, token);

        TextureDesc distfieldHeatmapDesc = {};
        distfieldHeatmapDesc.mWidth = COMPUTE_TEST_WIDTH;
        distfieldHeatmapDesc.mHeight = COMPUTE_TEST_HEIGHT;
        distfieldHeatmapDesc.mDepth = 1;
        distfieldHeatmapDesc.mArraySize = 1;
        distfieldHeatmapDesc.mMipLevels = 1;
        distfieldHeatmapDesc.mSampleCount = SAMPLE_COUNT_1;
        distfieldHeatmapDesc.mFormat = TinyImageFormat_R8G8B8A8_UNORM;
        distfieldHeatmapDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
        distfieldHeatmapDesc.mStartState = RESOURCE_STATE_SHADER_RESOURCE;
        TextureLoadDesc distfieldCpuOutDesc = {};
        distfieldCpuOutDesc.ppTexture = &pDistfieldCpuOutput;
        distfieldCpuOutDesc.pDesc = &distfieldHeatmapDesc;
        addResource(&distfieldCpuOutDesc, token);
        TextureLoadDesc distfieldGpuOutDesc = {};
        distfieldGpuOutDesc.ppTexture = &pDistfieldGpuOutput;
        distfieldGpuOutDesc.pDesc = &distfieldHeatmapDesc;
        addResource(&distfieldGpuOutDesc, token);
    }

    void exitDebugTextures()
    {
        removeResource(pBasicCpuOutput);
        removeResource(pBasicGpuOutput);
        removeResource(pDistfieldCpuInput);
        removeResource(pDistfieldGpuInput);
        removeResource(pDistfieldCpuOutput);
        removeResource(pDistfieldGpuOutput);
    }

    void loadTextureFromFloatBuffer(Texture* pTexture, float* pBuffer) 
    {
        int width = COMPUTE_TEST_WIDTH;
        int height = COMPUTE_TEST_HEIGHT;
        TextureUpdateDesc updateDesc = {};
        updateDesc.pTexture = pTexture;
        updateDesc.mBaseMipLevel = 0;
        updateDesc.mMipLevels = 1;
        updateDesc.mBaseArrayLayer = 0;
        updateDesc.mLayerCount = 1;

        beginUpdateResource(&updateDesc);
        TextureSubresourceUpdate subresDesc = updateDesc.getSubresourceUpdateDesc(0, 0);
        memcpy((uint8_t*)subresDesc.pMappedData, (uint8_t*)pBuffer, width * height * sizeof(float));
        endUpdateResource(&updateDesc);
    }

    void loadTextureFromDistanceFieldHeatMap(Texture* pTexture, float* pBuffer) {
        int width = COMPUTE_TEST_WIDTH;
        int height = COMPUTE_TEST_HEIGHT;
        float maxDistance = 0;
        for (uint32_t i = 0; i < width * height; i++) {
            float dist = abs(pBuffer[i]);
            if (dist > maxDistance) {
                maxDistance = dist;
            }
        }
        uint32_t* colorBuffer = (uint32_t*)malloc(width * height * sizeof(uint32_t));
        for (uint32_t i = 0; i < width * height; i++) {
            float normalized = (pBuffer[i] + maxDistance) / (2.0f * maxDistance);
            normalized = normalized < 0.0f ? 0.0f : (normalized > 1.0f ? 1.0f : normalized);
            normalized = normalized * normalized;
            uint8_t r = (uint8_t)(normalized * 255.0f);
            uint8_t b = (uint8_t)((1.0f - normalized) * 255.0f);
            uint8_t g = 0;
            uint8_t a = 255;

            colorBuffer[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        TextureUpdateDesc updateDesc = {};
        updateDesc.pTexture = pTexture;
        updateDesc.mBaseMipLevel = 0;
        updateDesc.mMipLevels = 1;
        updateDesc.mBaseArrayLayer = 0;
        updateDesc.mLayerCount = 1;

        beginUpdateResource(&updateDesc);
        TextureSubresourceUpdate subresDesc = updateDesc.getSubresourceUpdateDesc(0, 0);
        memcpy((uint8_t*)subresDesc.pMappedData, (uint8_t*)colorBuffer, width * height * sizeof(uint32_t));
        endUpdateResource(&updateDesc);
        free(colorBuffer);
    }
    
    void SetupDebugWindow()
    {
        float  scale = 1.0f;
        float2 screenSize = { (float)COMPUTE_TEST_WIDTH, (float)COMPUTE_TEST_HEIGHT };
        float2 texSize = screenSize * scale;

        if (!pDebugWindow)
        {
            UIComponentDesc UIComponentDesc = {};
            UIComponentDesc.mStartSize = vec2(COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT);
            UIComponentDesc.mStartPosition.setY(mSettings.mHeight * 0.1f);
            uiAddComponent("DEBUG Compute Outputs", &UIComponentDesc, &pDebugWindow);

            LabelWidget label = {};
            uiAddComponentWidget(pDebugWindow, "CPU Basic Output", &label, WIDGET_TYPE_LABEL);
            DebugTexturesWidget widget;
            widget.pTextures = &pBasicCpuOutput;
            widget.mTexturesCount = 1;
            widget.mTextureDisplaySize = texSize;
            // Basic Test Outputs
            uiAddComponentWidget(pDebugWindow, "CPU Basic Output", &widget, WIDGET_TYPE_DEBUG_TEXTURES);
            VerticalSeparatorWidget verticalSeperator = { 1 };
            uiAddComponentWidget(pDebugWindow, "Vertical separator", &verticalSeperator, WIDGET_TYPE_VERTICAL_SEPARATOR);
            uiAddComponentWidget(pDebugWindow, "GPU Basic Output", &label, WIDGET_TYPE_LABEL);
            widget.pTextures = &pBasicGpuOutput;
            uiAddComponentWidget(pDebugWindow, "GPU Basic Input", &widget, WIDGET_TYPE_DEBUG_TEXTURES);
            // Distance Field Inputs
            uiAddComponentWidget(pDebugWindow, "Vertical separator", &verticalSeperator, WIDGET_TYPE_VERTICAL_SEPARATOR);
            uiAddComponentWidget(pDebugWindow, "CPU Distfield Input", &label, WIDGET_TYPE_LABEL);
            widget.pTextures = &pDistfieldCpuInput;
            uiAddComponentWidget(pDebugWindow, "CPU Distfield Input", &widget, WIDGET_TYPE_DEBUG_TEXTURES);
            uiAddComponentWidget(pDebugWindow, "Vertical separator", &verticalSeperator, WIDGET_TYPE_VERTICAL_SEPARATOR);
            uiAddComponentWidget(pDebugWindow, "GPU Distfield Input", &label, WIDGET_TYPE_LABEL);
            widget.pTextures = &pDistfieldGpuInput;
            uiAddComponentWidget(pDebugWindow, "GPU Distfield Input", &widget, WIDGET_TYPE_DEBUG_TEXTURES);
            // Distance Field Outputs
            uiAddComponentWidget(pDebugWindow, "Vertical separator", &verticalSeperator, WIDGET_TYPE_VERTICAL_SEPARATOR);
            uiAddComponentWidget(pDebugWindow, "CPU Distfield Output", &label, WIDGET_TYPE_LABEL);
            widget.pTextures = &pDistfieldCpuOutput;
            uiAddComponentWidget(pDebugWindow, "CPU Distfield Output", &widget, WIDGET_TYPE_DEBUG_TEXTURES);
            uiAddComponentWidget(pDebugWindow, "Vertical separator", &verticalSeperator, WIDGET_TYPE_VERTICAL_SEPARATOR);
            uiAddComponentWidget(pDebugWindow, "GPU Distfield Output", &label, WIDGET_TYPE_LABEL);
            widget.pTextures = &pDistfieldGpuOutput;
            uiAddComponentWidget(pDebugWindow, "GPU Distfield Output", &widget, WIDGET_TYPE_DEBUG_TEXTURES);

            uiSetComponentActive(pDebugWindow, true);
        }
    }

    void runComputeTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "GPU Basic", 0x222222);
        resetCmdPool(pRenderer, pCmdPool);
        beginCmd(pCmd);

        cmdBindPipeline(pCmd, pComputePipeline);
        cmdBindDescriptorSet(pCmd, 0, pDescriptorSet);

        uint32_t groupSizeX = (COMPUTE_TEST_WIDTH + 15) / 16;
        uint32_t groupSizeY = (COMPUTE_TEST_HEIGHT + 15) / 16;
        cmdDispatch(pCmd, groupSizeX, groupSizeY, 1);

        endCmd(pCmd);

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.ppCmds = &pCmd;
        submitDesc.mSubmitDone = true;
        queueSubmit(pQueue, &submitDesc);

        waitQueueIdle(pQueue);
    }
    
    void runCPUComputeTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "CPU Basic", 0x222222);
        const ispc::ComputeTestData settings = { 
            .width = COMPUTE_TEST_WIDTH,
            .height = COMPUTE_TEST_HEIGHT
        };
        ispc::CS_MAIN_TEST(pBasicOutputBuffer, settings, COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT, 1);
    }

    void checkBasicTestOutputs()
    {
        float* gpuOutputData = (float*)pOutputBuffer->pCpuMappedAddress;
        LOGF(LogLevel::eINFO, "Check Basic Compute Test Results:");
        for (uint32_t i = 0; i < COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT; ++i) {
            if (pBasicOutputBuffer[i] != gpuOutputData[i]) {

                LOGF(LogLevel::eERROR, "MISMATCH AT [%d] = CPU:%f|GPU:%f", i, pBasicOutputBuffer[i], gpuOutputData[i]);
                gRunTests = false;
            }
        }
        LOGF(LogLevel::eINFO, "Basic Compute Test: PASSED");
    }

    void initializeDistanceFieldInput(float* inputBuffer, uint32_t width, uint32_t height) 
    {
        for (uint32_t i = 0; i < width * height; i++) 
        {
            inputBuffer[i] = 1.0f;
        }

        // Create a cross/plus shape in black (0.0)
        uint32_t centerX = width / 2;
        uint32_t centerY = height / 2;
        uint32_t armWidth = width / 8; 
        uint32_t armLength = height / 3;

        // Draw horizontal arm
        for (uint32_t y = centerY - armWidth/2; y < centerY + armWidth/2; y++) 
        {
            for (uint32_t x = centerX - armLength; x < centerX + armLength; x++) 
            {
                if (x < width && y < height) {
                    inputBuffer[y * width + x] = 0.0f;
                }
            }
        }

        // Draw vertical arm
        for (uint32_t y = centerY - armLength; y < centerY + armLength; y++) 
        {
            for (uint32_t x = centerX - armWidth/2; x < centerX + armWidth/2; x++) 
            {
                if (x < width && y < height) {
                    inputBuffer[y * width + x] = 0.0f;
                }
            }
        }
    }

    void runDistanceFieldTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "GPU Distance Field ", 0x222222);

        beginCmd(pCmd);
        
        cmdBindPipeline(pCmd, pDistFieldInitPipeline);
        cmdBindDescriptorSet(pCmd, 0, pDistFieldInitDescriptorSet);
        uint32_t groupSizeX = (COMPUTE_TEST_WIDTH + 15) / 16;
        uint32_t groupSizeY = (COMPUTE_TEST_HEIGHT + 15) / 16;
        cmdDispatch(pCmd, groupSizeX, groupSizeY, 1);

        BufferBarrier distfieldBarrier[5] = {};
        distfieldBarrier[0].pBuffer = pDistfieldBufferInput;
        distfieldBarrier[0].mCurrentState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[0].mNewState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[1].pBuffer = pDistfieldBufferOutput;
        distfieldBarrier[1].mCurrentState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[1].mNewState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[2].pBuffer = pDistfieldBufferSeed[0];
        distfieldBarrier[2].mCurrentState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[2].mNewState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[3].pBuffer = pDistfieldBufferSeed[1];
        distfieldBarrier[3].mCurrentState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[3].mNewState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[4].pBuffer = pDistfieldBufferParams;
        distfieldBarrier[4].mCurrentState = RESOURCE_STATE_UNORDERED_ACCESS;
        distfieldBarrier[4].mNewState = RESOURCE_STATE_UNORDERED_ACCESS;

        int currentSeedBuffer = 0;
        for (int step = 8; step >= 0; --step) {
            cmdResourceBarrier(pCmd, 5, distfieldBarrier, 0, NULL, 0, NULL);
            ispc::RootConstantData stepData = { (uint)step };
            cmdBindPushConstants(pCmd, pDistFieldFloodRootSignature, gDistfieldFloodPushConstantIndex, &stepData);
            cmdBindPipeline(pCmd, pDistFieldFloodPipeline);
            cmdBindDescriptorSet(pCmd, 0, pDistFieldFloodDescriptorSet[currentSeedBuffer]);
            cmdDispatch(pCmd, groupSizeX, groupSizeY, 1);
            currentSeedBuffer = 1 - currentSeedBuffer;
        }
        
        endCmd(pCmd);
        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.ppCmds = &pCmd;
        submitDesc.mSubmitDone = true;
        queueSubmit(pQueue, &submitDesc);
        waitQueueIdle(pQueue);
    }

    void runCPUDistanceFieldTest()
    {
        PROFILER_SET_CPU_SCOPE("Tests", "CPU Distance Field", 0x222222);

        ispc::CS_MAIN_DISTFIELD_INIT(pDistfieldCpuInputBuffer, pDistfieldCpuSeedBuffer[0], gDistfieldParams, COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT, 1);
        int currentSeedBuffer = 0;
        for (int step = 8; step >= 0; --step) {
            ispc::RootConstantData stepData = { (uint)step };
            ispc::CS_MAIN_DISTFIELD_FLOOD(pDistfieldCpuInputBuffer, pDistfieldCpuOutputBuffer, pDistfieldCpuSeedBuffer[currentSeedBuffer], pDistfieldCpuSeedBuffer[1-currentSeedBuffer], gDistfieldParams, stepData, COMPUTE_TEST_WIDTH, COMPUTE_TEST_HEIGHT, 1);
            currentSeedBuffer = 1 - currentSeedBuffer;
        }
    }

    void checkDistanceFieldOutputs()
    {
        float threshold = 0.001f;
        float* gpuOutputData = (float*)pDistfieldBufferOutput->pCpuMappedAddress;
        LOGF(LogLevel::eINFO, "Check Distance Field Compute Test Results:");
        for (uint32_t i = 0; i < COMPUTE_TEST_WIDTH * COMPUTE_TEST_HEIGHT; ++i) {
            float error = abs(pDistfieldCpuOutputBuffer[i] - gpuOutputData[i]);
            if (error > threshold) {
                LOGF(LogLevel::eERROR, "MISMATCH AT [%d] = CPU:%f|GPU:%f", i, pDistfieldCpuOutputBuffer[i], gpuOutputData[i]);
                gRunTests = false;
            }
        }
        LOGF(LogLevel::eINFO, "Distance Field Compute Test: PASSED");
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
    Buffer* pDistfieldBufferInput = NULL;
    Buffer* pDistfieldBufferOutput = NULL;
    Buffer* pDistfieldBufferParams = NULL;
    Buffer* pDistfieldBufferSeed[2] = { NULL, NULL };
    Shader* pComputeShader = NULL;
    Shader* pDistFieldInitShader = NULL;
    Shader* pDistFieldFloodShader = NULL;

    RootSignature* pRootSignature = NULL;
    RootSignature* pDistFieldInitRootSignature = NULL;
    RootSignature* pDistFieldFloodRootSignature = NULL;
    DescriptorSet* pDescriptorSet = NULL;
    DescriptorSet* pDistFieldInitDescriptorSet = NULL;
    DescriptorSet* pDistFieldFloodDescriptorSet[2] = { NULL, NULL };
    Pipeline* pComputePipeline = NULL;
    Pipeline* pDistFieldInitPipeline = NULL;
    Pipeline* pDistFieldFloodPipeline = NULL;
    Semaphore* pImageAcquiredSemaphore = NULL;
    UIComponent* pDebugWindow = NULL;

    float *pBasicOutputBuffer = NULL;
    float* pDistfieldCpuInputBuffer = NULL;
    float* pDistfieldCpuOutputBuffer = NULL;
    float* pDistfieldGpuInputBuffer = NULL;
    int32_t* pDistfieldCpuSeedBuffer[2] = { NULL, NULL };
    
    Texture* pBasicCpuOutput = NULL;
    Texture* pBasicGpuOutput = NULL;
    Texture* pDistfieldCpuInput = NULL;
    Texture* pDistfieldGpuInput = NULL;
    Texture* pDistfieldCpuOutput = NULL;
    Texture* pDistfieldGpuOutput = NULL;
};

DEFINE_APPLICATION_MAIN(CPUComputeTest)
