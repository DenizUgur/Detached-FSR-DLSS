// This file is part of the FidelityFX SDK.
// 
// Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.


#include "fsr3upscalerendermodule.h"
#include "validation_remap.h"
#include "render/dynamicresourcepool.h"
#include "render/profiler.h"
#include "render/rasterview.h"
#include "render/uploadheap.h"
#include "core/scene.h"

#include <functional>

using namespace cauldron;

void FSR3UpscaleRenderModule::Init(const json& initData)
{
    // Fetch needed resources
    m_pColorTarget = GetFramework()->GetColorTargetForCallback(GetName());
    m_pDepthTarget = GetFramework()->GetRenderTexture(L"DepthTarget");

    // Create render resolution opaque render target to use for auto-reactive mask generation
    TextureDesc desc = m_pColorTarget->GetDesc();
    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
    desc.Width = resInfo.RenderWidth;
    desc.Height = resInfo.RenderHeight;
    desc.Name = L"FSR3Upscale_OpaqueTexture";
    m_pOpaqueTexture = GetDynamicResourcePool()->CreateRenderTexture(&desc, [](TextureDesc& desc, uint32_t displayWidth, uint32_t displayHeight, uint32_t renderingWidth, uint32_t renderingHeight)
        {
            desc.Width = renderingWidth;
            desc.Height = renderingHeight;
        });

    // Assumed resources, need to check they are there
    m_pMotionVectors = GetFramework()->GetRenderTexture(L"GBufferMotionVectorRT");
    m_pReactiveMask = GetFramework()->GetRenderTexture(L"ReactiveMask");
    m_pCompositionMask = GetFramework()->GetRenderTexture(L"TransCompMask");
    CauldronAssert(ASSERT_CRITICAL, m_pMotionVectors && m_pReactiveMask && m_pCompositionMask, L"Could not get one of the needed resources for FSR3Upscale Rendermodule.");

    // Create raster views on the reactive mask and composition masks (for clearing and rendering)
    m_RasterViews.resize(2);
    m_RasterViews[0] = GetRasterViewAllocator()->RequestRasterView(m_pReactiveMask, ViewDimension::Texture2D);
    m_RasterViews[1] = GetRasterViewAllocator()->RequestRasterView(m_pCompositionMask, ViewDimension::Texture2D);

    // Set our render resolution function as that to use during resize to get render width/height from display width/height
    m_pUpdateFunc = [this](uint32_t displayWidth, uint32_t displayHeight) { return this->UpdateResolution(displayWidth, displayHeight); };

    // UI
    InitUI();
    
    // Register additional execution callbacks during the frame

    // Register a post-lighting callback to copy opaque texture
    ExecuteCallback callbackPreTrans = [this](double deltaTime, CommandList* pCmdList) {
        this->PreTransCallback(deltaTime, pCmdList);
    };
    ExecutionTuple callbackPreTransTuple = std::make_pair(L"FSR3UpscaleRenderModule::PreTransCallback", std::make_pair(this, callbackPreTrans));
    GetFramework()->RegisterExecutionCallback(L"TranslucencyRenderModule", true, callbackPreTransTuple);

    // Register a post-transparency callback to generate reactive mask
    ExecuteCallback callbackPostTrans = [this](double deltaTime, CommandList* pCmdList) {
        this->PostTransCallback(deltaTime, pCmdList);
    };
    ExecutionTuple callbackPostTransTuple = std::make_pair(L"FSR3UpscaleRenderModule::PostTransCallback", std::make_pair(this, callbackPostTrans));
    GetFramework()->RegisterExecutionCallback(L"TranslucencyRenderModule", false, callbackPostTransTuple);

    //////////////////////////////////////////////////////////////////////////
    // Finish up init

    // Start disabled as this will be enabled externally
    SetModuleEnabled(false);

    // That's all we need for now
    SetModuleReady(true);
}

FSR3UpscaleRenderModule::~FSR3UpscaleRenderModule()
{
    // Protection
    if (ModuleEnabled())
        EnableModule(false);    // Destroy FSR context
}

void FSR3UpscaleRenderModule::EnableModule(bool enabled)
{
    // If disabling the render module, we need to disable the upscaler with the framework
    if (enabled)
    {
        // Setup everything needed when activating FSR
        // Will also enable upscaling
        UpdatePreset(nullptr);

        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);

        // Setup Cauldron FidelityFX interface.
        const size_t scratchBufferSize = ffxGetScratchMemorySize(FFX_FSR3_CONTEXT_COUNT);
        void*        scratchBuffer     = calloc(scratchBufferSize, 1);
        memset(scratchBuffer, 0, scratchBufferSize);
        FfxErrorCode errorCode         = ffxGetInterface(&m_InitializationParameters.backendInterfaceUpscaling, GetDevice(), scratchBuffer, scratchBufferSize, FFX_FSR3_CONTEXT_COUNT);
        CauldronAssert(ASSERT_CRITICAL, errorCode == FFX_OK, L"Could not initialize the FidelityFX SDK backend");

        // Create the FSR3 context
        UpdateFSR3Context(true);

        // ... and register UI elements for active upscaler
        GetUIManager()->RegisterUIElements(m_UISection);
    }
    else
    {
        // Toggle this now so we avoid the context changes in OnResize
        SetModuleEnabled(enabled);

        GetFramework()->EnableUpscaling(false);

        // Destroy the FSR3 context
        UpdateFSR3Context(false);

        // Destroy the FidelityFX interface memory
        free(m_InitializationParameters.backendInterfaceUpscaling.scratchBuffer);

        // Deregister UI elements for inactive upscaler
        GetUIManager()->UnRegisterUIElements(m_UISection);
    }
}

void FSR3UpscaleRenderModule::InitUI()
{
    // Build UI options, but don't register them yet. Registration/Deregistration will be controlled by enabling/disabling the render module
    m_UISection.SectionName = "Upscaling";  // We will piggy-back on existing upscaling section"
    m_UISection.SectionType = UISectionType::Sample;

    // Setup scale preset options
    const char* preset[] = {"Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)", "Ultra Performance (3x)", "Custom"};
    std::vector<std::string> presetComboOptions;
    presetComboOptions.assign(preset, preset + _countof(preset));
    std::function<void(void*)> presetCallback = [this](void* pParams) { this->UpdatePreset(static_cast<int32_t*>(pParams)); };
    m_UISection.AddCombo("Scale Preset", reinterpret_cast<int32_t*>(&m_ScalePreset), &presetComboOptions, presetCallback);

    // Setup mip bias
    std::function<void(void*)> mipBiasCallback = [this](void* pParams) { this->UpdateMipBias(static_cast<float*>(pParams)); };
    m_UISection.AddFloatSlider("Mip LOD Bias", &m_MipBias, -5.f, 0.f, mipBiasCallback);

    // Setup scale factor (disabled for all but custom)
    std::function<void(void*)> ratioCallback = [this](void* pParams) { this->UpdateUpscaleRatio(static_cast<float*>(pParams)); };
    m_UISection.AddFloatSlider("Custom Scale", &m_UpscaleRatio, 1.f, 3.f, ratioCallback, &m_UpscaleRatioEnabled);

    // Reactive mask
    const char* masks[] = { "Disabled", "Manual Reactive Mask Generation", "Autogen FSR3 Helper Function" };
    std::vector<std::string> maskComboOptions;
    maskComboOptions.assign(masks, masks + _countof(masks));
    m_UISection.AddCombo("Reactive Mask Mode", reinterpret_cast<int32_t*>(&m_MaskMode), &maskComboOptions);

    // Use mask
    m_UISection.AddCheckBox("Use Transparency and Composition Mask", &m_UseMask);

    // Sharpening
    m_UISection.AddCheckBox("RCAS Sharpening", &m_RCASSharpen);
    m_UISection.AddFloatSlider("Sharpness", &m_Sharpness, 0.f, 1.f, nullptr, &m_RCASSharpen);
}

void FSR3UpscaleRenderModule::UpdatePreset(const int32_t* pOldPreset)
{
    switch (m_ScalePreset)
    {
    case FSR3ScalePreset::NativeAA:
        m_UpscaleRatio = 1.0f;
        break;
    case FSR3ScalePreset::Quality:
        m_UpscaleRatio = 1.5f;
        break;
    case FSR3ScalePreset::Balanced:
        m_UpscaleRatio = 1.7f;
        break;
    case FSR3ScalePreset::Performance:
        m_UpscaleRatio = 2.0f;
        break;
    case FSR3ScalePreset::UltraPerformance:
        m_UpscaleRatio = 3.0f;
        break;
    case FSR3ScalePreset::Custom:
    default:
        // Leave the upscale ratio at whatever it was
        break;
    }

    // Update whether we can update the custom scale slider
    m_UpscaleRatioEnabled = (m_ScalePreset == FSR3ScalePreset::Custom);

    // Update mip bias
    float oldValue = m_MipBias;
    if (m_ScalePreset != FSR3ScalePreset::Custom)
        m_MipBias = cMipBias[static_cast<uint32_t>(m_ScalePreset)];
    else
        m_MipBias = CalculateMipBias(m_UpscaleRatio);
    UpdateMipBias(&oldValue);

    // Update resolution since rendering ratios have changed
    GetFramework()->EnableUpscaling(true, m_pUpdateFunc);
}

void FSR3UpscaleRenderModule::UpdateUpscaleRatio(const float* pOldRatio)
{
    // Disable/Enable FSR3 upscaler since resolution ratios have changed
    GetFramework()->EnableUpscaling(true, m_pUpdateFunc);
}

void FSR3UpscaleRenderModule::UpdateMipBias(const float* pOldBias)
{
    // Update the scene MipLODBias to use
    GetScene()->SetMipLODBias(m_MipBias);
}

void FSR3UpscaleRenderModule::FfxMsgCallback(FfxMsgType type, const wchar_t* message)
{
    if (type == FFX_MESSAGE_TYPE_ERROR)
    {
        CauldronWarning(L"FSR3_API_DEBUG_ERROR: %ls", message);
    }
    else if (type == FFX_MESSAGE_TYPE_WARNING)
    {
        CauldronWarning(L"FSR3_API_DEBUG_WARNING: %ls", message);
    }
}

void FSR3UpscaleRenderModule::UpdateFSR3Context(bool enabled)
{
    if (enabled)
    {
        const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
        m_InitializationParameters.maxRenderSize.width      = resInfo.RenderWidth;
        m_InitializationParameters.maxRenderSize.height     = resInfo.RenderHeight;
        m_InitializationParameters.upscaleOutputSize.width  = resInfo.DisplayWidth;
        m_InitializationParameters.upscaleOutputSize.height = resInfo.DisplayHeight;
        m_InitializationParameters.displaySize.width        = resInfo.DisplayWidth;
        m_InitializationParameters.displaySize.height       = resInfo.DisplayHeight;

        // Enable auto-exposure by default
        m_InitializationParameters.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE;
            
        // initialize FSR3 for upscaling only
        m_InitializationParameters.flags |= FFX_FSR3_ENABLE_UPSCALING_ONLY;

        // Note, inverted depth and display mode are currently handled statically for the run of the sample.
        // If they become changeable at runtime, we'll need to modify how this information is queried
        static bool s_InvertedDepth = GetConfig()->InvertedDepth;
        
        // Setup inverted depth flag according to sample usage
        if (s_InvertedDepth)
            m_InitializationParameters.flags |= FFX_FSR3_ENABLE_DEPTH_INVERTED | FFX_FSR3_ENABLE_DEPTH_INFINITE;

        // Input data is HDR
        m_InitializationParameters.flags |= FFX_FSR3_ENABLE_HIGH_DYNAMIC_RANGE;

        // Do eror checking in debug
        #if defined(_DEBUG)
            m_InitializationParameters.flags |= FFX_FSR3_ENABLE_DEBUG_CHECKING;
            m_InitializationParameters.fpMessage = &FSR3UpscaleRenderModule::FfxMsgCallback;
        #endif // #if defined(_DEBUG)

        // Create the FSR3 context
        FfxErrorCode errorCode = ffxFsr3ContextCreate(&m_FSR3Context, &m_InitializationParameters);
        CauldronAssert(ASSERT_CRITICAL, errorCode == FFX_OK, L"Couldn't create the FidelityFX SDK FSR3 context.");
    }

    else
    {
        // Destroy the FSR3 context
        ffxFsr3ContextDestroy(&m_FSR3Context);
    }
}

ResolutionInfo FSR3UpscaleRenderModule::UpdateResolution(uint32_t displayWidth, uint32_t displayHeight)
{
    return { static_cast<uint32_t>((float)displayWidth / m_UpscaleRatio),
             static_cast<uint32_t>((float)displayHeight / m_UpscaleRatio),
             displayWidth, displayHeight };
}

void FSR3UpscaleRenderModule::OnResize(const ResolutionInfo& resInfo)
{
    if (!ModuleEnabled())
        return;

    // Need to recreate the FSR3 context on resource resize
    UpdateFSR3Context(false);   // Destroy
    UpdateFSR3Context(true);    // Re-create
}

void FSR3UpscaleRenderModule::Execute(double deltaTime, CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"FFX FSR3");
    const ResolutionInfo&   resInfo = GetFramework()->GetResolutionInfo();
    CameraComponent*        pCamera = GetScene()->GetCurrentCamera();

    // All cauldron resources come into a render module in a generic read state (ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource)
    FfxFsr3DispatchUpscaleDescription dispatchParameters = {};
    dispatchParameters.commandList                = ffxGetCommandList(pCmdList);
    dispatchParameters.color                      = ffxGetResource(m_pColorTarget->GetResource(), L"FSR3_Input_OutputColor", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchParameters.depth                      = ffxGetResource(m_pDepthTarget->GetResource(), L"FSR3_InputDepth", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchParameters.motionVectors              = ffxGetResource(m_pMotionVectors->GetResource(), L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchParameters.exposure                   = ffxGetResource(nullptr, L"FSR3_InputExposure", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatchParameters.upscaleOutput              = dispatchParameters.color;

    if (m_MaskMode != FSR3MaskMode::Disabled)
    {
        dispatchParameters.reactive = ffxGetResource(m_pReactiveMask->GetResource(), L"FSR3_InputReactiveMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    else
    {
        dispatchParameters.reactive = ffxGetResource(nullptr, L"FSR3_EmptyInputReactiveMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }

    if (m_UseMask)
    {
        dispatchParameters.transparencyAndComposition = ffxGetResource(m_pCompositionMask->GetResource(), L"FSR3_TransparencyAndCompositionMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    else
    {
        dispatchParameters.transparencyAndComposition = ffxGetResource(nullptr, L"FSR3_EmptyTransparencyAndCompositionMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }

    // Jitter is calculated earlier in the frame using a callback from the camera update
    dispatchParameters.jitterOffset.x         = -pCamera->GetJitter(resInfo.RenderWidth, resInfo.RenderHeight).getX();
    dispatchParameters.jitterOffset.y         = -pCamera->GetJitter(resInfo.RenderWidth, resInfo.RenderHeight).getY();
    dispatchParameters.motionVectorScale.x    = resInfo.fRenderWidth();
    dispatchParameters.motionVectorScale.y    = resInfo.fRenderHeight();
    dispatchParameters.reset                  = GetFramework()->GetResetFlag();
    dispatchParameters.enableSharpening       = m_RCASSharpen;
    dispatchParameters.sharpness              = m_Sharpness;

    // Cauldron keeps time in seconds, but FSR expects miliseconds
    dispatchParameters.frameTimeDelta         = static_cast<float>(deltaTime * 1000.f);

    dispatchParameters.preExposure            = GetScene()->GetSceneExposure();
    dispatchParameters.renderSize.width       = resInfo.RenderWidth;
    dispatchParameters.renderSize.height      = resInfo.RenderHeight;

    // Note, inverted depth and display mode are currently handled statically for the run of the sample.
    // If they become changeable at runtime, we'll need to modify how this information is queried
    static bool s_InvertedDepth = GetConfig()->InvertedDepth;

    // Setup camera params as required
    dispatchParameters.cameraFovAngleVertical = pCamera->GetFovY();
    if (s_InvertedDepth)
    {
        dispatchParameters.cameraFar = pCamera->GetNearPlane();
        dispatchParameters.cameraNear = FLT_MAX;
    }
    else
    {
        dispatchParameters.cameraFar = pCamera->GetFarPlane();
        dispatchParameters.cameraNear = pCamera->GetNearPlane();
    }

    FfxErrorCode errorCode = ffxFsr3ContextDispatchUpscale(&m_FSR3Context, &dispatchParameters);
    FFX_ASSERT(errorCode == FFX_OK);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);

    // We are now done with upscaling
    GetFramework()->SetUpscalingState(UpscalerState::PostUpscale);
}

void FSR3UpscaleRenderModule::PreTransCallback(double deltaTime, cauldron::CommandList* pCmdList)
{
    GPUScopedProfileCapture sampleMarker(pCmdList, L"Pre-Trans (FSR3)");

    std::vector<Barrier> barriers;
    barriers.push_back(Barrier::Transition(m_pReactiveMask->GetResource(), 
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, 
        ResourceState::RenderTargetResource));
    barriers.push_back(Barrier::Transition(m_pCompositionMask->GetResource(), 
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, 
        ResourceState::RenderTargetResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    // We need to clear the reactive and composition masks before any translucencies are rendered into them
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ClearRenderTarget(pCmdList, &m_RasterViews[0]->GetResourceView(), clearColor);
    ClearRenderTarget(pCmdList, &m_RasterViews[1]->GetResourceView(), clearColor);

    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pReactiveMask->GetResource(), 
        ResourceState::RenderTargetResource, 
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    barriers.push_back(Barrier::Transition(m_pCompositionMask->GetResource(), 
        ResourceState::RenderTargetResource,
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

    if (m_MaskMode != FSR3MaskMode::Auto)
        return;
        
    // Copy the color render target before we apply translucency
    {
        barriers.clear();
        barriers.push_back(Barrier::Transition(m_pColorTarget->GetResource(), 
            ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, 
            ResourceState::CopySource));
        barriers.push_back(Barrier::Transition(m_pOpaqueTexture->GetResource(), 
            ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource, 
            ResourceState::CopyDest));
        ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());

        TextureCopyDesc copyColor = TextureCopyDesc(m_pColorTarget->GetResource(), m_pOpaqueTexture->GetResource());
        CopyTextureRegion(pCmdList, &copyColor);
    }

    barriers.clear();
    barriers.push_back(Barrier::Transition(m_pColorTarget->GetResource(), 
        ResourceState::CopySource, 
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    barriers.push_back(Barrier::Transition(m_pOpaqueTexture->GetResource(), 
        ResourceState::CopyDest, 
        ResourceState::NonPixelShaderResource | ResourceState::PixelShaderResource));
    ResourceBarrier(pCmdList, static_cast<uint32_t>(barriers.size()), barriers.data());
}

void FSR3UpscaleRenderModule::PostTransCallback(double deltaTime, cauldron::CommandList* pCmdList)
{
    if (m_MaskMode != FSR3MaskMode::Auto)
        return;

    GPUScopedProfileCapture sampleMarker(pCmdList, L"Gen Reactive Mask (FSR3)");

    FfxFsr3GenerateReactiveDescription generateReactiveParameters = {};
    generateReactiveParameters.commandList = ffxGetCommandList(pCmdList);
    generateReactiveParameters.colorOpaqueOnly = ffxGetResource(m_pOpaqueTexture->GetResource(), L"FSR3_Input_Opaque_Color", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    generateReactiveParameters.colorPreUpscale = ffxGetResource(m_pColorTarget->GetResource(), L"FSR3_Input_PreUpscaleColor", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    generateReactiveParameters.outReactive = ffxGetResource(m_pReactiveMask->GetResource(), L"FSR3_InputReactiveMask", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    const ResolutionInfo& resInfo = GetFramework()->GetResolutionInfo();
    generateReactiveParameters.renderSize.width = resInfo.RenderWidth;
    generateReactiveParameters.renderSize.height = resInfo.RenderHeight;

    generateReactiveParameters.scale = 1.f;
    generateReactiveParameters.cutoffThreshold = 0.2f;
    generateReactiveParameters.binaryValue = 0.9f;
    generateReactiveParameters.flags = FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_TONEMAP |
                                       FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_APPLY_THRESHOLD |
                                       FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_USE_COMPONENTS_MAX;

    FfxErrorCode errorCode = ffxFsr3ContextGenerateReactiveMask(&m_FSR3Context, &generateReactiveParameters);
    FFX_ASSERT(errorCode == FFX_OK);

    // FidelityFX contexts modify the set resource view heaps, so set the cauldron one back
    SetAllResourceViewHeaps(pCmdList);
}
