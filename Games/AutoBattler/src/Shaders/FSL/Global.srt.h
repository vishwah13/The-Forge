#pragma once

// for low end iOS devices, do not use Argument buffers
BEGIN_SRT_NO_AB(SrtData)
#include "SharedSets.h"
    BEGIN_SRT_SET(PerBatch)
        DECL_BUFFER(PerBatch, Buffer(uint), gVisibleUnits) // one LOD's unit indices written by the cull pass
    END_SRT_SET(PerBatch)
END_SRT(SrtData)
