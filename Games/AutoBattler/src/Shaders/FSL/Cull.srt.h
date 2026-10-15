#pragma once

BEGIN_SRT_NO_AB(CullSrtData) // shares sets with SrtData, so the same argument buffer choice
#include "SharedSets.h"
    BEGIN_SRT_SET(PerBatch)
        DECL_RWBUFFER(PerBatch, RWBuffer(uint), gVisibleUnitsRW) // per LOD, indices of units that pass culling
        DECL_RWBUFFER(PerBatch, RWBuffer(uint), gUnitDrawArgsRW) // per LOD, indexed indirect draw arguments
    END_SRT_SET(PerBatch)
END_SRT(CullSrtData)
