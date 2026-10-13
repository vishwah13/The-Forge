// 40_AutoBattler: top-down auto battler. Gameplay lives in AutoBattlerSim (Flecs); this file owns
// rendering, camera, input and UI.

#include "AnimationBake.h"
#include "AutoBattlerSim.h"

#include <stdlib.h>
#include <string.h>

// Interfaces
#include "../../../../Common_3/Application/Interfaces/IApp.h"
#include "../../../../Common_3/Application/Interfaces/ICameraController.h"
#include "../../../../Common_3/Application/Interfaces/IFont.h"
#include "../../../../Common_3/Application/Interfaces/IProfiler.h"
#include "../../../../Common_3/Application/Interfaces/IUI.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"
#include "../../../../Common_3/Utilities/Interfaces/IThread.h"

// Rendering
#include "../../../../Common_3/Graphics/Interfaces/IGraphics.h"
#include "../../../../Common_3/Resources/ResourceLoader/Interfaces/IResourceLoader.h"
#include "../../../../Common_3/Utilities/RingBuffer.h"

// Animation
#include "../../../../Common_3/Resources/AnimationSystem/Animation/Rig.h"

#include "../../../../Common_3/Utilities/Math/MathTypes.h"

// fsl
#include "../../../../Common_3/Graphics/FSL/defaults.h"
#include "./Shaders/FSL/Global.srt.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

const uint32_t gDataBufferCount = 2;

Renderer*     pRenderer = NULL;
Queue*        pGraphicsQueue = NULL;
GpuCmdRing    gGraphicsCmdRing = {};
SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
Semaphore*    pImageAcquiredSemaphore = NULL;
uint32_t      gFrameIndex = 0;
ProfileToken  gGpuProfileToken;

Shader*        pUnitShader = NULL;
Shader*        pGroundShader = NULL;
Shader*        pBillboardShader = NULL;
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

// Must match FrameUniforms in Shaders/FSL/AutoBattler.h.fsl.
struct FrameUniforms
{
    mat4   mViewProj;
    float4 mCamRight;
    float4 mCamUp;
    float4 mHoverCell; // x, z = cell; y = 0 none, 1 can place, 2 cannot; w = cell size
    float4 mArena;     // x = arena size, y = world z where the enemy half starts
};
FrameUniforms gFrameUniforms = {};

Rig             gRig; // Stormtrooper skeleton
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

UIComponent* pGuiWindow = NULL;
int32_t      gSelectedType = UNIT_MELEE;
bool         gStartRequested = false;
bool         gRestartRequested = false;
InputEnum    CUSTOM_PLACE = {};
InputEnum    CUSTOM_SELL = {};
InputEnum    CUSTOM_ZOOM = {};

static const char* PHASE_NAMES[] = { "Prep", "Battle", "Victory", "Defeat" };

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
                exit(runAutoBattlerSelfTest("40_AutoBattler_selftest.txt") == 0 ? 0 : 1);
            if (strcmp(argv[i], "--smoke") == 0)
                gSmokeMode = true;
        }

        gRig.Initialize(RD_ANIMATIONS, "stormtrooper/skeleton.ozz");

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

        initSemaphore(pRenderer, &pImageAcquiredSemaphore);
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
        for (uint32_t i = 0; i < gDataBufferCount; ++i)
        {
            instanceDesc.mDesc.mElementCount = MAX_UNITS;
            instanceDesc.mDesc.mStructStride = sizeof(UnitInstance);
            instanceDesc.mDesc.mSize = (uint64_t)MAX_UNITS * sizeof(UnitInstance);
            instanceDesc.ppBuffer = &pUnitInstanceBuffers[i];
            addResource(&instanceDesc, NULL);
            instanceDesc.mDesc.mElementCount = MAX_BILLBOARDS;
            instanceDesc.mDesc.mStructStride = sizeof(BillboardInstance);
            instanceDesc.mDesc.mSize = (uint64_t)MAX_BILLBOARDS * sizeof(BillboardInstance);
            instanceDesc.ppBuffer = &pBillboardBuffers[i];
            addResource(&instanceDesc, NULL);
        }

        // Skinned Stormtrooper, same vertex layout as 28_Skinning.
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
        geomDesc.pFileName = "stormtrooper/riggedMesh.bin";
        geomDesc.pVertexLayout = &gUnitVertexLayout;
        geomDesc.ppGeometry = &pUnitGeom;
        geomDesc.ppGeometryData = &pUnitGeomData;
        geomDesc.mFlags = GEOMETRY_LOAD_FLAG_SHADOWED;
        addResource(&geomDesc, NULL);

        TextureLoadDesc textureDesc = {};
        textureDesc.mCreationFlag = TEXTURE_CREATION_FLAG_SRGB;
        textureDesc.pFileName = "Stormtrooper_D.tex";
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

        gRenderData.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * MAX_UNITS);
        gRenderData.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * MAX_BILLBOARDS);

        const uint32_t cores = getNumCPUCores();
        pSim = simCreate(cores > 1 ? cores - 1 : 1);

        waitForAllResourceLoads();

        // Bake idle/walk (retargeted from the stickFigure) and dance into one static palette table.
        if (!bakeAnimations(&gRig, pUnitGeomData, &gBaked))
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

        gFrameIndex = 0;
        return true;
    }

    void Exit()
    {
        simDestroy(pSim);
        pSim = NULL;
        tf_free(gRenderData.pUnits);
        tf_free(gRenderData.pBillboards);

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

        exitSemaphore(pRenderer, pImageAcquiredSemaphore);
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
            if (inputGetValue(0, CUSTOM_TOGGLE_UI))
                uiToggleActive();
            if (inputGetValue(0, CUSTOM_EXIT))
                requestShutdown();
        }

        if (gSmokeMode)
            updateSmoke(deltaTime);

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

        simStep(pSim, deltaTime);
        updateCameraInput(deltaTime);
        updateCamera(deltaTime);
        updatePicking();
        simFillRenderData(pSim, &gRenderData);
        // The sim says which clip and time each unit plays; turn that into its palette offset.
        for (uint32_t i = 0; i < gRenderData.mUnitCount; ++i)
        {
            float4& anim = gRenderData.pUnits[i].mAnim;
            anim.x = (float)bakedPaletteOffset(gBaked, (AnimClip)(uint32_t)anim.x, anim.y);
        }
        updateHud();
    }

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
    }

    void updateCameraInput(float deltaTime)
    {
        if (uiIsFocused())
            return;
        const float panSpeed = gCameraDistance * 0.8f;
        gCameraTarget.x = clampf(gCameraTarget.x + inputGetValue(0, CUSTOM_MOVE_X) * panSpeed * deltaTime, 0.0f, ARENA_SIZE);
        gCameraTarget.y = clampf(gCameraTarget.y + inputGetValue(0, CUSTOM_MOVE_Y) * panSpeed * deltaTime, 0.0f, ARENA_SIZE);
        gCameraDistance = clampf(gCameraDistance + inputGetValue(0, CUSTOM_ZOOM) * 5.0f, 20.0f, 120.0f);
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
        if (!rayToCell(float3(gCameraEye.getX(), gCameraEye.getY(), gCameraEye.getZ()), float3(dir.getX(), dir.getY(), dir.getZ()), &cellX,
                       &cellZ))
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
        const CameraMatrix proj = CameraMatrix::perspectiveReverseZ(CAMERA_FOV_X, aspectInverse, 0.5f, 500.0f);
        gFrameUniforms.mViewProj = proj.mCamera * view;
        gFrameUniforms.mCamRight = float4(gCameraRight.getX(), gCameraRight.getY(), gCameraRight.getZ(), 0.0f);
        gFrameUniforms.mCamUp = float4(gCameraUp.getX(), gCameraUp.getY(), gCameraUp.getZ(), 0.0f);
        gFrameUniforms.mArena = float4(ARENA_SIZE, (float)PLAYER_ROWS * CELL_SIZE, 0.0f, 0.0f);
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
            writeTextFile("40_AutoBattler_smoke.txt", summary);
            gSmokeDone = true;
            requestShutdown();
        }
    }

    void Draw()
    {
        if ((bool)pSwapChain->mEnableVsync != mSettings.mVSyncEnabled)
        {
            waitQueueIdle(pGraphicsQueue);
            ::toggleVSync(pRenderer, &pSwapChain);
        }

        uint32_t swapchainImageIndex;
        acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

        GpuCmdRingElement elem = getNextGpuCmdRingElement(&gGraphicsCmdRing, true, 1);
        FenceStatus       fenceStatus;
        getFenceStatus(pRenderer, elem.pFence, &fenceStatus);
        if (fenceStatus == FENCE_STATUS_INCOMPLETE)
            waitForFences(pRenderer, 1, &elem.pFence);
        resetCmdPool(pRenderer, elem.pCmdPool);

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
            cmdBindIndexBuffer(cmd, pUnitGeom->pIndexBuffer, pUnitGeom->mIndexType, (uint64_t)NULL);
            cmdDrawIndexedInstanced(cmd, pUnitGeom->mIndexCount, 0, gRenderData.mUnitCount, 0, 0);
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
        cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
        endCmd(cmd);

        FlushResourceUpdateDesc flushUpdateDesc = {};
        flushUpdateDesc.mNodeIndex = 0;
        flushResourceUpdates(&flushUpdateDesc);
        Semaphore* waitSemaphores[2] = { flushUpdateDesc.pOutSubmittedSemaphore, pImageAcquiredSemaphore };

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

    const char* GetName() { return "40_AutoBattler"; }

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
    }

    void removeShaders()
    {
        removeShader(pRenderer, pUnitShader);
        removeShader(pRenderer, pGroundShader);
        removeShader(pRenderer, pBillboardShader);
    }

    void addDescriptorSets()
    {
        DescriptorSetDesc setDesc = SRT_SET_DESC(SrtData, Persistent, 1, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPersistent);
        setDesc = SRT_SET_DESC(SrtData, PerFrame, gDataBufferCount, 0);
        addDescriptorSet(pRenderer, &setDesc, &pSetPerFrame);
    }

    void removeDescriptorSets()
    {
        removeDescriptorSet(pRenderer, pSetPersistent);
        removeDescriptorSet(pRenderer, pSetPerFrame);
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
    }

    void addPipelines()
    {
        RasterizerStateDesc cullNone = {};
        cullNone.mCullMode = CULL_MODE_NONE;
        RasterizerStateDesc cullFront = {}; // the Stormtrooper mesh is wound this way, as in 28_Skinning
        cullFront.mCullMode = CULL_MODE_FRONT;

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

        pipelineSettings.pRasterizerState = &cullFront;
        pipelineSettings.pVertexLayout = &gUnitVertexLayout;
        pipelineSettings.pShaderProgram = pUnitShader;
        addPipeline(pRenderer, &desc, &pUnitPipeline);
    }

    void removePipelines()
    {
        removePipeline(pRenderer, pUnitPipeline);
        removePipeline(pRenderer, pGroundPipeline);
        removePipeline(pRenderer, pBillboardPipeline);
    }
};

DEFINE_APPLICATION_MAIN(AutoBattler)
