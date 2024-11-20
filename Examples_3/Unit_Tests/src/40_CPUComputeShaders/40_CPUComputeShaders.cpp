/*
 * Copyright (c) 2017-2024 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 */

#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "ISPC/test.comp.h"

// Test parameters 
static const uint32_t COMPUTE_TEST_WIDTH = 256;
static const uint32_t COMPUTE_TEST_HEIGHT = 256;

struct ComputeTestData {
    uint32_t width;
    uint32_t height;
};

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

        // Create compute queue...
        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_COMPUTE;
        initQueue(pRenderer, &queueDesc, &pQueue);

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

        return true;
    }

    void Exit() override
    {
        waitQueueIdle(pQueue);

        removeResource(pOutputBuffer);
        removeResource(pUniformBuffer);

        exitCmd(pRenderer, pCmd);
        exitCmdPool(pRenderer, pCmdPool);
        exitQueue(pRenderer, pQueue);
        exitResourceLoaderInterface(pRenderer);
        exitRenderer(pRenderer);
        exitGPUConfiguration();
    }

    bool Load(ReloadDesc* pReloadDesc) override 
    {
        createShaders();
        createRootSignature();
        createDescriptorSet();
        createPipeline();
        updateDescriptors();

        // Run the compute test immediately after resource creation
        runComputeTest();
        runCPUComputeTest();
        requestShutdown();
        return true;
    }

    void Unload(ReloadDesc* pReloadDesc) override
    {
        waitQueueIdle(pQueue);

        removePipeline(pRenderer, pComputePipeline);
        removeDescriptorSet(pRenderer, pDescriptorSet);
        removeRootSignature(pRenderer, pRootSignature);
        removeShader(pRenderer, pComputeShader);
    }

    void Update(float deltaTime) override {}
    void Draw() override {}

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

    // Core renderer objects
    Renderer* pRenderer = nullptr;
    Queue* pQueue = nullptr;
    CmdPool* pCmdPool = nullptr;
    Cmd* pCmd = nullptr;

    // Resources
    Buffer* pOutputBuffer = nullptr;
    Buffer* pUniformBuffer = nullptr;

    // Pipeline objects  
    Shader* pComputeShader = nullptr;
    RootSignature* pRootSignature = nullptr;
    DescriptorSet* pDescriptorSet = nullptr;
    Pipeline* pComputePipeline = nullptr;
};

DEFINE_APPLICATION_MAIN(CPUComputeTest)