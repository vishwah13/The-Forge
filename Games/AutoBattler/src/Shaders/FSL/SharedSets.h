// Persistent and PerFrame sets shared by the graphics SRT (Global.srt.h) and the cull SRT (Cull.srt.h),
// so one pair of descriptor sets serves both. Included inside BEGIN_SRT, so no include guard.
    BEGIN_SRT_SET(Persistent)
        DECL_TEXTURE(Persistent, Tex2D(float4), gUnitTexture)
        DECL_TEXTURE(Persistent, Tex2D(float4), gGroundTexture)
        DECL_BUFFER(Persistent, Buffer(float4x4), gBonePalettes) // rig joint poses, every baked frame
        DECL_BUFFER(Persistent, Buffer(SkinJoint), gSkinJoints)  // per unit mesh joint
    END_SRT_SET(Persistent)
    BEGIN_SRT_SET(PerFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(FrameUniforms), gFrame)
        DECL_BUFFER(PerFrame, Buffer(UnitInstance), gUnitInstances)
        DECL_BUFFER(PerFrame, Buffer(BillboardInstance), gBillboards)
    END_SRT_SET(PerFrame)
