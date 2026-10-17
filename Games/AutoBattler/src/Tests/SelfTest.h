#pragma once

// Shared pieces of the headless self test (runAutoBattlerSelfTest in SelfTest.cpp).

#include "../Game/Sim.h"

#include "../../../../Common_3/Utilities/Interfaces/IFileSystem.h"

struct TestContext
{
    FileStream mFile;
    int32_t    mFailures;
};

void writeLine(TestContext* pCtx, const char* pText);
void check(TestContext* pCtx, bool ok, const char* pExpr, int32_t line);

#define CHECK(cond) check(pCtx, (cond), #cond, __LINE__)

// Helpers shared by the test files.
void runBattle(BattleSim* s, float seconds);
void stepFor(BattleSim* s, float seconds);
RenderData allocRenderData(uint32_t unitCapacity = MAX_UNITS);
void freeRenderData(RenderData* pData);

// Tests, in report order (see the table in SelfTest.cpp).
void testCellMath(TestContext* pCtx);
void testBuySell(TestContext* pCtx);
void testUnitCap(TestContext* pCtx);
void testWaveDeterministic(TestContext* pCtx);
void testStartBattleGuards(TestContext* pCtx);
void testMeleeBattleAndNextRound(TestContext* pCtx);
void testRangedProjectiles(TestContext* pCtx);
void testDefeatAndRestart(TestContext* pCtx);
void testTimeout(TestContext* pCtx);
void testCoincidentUnitsSeparate(TestContext* pCtx);
void testDeltaTimeClamp(TestContext* pCtx);
void testRestartMidBattle(TestContext* pCtx);
void testThreadCountDoesNotChangeStress(TestContext* pCtx);
void testThreadCountDoesNotChangeOutcome(TestContext* pCtx);
void testStepTimings(TestContext* pCtx);
void testFlecsDeleteKeepsEntityData(TestContext* pCtx);
void testRenderDataPrep(TestContext* pCtx);
void testRenderDataBattle(TestContext* pCtx);
void testUnitInstancePacking(TestContext* pCtx);
void testSkinTable(TestContext* pCtx);
void testBakedPoseBlend(TestContext* pCtx);
void testAnimationState(TestContext* pCtx);
void testStressSetup(TestContext* pCtx);
void testUnitCulling(TestContext* pCtx);
void testUnitLodSelection(TestContext* pCtx);
void testUnitLodBuild(TestContext* pCtx);
void testNearestEnemySearch(TestContext* pCtx);
