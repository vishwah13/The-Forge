#pragma once

#include "../../../../Common_3/Game/ThirdParty/OpenSource/flecs/flecs.h"

// Flecs components of the battle simulation. Ids are defined (ECS_COMPONENT_DECLARE) once in Sim.cpp and
// registered per world with AB_DEFINE_COMPONENT.
// Components
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
    float    animTime; // animation clock, offset per unit so crowds are not in lockstep
    uint32_t moving;   // walked this frame (drives the walk vs idle clip)
    uint32_t attacking; // in range of its target this frame (drives the attack clip)
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

extern ECS_COMPONENT_DECLARE(Position);
extern ECS_COMPONENT_DECLARE(Unit);
extern ECS_COMPONENT_DECLARE(Health);
extern ECS_COMPONENT_DECLARE(Attack);
extern ECS_COMPONENT_DECLARE(GridSlot);
extern ECS_COMPONENT_DECLARE(Projectile);

// Component ids are globals; reset them so each new world registers fresh ids.
#define AB_DEFINE_COMPONENT(world, T) \
    ecs_id(T) = 0;                    \
    ECS_COMPONENT_DEFINE(world, T)
