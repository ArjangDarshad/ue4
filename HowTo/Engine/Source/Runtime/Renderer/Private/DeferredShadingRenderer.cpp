// Copyright 1998-2013 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	DeferredShadingRenderer.cpp: Top level rendering loop for deferred shading
=============================================================================*/

#include "RendererPrivate.h"
#include "EnginePrivate.h"
#include "ScenePrivate.h"
#include "ScreenRendering.h"
#include "SceneFilterRendering.h"
#include "VisualizeTexture.h"
#include "PostProcessReconstructAA.h"
#include "CompositionLighting.h"
#include "FXSystem.h"
#include "OneColorShader.h"

int32 GRenderMovableObjectsInDepthOnlyPass = 0;

/** Affects static draw lists so must reload level to propagate. */
static FAutoConsoleVariableRef CVarRenderMovableObjectsInDepthOnlyPass(
	TEXT("r.RenderMovableObjectsInDepthOnlyPass"),
	GRenderMovableObjectsInDepthOnlyPass,
	TEXT("Whether to render movable objects into the depth only pass.  Movable objects are typically not good occluders so this defaults to off."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	);

/*-----------------------------------------------------------------------------
	FDeferredShadingSceneRenderer
-----------------------------------------------------------------------------*/

FDeferredShadingSceneRenderer::FDeferredShadingSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
:	FSceneRenderer(InViewFamily, HitProxyConsumer)
,	TranslucentSelfShadowLayout(0, 0, 0, 0)
,	CachedTranslucentSelfShadowLightId(INDEX_NONE)
{
	if (FPlatformProperties::SupportsWindowedMode() == false)
	{
		bUseDepthOnlyPass = true;
	}
	else
	{
		bUseDepthOnlyPass = 
			// Use a depth only pass if we are using full blown HQ lightmaps
			// Otherwise base pass pixel shaders will be cheap and there will be little benefit to rendering a depth only pass
			(GSystemSettings.bAllowHighQualityLightMaps && ViewFamily.EngineShowFlags.Lighting);
	}

	static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("Compat.bUseDepthOnlyPass"));
	const int32 bUseDepthOnlyPassDesiredSetting = ICVar->GetInt();

	if (bUseDepthOnlyPassDesiredSetting == 0)
	{
		bUseDepthOnlyPass = false;
	}
	else if (bUseDepthOnlyPassDesiredSetting == 1)
	{
		bUseDepthOnlyPass = true;
	}

	// Shader complexity requires depth only pass to display masked material cost correctly
	if (ViewFamily.EngineShowFlags.ShaderComplexity)
	{
		bUseDepthOnlyPass = true;
	}

}

/** 
* Clears a view. 
*/
void FDeferredShadingSceneRenderer::ClearView()
{
	// Clear the G Buffer render targets
	const bool bClearBlack = Views[0].Family->EngineShowFlags.ShaderComplexity || Views[0].Family->EngineShowFlags.StationaryLightOverlap;
	GSceneRenderTargets.ClearGBufferTargets( bClearBlack ? FLinearColor(0,0,0,0) : Views[0].BackgroundColor );
}

namespace
{
	FVector4 ClearQuadVertices[4] = 
	{
		FVector4( -1.0f,  1.0f, 1.0f, 1.0f ),
		FVector4(  1.0f,  1.0f, 1.0f, 1.0f ),
		FVector4( -1.0f, -1.0f, 1.0f, 1.0f ),
		FVector4(  1.0f, -1.0f, 1.0f, 1.0f )
	};
}

extern FGlobalBoundShaderState GClearMRTBoundShaderState[8];

/** 
* Clears view where Z is still at the maximum value (ie no geometry rendered)
*/
void FDeferredShadingSceneRenderer::ClearGBufferAtMaxZ()
{
	// Assumes BeginRenderingSceneColor() has been called before this function
	SCOPED_DRAW_EVENT(ClearGBufferAtMaxZ, DEC_SCENE_ITEMS);

	// Clear the G Buffer render targets
	const bool bClearBlack = Views[0].Family->EngineShowFlags.ShaderComplexity || Views[0].Family->EngineShowFlags.StationaryLightOverlap;
	// Same clear color from RHIClearMRT
	FLinearColor ClearColors[6] = 
		{bClearBlack ? FLinearColor(0,0,0,0) : Views[0].BackgroundColor, FLinearColor(0.5f,0.5f,0.5f,0), FLinearColor(0,0,0,1), FLinearColor(0,0,0,0), FLinearColor(0,1,1,1), FLinearColor(1,1,1,1)};

	uint32 NumActiveRenderTargets = GSceneRenderTargets.GetNumGBufferTargets();
	
	TShaderMapRef<FOneColorVS> VertexShader(GetGlobalShaderMap());
	FOneColorPS* PixelShader = NULL; 

	// Assume for now all code path supports SM4, otherwise render target numbers are changed
	switch(NumActiveRenderTargets)
	{
	case 5:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<5> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	case 6:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<6> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	default:
	case 1:
		{
			TShaderMapRef<TOneColorPixelShaderMRT<1> > MRTPixelShader(GetGlobalShaderMap());
			PixelShader = *MRTPixelShader;
		}
		break;
	}

	// Opaque rendering, depth test but no depth writes
	RHISetRasterizerState( TStaticRasterizerState<FM_Solid,CM_None>::GetRHI() );
	RHISetBlendState(TStaticBlendStateWriteMask<>::GetRHI());
	// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
	RHISetDepthStencilState(TStaticDepthStencilState<false,CF_GreaterEqual>::GetRHI());

	// Clear each viewport by drawing background color at MaxZ depth
	for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
	{
		SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("ClearView%d"), ViewIndex);

		FViewInfo& View = Views[ViewIndex];

		// Set viewport for this view
		RHISetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);

		// Setup PS
		SetGlobalBoundShaderState(GClearMRTBoundShaderState[NumActiveRenderTargets - 1], GetVertexDeclarationFVector4(), *VertexShader, PixelShader);
		SetShaderValueArray(PixelShader->GetPixelShader(),PixelShader->ColorParameter, ClearColors, NumActiveRenderTargets);

		// Render quad
		RHIDrawPrimitiveUP(PT_TriangleStrip, 2, ClearQuadVertices, sizeof(ClearQuadVertices[0]) );
	}
}

bool FDeferredShadingSceneRenderer::RenderBasePassStaticDataMasked(FViewInfo& View)
{
	bool bDirty = false;

	{
		// Draw the scene's base pass draw lists.
		FScene::EBasePassDrawListType MaskedDrawType = FScene::EBasePass_Masked;
		{
			SCOPED_DRAW_EVENT(StaticMaskedNoLightmap, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassNoLightMapDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassSimpleDynamicLightingDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedVolumeIndirectLightingDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedPointIndirectLightingDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
		{
			SCOPED_DRAW_EVENT(StaticMaskedLightmapped, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassHighQualityLightMapDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassDistanceFieldShadowMapLightMapDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassLowQualityLightMapDrawList[MaskedDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
	}

	return bDirty;
}

bool FDeferredShadingSceneRenderer::RenderBasePassStaticDataDefault(FViewInfo& View)
{
	bool bDirty = false;

	{
		FScene::EBasePassDrawListType OpaqueDrawType = FScene::EBasePass_Default;
		{
			SCOPED_DRAW_EVENT(StaticOpaqueNoLightmap, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassNoLightMapDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassSimpleDynamicLightingDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedVolumeIndirectLightingDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassCachedPointIndirectLightingDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
		{
			SCOPED_DRAW_EVENT(StaticOpaqueLightmapped, DEC_SCENE_ITEMS);
			bDirty |= Scene->BasePassHighQualityLightMapDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassDistanceFieldShadowMapLightMapDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
			bDirty |= Scene->BasePassLowQualityLightMapDrawList[OpaqueDrawType].DrawVisible(View,View.StaticMeshVisibilityMap,View.StaticMeshBatchVisibility);
		}
	}

	return bDirty;
}

void FDeferredShadingSceneRenderer::SortBasePassStaticData(FVector ViewPosition)
{
	// If we're not using a depth only pass, sort the static draw list buckets roughly front to back, to maximize HiZ culling
	// Note that this is only a very rough sort, since it does not interfere with state sorting, and each list is sorted separately
	if (!bUseDepthOnlyPass)
	{
		SCOPE_CYCLE_COUNTER(STAT_SortStaticDrawLists);

		for (int32 DrawType = 0; DrawType < FScene::EBasePass_MAX; DrawType++)
		{
			Scene->BasePassNoLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassSimpleDynamicLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassCachedVolumeIndirectLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassCachedPointIndirectLightingDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassHighQualityLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassDistanceFieldShadowMapLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
			Scene->BasePassLowQualityLightMapDrawList[DrawType].SortFrontToBack(ViewPosition);
		}
	}
}

/**
* Renders the basepass for the static data of a given View.
*
* @return true if anything was rendered to scene color
*/
bool FDeferredShadingSceneRenderer::RenderBasePassStaticData(FViewInfo& View)
{
	bool bDirty = false;

	SCOPE_CYCLE_COUNTER(STAT_StaticDrawListDrawTime);

	// When using a depth-only pass, the default opaque geometry's depths are already
	// in the depth buffer at this point, so rendering masked next will already cull
	// as efficiently as it can, while also increasing the ZCull efficiency when
	// rendering the default opaque geometry afterward.
	if( bUseDepthOnlyPass )
	{
		bDirty |= RenderBasePassStaticDataMasked(View);
		bDirty |= RenderBasePassStaticDataDefault(View);
	}
	else
	{
		// Otherwise, in the case where we're not using a depth-only pre-pass, there
		// is an advantage to rendering default opaque first to help cull the more
		// expensive masked geometry.
		bDirty |= RenderBasePassStaticDataDefault(View);
		bDirty |= RenderBasePassStaticDataMasked(View);
	}

	return bDirty;
}

/**
* Renders the basepass for the dynamic data of a given DPG and View.
*
* @return true if anything was rendered to scene color
*/

bool FDeferredShadingSceneRenderer::RenderBasePassDynamicData(FViewInfo& View)
{
	bool bDirty=0;

	if( !View.Family->EngineShowFlags.CompositeEditorPrimitives )
	{
		// Draw the base pass for the view's batched mesh elements.
		bDirty = DrawViewElements<FBasePassOpaqueDrawingPolicyFactory>(View,FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet), SDPG_World, true) || bDirty;

		// Draw the view's batched simple elements(lines, sprites, etc).
		bDirty = View.BatchedViewElements.Draw(View.ViewProjectionMatrix, View.ViewRect.Width(), View.ViewRect.Height(), false) || bDirty;
		
		// Draw foreground objects last
		bDirty = DrawViewElements<FBasePassOpaqueDrawingPolicyFactory>(View,FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet), SDPG_Foreground, true) || bDirty;

		// Draw the view's batched simple elements(lines, sprites, etc).
		bDirty = View.TopBatchedViewElements.Draw(View.ViewProjectionMatrix, View.ViewRect.Width(), View.ViewRect.Height(), false) || bDirty;

	}

	return bDirty;
}

/**
* Renders the basepass for a given DPG and View.
* @return true if anything was rendered to scene color
*/
bool FDeferredShadingSceneRenderer::RenderBasePass(FViewInfo& View)
{
	bool bDirty=0;

	// Render the base pass static data
	bDirty |= RenderBasePassStaticData(View);

	{
		SCOPE_CYCLE_COUNTER(STAT_DynamicPrimitiveDrawTime);
		SCOPED_DRAW_EVENT(Dynamic, DEC_SCENE_ITEMS);

		if( View.VisibleDynamicPrimitives.Num() > 0 )
		{
			// Draw the dynamic non-occluded primitives using a base pass drawing policy.
			TDynamicPrimitiveDrawer<FBasePassOpaqueDrawingPolicyFactory> Drawer(
				&View,FBasePassOpaqueDrawingPolicyFactory::ContextType(false, ESceneRenderTargetsMode::DontSet),true);
			for(int32 PrimitiveIndex = 0;PrimitiveIndex < View.VisibleDynamicPrimitives.Num();PrimitiveIndex++)
			{
				const FPrimitiveSceneInfo* PrimitiveSceneInfo = View.VisibleDynamicPrimitives[PrimitiveIndex];
				int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
				const FPrimitiveViewRelevance& PrimitiveViewRelevance = View.PrimitiveViewRelevanceMap[PrimitiveId];

				const bool bVisible = View.PrimitiveVisibilityMap[PrimitiveId];

				// Only draw the primitive if it's visible
				if( bVisible && 
					// only draw opaque and masked primitives if wireframe is disabled
					(PrimitiveViewRelevance.bOpaqueRelevance || ViewFamily.EngineShowFlags.Wireframe) &&
					PrimitiveViewRelevance.bRenderInMainPass
					)
				{
					FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
					Drawer.SetPrimitive(PrimitiveSceneInfo->Proxy);
					PrimitiveSceneInfo->Proxy->DrawDynamicElements(
						&Drawer,
						&View			
						);
				}
			}
			bDirty |= Drawer.IsDirty(); 
		}

		bDirty |= RenderBasePassDynamicData(View);
	}

	return bDirty;
}

/** Render the TexturePool texture */
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
void FDeferredShadingSceneRenderer::RenderVisualizeTexturePool()
{
	TRefCountPtr<IPooledRenderTarget> VisualizeTexturePool;

	/** Resolution for the texture pool visualizer texture. */
	enum
	{
		TexturePoolVisualizerSizeX = 280,
		TexturePoolVisualizerSizeY = 140,
	};

	FPooledRenderTargetDesc Desc(FPooledRenderTargetDesc::Create2DDesc(FIntPoint(TexturePoolVisualizerSizeX, TexturePoolVisualizerSizeY), PF_B8G8R8A8, TexCreate_None, TexCreate_None, false));
	GRenderTargetPool.FindFreeElement(Desc, VisualizeTexturePool, TEXT("VisualizeTexturePool"));
	
	uint32 Pitch;
	FColor* TextureData = (FColor*)RHILockTexture2D((FTexture2DRHIRef&)VisualizeTexturePool->GetRenderTargetItem().ShaderResourceTexture, 0, RLM_WriteOnly, Pitch, false );
	if(TextureData)
	{
		// clear with grey to get reliable background color
		FMemory::Memset(TextureData, 0x88, TexturePoolVisualizerSizeX * TexturePoolVisualizerSizeY * 4);
		RHIGetTextureMemoryVisualizeData(TextureData, TexturePoolVisualizerSizeX, TexturePoolVisualizerSizeY, Pitch, 4096);
	}

	RHIUnlockTexture2D((FTexture2DRHIRef&)VisualizeTexturePool->GetRenderTargetItem().ShaderResourceTexture, 0, false);

	FIntPoint RTExtent = GSceneRenderTargets.GetBufferSizeXY();

	FVector2D Tex00 = FVector2D(0, 0);
	FVector2D Tex11 = FVector2D(1, 1);

//todo	VisualizeTexture(*VisualizeTexturePool, ViewFamily.RenderTarget, FIntRect(0, 0, RTExtent.X, RTExtent.Y), RTExtent, 1.0f, 0.0f, 0.0f, Tex00, Tex11, 1.0f, false);
}
#endif


/** 
* Finishes the view family rendering.
*/
void FDeferredShadingSceneRenderer::RenderFinish()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	{
		static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VisualizeTexturePool"));

		if(ICVar->GetInt())
		{
			RenderVisualizeTexturePool();
		}
	}

#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	FSceneRenderer::RenderFinish();

	//grab the new transform out of the proxies for next frame
	if(ViewFamily.EngineShowFlags.MotionBlur)
	{
		Scene->MotionBlurInfoData.UpdateMotionBlurCache();
	}
}

/** 
* Renders the view family. 
*/
void FDeferredShadingSceneRenderer::Render()
{
	if(!ViewFamily.EngineShowFlags.Rendering)
	{
		return;
	}

	// Initialize global system textures (pass-through if already initialized).
	GSystemTextures.InitializeTextures();

	// Allocate the maximum scene render target space for the current view family.
	GSceneRenderTargets.Allocate(ViewFamily);

	// Find the visible primitives.
	InitViews();

	const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

	static IConsoleVariable* ClearMethodCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ClearSceneMethod"));
	bool bRequiresRHIClear = true;
	bool bRequiresFarZQuadClear = false;

	if (ClearMethodCVar)
	{
		switch (ClearMethodCVar->GetInt())
		{
		case 0: // No clear
			{
				bRequiresRHIClear = false;
				bRequiresFarZQuadClear = false;
				break;
			}
		
		case 1: // RHIClear
			{
				bRequiresRHIClear = true;
				bRequiresFarZQuadClear = false;
				break;
			}

		case 2: // Clear using far-z quad
			{
				bRequiresFarZQuadClear = true;
				bRequiresRHIClear = false;
				break;
			}
		}
	}

	// Always perform a full buffer clear for wireframe, shader complexity view mode, and stationary light overlap viewmode.
	if (bIsWireframe || ViewFamily.EngineShowFlags.ShaderComplexity || ViewFamily.EngineShowFlags.StationaryLightOverlap)
	{
		bRequiresRHIClear = true;
	}

	// force using occ queries for wireframe if rendering is parented or frozen in the first view
	check(Views.Num());
	#if (UE_BUILD_SHIPPING || UE_BUILD_TEST)
		const bool bIsViewFrozen = false;
		const bool bHasViewParent = false;
	#else
		const bool bIsViewFrozen = Views[0].State && ((FSceneViewState*)Views[0].State)->bIsFrozen;
		const bool bHasViewParent = Views[0].State && ((FSceneViewState*)Views[0].State)->HasViewParent();
	#endif
	const bool bIsOcclusionTesting = DoOcclusionQueries() && (!bIsWireframe || bIsViewFrozen || bHasViewParent);

	// Dynamic vertex and index buffers need to be committed before rendering.
	FGlobalDynamicVertexBuffer::Get().Commit();
	FGlobalDynamicIndexBuffer::Get().Commit();

	// Notify the FX system that the scene is about to be rendered.
	if (Scene->FXSystem)
	{
		Scene->FXSystem->PreRender();
	}

	// Draw the scene pre-pass, populating the scene depth buffer and HiZ
	RenderPrePass();
	
	// Clear scene color buffer if necessary.
	if ( bRequiresRHIClear )
	{
		ClearView();

		// Only clear once.
		bRequiresRHIClear = false;
	}

	// Begin rendering to scene color
	GSceneRenderTargets.BeginRenderingSceneColor(true);

	if(!Views[0].TemporalReprojectionPhase || !ViewFamily.EngineShowFlags.TemporalReprojection)
	{
		RenderBasePass();
	}

	if (bRequiresFarZQuadClear)
	{
		// Clears view by drawing quad at maximum Z
		// TODO: if all the platforms have fast color clears, we can replace this with an RHIClear.
		ClearGBufferAtMaxZ();

		bRequiresFarZQuadClear = false;
	}
	
	bool bCustomGBufferResolve = GRHIFeatureLevel >= ERHIFeatureLevel::SM5 && GSceneRenderTargets.GetGBufferMSAASampleCount() > 1;

	if (bCustomGBufferResolve)
	{
		// Resolve the GBuffers and the scene depth for deferred shading.
		FRenderingCompositePassContext CompositeContext(Views[0]);

		FRenderingCompositePass* Pass = CompositeContext.Graph.RegisterPass(new FRCPassPostProcessCustomGBufferResolve());
			
		CompositeContext.Root->AddDependency(Pass);
		CompositeContext.Process(TEXT("CustomBufferResolve"));
	}
	else
	{
		GSceneRenderTargets.ResolveSceneColor(FResolveRect(0, 0, ViewFamily.FamilySizeX, ViewFamily.FamilySizeY));
		GSceneRenderTargets.ResolveSceneDepthTexture();
	}

	// Resolve the scene depth to an auxiliary texture when SM3/SM4 is in use. This needs to happen so the auxiliary texture can be bound as a shader parameter
	// while the primary scene depth texture can be bound as the target. Simultaneously binding a single DepthStencil resource as a parameter and target
	// is unsupported in d3d feature level 10.
	if(!(GRHIFeatureLevel >= ERHIFeatureLevel::SM5) && GRHIFeatureLevel >= ERHIFeatureLevel::SM4)
	{
		GSceneRenderTargets.ResolveSceneDepthToAuxiliaryTexture();
	}
	
	RenderCustomDepthPass();

	// Notify the FX system that opaque primitives have been rendered and we now have a valid depth buffer.
	if (Scene->FXSystem && Views.IsValidIndex(0))
	{
		Scene->FXSystem->PostRenderOpaque(
			Views.GetTypedData(),
			GSceneRenderTargets.GetSceneDepthTexture(),
			GSceneRenderTargets.GetGBufferATexture()
			);
	}

	// Update the quarter-sized depth buffer with the current contents of the scene depth texture.
	// This needs to happen before occlusion tests, which makes use of the small depth buffer.
	UpdateDownsampledDepthSurface();

	GetRendererModule().RenderPostOpaqueExtensions();

	// Issue occlusion queries
	// This is done after the downsampled depth buffer is created so that it can be used for issuing queries
	if ( bIsOcclusionTesting )
	{
		BeginOcclusionTests();
	}
	
	// Render lighting.
	if (ViewFamily.EngineShowFlags.Lighting
		&& GRHIFeatureLevel >= ERHIFeatureLevel::SM4
		&& (!Views[0].TemporalReprojectionPhase || !ViewFamily.EngineShowFlags.TemporalReprojection)
		)
	{
		// e.g. ambient cubemaps, ambient occlusion, deferred decals
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView,Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			GCompositionLighting.Process(Views[ViewIndex]);
		}
		
		// Clear the translucent lighting volumes before we accumulate
		ClearTranslucentVolumeLighting();

		if (ViewFamily.EngineShowFlags.DirectLighting)
		{
			RenderLights();
		}

		InjectAmbientCubemapTranslucentVolumeLighting();

		CompositeIndirectTranslucentVolumeLighting();

		// Filter the translucency lighting volume now that it is complete
		FilterTranslucentVolumeLighting();

		// Render reflections that only operate on opaque pixels
		RenderDeferredReflections();
	}

	if( ViewFamily.EngineShowFlags.StationaryLightOverlap &&
		GRHIFeatureLevel >= ERHIFeatureLevel::SM4)
	{
		RenderStationaryLightOverlap();
	}

	FLightShaftsOutput LightShaftOutput;

	// Draw Lightshafts
	if (ViewFamily.EngineShowFlags.LightShafts)
	{
		LightShaftOutput = RenderLightShaftOcclusion();
	}

	// Draw atmosphere
	if(ShouldRenderAtmosphere(ViewFamily))
	{
		if (Scene->AtmosphericFog)
		{
			// Update RenderFlag based on LightShaftTexture is valid or not
			if (LightShaftOutput.bRendered)
			{
				Scene->AtmosphericFog->RenderFlag &= EAtmosphereRenderFlag::E_LightShaftMask;
			}
			else
			{
				Scene->AtmosphericFog->RenderFlag |= EAtmosphereRenderFlag::E_DisableLightShaft;
			}
#if WITH_EDITOR
			if (Scene->bIsEditorScene)
			{
				// Precompute Atmospheric Textures
				Scene->AtmosphericFog->PrecomputeTextures(Views.GetTypedData(), &ViewFamily);
			}
#endif
			RenderAtmosphere(LightShaftOutput);
		}
	}

	// Draw fog.
	if(ShouldRenderFog(ViewFamily))
	{
		RenderFog(LightShaftOutput);
	}

	// No longer needed, release
	LightShaftOutput.LightShaftOcclusion = NULL;

	// Draw translucency.
	if(ViewFamily.EngineShowFlags.Translucency)
	{
		SCOPE_CYCLE_COUNTER(STAT_TranslucencyDrawTime);

		if(ViewFamily.EngineShowFlags.Refraction)
		{
			// to apply refraction effect by distorting the scene color
			RenderDistortion();
		}
		RenderTranslucency();
	}

	if (ViewFamily.EngineShowFlags.LightShafts)
	{
		RenderLightShaftBloom();
	}

	// Resolve the scene color for post processing.
	if(!bCustomGBufferResolve)
	{
		GSceneRenderTargets.ResolveSceneColor(FResolveRect(0, 0, ViewFamily.FamilySizeX, ViewFamily.FamilySizeY));
	}

	// Finish rendering for each view.
	{
		SCOPED_DRAW_EVENT(FinishRendering, DEC_SCENE_ITEMS);
		SCOPE_CYCLE_COUNTER(STAT_FinishRenderViewTargetTime);
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{	
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			FinishRenderViewTarget(&Views[ViewIndex]);
		}
	}

	RenderFinish();
}

/** Renders the scene's prepass and occlusion queries */
bool FDeferredShadingSceneRenderer::RenderPrePass()
{
	SCOPED_DRAW_EVENT(PrePass, DEC_SCENE_ITEMS);
	SCOPE_CYCLE_COUNTER(STAT_DepthDrawTime);

	bool bDirty = false;

	GSceneRenderTargets.BeginRenderingPrePass();

	// Clear the depth buffer.
	// Note, this is a reversed Z depth surface, so 0.0f is the far plane.
	RHIClear(false,FLinearColor::Black,true,0.0f,true,0, FIntRect());

	// Draw a depth pass to avoid overdraw in the other passes.
	if(bUseDepthOnlyPass)
	{
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);

			const FViewInfo& View = Views[ViewIndex];

			// Disable color writes, enable depth tests and writes.
			RHISetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
			// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
			RHISetDepthStencilState(TStaticDepthStencilState<true,CF_GreaterEqual>::GetRHI());
			RHISetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);

			// Draw the static occluder primitives using a depth drawing policy.
			{
				// Draw opaque occluders which support a separate position-only
				// vertex buffer to minimize vertex fetch bandwidth, which is
				// often the bottleneck during the depth only pass.
				SCOPED_DRAW_EVENT(PosOnlyOpaque, DEC_SCENE_ITEMS);
				bDirty |= Scene->PositionOnlyDepthDrawList.DrawVisible(View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility);
			}
			{
				// Draw opaque occluders, using double speed z where supported.
				SCOPED_DRAW_EVENT(Opaque, DEC_SCENE_ITEMS);
				bDirty |= Scene->DepthDrawList.DrawVisible(View,View.StaticMeshOccluderMap,View.StaticMeshBatchVisibility);
			}

			const bool bShowShaderComplexity = View.Family->EngineShowFlags.ShaderComplexity;
			// Only render masked materials if scene depth needs to be up to date after the prepass, or if shader complexity is enabled
			const EDepthDrawingMode DepthDrawingMode = 
				bShowShaderComplexity
				? DDM_AllOccluders : DDM_NonMaskedOnly;
			// Draw the dynamic occluder primitives using a depth drawing policy.
			TDynamicPrimitiveDrawer<FDepthDrawingPolicyFactory> Drawer(&View,FDepthDrawingPolicyFactory::ContextType(DepthDrawingMode),true);
			{
				SCOPED_DRAW_EVENT(Dynamic, DEC_SCENE_ITEMS);
				for(int32 PrimitiveIndex = 0;PrimitiveIndex < View.VisibleDynamicPrimitives.Num();PrimitiveIndex++)
				{
					const FPrimitiveSceneInfo* PrimitiveSceneInfo = View.VisibleDynamicPrimitives[PrimitiveIndex];
					int32 PrimitiveId = PrimitiveSceneInfo->GetIndex();
					const FPrimitiveViewRelevance& PrimitiveViewRelevance = View.PrimitiveViewRelevanceMap[PrimitiveId];
					extern float GMinScreenRadiusForDepthPrepass;
					const float LODFactorDistanceSquared = (PrimitiveSceneInfo->Proxy->GetBounds().Origin - View.ViewMatrices.ViewOrigin).SizeSquared() * FMath::Square(View.LODDistanceFactor);

					// Only render primitives marked as occluders
					bool bShouldUseAsOccluder = PrimitiveSceneInfo->Proxy->ShouldUseAsOccluder()
						// Only render static objects unless movable are requested
						&& (!PrimitiveSceneInfo->Proxy->IsMovable() || GRenderMovableObjectsInDepthOnlyPass)
						// And if the primitive takes up enough screen space to be a good occluder, or shader complexity is enabled
						&& (FMath::Square(PrimitiveSceneInfo->Proxy->GetBounds().SphereRadius) > GMinScreenRadiusForDepthPrepass * GMinScreenRadiusForDepthPrepass * LODFactorDistanceSquared);

					// All primitives should be rendered when shader complexity view mode is enabled.
					bShouldUseAsOccluder |= bShowShaderComplexity;

					// Only render opaque primitives marked as occluders
					if (bShouldUseAsOccluder && PrimitiveViewRelevance.bOpaqueRelevance && PrimitiveViewRelevance.bRenderInMainPass)
					{
						FScopeCycleCounter Context(PrimitiveSceneInfo->Proxy->GetStatId());
						Drawer.SetPrimitive(PrimitiveSceneInfo->Proxy);
						PrimitiveSceneInfo->Proxy->DrawDynamicElements(
							&Drawer,
							&View
							);
					}
				}
			}
			bDirty |= Drawer.IsDirty();
		}
	}

	GSceneRenderTargets.FinishRenderingPrePass();

	return bDirty;
}

/**
 * Renders the scene's base pass 
 * @return true if anything was rendered
 */
bool FDeferredShadingSceneRenderer::RenderBasePass()
{
	bool bDirty = false;

	if(ViewFamily.EngineShowFlags.LightMapDensity && AllowDebugViewmodes())
	{
		// Override the base pass with the lightmap density pass if the viewmode is enabled.
		bDirty = RenderLightMapDensities();
	}
	else
	{
		SCOPED_DRAW_EVENT(BasePass, DEC_SCENE_ITEMS);
		SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);

		// Draw the scene's emissive and light-map color.
		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			SCOPED_CONDITIONAL_DRAW_EVENTF(EventView, Views.Num() > 1, DEC_SCENE_ITEMS, TEXT("View%d"), ViewIndex);
			FViewInfo& View = Views[ViewIndex];

			if (ViewFamily.EngineShowFlags.ShaderComplexity)
			{
				// Additive blending when shader complexity viewmode is enabled.
				RHISetBlendState(TStaticBlendState<CW_RGBA,BO_Add,BF_One,BF_One,BO_Add,BF_Zero,BF_One>::GetRHI());
				// Disable depth writes as we have a full depth prepass.
				RHISetDepthStencilState(TStaticDepthStencilState<false,CF_GreaterEqual>::GetRHI());
			}
			else
			{
				// Opaque blending for all G buffer targets, depth tests and writes.
				RHISetBlendState(TStaticBlendStateWriteMask<CW_RGBA, CW_RGBA, CW_RGBA, CW_RGBA>::GetRHI());
				// Note, this is a reversed Z depth surface, using CF_GreaterEqual.
				RHISetDepthStencilState(TStaticDepthStencilState<true,CF_GreaterEqual>::GetRHI());
			}
			RHISetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);

			bDirty |= RenderBasePass(View);
		}
	}

	return bDirty;
}

/** A simple pixel shader used on PC to read scene depth from scene color alpha and write it to a downsized depth buffer. */
class FDownsampleSceneDepthPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FDownsampleSceneDepthPS,Global);
public:

	static bool ShouldCache(EShaderPlatform Platform)
	{ 
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM3);
	}

	FDownsampleSceneDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer):
		FGlobalShader(Initializer)
	{
		SceneTextureParameters.Bind(Initializer.ParameterMap);
		ProjectionScaleBias.Bind(Initializer.ParameterMap,TEXT("ProjectionScaleBias"));
		SourceTexelOffsets01.Bind(Initializer.ParameterMap,TEXT("SourceTexelOffsets01"));
		SourceTexelOffsets23.Bind(Initializer.ParameterMap,TEXT("SourceTexelOffsets23"));
	}
	FDownsampleSceneDepthPS() {}

	void SetParameters(const FSceneView& View)
	{
		FGlobalShader::SetParameters(GetPixelShader(), View);

		// Used to remap view space Z (which is stored in scene color alpha) into post projection z and w so we can write z/w into the downsized depth buffer
		const FVector2D ProjectionScaleBiasValue(View.ViewMatrices.ProjMatrix.M[2][2], View.ViewMatrices.ProjMatrix.M[3][2]);
		SetShaderValue(GetPixelShader(), ProjectionScaleBias, ProjectionScaleBiasValue);

		FIntPoint BufferSize = GSceneRenderTargets.GetBufferSizeXY();

		const uint32 DownsampledBufferSizeX = BufferSize.X / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor();
		const uint32 DownsampledBufferSizeY = BufferSize.Y / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor();

		// Offsets of the four full resolution pixels corresponding with a low resolution pixel
		const FVector4 Offsets01(0.0f, 0.0f, 1.0f / DownsampledBufferSizeX, 0.0f);
		SetShaderValue(GetPixelShader(), SourceTexelOffsets01, Offsets01);
		const FVector4 Offsets23(0.0f, 1.0f / DownsampledBufferSizeY, 1.0f / DownsampledBufferSizeX, 1.0f / DownsampledBufferSizeY);
		SetShaderValue(GetPixelShader(), SourceTexelOffsets23, Offsets23);
		SceneTextureParameters.Set(GetPixelShader());
	}

	virtual bool Serialize(FArchive& Ar)
	{
		bool bShaderHasOutdatedParameters = FGlobalShader::Serialize(Ar);
		Ar << ProjectionScaleBias;
		Ar << SourceTexelOffsets01;
		Ar << SourceTexelOffsets23;
		Ar << SceneTextureParameters;
		return bShaderHasOutdatedParameters;
	}

	FShaderParameter ProjectionScaleBias;
	FShaderParameter SourceTexelOffsets01;
	FShaderParameter SourceTexelOffsets23;
	FSceneTextureShaderParameters SceneTextureParameters;
};

IMPLEMENT_SHADER_TYPE(,FDownsampleSceneDepthPS,TEXT("DownsampleDepthPixelShader"),TEXT("Main"),SF_Pixel);

FGlobalBoundShaderState DownsampleDepthBoundShaderState;

/** Updates the downsized depth buffer with the current full resolution depth buffer. */
void FDeferredShadingSceneRenderer::UpdateDownsampledDepthSurface()
{
	if (GSceneRenderTargets.UseDownsizedOcclusionQueries() && IsFeatureLevelSupported(GRHIShaderPlatform, ERHIFeatureLevel::SM3))
	{
		RHISetRenderTarget(NULL, GSceneRenderTargets.GetSmallDepthSurface());

		SCOPED_DRAW_EVENT(DownsampleDepth, DEC_SCENE_ITEMS);

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			// Set shaders and texture
			TShaderMapRef<FScreenVS> ScreenVertexShader(GetGlobalShaderMap());
			TShaderMapRef<FDownsampleSceneDepthPS> PixelShader(GetGlobalShaderMap());

			extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

			SetGlobalBoundShaderState(DownsampleDepthBoundShaderState, GFilterVertexDeclaration.VertexDeclarationRHI, *ScreenVertexShader, *PixelShader);

			RHISetBlendState(TStaticBlendState<CW_NONE>::GetRHI());
			RHISetRasterizerState(TStaticRasterizerState<FM_Solid,CM_None>::GetRHI());
			RHISetDepthStencilState(TStaticDepthStencilState<true,CF_Always>::GetRHI());

			PixelShader->SetParameters(View);

			const uint32 DownsampledX = FMath::Trunc(View.ViewRect.Min.X / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledY = FMath::Trunc(View.ViewRect.Min.Y / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledSizeX = FMath::Trunc(View.ViewRect.Width() / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());
			const uint32 DownsampledSizeY = FMath::Trunc(View.ViewRect.Height() / GSceneRenderTargets.GetSmallColorDepthDownsampleFactor());

			RHISetViewport(DownsampledX, DownsampledY, 0.0f, DownsampledX + DownsampledSizeX, DownsampledY + DownsampledSizeY, 1.0f);

			DrawDenormalizedQuad(
				0, 0,
				DownsampledSizeX, DownsampledSizeY,
				View.ViewRect.Min.X, View.ViewRect.Min.Y,
				View.ViewRect.Width(), View.ViewRect.Height(),
				FIntPoint(DownsampledSizeX, DownsampledSizeY),
				GSceneRenderTargets.GetBufferSizeXY()
				);
		}
	}
}

bool FDeferredShadingSceneRenderer::ShouldCompositeEditorPrimitives(const FViewInfo& View)
{
	// If the show flag is enabled and any elements that needed compositing were drawn then compositing should be done
	return View.Family->EngineShowFlags.CompositeEditorPrimitives && 
		( View.ViewMeshElements.Num() || View.TopViewMeshElements.Num() || View.BatchedViewElements.HasPrimsToDraw() || View.TopBatchedViewElements.HasPrimsToDraw() || View.VisibleEditorPrimitives.Num() );
}
