#include "SimInternal.h"


#include "../../../../Common_3/Utilities/Interfaces/IMemory.h"

// Systems
void applyDamage(ecs_world_t* pWorld, ecs_entity_t target, float damage)
{
    if (!ecs_is_alive(pWorld, target))
        return;
    Health* pHealth = ecs_get_mut(pWorld, target, Health);
    if (pHealth)
    {
        pHealth->hp -= damage;
        pHealth->hitFlash = 1.0f;
    }
}

static void HealthSystem(ecs_iter_t* it)
{
    ScopedUSec timer(&((BattleSim*)it->ctx)->mTimings.mHealthUs);
    Health*    health = ecs_field(it, Health, 0);
    for (int32_t i = 0; i < it->count; ++i)
    {
        health[i].hitFlash = health[i].hitFlash > 0.0f ? health[i].hitFlash - it->delta_time * 5.0f : 0.0f;
        if (health[i].hp <= 0.0f)
            ecs_delete(it->world, it->entities[i]);
    }
}

void healthRegisterSystems(BattleSim* p)
{
    addSystem(p, "HealthSystem", EcsPostUpdate, HealthSystem, false, ecs_id(Health));
}
