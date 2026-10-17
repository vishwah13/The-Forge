#include "SelfTest.h"

#include "../Game/UnitInstance.h"

#include <stdio.h>
#include <string.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

void writeLine(TestContext* pCtx, const char* pText)
{
    fsWriteToStream(&pCtx->mFile, pText, strlen(pText));
    fsWriteToStream(&pCtx->mFile, "\n", 1);
}

void check(TestContext* pCtx, bool ok, const char* pExpr, int32_t line)
{
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "  %s line %d: %s", ok ? "pass" : "FAIL", line, pExpr);
    writeLine(pCtx, buffer);
    if (!ok)
        ++pCtx->mFailures;
}

void runBattle(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps && simGetPhase(s) == PHASE_BATTLE; ++i)
        simStep(s, 1.0f / 60.0f);
}

void stepFor(BattleSim* s, float seconds)
{
    const int32_t steps = (int32_t)(seconds * 60.0f);
    for (int32_t i = 0; i < steps; ++i)
        simStep(s, 1.0f / 60.0f);
}

RenderData allocRenderData(uint32_t unitCapacity)
{
    RenderData data = {};
    data.pUnits = (UnitInstance*)tf_malloc(sizeof(UnitInstance) * unitCapacity);
    data.pBillboards = (BillboardInstance*)tf_malloc(sizeof(BillboardInstance) * (unitCapacity + MAX_PROJECTILES));
    data.pUnitAnims = (UnitAnim*)tf_malloc(sizeof(UnitAnim) * unitCapacity);
    return data;
}

void freeRenderData(RenderData* pData)
{
    tf_free(pData->pUnits);
    tf_free(pData->pBillboards);
    tf_free(pData->pUnitAnims);
}

typedef void (*TestFn)(TestContext*);

struct NamedTest
{
    const char* pName;
    TestFn      pFn;
};

int runAutoBattlerSelfTest(const char* pResultFileName)
{
    TestContext ctx = {};
    if (!fsOpenStreamFromPath(RD_LOG, pResultFileName, FM_WRITE, &ctx.mFile))
        return 1;

    const NamedTest tests[] = {
        { "cell math", testCellMath },
        { "buy and sell", testBuySell },
        { "unit cap", testUnitCap },
        { "wave is deterministic", testWaveDeterministic },
        { "start battle guards", testStartBattleGuards },
        { "melee battle and next round", testMeleeBattleAndNextRound },
        { "ranged projectiles", testRangedProjectiles },
        { "defeat and restart", testDefeatAndRestart },
        { "timeout", testTimeout },
        { "coincident units separate", testCoincidentUnitsSeparate },
        { "delta time clamp", testDeltaTimeClamp },
        { "restart mid battle", testRestartMidBattle },
        { "thread count does not change outcome", testThreadCountDoesNotChangeOutcome },
        { "thread count does not change a stress battle", testThreadCountDoesNotChangeStress },
        { "flecs delete keeps entity data", testFlecsDeleteKeepsEntityData },
        { "render data in prep", testRenderDataPrep },
        { "render data in battle", testRenderDataBattle },
        { "unit instance packing", testUnitInstancePacking },
        { "skin table", testSkinTable },
        { "baked pose blend", testBakedPoseBlend },
        { "animation state", testAnimationState },
        { "stress setup", testStressSetup },
        { "step timings", testStepTimings },
        { "unit culling", testUnitCulling },
        { "unit LOD selection", testUnitLodSelection },
        { "unit LOD build", testUnitLodBuild },
        { "nearest enemy search", testNearestEnemySearch },
    };
    for (uint32_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        writeLine(&ctx, tests[i].pName);
        tests[i].pFn(&ctx);
    }

    char summary[64];
    snprintf(summary, sizeof(summary), "RESULT: %d failure(s)", ctx.mFailures);
    writeLine(&ctx, summary);
    fsCloseStream(&ctx.mFile);
    return ctx.mFailures;
}
