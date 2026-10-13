#include "AutoBattlerSim.h"

#include <math.h>
#include <string.h>

#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"
#include "../../../../Common_3/Utilities/Interfaces/ILog.h"

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

const UnitStats gUnitStats[UNIT_TYPE_COUNT] = {
    // name     side cost  hp      damage cooldown range speed scale
    { "Melee", 5, 2, 100.0f, 15.0f, 0.8f, 1.2f, 3.0f, 1.0f },
    { "Ranged", 4, 3, 60.0f, 12.0f, 1.0f, 8.0f, 2.5f, 0.9f },
    { "Tank", 3, 4, 250.0f, 8.0f, 1.2f, 1.4f, 1.8f, 1.3f },
};

static const float RETARGET_INTERVAL = 0.25f;

//--------------------------------------------------------------------------------------------
// Components
//--------------------------------------------------------------------------------------------
struct Position
{
    float x, z;
};

struct Unit
{
    uint8_t  team;
    uint8_t  type;
    uint16_t cell; // board cell index, 0xFFFF when not spawned from the board
    float    radius;
    float    yaw;
};

struct Health
{
    float hp, maxHp, hitFlash;
};

struct Attack
{
    ecs_entity_t target;
    float        timer;
    float        retargetTimer;
    uint32_t     inRange;
};

struct GridSlot
{
    uint32_t slot; // index into this frame's grid snapshot
};

struct Projectile
{
    ecs_entity_t target;
    float        x, y, z;
    float        damage;
};

ECS_COMPONENT_DECLARE(Position);
ECS_COMPONENT_DECLARE(Unit);
ECS_COMPONENT_DECLARE(Health);
ECS_COMPONENT_DECLARE(Attack);
ECS_COMPONENT_DECLARE(GridSlot);
ECS_COMPONENT_DECLARE(Projectile);

// Component ids are globals; reset them so each new world registers fresh ids.
#define AB_DEFINE_COMPONENT(world, T) \
    ecs_id(T) = 0;                    \
    ECS_COMPONENT_DEFINE(world, T)

struct BattleSim
{
    ecs_world_t* pWorld;
    ecs_query_t* pUnitQuery; // Position, Unit, Health
    ecs_query_t* pProjectileQuery;
    int8_t       mBoard[ARENA_CELLS * ARENA_CELLS]; // UnitType per cell, UNIT_NONE when empty
    Phase        mPhase;
    int32_t      mRound;
    int32_t      mGold;
    float        mBattleTime;
    float        mResultTime;
    uint32_t     mProjectileCount;
    uint32_t     mSpawnCounter;
    const char*  pMessage;
};

//--------------------------------------------------------------------------------------------
// Board helpers
//--------------------------------------------------------------------------------------------
static bool cellInArena(int32_t x, int32_t z) { return x >= 0 && x < ARENA_CELLS && z >= 0 && z < ARENA_CELLS; }

static int32_t cellIndex(int32_t x, int32_t z) { return z * ARENA_CELLS + x; }

static uint32_t squadSize(UnitType type)
{
    const uint32_t side = (uint32_t)gUnitStats[type].mSquadSide;
    return side * side;
}

static uint32_t boardUnitCount(const BattleSim* p)
{
    uint32_t units = 0;
    for (int32_t i = 0; i < ARENA_CELLS * ARENA_CELLS; ++i)
        if (p->mBoard[i] != UNIT_NONE)
            units += squadSize((UnitType)p->mBoard[i]);
    return units;
}

bool worldToCell(float x, float z, int32_t* pCellX, int32_t* pCellZ)
{
    // Written as a positive test so NaN is rejected too.
    if (!(x >= 0.0f && x < ARENA_SIZE && z >= 0.0f && z < ARENA_SIZE))
        return false;
    const int32_t cellX = (int32_t)(x / CELL_SIZE);
    const int32_t cellZ = (int32_t)(z / CELL_SIZE);
    *pCellX = cellX < ARENA_CELLS ? cellX : ARENA_CELLS - 1;
    *pCellZ = cellZ < ARENA_CELLS ? cellZ : ARENA_CELLS - 1;
    return true;
}

float2 cellCenter(int32_t cellX, int32_t cellZ) { return float2(((float)cellX + 0.5f) * CELL_SIZE, ((float)cellZ + 0.5f) * CELL_SIZE); }

bool rayToCell(const float3& origin, const float3& dir, int32_t* pCellX, int32_t* pCellZ)
{
    if (!(dir.y < -1e-4f)) // parallel to the ground or pointing up
        return false;
    const float t = -origin.y / dir.y;
    if (t < 0.0f)
        return false;
    return worldToCell(origin.x + dir.x * t, origin.z + dir.z * t, pCellX, pCellZ);
}

//--------------------------------------------------------------------------------------------
// Spawning
//--------------------------------------------------------------------------------------------
static void spawnUnit(BattleSim* p, Team team, UnitType type, float x, float z, uint16_t cell)
{
    const UnitStats&   stats = gUnitStats[type];
    const ecs_entity_t e = ecs_new(p->pWorld);

    const Position pos = { x, z };
    const Unit     unit = { (uint8_t)team, (uint8_t)type, cell, UNIT_RADIUS * stats.mScale, team == TEAM_PLAYER ? 0.0f : PI };
    const Health   health = { stats.mHp, stats.mHp, 0.0f };
    // Stagger retargeting so only a slice of the army searches each frame.
    const Attack   attack = { 0, 0.0f, RETARGET_INTERVAL * (float)(p->mSpawnCounter++ % 16u) / 16.0f, 0u };
    const GridSlot slot = { UINT32_MAX };

    ecs_set(p->pWorld, e, Position, pos);
    ecs_set(p->pWorld, e, Unit, unit);
    ecs_set(p->pWorld, e, Health, health);
    ecs_set(p->pWorld, e, Attack, attack);
    ecs_set(p->pWorld, e, GridSlot, slot);
}

static void spawnSquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const int32_t side = gUnitStats[type].mSquadSide;
    const float   spacing = CELL_SIZE / (float)side;
    const Team    team = cellZ < PLAYER_ROWS ? TEAM_PLAYER : TEAM_ENEMY;
    for (int32_t j = 0; j < side; ++j)
        for (int32_t i = 0; i < side; ++i)
            spawnUnit(p, team, type, (float)cellX * CELL_SIZE + ((float)i + 0.5f) * spacing,
                      (float)cellZ * CELL_SIZE + ((float)j + 0.5f) * spacing, (uint16_t)cellIndex(cellX, cellZ));
}

// Units on the field always mirror the board while not in battle.
static void rebuildFromBoard(BattleSim* p)
{
    ecs_delete_with(p->pWorld, ecs_id(Unit));
    ecs_delete_with(p->pWorld, ecs_id(Projectile));
    p->mProjectileCount = 0;
    for (int32_t z = 0; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
                spawnSquad(p, x, z, (UnitType)p->mBoard[cellIndex(x, z)]);
}

static uint32_t nextRandom(uint32_t* pState)
{
    uint32_t s = *pState;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    *pState = s;
    return s;
}

// Fills the enemy half for the current round. Seeded by the round so waves are repeatable.
static void generateWave(BattleSim* p)
{
    for (int32_t z = PLAYER_ROWS; z < ARENA_CELLS; ++z)
        for (int32_t x = 0; x < ARENA_CELLS; ++x)
            p->mBoard[cellIndex(x, z)] = UNIT_NONE;

    uint32_t state = 0x9E3779B9u ^ ((uint32_t)p->mRound * 2654435761u);
    if (state == 0)
        state = 1;
    int32_t  budget = 6 + 4 * p->mRound;
    uint32_t units = boardUnitCount(p);
    for (int32_t attempt = 0; attempt < 256 && budget >= gUnitStats[UNIT_MELEE].mCost; ++attempt)
    {
        const UnitType type = (UnitType)(nextRandom(&state) % (uint32_t)UNIT_TYPE_COUNT);
        const int32_t  x = (int32_t)(nextRandom(&state) % (uint32_t)ARENA_CELLS);
        const int32_t  z = PLAYER_ROWS + (int32_t)(nextRandom(&state) % (uint32_t)(ARENA_CELLS - PLAYER_ROWS));
        if (gUnitStats[type].mCost > budget || p->mBoard[cellIndex(x, z)] != UNIT_NONE)
            continue;
        if (units + squadSize(type) > MAX_UNITS)
            break;
        p->mBoard[cellIndex(x, z)] = type;
        budget -= gUnitStats[type].mCost;
        units += squadSize(type);
    }
}

//--------------------------------------------------------------------------------------------
// Lifetime
//--------------------------------------------------------------------------------------------
BattleSim* simCreate(uint32_t threadCount)
{
    static bool sEcsInitialized = false;
    if (!sEcsInitialized)
    {
        initEntityComponentSystem();
        ecs_log_set_level(-2);
        sEcsInitialized = true;
    }

    BattleSim* p = (BattleSim*)tf_calloc(1, sizeof(BattleSim));
    p->pWorld = ecs_init();
    ecs_set_threads(p->pWorld, (int32_t)(threadCount > 0 ? threadCount : 1));

    AB_DEFINE_COMPONENT(p->pWorld, Position);
    AB_DEFINE_COMPONENT(p->pWorld, Unit);
    AB_DEFINE_COMPONENT(p->pWorld, Health);
    AB_DEFINE_COMPONENT(p->pWorld, Attack);
    AB_DEFINE_COMPONENT(p->pWorld, GridSlot);
    AB_DEFINE_COMPONENT(p->pWorld, Projectile);

    ecs_query_desc_t unitQuery = {};
    unitQuery.terms[0].id = ecs_id(Position);
    unitQuery.terms[1].id = ecs_id(Unit);
    unitQuery.terms[2].id = ecs_id(Health);
    p->pUnitQuery = ecs_query_init(p->pWorld, &unitQuery);

    ecs_query_desc_t projectileQuery = {};
    projectileQuery.terms[0].id = ecs_id(Projectile);
    p->pProjectileQuery = ecs_query_init(p->pWorld, &projectileQuery);

    simRestart(p);
    return p;
}

void simDestroy(BattleSim* p)
{
    if (!p)
        return;
    ecs_query_fini(p->pUnitQuery);
    ecs_query_fini(p->pProjectileQuery);
    ecs_fini(p->pWorld);
    tf_free(p);
}

void simRestart(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    p->mRound = 1;
    p->mGold = START_GOLD;
    p->mBattleTime = 0.0f;
    p->mResultTime = 0.0f;
    p->pMessage = "";
    generateWave(p);
    rebuildFromBoard(p);
    p->mPhase = PHASE_PREP;
}

//--------------------------------------------------------------------------------------------
// Board editing
//--------------------------------------------------------------------------------------------
static const char* buyRefusal(const BattleSim* p, int32_t x, int32_t z, UnitType type)
{
    if (p->mPhase != PHASE_PREP)
        return "Squads can only be placed during prep";
    if (type < 0 || type >= UNIT_TYPE_COUNT)
        return "Pick a unit type";
    if (!cellInArena(x, z) || z >= PLAYER_ROWS)
        return "Place squads on your half (bottom)";
    if (p->mBoard[cellIndex(x, z)] != UNIT_NONE)
        return "That cell already has a squad";
    if (p->mGold < gUnitStats[type].mCost)
        return "Not enough gold";
    if (boardUnitCount(p) + squadSize(type) > MAX_UNITS)
        return "Unit cap reached";
    return NULL;
}

bool simCanBuySquad(const BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type) { return buyRefusal(p, cellX, cellZ, type) == NULL; }

bool simBuySquad(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    const char* pRefusal = buyRefusal(p, cellX, cellZ, type);
    if (pRefusal)
    {
        p->pMessage = pRefusal;
        return false;
    }
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    p->mGold -= gUnitStats[type].mCost;
    p->pMessage = "";
    spawnSquad(p, cellX, cellZ, type);
    return true;
}

bool simSellSquad(BattleSim* p, int32_t cellX, int32_t cellZ)
{
    if (p->mPhase != PHASE_PREP)
    {
        p->pMessage = "Squads can only be sold during prep";
        return false;
    }
    if (!cellInArena(cellX, cellZ) || cellZ >= PLAYER_ROWS || p->mBoard[cellIndex(cellX, cellZ)] == UNIT_NONE)
    {
        p->pMessage = "Nothing of yours to sell there";
        return false;
    }
    p->mGold += gUnitStats[p->mBoard[cellIndex(cellX, cellZ)]].mCost;
    p->mBoard[cellIndex(cellX, cellZ)] = UNIT_NONE;
    p->pMessage = "";
    rebuildFromBoard(p);
    return true;
}

UnitType simGetCell(const BattleSim* p, int32_t cellX, int32_t cellZ)
{
    return cellInArena(cellX, cellZ) ? (UnitType)p->mBoard[cellIndex(cellX, cellZ)] : UNIT_NONE;
}

//--------------------------------------------------------------------------------------------
// Queries
//--------------------------------------------------------------------------------------------
Phase simGetPhase(const BattleSim* p) { return p->mPhase; }

int32_t simGetRound(const BattleSim* p) { return p->mRound; }

int32_t simGetGold(const BattleSim* p) { return p->mGold; }

const char* simGetMessage(const BattleSim* p) { return p->pMessage; }

uint32_t simGetUnitCount(BattleSim* p, Team team)
{
    uint32_t   count = 0;
    ecs_iter_t it = ecs_query_iter(p->pWorld, p->pUnitQuery);
    while (ecs_query_next(&it))
    {
        const Unit* unit = ecs_field(&it, Unit, 1);
        for (int32_t i = 0; i < it.count; ++i)
            if (unit[i].team == team)
                ++count;
    }
    return count;
}

//--------------------------------------------------------------------------------------------
// Test hooks
//--------------------------------------------------------------------------------------------
bool simTestSetCell(BattleSim* p, int32_t cellX, int32_t cellZ, UnitType type)
{
    if (!cellInArena(cellX, cellZ))
        return false;
    const int8_t previous = p->mBoard[cellIndex(cellX, cellZ)];
    p->mBoard[cellIndex(cellX, cellZ)] = type;
    if (boardUnitCount(p) > MAX_UNITS)
    {
        p->mBoard[cellIndex(cellX, cellZ)] = previous;
        return false;
    }
    rebuildFromBoard(p);
    return true;
}

void simTestClearBoard(BattleSim* p)
{
    memset(p->mBoard, UNIT_NONE, sizeof(p->mBoard));
    rebuildFromBoard(p);
}

void simTestSetGold(BattleSim* p, int32_t gold) { p->mGold = gold; }
