// AutoBattler: top-down auto battler. Gameplay lives in AutoBattlerSim (Flecs); this file owns
// rendering, camera, input and UI.

#include "AnimationBake.h"
#include "UnitInstance.h"
#include "AutoBattlerSim.h"
#include "UnitCulling.h"
#include "UnitLod.h"

#include <stdlib.h>
#include <string.h>

// Interfaces
#include "../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../Common_3/Utilities/Interfaces/IThread.h"
#include "../../../Common_3/Utilities/Interfaces/ITime.h"

// Rendering
#include "../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../Common_3/Utilities/RingBuffer.h"

// Animation
#include "../../../Common_3/Resources/AnimationSystem/Animation/Rig.h"

#include "../../../Common_3/Utilities/Math/MathTypes.h"

// fsl
#include "../../../Common_3/Graphics/FSL/defaults.h"
#include "./Shaders/FSL/Cull.srt.h"
#include "./Shaders/FSL/Global.srt.h"

#include "../../../Common_3/Utilities/Interfaces/IMemory.h"

const uint32_t gDataBufferCount = 2;

Renderer*     pRenderer = NULL;
Queue*        pGraphicsQueue = NULL;
GpuCmdRing    gGraphicsCmdRing = {};
SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
Semaphore*    pImageAcquiredSemaphore[gDataBufferCount] = {};
uint32_t      gFrameIndex = 0;
ProfileToken  gGpuProfileToken;

Shader*        pUnitShader = NULL;
Shader*        pGroundShader = NULL;
Shader*        pBillboardShader = NULL;
Shader*        pCullClearShader = NULL;
Shader*        pCullShader = NULL;
Pipeline*      pCullClearPipeline = NULL;
Pipeline*      pCullPipeline = NULL;
DescriptorSet* pSetUnitsPerBatch = NULL; // graphics: one set per LOD, each that LOD's visible unit list
DescriptorSet* pSetCullPerBatch = NULL;  // compute: visible unit list and draw arguments, writable
Pipeline*      pUnitPipeline = NULL;
Pipeline*      pGroundPipeline = NULL;
Pipeline*      pBillboardPipeline = NULL;
DescriptorSet* pSetPersistent = NULL;
DescriptorSet* pSetPerFrame = NULL;

VertexLayout  gUnitVertexLayout = {};
Geometry*     pUnitGeom = NULL;
GeometryData* pUnitGeomData = NULL;
Texture*      pUnitTexture = NULL;
Texture*      pGroundTexture = NULL;

Buffer* pFrameUniformBuffers[gDataBufferCount] = {};
Buffer* pBonePaletteBuffer = NULL; // every baked animation frame, static
Buffer* pUnitInstanceBuffers[gDataBufferCount] = {};
Buffer* pBillboardBuffers[gDataBufferCount] = {};

// GPU culling: the cull pass sorts visible units into one list per LOD and fills one indexed indirect
// draw per LOD. One copy each is enough: the queue runs the cull and the draws of each frame in order.
static const uint32_t UNIT_DRAW_ARG_COUNT = 5; // IndirectDrawIndexArguments
static const uint32_t CULL_GROUP_SIZE = 64;    // must match CULL_GROUP_SIZE in Shaders/FSL/AutoBattler.h.fsl
static const uint32_t DEBUG_LOD_COLORS = 1;    // must match DEBUG_LOD_COLORS in Shaders/FSL/AutoBattler.h.fsl
bool                  gShowLodColors = false;
Buffer*             pVisibleUnitsBuffer = NULL; // UNIT_LOD_COUNT lists of unit capacity entries
Buffer*             pUnitDrawArgsBuffer = NULL; // UNIT_LOD_COUNT draws
Buffer*             pUnitLodIndexBuffer = NULL; // every LOD's indices into the unit mesh's vertices
uint32_t              gUnitCapacity = 0;
uint32_t              gLodFirstIndex[UNIT_LOD_COUNT] = {};
uint32_t              gLodIndexCount[UNIT_LOD_COUNT] = {};

// Pipeline statistics of the unit draws confirm how many triangles the GPU actually drew.
QueryPool* pUnitStatsQueryPool[gDataBufferCount] = {};
uint64_t     gCpuTriangles[gDataBufferCount] = {}; // CPU reference for the frame that used each query
uint64_t     gGpuTriangles = 0;                    // from the latest resolved query
int64_t      gGpuCpuMismatch = 0;                  // gpu - cpu triangles for that same frame
uint32_t     gCpuLodUnits[UNIT_LOD_COUNT] = {};    // units per LOD in the latest CPU reference cull

// Must match FrameUniforms in Shaders/FSL/AutoBattler.h.fsl.
struct FrameUniforms
{
    mat4     mViewProj;
    float4   mCamRight;
    float4   mCamUp;
    float4   mHoverCell; // x, z = cell; y = 0 none, 1 can place, 2 cannot; w = cell size
    float4   mArena;     // x = arena size, y = world z where the enemy half starts
    float4   mCullPlanes[6];
    float4   mUnitBounds; // x = bounding sphere centre height, y = radius (model units), z, w = LOD distances in radii
    float4   mCameraPos;
    float4   mTeamColors[TEAM_COUNT]; // rgb
    uint32_t mUnitCount;
    uint32_t mUnitCapacity;
    uint32_t mDebugFlags; // DEBUG_* bits
    uint32_t mPad;
    uint32_t mLodFirstIndex[4]; // uint4 in the shader; UNIT_LOD_COUNT used
    uint32_t mLodIndexCount[4];
};
FrameUniforms gFrameUniforms = {};

// Unit model, imported from Mixamo with Tools/MixamoImport (--name paladin).
static const char* UNIT_SKELETON = "paladin/skeleton.ozz";
static const char* UNIT_MESH = "paladin/riggedMesh.bin";
static const char* UNIT_TEXTURE = "paladin_D.tex";
// One clip per AnimClip. Walking falls back to idle until a walking.ozz has been imported.
static const char* UNIT_CLIPS[ANIM_CLIP_COUNT] = { "paladin/animations/neutral_idle.ozz", "paladin/animations/walking.ozz",
                                                   "paladin/animations/shooting.ozz", "paladin/animations/silly_dancing.ozz" };

static const float3 TEAM_COLORS[TEAM_COUNT] = { float3(0.30f, 0.55f, 1.00f), float3(1.00f, 0.30f, 0.25f) };

Rig             gRig; // unit model skeleton
BakedAnimations gBaked = {};

BattleSim* pSim = NULL;
RenderData gRenderData = {};

// Camera orbits a ground target at a fixed pitch.
const float CAMERA_PITCH = 55.0f * PI / 180.0f;
const float CAMERA_FOV_X = PI / 3.0f;
float2      gCameraTarget = float2(ARENA_SIZE * 0.5f, ARENA_SIZE * 0.45f);
float       gCameraDistance = 75.0f;
vec3        gCameraEye;
vec3        gCameraForward;
vec3        gCameraRight;
vec3        gCameraUp;

uint32_t     gFontID = 0;
FontDrawDesc gTextDraw;
char         gHudLines[3][256] = {};

// --smoke: plays a large battle unattended, writes a summary file and quits.
bool     gSmokeMode = false;
bool     gSmokeDone = false;
uint32_t gSmokeFrames = 0;
uint32_t gSmokeResultFrame = 0;
uint32_t gSmokeStartUnits = 0;
float    gSmokeSeconds = 0.0f;
Phase    gSmokeResult = PHASE_PREP;

// --stress: runs ever larger battles on a world sized to each army, writes per-tier timings and quits.
// The sim advances a fixed STRESS_STEP per frame (slow motion when the frame is slower), so every tier
// measures the same battle seconds with the same sim work per frame.
static const uint32_t STRESS_TIERS[] = { 4096, 8192, 16384, 32768, 65536 }; // total units, both teams
static const float    STRESS_STEP = 1.0f / 60.0f;
static const uint32_t STRESS_WARMUP_FRAMES = 60;   // battle seconds 0-1
static const uint32_t STRESS_MEASURE_FRAMES = 300; // battle seconds 1-6
static const char*    STRESS_VIEWS[] = { "field", "zoom" }; // whole field on screen, then a gameplay camera
static const float    STRESS_ZOOM_DISTANCE = 60.0f;
static const uint32_t STRESS_RUNS = TF_ARRAY_COUNT(STRESS_TIERS) * TF_ARRAY_COUNT(STRESS_VIEWS);
bool     gStressMode = false;
uint32_t gStressRun = 0; // tier = run / view count, view = run % view count
uint32_t gStressFrame = 0;
int64_t  gStressSimUs = 0;
float    gStressGpuMs = 0.0f;
float    gStressSeconds = 0.0f;
uint64_t gStressGpuTriangles = 0; // sums over measured frames
uint64_t gStressLodUnits[UNIT_LOD_COUNT] = {};
int64_t  gStressWorstMismatch = 0; // gpu - cpu triangles
char     gStressReport[4096] = {};

UIComponent* pGuiWindow = NULL;
int32_t      gSelectedType = UNIT_MELEE;
bool         gStartRequested = false;
bool         gRestartRequested = false;
InputEnum    CUSTOM_PLACE = {};
InputEnum    CUSTOM_SELL = {};
InputEnum    CUSTOM_ZOOM = {};

static const char* PHASE_NAMES[] = { "Prep", "Battle", "Victory", "Defeat" };

static void writeTextFile(const char* pFileName, const char* pText)
{
    FileStream fs = {};
    if (!fsOpenStreamFromPath(RD_LOG, pFileName, FM_WRITE, &fs))
        return;
    fsWriteToStream(&fs, pText, strlen(pText));
    fsCloseStream(&fs);
}

class AutoBattler: public IApp
{
public:
    bool Init()
    {
        for (int i = 1; i < argc; ++i)
        {
            if (strcmp(argv[i], "--selftest") == 0)
                exit(runAutoBattlerSelfTest("AutoBattler_selftest.txt") == 0 ? 0 : 1);
            if (strcmp(argv[i], "--smoke") == 0)
                gSmokeMode = true;
            if (strcmp(argv[i], "--stress") == 0)
                gStressMode = true;
        }

        gRig.Initialize(RD_ANIMATIONS, UNIT_SKELETON);

        RendererDesc settings;
        memset(&settings, 0, sizeof(settings));
        initGPUConfiguration(settings.pExtendedSettings);
        initRenderer(GetName(), &settings, &pRenderer);
        if (!pRenderer)
        {
            ShowUnsupportedMessage(getUnsupportedGPUMsg());
            return false;
        }
        setupGPUConfigurationPlatformParameters(pRenderer, settings.pExtendedSettings);

        QueueDesc queueDesc = {};
        queueDesc.mType = QUEUE_TYPE_GRAPHICS;
        queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
        initQueue(pRenderer, &queueDesc, &pGraphicsQueue);

        GpuCmdRingDesc cmdRingDesc = {};
        cmdRingDesc.pQueue = pGraphicsQueue;
        cmdRingDesc.mPoolCount = gDataBufferCount;
        cmdRingDesc.mCmdPerPoolCount = 1;
        cmdRingDesc.mAddSyncPrimitives = true;
        initGpuCmdRing(pRenderer, &cmdRingDesc, &gGraphicsCmdRing);

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
            initSemaphore(pRenderer, &pImageAcquiredSemaphore[i]);
        initResourceLoaderInterface(pRenderer);

        RootSignatureDesc rootDesc = {};
        INIT_RS_DESC(rootDesc, "default.rootsig", "compute.rootsig");
        initRootSignature(pRenderer, &rootDesc);

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

        ProfilerDesc profiler = {};
        profiler.pRenderer = pRenderer;
        initProfiler(&profiler);
        gGpuProfileToken = initGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

        // Per-frame buffers.
        BufferLoadDesc ubDesc = {};
        ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            ubDesc.mDesc.mSize = sizeof(FrameUniforms);
            ubDesc.ppBuffer = &pFrameUniformBuffers[i];
            addResource(&ubDesc, NULL);
        }

        BufferLoadDesc instanceDesc = {};
        instanceDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
        instanceDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
        instanceDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_NONE;
        instanceDesc.mDesc.mStartState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        instanceDesc.mDesc.mFirstElement = 0;
        const uint32_t unitCapacity = gStressMode ? STRESS_TIERS[TF_ARRAY_COUNT(STRESS_TIERS) - 1] : MAX_UNITS;
        const uint32_t billboardCapacity = unitCapacity + MAX_PROJECTILES;
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            instanceDesc.mDesc.mElementCount = unitCapacity;
            instanceDesc.mDesc.mStructStride = sizeof(UnitInstance);
            instanceDesc.mDesc.mSize = (uint64_t)unitCapacity * sizeof(UnitInstance);
            instanceDesc.ppBuffer = &pUnitInstanceBuffers[i];
            addResource(&instanceDesc, NULL);
            instanceDesc.mDesc.mElementCount = billboardCapacity;
            instanceDesc.mDesc.mStructStride = sizeof(BillboardInstance);
            instanceDesc.mDesc.mSize = (uint64_t)billboardCapacity * sizeof(BillboardInstance);
            instanceDesc.ppBuffer = &pBillboardBuffers[i];
            addResource(&instanceDesc, NULL);
        }

        // Skinned unit model, same vertex layout as 28_Skinning.
        gUnitVertexLayout.mBindingCount = 1;
        gUnitVertexLayout.mAttribCount = 5;
        gUnitVertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
        gUnitVertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
        gUnitVertexLayout.mAttribs[0].mBinding = 0;
        gUnitVertexLayout.mAttribs[0].mLocation = 0;
        gUnitVertexLayout.mAttribs[0].mOffset = 0;
        gUnitVertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
        gUnitVertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32_UINT;
        gUnitVertexLayout.mAttribs[1].mBinding = 0;
        gUnitVertexLayout.mAttribs[1].mLocation = 1;
        gUnitVertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);
        gUnitVertexLayout.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
        gUnitVertexLayout.mAttribs[2].mFormat = TinyImageFormat_R32_UINT;
        gUnitVertexLayout.mAttribs[2].mBinding = 0;
        gUnitVertexLayout.mAttribs[2].mLocation = 2;
        gUnitVertexLayout.mAttribs[2].mOffset = 3 * sizeof(float) + sizeof(uint32_t);
        gUnitVertexLayout.mAttribs[3].mSemantic = SEMANTIC_WEIGHTS;
        gUnitVertexLayout.mAttribs[3].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
        gUnitVertexLayout.mAttribs[3].mBinding = 0;
        gUnitVertexLayout.mAttribs[3].mLocation = 3;
        gUnitVertexLayout.mAttribs[3].mOffset = 3 * sizeof(float) + sizeof(uint32_t) * 2;
        gUnitVertexLayout.mAttribs[4].mSemantic = SEMANTIC_JOINTS;
        gUnitVertexLayout.mAttribs[4].mFormat = TinyImageFormat_R16G16B16A16_UINT;
        gUnitVertexLayout.mAttribs[4].mBinding = 0;
        gUnitVertexLayout.mAttribs[4].mLocation = 4;
        gUnitVertexLayout.mAttribs[4].mOffset = 7 * sizeof(float) + sizeof(uint32_t) * 2;

        GeometryLoadDesc geomDesc = {};
        geomDesc.pFileName = UNIT_MESH;
        geomDesc.pVertexLayout = &gUnitVertexLayout;
        geomDesc.ppGeometry = &pUnitGeom;
        geomDesc.ppGeometryData = &pUnitGeomData;
        geomDesc.mFlags = GEOMETRY_LOAD_FLAG_SHADOWED; // indices and positions for the LODs
        addResource(&geomDesc, NULL);

        TextureLoadDesc textureDesc = {};
        textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
        textureDesc.pFileName = UNIT_TEXTURE;
        textureDesc.ppTexture = &pUnitTexture;
        addResource(&textureDesc, NULL);
        textureDesc.pFileName = "PBR/copper_tiles_01/1K/Albedo.tex";
        textureDesc.ppTexture = &pGroundTexture;
        addResource(&textureDesc, NULL);

        AddCustomInputBindings();
        inputAddCustomBindings(R"(
ab_place; button; MOUSE_1; released
ab_sell; button; MOUSE_2; released
ab_zoom; buttonanalog; MOUSE_WHEEL_DOWN; 1.0f; MOUSE_WHEEL_UP; -1.0f)");
        CUSTOM_PLACE = inputGetCustomBindingEnum("ab_place");
        CUSTOM_SELL = inputGetCustomBindingEnum("ab_sell");
        CUSTOM_ZOOM = inputGetCustomBindingEnum("ab_zoom");

        BufferLoadDesc cullDesc = {};
        cullDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER;
        cullDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        cullDesc.mDesc.mStartState = RESOURCE_STATE_UNORDERED_ACCESS;
        gUnitCapacity = unitCapacity;
        cullDesc.mDesc.mElementCount = (uint64_t)UNIT_LOD_COUNT * unitCapacity;
        cullDesc.mDesc.mStructStride = sizeof(uint32_t);
        cullDesc.mDesc.mSize = cullDesc.mDesc.mElementCount * sizeof(uint32_t);
        cullDesc.mDesc.pName = "Visible units";
        cullDesc.ppBuffer = &pVisibleUnitsBuffer;
        addResource(&cullDesc, NULL);
        cullDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER | DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_INDIRECT_BUFFER;
        cullDesc.mDesc.mElementCount = UNIT_LOD_COUNT * UNIT_DRAW_ARG_COUNT;
        cullDesc.mDesc.mSize = cullDesc.mDesc.mElementCount * sizeof(uint32_t);
        cullDesc.mDesc.pName = "Unit draw arguments";
        cullDesc.ppBuffer = &pUnitDrawArgsBuffer;
        addResource(&cullDesc, NULL);

        if (pRenderer->pGpu->mPipelineStatsQueries)
        {
            QueryPoolDesc poolDesc = {};
            poolDesc.mQueryCount = 1;
            poolDesc.mType = QUERY_TYPE_PIPELINE_STATISTICS;
            for (uint32_t i = 0; i < gDataBufferCount; ++i)
                initQueryPool(pRenderer, &poolDesc, &pUnitStatsQueryPool[i]);
        }

        gRenderData.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * unitCapacity);
        gRenderData.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * billboardCapacity);
        gRenderData.pUnitAnims = (UnitAnim*)tf_malloc(sizeof(UnitAnim) * unitCapacity);

        const uint32_t cores = getNumCPUCores();
        pSim = simCreate(cores > 1 ? cores - 1 : 1, unitCapacity);

        waitForAllResourceLoads();
        if (!addUnitLods())
            return false;

        // Bake every clip into one static palette table.
        const char* clips[ANIM_CLIP_COUNT];
        memcpy(clips, UNIT_CLIPS, sizeof(clips));
        FileStream walk = {};
        if (fsOpenStreamFromPath(RD_ANIMATIONS, clips[ANIM_WALK], FM_READ, &walk))
            fsCloseStream(&walk);
        else
        {
            LOGF(eWARNING, "%s not found; units walk with the idle clip", clips[ANIM_WALK]);
            clips[ANIM_WALK] = clips[ANIM_IDLE];
        }
        if (!bakeAnimations(&gRig, pUnitGeomData, clips, &gBaked))
            return false;
        BufferLoadDesc paletteDesc = {};
        paletteDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_BUFFER;
        paletteDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        paletteDesc.mDesc.mStartState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        paletteDesc.mDesc.mFirstElement = 0;
        paletteDesc.mDesc.mElementCount = (uint64_t)gBaked.mFrameTotal * gBaked.mJointCount;
        paletteDesc.mDesc.mStructStride = sizeof(mat4);
        paletteDesc.mDesc.mSize = paletteDesc.mDesc.mElementCount * sizeof(mat4);
        paletteDesc.pData = gBaked.pPalettes;
        paletteDesc.ppBuffer = &pBonePaletteBuffer;
        addResource(&paletteDesc, NULL);
        waitForAllResourceLoads();
        freeBakedAnimations(&gBaked); // uploaded; offsets and frame counts stay valid

        return true;
    }

    void Exit()
    {
        simDestroy(pSim);
        pSim = NULL;
        tf_free(gRenderData.pUnits);
        tf_free(gRenderData.pBillboards);
        tf_free(gRenderData.pUnitAnims);

        gRig.Exit();

        exitProfiler();
        exitUserInterface();
        exitFontSystem();

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            removeResource(pFrameUniformBuffers[i]);
            removeResource(pUnitInstanceBuffers[i]);
            removeResource(pBillboardBuffers[i]);
        }
        removeResource(pUnitGeomData);
        pUnitGeomData = NULL;
        removeResource(pUnitGeom);
        pUnitGeom = NULL;
        removeResource(pUnitTexture);
        removeResource(pGroundTexture);
        removeResource(pBonePaletteBuffer);
        removeResource(pVisibleUnitsBuffer);
        removeResource(pUnitDrawArgsBuffer);
        removeResource(pUnitLodIndexBuffer);
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
            if (pUnitStatsQueryPool[i])
                exitQueryPool(pRenderer, pUnitStatsQueryPool[i]);

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
            exitSemaphore(pRenderer, pImageAcquiredSemaphore[i]);
        exitGpuCmdRing(pRenderer, &gGraphicsCmdRing);
        exitRootSignature(pRenderer);
        exitResourceLoaderInterface(pRenderer);
        exitQueue(pRenderer, pGraphicsQueue);
        exitRenderer(pRenderer);
        exitGPUConfiguration();
        pRenderer = NULL;
    }

    bool Load(ReloadDesc* pReloadDesc)
    {
        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            addShaders();
            addDescriptorSets();
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            loadProfilerUI(mSettings.mWidth, mSettings.mHeight);
            addGui();
            if (!addSwapChain())
                return false;
            if (!addDepthBuffer())
                return false;
        }

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            addPipelines();

        prepareDescriptorSets();

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

    void Unload(ReloadDesc* pReloadDesc)
    {
        waitQueueIdle(pGraphicsQueue);
        unloadFontSystem(pReloadDesc->mType);
        unloadUserInterface(pReloadDesc->mType);

        if (pReloadDesc->mType & (RELOAD_TYPE_SHADER | RELOAD_TYPE_RENDERTARGET))
            removePipelines();

        if (pReloadDesc->mType & (RELOAD_TYPE_RESIZE | RELOAD_TYPE_RENDERTARGET))
        {
            removeSwapChain(pRenderer, pSwapChain);
            removeRenderTarget(pRenderer, pDepthBuffer);
            unloadProfilerUI();
            uiRemoveComponent(pGuiWindow);
        }

        if (pReloadDesc->mType & RELOAD_TYPE_SHADER)
        {
            removeDescriptorSets();
            removeShaders();
        }
    }

    void Update(float deltaTime)
    {
        if (!uiIsFocused())
        {
            if (inputGetValue(0, CUSTOM_TOGGLE_FULLSCREEN))
                toggleFullscreen(pWindow);
            if (inputGetValue(0, CUSTOM_EXIT))
                requestShutdown();
        }

        if (gSmokeMode)
            updateSmoke(deltaTime);
        if (gStressMode)
            updateStress(deltaTime);

        if (gStartRequested)
        {
            gStartRequested = false;
            simStartBattle(pSim);
        }
        if (gRestartRequested)
        {
            gRestartRequested = false;
            simRestart(pSim);
        }

        const int64_t simStartUs = getUSec(true);
        simStep(pSim, gStressMode ? STRESS_STEP : deltaTime);
        gStressSimUs += getUSec(true) - simStartUs;
        updateCameraInput(deltaTime);
        updateCamera(deltaTime);
        updatePicking();
        simFillRenderData(pSim, &gRenderData);
        updateCullUniforms();
        // The sim says which clip and time each unit plays; turn that into its pose in the baked table.
        for (uint32_t i = 0; i < gRenderData.mUnitCount; ++i)
        {
            const BakedPose pose = bakedPose(gBaked, gRenderData.pUnitAnims[i].mClip, gRenderData.pUnitAnims[i].mTime);
            gRenderData.pUnits[i].mPose = packUnitPose(pose.mOffset, pose.mBlend);
        }
        updateHud();
    }

    // Squad radio buttons, Start Battle / Restart and the LOD debug view; button presses are applied in Update.
    void addGui()
    {
        UIComponentDesc guiDesc = {};
        guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.78f, mSettings.mHeight * 0.05f);
        guiDesc.mStartSize = vec2(300.0f, 240.0f);
        guiDesc.mFontID = 0;
        guiDesc.mFontSize = 16.0f;
        uiAddComponent("Auto Battler", &guiDesc, &pGuiWindow);

        for (int32_t t = 0; t < UNIT_TYPE_COUNT; ++t)
        {
            char label[64];
            snprintf(label, sizeof(label), "%s squad x%d (%d gold)", gUnitStats[t].pName, gUnitStats[t].mSquadSide * gUnitStats[t].mSquadSide,
                     gUnitStats[t].mCost);
            RadioButtonWidget radio = {};
            radio.pData = &gSelectedType;
            radio.mRadioId = t;
            uiAddComponentWidget(pGuiWindow, label, &radio, WIDGET_TYPE_RADIO_BUTTON);
        }

        ButtonWidget startButton = {};
        UIWidget*    pStart = uiAddComponentWidget(pGuiWindow, "Start Battle", &startButton, WIDGET_TYPE_BUTTON);
        uiSetWidgetOnEditedCallback(pStart, NULL, [](void*) { gStartRequested = true; });

        ButtonWidget restartButton = {};
        UIWidget*    pRestart = uiAddComponentWidget(pGuiWindow, "Restart", &restartButton, WIDGET_TYPE_BUTTON);
        uiSetWidgetOnEditedCallback(pRestart, NULL, [](void*) { gRestartRequested = true; });

        CheckboxWidget lodColors = {};
        lodColors.pData = &gShowLodColors;
        uiAddComponentWidget(pGuiWindow, "Debug: LOD colours (0 G, 1 Y, 2 R)", &lodColors, WIDGET_TYPE_CHECKBOX);
    }

    void updateCameraInput(float deltaTime)
    {
        if (uiIsFocused())
            return;
        const float panSpeed = gCameraDistance * 0.8f;
        const float worldSize = simGetWorldSize(pSim);
        gCameraTarget.x = clampf(gCameraTarget.x + inputGetValue(0, CUSTOM_MOVE_X) * panSpeed * deltaTime, 0.0f, worldSize);
        gCameraTarget.y = clampf(gCameraTarget.y + inputGetValue(0, CUSTOM_MOVE_Y) * panSpeed * deltaTime, 0.0f, worldSize);
        gCameraDistance = clampf(gCameraDistance + inputGetValue(0, CUSTOM_ZOOM) * 5.0f, 20.0f, fmaxf(120.0f, 1.5f * worldSize));
    }

    // Casts the mouse ray onto the ground, highlights the cell and buys/sells on click (prep only).
    void updatePicking()
    {
        if (simGetPhase(pSim) != PHASE_PREP || uiIsFocused())
            return;
        const float ndcX = 2.0f * inputGetValue(0, CUSTOM_PT_X) / (float)mSettings.mWidth - 1.0f;
        const float ndcY = 1.0f - 2.0f * inputGetValue(0, CUSTOM_PT_Y) / (float)mSettings.mHeight;
        const float tanHalfX = tanf(CAMERA_FOV_X * 0.5f);
        const float tanHalfY = tanHalfX * (float)mSettings.mHeight / (float)mSettings.mWidth;
        const vec3  dir = normalize(gCameraForward + gCameraRight * (ndcX * tanHalfX) + gCameraUp * (ndcY * tanHalfY));

        int32_t cellX = 0;
        int32_t cellZ = 0;
        if (!rayToCell(v3ToF3(gCameraEye), v3ToF3(dir), &cellX, &cellZ))
            return;

        const bool canBuy = simCanBuySquad(pSim, cellX, cellZ, (UnitType)gSelectedType);
        gFrameUniforms.mHoverCell = float4((float)cellX, canBuy ? 1.0f : 2.0f, (float)cellZ, CELL_SIZE);
        if (inputGetValue(0, CUSTOM_PLACE))
            simBuySquad(pSim, cellX, cellZ, (UnitType)gSelectedType);
        if (inputGetValue(0, CUSTOM_SELL))
            simSellSquad(pSim, cellX, cellZ);
    }

    void updateCamera(float deltaTime)
    {
        UNREF_PARAM(deltaTime);
        const vec3 target = vec3(gCameraTarget.x, 0.0f, gCameraTarget.y);
        gCameraEye = target + vec3(0.0f, sinf(CAMERA_PITCH), -cosf(CAMERA_PITCH)) * gCameraDistance;
        gCameraForward = normalize(target - gCameraEye);
        gCameraRight = normalize(cross(vec3(0.0f, 1.0f, 0.0f), gCameraForward));
        gCameraUp = cross(gCameraForward, gCameraRight);

        const float        aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
        const mat4         view = mat4::lookAtLH(Point3(gCameraEye), Point3(target), vec3(0.0f, 1.0f, 0.0f));
        const mat4         proj = mat4::perspectiveLH_ReverseZ(CAMERA_FOV_X, aspectInverse, 0.5f, 2000.0f);
        gFrameUniforms.mViewProj = proj * view;
        gFrameUniforms.mCamRight = float4(v3ToF3(gCameraRight), 0.0f);
        gFrameUniforms.mCamUp = float4(v3ToF3(gCameraUp), 0.0f);
        const float worldSize = simGetWorldSize(pSim);
        gFrameUniforms.mArena = float4(worldSize, 0.5f * worldSize, 0.0f, 0.0f); // the enemy half starts halfway
        gFrameUniforms.mHoverCell = float4(0.0f, 0.0f, 0.0f, CELL_SIZE);
    }

    void updateHud()
    {
        const Phase phase = simGetPhase(pSim);
        snprintf(gHudLines[0], sizeof(gHudLines[0]), "Round %d    Gold %d    Blue %u vs Red %u    %s %.0f / %.0f s", simGetRound(pSim),
                 simGetGold(pSim), simGetUnitCount(pSim, TEAM_PLAYER), simGetUnitCount(pSim, TEAM_ENEMY), PHASE_NAMES[phase],
                 simGetBattleTime(pSim), BATTLE_TIME_LIMIT);
        static const char* HINTS[] = {
            "Left-click your (blue) half to buy the selected squad, right-click to sell. WASD pans, wheel zooms.",
            "Battle in progress",
            "VICTORY! Next round starting...",
            "DEFEAT - press Restart",
        };
        snprintf(gHudLines[1], sizeof(gHudLines[1]), "%s", HINTS[phase]);
        snprintf(gHudLines[2], sizeof(gHudLines[2]), "%s", simGetMessage(pSim));
    }

    void updateSmoke(float deltaTime)
    {
        if (gSmokeDone)
            return;
        if (gSmokeFrames == 0)
        {
            simTestClearBoard(pSim);
            simTestSetGold(pSim, 100000);
            for (int32_t x = 0; x < ARENA_CELLS; ++x)
            {
                for (int32_t z = 0; z < 3; ++z)
                    simBuySquad(pSim, x, z, UNIT_MELEE);
                simBuySquad(pSim, x, 3, UNIT_RANGED);
                simBuySquad(pSim, x, 4, UNIT_TANK);
                for (int32_t z = PLAYER_ROWS; z < PLAYER_ROWS + 3; ++z)
                    simTestSetCell(pSim, x, z, UNIT_MELEE);
            }
            gSmokeStartUnits = simGetUnitCount(pSim, TEAM_PLAYER) + simGetUnitCount(pSim, TEAM_ENEMY);
            simStartBattle(pSim);
            // Frame the front line closely so screenshots show facing, health bars and projectiles.
            gCameraTarget = float2(ARENA_SIZE * 0.5f, (float)PLAYER_ROWS * CELL_SIZE);
            gCameraDistance = 35.0f;
        }
        ++gSmokeFrames;
        gSmokeSeconds += deltaTime;

        const Phase phase = simGetPhase(pSim);
        if (gSmokeResult == PHASE_PREP && (phase == PHASE_VICTORY || phase == PHASE_DEFEAT))
        {
            gSmokeResult = phase;
            gSmokeResultFrame = gSmokeFrames;
        }
        // Keep running ~2 s into the result so the dance path is exercised too.
        if ((gSmokeResult != PHASE_PREP && gSmokeFrames > gSmokeResultFrame + 120) || gSmokeSeconds > 150.0f)
        {
            char summary[256];
            snprintf(summary, sizeof(summary), "units_at_start=%u\nframes=%u\nseconds=%.1f\navg_frame_ms=%.2f\nresult=%s\n", gSmokeStartUnits,
                     gSmokeFrames, gSmokeSeconds, 1000.0f * gSmokeSeconds / (float)gSmokeFrames, PHASE_NAMES[gSmokeResult]);
            writeTextFile("AutoBattler_smoke.txt", summary);
            gSmokeDone = true;
            requestShutdown();
        }
    }

    // Each tier runs once per view: warm up, then average sim, GPU and frame time, and the units the GPU
    // drew against the CPU reference cull of the same frames.
    void updateStress(float deltaTime)
    {
        if (gStressRun >= STRESS_RUNS)
            return;
        const uint32_t tier = gStressRun / TF_ARRAY_COUNT(STRESS_VIEWS);
        const uint32_t view = gStressRun % TF_ARRAY_COUNT(STRESS_VIEWS);
        if (gStressFrame == 0)
        {
            if (gStressRun == 0)
                snprintf(gStressReport, sizeof(gStressReport),
                         "# units  view   world_m  frame_ms  sim_ms  gpu_ms  fps   lod0   lod1   lod2  gpu_mtris  worst_gpu_minus_cpu_tris  "
                         "alive_at_end\n");
            simStressSetup(pSim, STRESS_TIERS[tier] / 2);
            const float worldSize = simGetWorldSize(pSim);
            gCameraTarget = view == 0 ? float2(0.5f * worldSize, 0.45f * worldSize) : float2(0.5f * worldSize, 0.5f * worldSize);
            gCameraDistance = view == 0 ? 1.1f * worldSize : STRESS_ZOOM_DISTANCE;
        }
        ++gStressFrame;
        if (gStressFrame == STRESS_WARMUP_FRAMES)
        {
            gStressSimUs = 0;
            gStressGpuMs = 0.0f;
            gStressSeconds = 0.0f;
            gStressGpuTriangles = 0;
            memset(gStressLodUnits, 0, sizeof(gStressLodUnits));
            gStressWorstMismatch = 0;
        }
        else if (gStressFrame > STRESS_WARMUP_FRAMES)
        {
            gStressGpuMs += getGpuProfileTime(gGpuProfileToken);
            gStressSeconds += deltaTime;
            gStressGpuTriangles += gGpuTriangles;
            for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
                gStressLodUnits[lod] += gCpuLodUnits[lod];
            if (llabs(gGpuCpuMismatch) > llabs(gStressWorstMismatch))
                gStressWorstMismatch = gGpuCpuMismatch;
        }
        if (gStressFrame < STRESS_WARMUP_FRAMES + STRESS_MEASURE_FRAMES)
            return;

        // Updates run before simStep, so the reset at the warm-up frame and this report each bracket exactly N steps.
        const float  frames = (float)STRESS_MEASURE_FRAMES;
        const float  frameMs = 1000.0f * gStressSeconds / frames;
        const size_t used = strlen(gStressReport);
        snprintf(gStressReport + used, sizeof(gStressReport) - used,
                 "%7u  %-5s  %7.0f  %8.2f  %6.2f  %6.2f  %4.0f  %5.0f  %5.0f  %5.0f  %9.2f  %24lld  %u\n", STRESS_TIERS[tier],
                 STRESS_VIEWS[view], simGetWorldSize(pSim), frameMs, (float)gStressSimUs / 1000.0f / frames, gStressGpuMs / frames,
                 1000.0f / frameMs, (double)gStressLodUnits[0] / frames, (double)gStressLodUnits[1] / frames,
                 (double)gStressLodUnits[2] / frames, (double)gStressGpuTriangles / frames / 1e6, (long long)gStressWorstMismatch,
                 simGetUnitCount(pSim, TEAM_PLAYER) + simGetUnitCount(pSim, TEAM_ENEMY));
        ++gStressRun;
        gStressFrame = 0;
        if (gStressRun == STRESS_RUNS)
        {
            writeTextFile("AutoBattler_stress.txt", gStressReport);
            requestShutdown();
        }
    }

    // Frustum planes and counts for the GPU cull pass.
    void updateCullUniforms()
    {
        frustumPlanesFromViewProj(gFrameUniforms.mViewProj, gFrameUniforms.mCullPlanes);
        gFrameUniforms.mUnitBounds = float4(UNIT_BOUND_CENTER_Y, UNIT_BOUND_RADIUS, UNIT_LOD_DISTANCES[0], UNIT_LOD_DISTANCES[1]);
        gFrameUniforms.mCameraPos = float4(v3ToF3(gCameraEye), 1.0f);
        for (uint32_t t = 0; t < TEAM_COUNT; ++t)
            gFrameUniforms.mTeamColors[t] = float4(TEAM_COLORS[t], 1.0f);
        gFrameUniforms.mUnitCount = gRenderData.mUnitCount;
        gFrameUniforms.mUnitCapacity = gUnitCapacity;
        gFrameUniforms.mDebugFlags = gShowLodColors ? DEBUG_LOD_COLORS : 0u;
        for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
        {
            gFrameUniforms.mLodFirstIndex[lod] = gLodFirstIndex[lod];
            gFrameUniforms.mLodIndexCount[lod] = gLodIndexCount[lod];
        }
    }

    // CPU reference of the cull pass: units per LOD, and the triangles the GPU should draw for them.
    uint64_t cullOnCpu(uint32_t lodUnits[UNIT_LOD_COUNT])
    {
        memset(lodUnits, 0, sizeof(uint32_t) * UNIT_LOD_COUNT);
        const float3 eye = gFrameUniforms.mCameraPos.getXYZ();
        for (uint32_t i = 0; i < gRenderData.mUnitCount; ++i)
        {
            const UnitInstance& u = gRenderData.pUnits[i];
            const int32_t       lod = unitLod(gFrameUniforms.mCullPlanes, eye, float3(u.mX, 0.0f, u.mZ), u.mScale);
            if (lod >= 0)
                ++lodUnits[lod];
        }
        uint64_t triangles = 0;
        for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
            triangles += (uint64_t)lodUnits[lod] * (gLodIndexCount[lod] / 3);
        return triangles;
    }

    // Simplified index lists over the unit mesh's own vertices, packed into one index buffer.
    bool addUnitLods()
    {
        const GeometryData::ShadowData* pCpu = pUnitGeomData->pShadow;
        const uint32_t                 indexCount = pUnitGeom->mIndexCount;
        uint32_t*                      pIndices = (uint32_t*)tf_malloc(sizeof(uint32_t) * indexCount);
        for (uint32_t i = 0; i < indexCount; ++i)
            pIndices[i] = pUnitGeom->mIndexType == INDEX_TYPE_UINT16 ? ((const uint16_t*)pCpu->pIndices)[i] : ((const uint32_t*)pCpu->pIndices)[i];

        UnitLods   lods = {};
        const bool built = buildUnitLods(pIndices, indexCount, (const float*)pCpu->pAttributes[SEMANTIC_POSITION], pUnitGeom->mVertexCount,
                                         pCpu->mVertexStrides[SEMANTIC_POSITION], &lods);
        tf_free(pIndices);
        removeGeometryShadowData(pUnitGeomData);
        if (!built)
        {
            LOGF(eERROR, "Could not build the unit LODs");
            return false;
        }
        for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
        {
            gLodFirstIndex[lod] = lods.mFirstIndex[lod];
            gLodIndexCount[lod] = lods.mIndexCount[lod];
            LOGF(eINFO, "Unit LOD%u: %u triangles", lod, lods.mIndexCount[lod] / 3);
        }

        BufferLoadDesc indexDesc = {};
        indexDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_INDEX_BUFFER;
        indexDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
        indexDesc.mDesc.mStartState = RESOURCE_STATE_INDEX_BUFFER;
        indexDesc.mDesc.mSize = sizeof(uint32_t) * lods.mTotalIndexCount;
        indexDesc.mDesc.pName = "Unit LOD indices";
        indexDesc.pData = lods.pIndices;
        indexDesc.ppBuffer = &pUnitLodIndexBuffer;
        addResource(&indexDesc, NULL);
        waitForAllResourceLoads();
        freeUnitLods(&lods);
        return true;
    }

    void Draw()
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore[gFrameIndex], NULL, &swapchainImageIndex);

        GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);
        FenceStatus       fenceStatus = {};
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);
        resetCmdPool(pRenderer, elem.pCmdPool);

        // This frame slot's GPU work is finished: read how many units its unit draw drew.
        QueryPool* pStatsPool = pUnitStatsQueryPool[gFrameIndex];
        if (pStatsPool)
        {
            QueryData stats = {};
            getQueryData(pRenderer, pStatsPool, 0, &stats);
            if (stats.mValid)
            {
                gGpuTriangles = stats.mPipelineStats.mIAPrimitives;
                gGpuCpuMismatch = (int64_t)gGpuTriangles - (int64_t)gCpuTriangles[gFrameIndex];
            }
            // The reference is only needed while measuring; it costs a pass over every unit.
            gCpuTriangles[gFrameIndex] = gStressMode ? cullOnCpu(gCpuLodUnits) : 0;
        }

        // Upload this frame's data (after the fence, so the GPU is done with these buffers).
        BufferUpdateDesc frameUpdate = { pFrameUniformBuffers[gFrameIndex] };
        beginUpdateResource(&frameUpdate);
        memcpy(frameUpdate.pMappedData, &gFrameUniforms, sizeof(gFrameUniforms));
        endUpdateResource(&frameUpdate);

        if (gRenderData.mUnitCount > 0)
        {
            BufferUpdateDesc unitUpdate = { pUnitInstanceBuffers[gFrameIndex] };
            unitUpdate.mCurrentState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            beginUpdateResource(&unitUpdate);
            memcpy(unitUpdate.pMappedData, gRenderData.pUnits, sizeof(UnitInstance) * gRenderData.mUnitCount);
            endUpdateResource(&unitUpdate);
        }
        if (gRenderData.mBillboardCount > 0)
        {
            BufferUpdateDesc billboardUpdate = { pBillboardBuffers[gFrameIndex] };
            billboardUpdate.mCurrentState = RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            beginUpdateResource(&billboardUpdate);
            memcpy(billboardUpdate.pMappedData, gRenderData.pBillboards, sizeof(BillboardInstance) * gRenderData.mBillboardCount);
            endUpdateResource(&billboardUpdate);
        }

        RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
        Cmd*          cmd = elem.pCmds[0];
        beginCmd(cmd);
        cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);
        if (pStatsPool)
            cmdResetQuery(cmd, pStatsPool, 0, 1);

        if (gRenderData.mUnitCount > 0)
            cmdCullUnits(cmd);

        RenderTargetBarrier barriers[] = { { pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET } };
        cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

        BindRenderTargetsDesc bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_CLEAR };
        bindRenderTargets.mDepthStencil = { pDepthBuffer, LOAD_ACTION_CLEAR };
        cmdBindRenderTargets(cmd, &bindRenderTargets);
        cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
        cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

        cmdBeginDebugMarker(cmd, 1, 0, 1, "Ground");
        cmdBindPipeline(cmd, pGroundPipeline);
        cmdBindDescriptorSet(cmd, 0, pSetPersistent);
        cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
        cmdDraw(cmd, 6, 0);
        cmdEndDebugMarker(cmd);

        if (gRenderData.mUnitCount > 0)
        {
            cmdBeginDebugMarker(cmd, 1, 0, 1, "Units");
            cmdBindPipeline(cmd, pUnitPipeline);
            cmdBindDescriptorSet(cmd, 0, pSetPersistent);
            cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
            cmdBindVertexBuffer(cmd, 1, &pUnitGeom->pVertexBuffers[0], pUnitGeom->mVertexStrides, (uint64_t*)NULL);
            cmdBindIndexBuffer(cmd, pUnitLodIndexBuffer, INDEX_TYPE_UINT32, (uint64_t)NULL);
            QueryDesc query = { 0 };
            if (pStatsPool)
                cmdBeginQuery(cmd, pStatsPool, &query);
            // One draw per LOD; set `lod` points the vertex shader at that LOD's visible list.
            for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
            {
                cmdBindDescriptorSet(cmd, lod, pSetUnitsPerBatch);
                cmdExecuteIndirect(cmd, INDIRECT_DRAW_INDEX, 1, pUnitDrawArgsBuffer, lod * UNIT_DRAW_ARG_COUNT * sizeof(uint32_t), NULL, 0);
            }
            if (pStatsPool)
                cmdEndQuery(cmd, pStatsPool, &query);
            cmdEndDebugMarker(cmd);
        }

        if (gRenderData.mBillboardCount > 0)
        {
            cmdBeginDebugMarker(cmd, 1, 0, 1, "Billboards");
            cmdBindPipeline(cmd, pBillboardPipeline);
            cmdBindDescriptorSet(cmd, 0, pSetPersistent);
            cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
            cmdDraw(cmd, 6 * gRenderData.mBillboardCount, 0);
            cmdEndDebugMarker(cmd);
        }

        cmdBeginDebugMarker(cmd, 0, 1, 0, "UI");
        bindRenderTargets = {};
        bindRenderTargets.mRenderTargetCount = 1;
        bindRenderTargets.mRenderTargets[0] = { pRenderTarget, LOAD_ACTION_LOAD };
        cmdBindRenderTargets(cmd, &bindRenderTargets);

        gTextDraw.mFontID = gFontID;
        gTextDraw.mFontColor = 0xff00ffff;
        gTextDraw.mFontSize = 18.0f;
        const float2 profileSize = cmdDrawCpuProfile(cmd, float2(8.0f, 15.0f), &gTextDraw);
        if (uiIsRenderingEnabled())
        {
            gTextDraw.mFontColor = 0xffffffff;
            gTextDraw.mFontSize = 22.0f;
            for (uint32_t i = 0; i < 3; ++i)
            {
                gTextDraw.pText = gHudLines[i];
                cmdDrawTextWithFont(cmd, float2(8.0f, profileSize.y + 40.0f + 28.0f * (float)i), &gTextDraw);
            }
        }
        cmdDrawUserInterface(cmd);
        cmdBindRenderTargets(cmd, NULL);
        cmdEndDebugMarker(cmd);

        barriers[0] = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
        cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);
        if (gRenderData.mUnitCount > 0)
        {
            // Back to writable for the next frame's cull pass.
            BufferBarrier toWrite[] = { { pVisibleUnitsBuffer, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
                                          { pUnitDrawArgsBuffer, RESOURCE_STATE_INDIRECT_ARGUMENT, RESOURCE_STATE_UNORDERED_ACCESS } };
            cmdResourceBarrier(cmd, TF_ARRAY_COUNT(toWrite), toWrite, 0, NULL, 0, NULL);
        }
        if (pStatsPool)
            cmdResolveQuery(cmd, pStatsPool, 0, 1);
        cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
        endCmd(cmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore[gFrameIndex] };

        QueueSubmitDesc submitDesc = {};
        submitDesc.mCmdCount = 1;
        submitDesc.mSignalSemaphoreCount = 1;
        submitDesc.mWaitSemaphoreCount = TF_ARRAY_COUNT(waitSemaphores);
        submitDesc.ppCmds = &cmd;
        submitDesc.ppSignalSemaphores = &elem.pSemaphore;
        submitDesc.ppWaitSemaphores = waitSemaphores;
        submitDesc.pSignalFence = elem.pFence;
        queueSubmit(pGraphicsQueue, &submitDesc);

        QueuePresentDesc presentDesc = {};
        presentDesc.mIndex = (uint8_t)swapchainImageIndex;
        presentDesc.mWaitSemaphoreCount = 1;
        presentDesc.ppWaitSemaphores = &elem.pSemaphore;
        presentDesc.pSwapChain = pSwapChain;
        presentDesc.mSubmitDone = true;
        queuePresent(pGraphicsQueue, &presentDesc);
        flipProfiler();

        gFrameIndex = (gFrameIndex + 1) % gDataBufferCount;
    }

    // Resets the draw arguments, then culls every unit into the visible list on the GPU.
    void cmdCullUnits(Cmd* cmd)
    {
        const ProfileToken cullToken = cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Cull units");
        cmdBindPipeline(cmd, pCullClearPipeline);
        cmdBindDescriptorSet(cmd, 0, pSetPersistent);
        cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
        cmdBindDescriptorSet(cmd, 0, pSetCullPerBatch);
        cmdDispatch(cmd, 1, 1, 1);

        BufferBarrier clearDone = { pUnitDrawArgsBuffer, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_UNORDERED_ACCESS };
        cmdResourceBarrier(cmd, 1, &clearDone, 0, NULL, 0, NULL);

        cmdBindPipeline(cmd, pCullPipeline);
        cmdBindDescriptorSet(cmd, 0, pSetPersistent);
        cmdBindDescriptorSet(cmd, gFrameIndex, pSetPerFrame);
        cmdBindDescriptorSet(cmd, 0, pSetCullPerBatch);
        cmdDispatch(cmd, (gRenderData.mUnitCount + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, 1, 1);

        BufferBarrier toDraw[] = { { pVisibleUnitsBuffer, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE },
                                     { pUnitDrawArgsBuffer, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_INDIRECT_ARGUMENT } };
        cmdResourceBarrier(cmd, TF_ARRAY_COUNT(toDraw), toDraw, 0, NULL, 0, NULL);
        cmdEndGpuTimestampQuery(cmd, cullToken);
    }

    const char* GetName() { return "AutoBattler"; }

    bool addSwapChain()
    {
        SwapChainDesc swapChainDesc = {};
        swapChainDesc.mWindowHandle = pWindow->handle;
        swapChainDesc.mPresentQueueCount = 1;
        swapChainDesc.ppPresentQueues = &pGraphicsQueue;
        swapChainDesc.mWidth = mSettings.mWidth;
        swapChainDesc.mHeight = mSettings.mHeight;
        swapChainDesc.mImageCount = getRecommendedSwapchainImageCount(pRenderer, &pWindow->handle);
        swapChainDesc.mColorFormat = getSupportedSwapchainFormat(pRenderer, &swapChainDesc, COLOR_SPACE_SDR_SRGB);
        swapChainDesc.mColorSpace = COLOR_SPACE_SDR_SRGB;
        swapChainDesc.mColorClearValue = { { 0.08f, 0.09f, 0.11f, 1.0f } };
        swapChainDesc.mEnableVsync = mSettings.mVSyncEnabled;
        ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);
        return pSwapChain != NULL;
    }

    bool addDepthBuffer()
    {
        ESRAM_BEGIN_ALLOC(pRenderer, "Depth", 0);
        RenderTargetDesc depthRT = {};
        depthRT.mArraySize = 1;
        depthRT.mClearValue.depth = 0.0f;
        depthRT.mClearValue.stencil = 0;
        depthRT.mDepth = 1;
        depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
        depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
        depthRT.mHeight = mSettings.mHeight;
        depthRT.mSampleCount = SAMPLE_COUNT_1;
        depthRT.mSampleQuality = 0;
        depthRT.mWidth = mSettings.mWidth;
        depthRT.mFlags = TEXTURE_CREATION_FLAG_ESRAM | TEXTURE_CREATION_FLAG_ON_TILE | TEXTURE_CREATION_FLAG_VR_MULTIVIEW;
        addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);
        ESRAM_END_ALLOC(pRenderer);
        return pDepthBuffer != NULL;
    }

    void addShaders()
    {
        ShaderLoadDesc unitShader = {};
        unitShader.mVert.pFileName = "unit.vert";
        unitShader.mFrag.pFileName = "unit.frag";
        addShader(pRenderer, &unitShader, &pUnitShader);

        ShaderLoadDesc groundShader = {};
        groundShader.mVert.pFileName = "ground.vert";
        groundShader.mFrag.pFileName = "ground.frag";
        addShader(pRenderer, &groundShader, &pGroundShader);

        ShaderLoadDesc billboardShader = {};
        billboardShader.mVert.pFileName = "billboard.vert";
        billboardShader.mFrag.pFileName = "billboard.frag";
        addShader(pRenderer, &billboardShader, &pBillboardShader);

        ShaderLoadDesc cullShader = {};
        cullShader.mComp.pFileName = "cullclear.comp";
        addShader(pRenderer, &cullShader, &pCullClearShader);
        cullShader.mComp.pFileName = "cull.comp";
        addShader(pRenderer, &cullShader, &pCullShader);
    }

    void removeShaders()
    {
        removeShader(pRenderer, pUnitShader);
        removeShader(pRenderer, pGroundShader);
        removeShader(pRenderer, pBillboardShader);
        removeShader(pRenderer, pCullClearShader);
        removeShader(pRenderer, pCullShader);
    }

    void addDescriptorSets()
    {
        DescriptorSetDesc setDesc = SRT_SET_DESC(SrtData, Persistent, 1, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPersistent);
        setDesc = SRT_SET_DESC(SrtData, PerFrame, gDataBufferCount, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPerFrame);
        setDesc = SRT_SET_DESC(SrtData, PerBatch, UNIT_LOD_COUNT, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetUnitsPerBatch);
        setDesc = SRT_SET_DESC(CullSrtData, PerBatch, 1, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetCullPerBatch);
    }

    void removeDescriptorSets()
    {
        removeDescriptorSet(pRenderer, pSetPersistent);
        removeDescriptorSet(pRenderer, pSetPerFrame);
        removeDescriptorSet(pRenderer, pSetUnitsPerBatch);
        removeDescriptorSet(pRenderer, pSetCullPerBatch);
    }

    void prepareDescriptorSets()
    {
        DescriptorData persistent[3] = {};
        persistent[0].mIndex = SRT_RES_IDX(SrtData, Persistent, gUnitTexture);
        persistent[0].ppTextures = &pUnitTexture;
        persistent[1].mIndex = SRT_RES_IDX(SrtData, Persistent, gGroundTexture);
        persistent[1].ppTextures = &pGroundTexture;
        persistent[2].mIndex = SRT_RES_IDX(SrtData, Persistent, gBonePalettes);
        persistent[2].ppBuffers = &pBonePaletteBuffer;
        updateDescriptorSet(pRenderer, 0, pSetPersistent, 3, persistent);

        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            DescriptorData perFrame[3] = {};
            perFrame[0].mIndex = SRT_RES_IDX(SrtData, PerFrame, gFrame);
            perFrame[0].ppBuffers = &pFrameUniformBuffers[i];
            perFrame[1].mIndex = SRT_RES_IDX(SrtData, PerFrame, gUnitInstances);
            perFrame[1].ppBuffers = &pUnitInstanceBuffers[i];
            perFrame[2].mIndex = SRT_RES_IDX(SrtData, PerFrame, gBillboards);
            perFrame[2].ppBuffers = &pBillboardBuffers[i];
            updateDescriptorSet(pRenderer, i, pSetPerFrame, 3, perFrame);
        }

        for (uint32_t lod = 0; lod < UNIT_LOD_COUNT; ++lod)
        {
            DescriptorDataRange range = { lod * gUnitCapacity * (uint32_t)sizeof(uint32_t), gUnitCapacity * (uint32_t)sizeof(uint32_t),
                                            (uint32_t)sizeof(uint32_t) };
            DescriptorData      units = {};
            units.mIndex = SRT_RES_IDX(SrtData, PerBatch, gVisibleUnits);
            units.ppBuffers = &pVisibleUnitsBuffer;
            units.pRanges = &range;
            updateDescriptorSet(pRenderer, lod, pSetUnitsPerBatch, 1, &units);
        }

        DescriptorData cull[2] = {};
        cull[0].mIndex = SRT_RES_IDX(CullSrtData, PerBatch, gVisibleUnitsRW);
        cull[0].ppBuffers = &pVisibleUnitsBuffer;
        cull[1].mIndex = SRT_RES_IDX(CullSrtData, PerBatch, gUnitDrawArgsRW);
        cull[1].ppBuffers = &pUnitDrawArgsBuffer;
        updateDescriptorSet(pRenderer, 0, pSetCullPerBatch, 2, cull);
    }

    void addPipelines()
    {
        RasterizerStateDesc cullNone = {};
        cullNone.mCullMode = CULL_MODE_NONE;
        RasterizerStateDesc cullBack = {}; // the unit model is exported from Blender with glTF's counter-clockwise front faces
        cullBack.mCullMode = CULL_MODE_BACK;

        DepthStateDesc depthState = {};
        depthState.mDepthTest = true;
        depthState.mDepthWrite = true;
        depthState.mDepthFunc = CMP_GEQUAL;

        PipelineDesc desc = {};
        desc.mType = PIPELINE_TYPE_GRAPHICS;
        PIPELINE_LAYOUT_DESC(desc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame), NULL, NULL);
        GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
        pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
        pipelineSettings.mRenderTargetCount = 1;
        pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
        pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
        pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
        pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
        pipelineSettings.pDepthState = &depthState;

        pipelineSettings.pRasterizerState = &cullNone;
        pipelineSettings.pVertexLayout = NULL;
        pipelineSettings.pShaderProgram = pGroundShader;
        addPipeline(pRenderer, &desc, &pGroundPipeline);

        pipelineSettings.pShaderProgram = pBillboardShader;
        addPipeline(pRenderer, &desc, &pBillboardPipeline);

        PipelineDesc unitDesc = desc;
        PIPELINE_LAYOUT_DESC(unitDesc, SRT_LAYOUT_DESC(SrtData, Persistent), SRT_LAYOUT_DESC(SrtData, PerFrame),
                             SRT_LAYOUT_DESC(SrtData, PerBatch), NULL);
        unitDesc.mGraphicsDesc.pRasterizerState = &cullBack;
        unitDesc.mGraphicsDesc.pVertexLayout = &gUnitVertexLayout;
        unitDesc.mGraphicsDesc.pShaderProgram = pUnitShader;
        addPipeline(pRenderer, &unitDesc, &pUnitPipeline);

        PipelineDesc cullDesc = {};
        cullDesc.mType = PIPELINE_TYPE_COMPUTE;
        PIPELINE_LAYOUT_DESC(cullDesc, SRT_LAYOUT_DESC(CullSrtData, Persistent), SRT_LAYOUT_DESC(CullSrtData, PerFrame),
                             SRT_LAYOUT_DESC(CullSrtData, PerBatch), NULL);
        cullDesc.mComputeDesc.pShaderProgram = pCullClearShader;
        addPipeline(pRenderer, &cullDesc, &pCullClearPipeline);
        cullDesc.mComputeDesc.pShaderProgram = pCullShader;
        addPipeline(pRenderer, &cullDesc, &pCullPipeline);
    }

    void removePipelines()
    {
        removePipeline(pRenderer, pUnitPipeline);
        removePipeline(pRenderer, pGroundPipeline);
        removePipeline(pRenderer, pBillboardPipeline);
        removePipeline(pRenderer, pCullClearPipeline);
        removePipeline(pRenderer, pCullPipeline);
    }
};

DEFINE_APPLICATION_MAIN(AutoBattler)
