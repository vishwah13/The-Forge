#include "SimInternal.h"

#include <math.h>

#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

// Single-threaded: damages other entities and spawns projectiles.
static void AttackSystem(ecs_iter_t* it)
{
    BattleSim*      p = (BattleSim*)it->ctx;
    ScopedUSec      timer(&p->mTimings.mAttackUs);
    const Position* pos = ecs_field(it, Position, 0);
    const Unit*     unit = ecs_field(it, Unit, 1);
    Attack*         attack = ecs_field(it, Attack, 2);

    for (int32_t i = 0; i < it->count; ++i)
    {
        Attack& a = attack[i];
        a.timer -= it->delta_time;
        if (a.timer < 0.0f)
            a.timer = 0.0f;
        if (a.timer > 0.0f || !a.inRange || !a.target || !ecs_is_alive(it->world, a.target))
            continue;

        const UnitStats& stats = gUnitStats[unit[i].type];
        a.timer = stats.mCooldown;
        if (unit[i].type == UNIT_RANGED)
        {
            if (p->mProjectileCount >= MAX_PROJECTILES)
                continue;
            const Projectile   projectile = { a.target, pos[i].x, 1.2f * stats.mScale, pos[i].z, stats.mDamage };
            const ecs_entity_t e = ecs_new(it->world);
            ecs_set(it->world, e, Projectile, projectile);
            ++p->mProjectileCount;
        }
        else
        {
            applyDamage(it->world, a.target, stats.mDamage);
        }
    }
}

static void ProjectileSystem(ecs_iter_t* it)
{
    BattleSim*  p = (BattleSim*)it->ctx;
    ScopedUSec  timer(&p->mTimings.mProjectileUs);
    Projectile* projectile = ecs_field(it, Projectile, 0);
    const float step = PROJECTILE_SPEED * it->delta_time;

    for (int32_t i = 0; i < it->count; ++i)
    {
        Projectile& pr = projectile[i];
        if (!pr.target || !ecs_is_alive(it->world, pr.target))
        {
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        const Position* pTargetPos = ecs_get(it->world, pr.target, Position);
        const float     dx = pTargetPos->x - pr.x;
        const float     dz = pTargetPos->z - pr.z;
        const float     dist = sqrtf(dx * dx + dz * dz);
        if (dist <= step + 0.3f)
        {
            applyDamage(it->world, pr.target, pr.damage);
            ecs_delete(it->world, it->entities[i]);
            --p->mProjectileCount;
            continue;
        }
        pr.x += dx / dist * step;
        pr.z += dz / dist * step;
    }
}

// Declaration order is execution order within a phase.
void combatRegisterSystems(BattleSim* p)
{
    addSystem(p, "AttackSystem", EcsOnUpdate, AttackSystem, false, ecs_id(Position), ecs_id(Unit), ecs_id(Attack));
    addSystem(p, "ProjectileSystem", EcsOnUpdate, ProjectileSystem, false, ecs_id(Projectile));
}
