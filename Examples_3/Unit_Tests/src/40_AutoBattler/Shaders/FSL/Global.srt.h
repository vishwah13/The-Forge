#pragma once

// for low end iOS devices, do not use Argument buffers
BEGIN_SRT_NO_AB(SrtData)
    BEGIN_SRT_SET(Persistent)
        DECL_TEXTURE(Persistent, Tex2D(float4), gUnitTexture)
        DECL_TEXTURE(Persistent, Tex2D(float4), gGroundTexture)
    END_SRT_SET(Persistent)
    BEGIN_SRT_SET(PerFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(FrameUniforms), gFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(BoneMatrices), gBoneMatrices)
        DECL_BUFFER(PerFrame, Buffer(UnitInstance), gUnitInstances)
        DECL_BUFFER(PerFrame, Buffer(BillboardInstance), gBillboards)
    END_SRT_SET(PerFrame)
END_SRT(SrtData)
