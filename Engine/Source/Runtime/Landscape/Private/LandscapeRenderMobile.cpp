// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
LandscapeRenderMobile.cpp: Landscape Rendering without using vertex texture fetch
=============================================================================*/

#include "LandscapeRenderMobile.h"
#include "ShaderParameterUtils.h"
#include "Serialization/BufferArchive.h"
#include "Serialization/MemoryReader.h"
#include "PrimitiveSceneInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "HAL/LowLevelMemTracker.h"
#include "MeshMaterialShader.h"
#include "Runtime/Renderer/Private/SceneCore.h"

//@StarLight code - BEGIN LandScapeInstance, Added by yanjianhong
#include "ProfilingDebugging/LoadTimeTracker.h"
#include "LandscapeLight.h"
#include "LandscapeDataAccess.h"
//@StarLight code - END LandScapeInstance, Added by yanjianhong

// Debug CVar for disabling the loading of landscape hole meshes
static TAutoConsoleVariable<int32> CVarMobileLandscapeHoleMesh(
	TEXT("r.Mobile.LandscapeHoleMesh"),
	1,
	TEXT("Set to 0 to skip loading of landscape hole meshes on mobile."),
	ECVF_Default);

bool FLandscapeVertexFactoryMobile::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	auto FeatureLevel = GetMaxSupportedFeatureLevel(Parameters.Platform);
	return (FeatureLevel == ERHIFeatureLevel::ES3_1) &&
		(Parameters.MaterialParameters.bIsUsedWithLandscape || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
}

void FLandscapeVertexFactoryMobile::InitRHI()
{
	// list of declaration items
	FVertexDeclarationElementList Elements;

	// position decls
	Elements.Add(AccessStreamComponent(MobileData.PositionComponent,0));

	if (MobileData.LODHeightsComponent.Num())
	{
		const int32 BaseAttribute = 1;
		for(int32 Index = 0;Index < MobileData.LODHeightsComponent.Num();Index++)
		{
			Elements.Add(AccessStreamComponent(MobileData.LODHeightsComponent[Index], BaseAttribute + Index));
		}
	}

	// create the actual device decls
	InitDeclaration(Elements);
}

/** Shader parameters for use with FLandscapeVertexFactory */
class FLandscapeVertexFactoryMobileVertexShaderParameters : public FVertexFactoryShaderParameters
{
	DECLARE_INLINE_TYPE_LAYOUT(FLandscapeVertexFactoryMobileVertexShaderParameters, NonVirtual);
public:
	/**
	* Bind shader constants by name
	* @param	ParameterMap - mapping of named shader constants to indices
	*/
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		TexCoordOffsetParameter.Bind(ParameterMap,TEXT("TexCoordOffset"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimeVS);

		const FLandscapeBatchElementParams* BatchElementParams = (const FLandscapeBatchElementParams*)BatchElement.UserData;
		check(BatchElementParams);

		const FLandscapeComponentSceneProxyMobile* SceneProxy = (const FLandscapeComponentSceneProxyMobile*)BatchElementParams->SceneProxy;
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeUniformShaderParameters>(),*BatchElementParams->LandscapeUniformShaderParametersResource);

		if (TexCoordOffsetParameter.IsBound())
		{
			FVector CameraLocalPos3D = SceneProxy->WorldToLocal.TransformPosition(InView->ViewMatrices.GetViewOrigin());

			FVector2D TexCoordOffset(
				CameraLocalPos3D.X + SceneProxy->SectionBase.X,
				CameraLocalPos3D.Y + SceneProxy->SectionBase.Y
			);
			ShaderBindings.Add(TexCoordOffsetParameter, TexCoordOffset);
		}

		if (SceneProxy->bRegistered)
		{
			ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeSectionLODUniformParameters>(), LandscapeRenderSystems.FindChecked(SceneProxy->LandscapeKey)->UniformBuffer);
		}
		else
		{
			ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeSectionLODUniformParameters>(), GNullLandscapeRenderSystemResources.UniformBuffer);
		}
			}

protected:
	LAYOUT_FIELD(FShaderParameter, TexCoordOffsetParameter);
};

/** Shader parameters for use with FLandscapeVertexFactory */
class FLandscapeVertexFactoryMobilePixelShaderParameters : public FLandscapeVertexFactoryPixelShaderParameters
{
	DECLARE_INLINE_TYPE_LAYOUT(FLandscapeVertexFactoryMobilePixelShaderParameters, NonVirtual);
public:
	/**
	* Bind shader constants by name
	* @param	ParameterMap - mapping of named shader constants to indices
	*/
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FLandscapeVertexFactoryPixelShaderParameters::Bind(ParameterMap);
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimePS);
		
		FLandscapeVertexFactoryPixelShaderParameters::GetElementShaderBindings(Scene, InView, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement, ShaderBindings, VertexStreams);
	}
};

/**
  * Shader parameters for use with FLandscapeFixedGridVertexFactory
  * Simple grid rendering (without dynamic lod blend) needs a simpler fixed setup.
  */
class FLandscapeFixedGridVertexFactoryMobileVertexShaderParameters : public FLandscapeVertexFactoryMobileVertexShaderParameters
{
public:
	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimeVS);

		const FLandscapeBatchElementParams* BatchElementParams = (const FLandscapeBatchElementParams*)BatchElement.UserData;
		check(BatchElementParams);
		const FLandscapeComponentSceneProxyMobile* SceneProxy = (const FLandscapeComponentSceneProxyMobile*)BatchElementParams->SceneProxy;
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeUniformShaderParameters>(), *BatchElementParams->LandscapeUniformShaderParametersResource);
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeFixedGridUniformShaderParameters>(), SceneProxy->LandscapeFixedGridUniformShaderParameters[BatchElementParams->CurrentLOD]);

		if (TexCoordOffsetParameter.IsBound())
		{
			ShaderBindings.Add(TexCoordOffsetParameter, FVector4(ForceInitToZero));
		}
	}
};

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactoryMobile, SF_Vertex, FLandscapeVertexFactoryMobileVertexShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeVertexFactoryMobile, SF_Pixel, FLandscapeVertexFactoryMobilePixelShaderParameters);

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactoryMobile, SF_Vertex, FLandscapeFixedGridVertexFactoryMobileVertexShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeFixedGridVertexFactoryMobile, SF_Pixel, FLandscapeVertexFactoryMobilePixelShaderParameters);

IMPLEMENT_VERTEX_FACTORY_TYPE(FLandscapeVertexFactoryMobile, "/Engine/Private/LandscapeVertexFactory.ush", true, true, true, false, false);
IMPLEMENT_VERTEX_FACTORY_TYPE_EX(FLandscapeFixedGridVertexFactoryMobile, "/Engine/Private/LandscapeVertexFactory.ush", true, true, true, false, false, true, false);

void FLandscapeFixedGridVertexFactoryMobile::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLandscapeVertexFactoryMobile::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	OutEnvironment.SetDefine(TEXT("FIXED_GRID"), TEXT("1"));
}
	
bool FLandscapeFixedGridVertexFactoryMobile::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return GetMaxSupportedFeatureLevel(Parameters.Platform) == ERHIFeatureLevel::ES3_1 &&
		(Parameters.MaterialParameters.bIsUsedWithLandscape || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
}

/**
* Initialize the RHI for this rendering resource
*/
void FLandscapeVertexBufferMobile::InitRHI()
{
	// create a static vertex buffer
	FRHIResourceCreateInfo CreateInfo;
	void* VertexDataPtr = nullptr;
	VertexBufferRHI = RHICreateAndLockVertexBuffer(VertexData.Num(), BUF_Static, CreateInfo, VertexDataPtr);

	// Copy stored platform data and free CPU copy
	FMemory::Memcpy(VertexDataPtr, VertexData.GetData(), VertexData.Num());
	VertexData.Empty();

	RHIUnlockVertexBuffer(VertexBufferRHI);
}

struct FLandscapeMobileHoleData
{
	FRawStaticIndexBuffer16or32Interface* IndexBuffer = nullptr;
	int32 NumHoleLods;
	int32 IndexBufferSize;
	int32 MinHoleIndex;
	int32 MaxHoleIndex;

	~FLandscapeMobileHoleData()
	{
		if (IndexBuffer != nullptr)
		{
			DEC_DWORD_STAT_BY(STAT_LandscapeHoleMem, IndexBuffer->GetResourceDataSize());
			IndexBuffer->ReleaseResource();
			delete IndexBuffer;
		}
	}
};

template <typename INDEX_TYPE>
void SerializeLandscapeMobileHoleData(FMemoryArchive& Ar, FLandscapeMobileHoleData& HoleData)
{
	Ar << HoleData.MinHoleIndex;
	Ar << HoleData.MaxHoleIndex;

	TArray<INDEX_TYPE> IndexData;
	Ar << HoleData.IndexBufferSize;
	IndexData.SetNumUninitialized(HoleData.IndexBufferSize);
	Ar.Serialize(IndexData.GetData(), HoleData.IndexBufferSize * sizeof(INDEX_TYPE));

	const bool bLoadHoleMeshData = HoleData.IndexBufferSize > 0 && CVarMobileLandscapeHoleMesh.GetValueOnGameThread();
	if (bLoadHoleMeshData)
	{
		FRawStaticIndexBuffer16or32<INDEX_TYPE>* IndexBuffer = new FRawStaticIndexBuffer16or32<INDEX_TYPE>(false);
		IndexBuffer->AssignNewBuffer(IndexData);
		HoleData.IndexBuffer = IndexBuffer;
		BeginInitResource(HoleData.IndexBuffer);
		INC_DWORD_STAT_BY(STAT_LandscapeHoleMem, HoleData.IndexBuffer->GetResourceDataSize());
	}
}

/**
 * Container for FLandscapeVertexBufferMobile that we can reference from a thread-safe shared pointer
 * while ensuring the vertex buffer is always destroyed on the render thread.
 **/
struct FLandscapeMobileRenderData
{
	FLandscapeVertexBufferMobile* VertexBuffer = nullptr;
	FLandscapeMobileHoleData* HoleData = nullptr;
	FOccluderVertexArraySP OccluderVerticesSP;

	FLandscapeMobileRenderData(const TArray<uint8>& InPlatformData)
	{
		FMemoryReader MemAr(InPlatformData);

		int32 NumHoleLods;
		MemAr << NumHoleLods;
		if (NumHoleLods > 0)
		{
			HoleData = new FLandscapeMobileHoleData;
			HoleData->NumHoleLods = NumHoleLods;

			bool b16BitIndices;
			MemAr << b16BitIndices;
			if (b16BitIndices)
			{
				SerializeLandscapeMobileHoleData<uint16>(MemAr, *HoleData);
			}
			else
			{
				SerializeLandscapeMobileHoleData<uint32>(MemAr, *HoleData);
			}
		}

		{
			int32 VertexCount;
			MemAr << VertexCount;
			TArray<uint8> VertexData;
			VertexData.SetNumUninitialized(VertexCount*sizeof(FLandscapeMobileVertex));
			MemAr.Serialize(VertexData.GetData(), VertexData.Num());
			VertexBuffer = new FLandscapeVertexBufferMobile(MoveTemp(VertexData));
		}
		
		{
			int32 NumOccluderVertices;
			MemAr << NumOccluderVertices;
			if (NumOccluderVertices > 0)
			{
				OccluderVerticesSP = MakeShared<FOccluderVertexArray, ESPMode::ThreadSafe>();
				OccluderVerticesSP->SetNumUninitialized(NumOccluderVertices);
				MemAr.Serialize(OccluderVerticesSP->GetData(), NumOccluderVertices * sizeof(FVector));

				INC_DWORD_STAT_BY(STAT_LandscapeOccluderMem, OccluderVerticesSP->GetAllocatedSize());
			}
		}
	}

	~FLandscapeMobileRenderData()
	{
		// Make sure the vertex buffer is always destroyed from the render thread 
		if (VertexBuffer != nullptr)
		{
			if (IsInRenderingThread())
			{
				delete VertexBuffer;
				delete HoleData;
			}
			else
			{
				FLandscapeVertexBufferMobile* InVertexBuffer = VertexBuffer;
				FLandscapeMobileHoleData* InHoleData = HoleData;
				ENQUEUE_RENDER_COMMAND(InitCommand)(
					[InVertexBuffer, InHoleData](FRHICommandListImmediate& RHICmdList)
				{
					delete InVertexBuffer;
					delete InHoleData;
				});
			}
		}

		if (OccluderVerticesSP.IsValid())
		{
			DEC_DWORD_STAT_BY(STAT_LandscapeOccluderMem, OccluderVerticesSP->GetAllocatedSize());
		}
	}
};

FLandscapeComponentSceneProxyMobile::FLandscapeComponentSceneProxyMobile(ULandscapeComponent* InComponent)
	: FLandscapeComponentSceneProxy(InComponent)
	, MobileRenderData(InComponent->PlatformData.GetRenderData())
{
	check(InComponent);
	
	check(InComponent->MobileMaterialInterfaces.Num() > 0);
	check(InComponent->MobileWeightmapTextures.Num() > 0);

	WeightmapTextures = InComponent->MobileWeightmapTextures;
	NormalmapTexture = InComponent->MobileWeightmapTextures[0];

#if WITH_EDITOR
	TArray<FWeightmapLayerAllocationInfo>& LayerAllocations = InComponent->MobileWeightmapLayerAllocations.Num() ? InComponent->MobileWeightmapLayerAllocations : InComponent->GetWeightmapLayerAllocations();
	LayerColors.Empty();
	for (auto& Allocation : LayerAllocations)
	{
		if (Allocation.LayerInfo != nullptr)
		{
			LayerColors.Add(Allocation.LayerInfo->LayerUsageDebugColor);
		}
	}
#endif
}

int32 FLandscapeComponentSceneProxyMobile::CollectOccluderElements(FOccluderElementsCollector& Collector) const
{
	if (MobileRenderData->OccluderVerticesSP.IsValid() && SharedBuffers->OccluderIndicesSP.IsValid())
	{
		Collector.AddElements(MobileRenderData->OccluderVerticesSP, SharedBuffers->OccluderIndicesSP, GetLocalToWorld());
		return 1;
	}

	return 0;
}

FLandscapeComponentSceneProxyMobile::~FLandscapeComponentSceneProxyMobile()
{
	if (VertexFactory)
	{
		delete VertexFactory;
		VertexFactory = NULL;
	}
}

SIZE_T FLandscapeComponentSceneProxyMobile::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FLandscapeComponentSceneProxyMobile::CreateRenderThreadResources()
{
	LLM_SCOPE(ELLMTag::Landscape);

	if (IsComponentLevelVisible())
	{
		RegisterNeighbors(this);
	}
	
	auto FeatureLevel = GetScene().GetFeatureLevel();
	
	// Use only index buffers from the shared buffers since the vertex buffers are unique per proxy on mobile
	SharedBuffers = FLandscapeComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey);
	if (SharedBuffers == nullptr)
	{
		int32 NumOcclusionVertices = MobileRenderData->OccluderVerticesSP.IsValid() ? MobileRenderData->OccluderVerticesSP->Num() : 0;
				
		SharedBuffers = new FLandscapeSharedBuffers(
			SharedBuffersKey, SubsectionSizeQuads, NumSubsections,
			GetScene().GetFeatureLevel(), false, NumOcclusionVertices);

		FLandscapeComponentSceneProxy::SharedBuffersMap.Add(SharedBuffersKey, SharedBuffers);
	}
	SharedBuffers->AddRef();

	// Init vertex buffer
	{
		check(MobileRenderData->VertexBuffer);
		MobileRenderData->VertexBuffer->InitResource();

		FLandscapeVertexFactoryMobile* LandscapeVertexFactory = new FLandscapeVertexFactoryMobile(FeatureLevel);
		LandscapeVertexFactory->MobileData.PositionComponent = FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, Position), sizeof(FLandscapeMobileVertex), VET_UByte4N);
		for (uint32 Index = 0; Index < LANDSCAPE_MAX_ES_LOD_COMP; ++Index)
		{
			LandscapeVertexFactory->MobileData.LODHeightsComponent.Add
			(FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, LODHeights) + sizeof(uint8) * 4 * Index, sizeof(FLandscapeMobileVertex), VET_UByte4N));
		}

		LandscapeVertexFactory->InitResource();
		VertexFactory = LandscapeVertexFactory;
	}

	// Init vertex buffer for rendering to virtual texture
	if (UseVirtualTexturing(FeatureLevel))
	{
		FLandscapeFixedGridVertexFactoryMobile* LandscapeVertexFactory = new FLandscapeFixedGridVertexFactoryMobile(FeatureLevel);
		LandscapeVertexFactory->MobileData.PositionComponent = FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, Position), sizeof(FLandscapeMobileVertex), VET_UByte4N);
		
		for (uint32 Index = 0; Index < LANDSCAPE_MAX_ES_LOD_COMP; ++Index)
		{
			LandscapeVertexFactory->MobileData.LODHeightsComponent.Add
			(FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, LODHeights) + sizeof(uint8) * 4 * Index, sizeof(FLandscapeMobileVertex), VET_UByte4N));
		}
		
		LandscapeVertexFactory->InitResource();
		FixedGridVertexFactory = LandscapeVertexFactory;
	}

	// Assign LandscapeUniformShaderParameters
	LandscapeUniformShaderParameters.InitResource();

	// Create per Lod uniform buffers
	LandscapeFixedGridUniformShaderParameters.AddDefaulted(MaxLOD + 1);
	for (int32 LodIndex = 0; LodIndex <= MaxLOD; ++LodIndex)
	{
		LandscapeFixedGridUniformShaderParameters[LodIndex].InitResource();
		FLandscapeFixedGridUniformShaderParameters Parameters;
		Parameters.LodValues = FVector4(
			LodIndex,
			0.f,
			(float)((SubsectionSizeVerts >> LodIndex) - 1),
			1.f / (float)((SubsectionSizeVerts >> LodIndex) - 1));
		LandscapeFixedGridUniformShaderParameters[LodIndex].SetContents(Parameters);
	}
}

TSharedPtr<FLandscapeMobileRenderData, ESPMode::ThreadSafe> FLandscapeComponentDerivedData::GetRenderData()
{
	// This function is expected to be called from either the GameThread or via ParallelFor from the GameThread
	check(!IsInRenderingThread());

	if (FPlatformProperties::RequiresCookedData() && CachedRenderData.IsValid())
	{
		// on device we can re-use the cached data if we are re-registering our component.
		return CachedRenderData;
	}
	else
	{
		check(CompressedLandscapeData.Num() > 0);

		FMemoryReader Ar(CompressedLandscapeData);

		// Note: change LANDSCAPE_FULL_DERIVEDDATA_VER when modifying the serialization layout
		int32 UncompressedSize;
		Ar << UncompressedSize;

		int32 CompressedSize;
		Ar << CompressedSize;

		TArray<uint8> CompressedData;
		CompressedData.Empty(CompressedSize);
		CompressedData.AddUninitialized(CompressedSize);
		Ar.Serialize(CompressedData.GetData(), CompressedSize);

		TArray<uint8> UncompressedData;
		UncompressedData.Empty(UncompressedSize);
		UncompressedData.AddUninitialized(UncompressedSize);

		verify(FCompression::UncompressMemory(NAME_Zlib, UncompressedData.GetData(), UncompressedSize, CompressedData.GetData(), CompressedSize));

		TSharedPtr<FLandscapeMobileRenderData, ESPMode::ThreadSafe> RenderData = MakeShareable(new FLandscapeMobileRenderData(MoveTemp(UncompressedData)));

		// if running on device		
		if (FPlatformProperties::RequiresCookedData())
		{
			// free the compressed data now that we have used it to create the render data.
			CompressedLandscapeData.Empty();
			// store a reference to the render data so we can use it again should the component be reregistered.
			CachedRenderData = RenderData;
		}

		return RenderData;
	}
}

void FLandscapeComponentSceneProxyMobile::ApplyMeshElementModifier(FMeshBatchElement& InOutMeshElement, int32 InLodIndex) const
{
	const bool bHoleDataExists = MobileRenderData->HoleData != nullptr && MobileRenderData->HoleData->IndexBuffer != nullptr && InLodIndex < MobileRenderData->HoleData->NumHoleLods;
	if (bHoleDataExists)
	{
		FLandscapeMobileHoleData const& HoleData = *MobileRenderData->HoleData;
		InOutMeshElement.IndexBuffer = HoleData.IndexBuffer;
		InOutMeshElement.NumPrimitives = HoleData.IndexBufferSize / 3;
		InOutMeshElement.FirstIndex = 0;
		InOutMeshElement.MinVertexIndex = HoleData.MinHoleIndex;
		InOutMeshElement.MaxVertexIndex = HoleData.MaxHoleIndex;
	}
}

//@StarLight code - BEGIN Optimize terrain LOD, Added by zhuyule
int32 GLandscapeOptimizeLOD = 0;
FAutoConsoleVariableRef CVarLandscapeOptimizeLOD(
	TEXT("r.LandscapeOptimizeLOD"),
	GLandscapeOptimizeLOD,
	TEXT("Optimize LOD for landscape/terrain meshes."),
	ECVF_Scalability
);

bool FLandscapeComponentSceneProxyMobile::IsUsingCustomLODRules() const
{ 
	return GLandscapeOptimizeLOD > 0; 
}

//FLODMask FLandscapeComponentSceneProxyMobile::GetCustomLOD(const FSceneView& InView, float InViewLODScale, int32 InForcedLODLevel, float& OutScreenSizeSquared) const
//{
//	FLODMask LODToRender;
//	//const FSceneView& LODView = GetLODView(InView);
//
//	//const int32 NumMeshes = GetPrimitiveSceneInfo()->StaticMeshRelevances.Num();
//
//	//int32 MinLODFound = INT_MAX;
//	//bool bFoundLOD = false;
//	//OutScreenSizeSquared = ComputeBoundsScreenSquared(GetBounds(), InView.ViewMatrices.GetViewOrigin(), LODView);
//
//	//for (int32 MeshIndex = NumMeshes - 1; MeshIndex >= 0; --MeshIndex)
//	//{
//	//	const FStaticMeshBatchRelevance& Mesh = GetPrimitiveSceneInfo()->StaticMeshRelevances[MeshIndex];
//
//	//	float MeshScreenSize = Mesh.ScreenSize;
//
//	//	if (FMath::Square(MeshScreenSize * 0.5f) >= OutScreenSizeSquared)
//	//	{
//	//		LODToRender.SetLOD(Mesh.LODIndex);
//	//		bFoundLOD = true;
//	//		break;
//	//	}
//
//	//	MinLODFound = FMath::Min<int32>(MinLODFound, Mesh.LODIndex);
//	//}
//	//// If no LOD was found matching the screen size, use the lowest in the array instead of LOD 0, to handle non-zero MinLOD
//	//if (!bFoundLOD)
//	//{
//	//	LODToRender.SetLOD(MinLODFound);
//	//}
//
//	return LODToRender;
//}
//@StarLight code - END Optimize terrain LOD, Added by zhuyule



//@StarLight code - BEGIN LandScapeInstance, Added by yanjianhong----------------------------------------------------

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeComponentClusterUniformBuffer, "ComponentClusterParameters");

//IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FLandscapeClusterLODUniformBuffer, "GlobalClusterParameters");

FORCEINLINE bool IntersectBox8Plane(const FVector& InOrigin, const FVector& InExtent, const FPlane* PermutedPlanePtr)
{
	// this removes a lot of the branches as we know there's 8 planes
	// copied directly out of ConvexVolume.cpp
	const VectorRegister Origin = VectorLoadFloat3(&InOrigin);
	const VectorRegister Extent = VectorLoadFloat3(&InExtent);

	const VectorRegister PlanesX_0 = VectorLoadAligned(&PermutedPlanePtr[0]);
	const VectorRegister PlanesY_0 = VectorLoadAligned(&PermutedPlanePtr[1]);
	const VectorRegister PlanesZ_0 = VectorLoadAligned(&PermutedPlanePtr[2]);
	const VectorRegister PlanesW_0 = VectorLoadAligned(&PermutedPlanePtr[3]);

	const VectorRegister PlanesX_1 = VectorLoadAligned(&PermutedPlanePtr[4]);
	const VectorRegister PlanesY_1 = VectorLoadAligned(&PermutedPlanePtr[5]);
	const VectorRegister PlanesZ_1 = VectorLoadAligned(&PermutedPlanePtr[6]);
	const VectorRegister PlanesW_1 = VectorLoadAligned(&PermutedPlanePtr[7]);

	// Splat origin into 3 vectors
	VectorRegister OrigX = VectorReplicate(Origin, 0);
	VectorRegister OrigY = VectorReplicate(Origin, 1);
	VectorRegister OrigZ = VectorReplicate(Origin, 2);
	// Splat the already abs Extent for the push out calculation
	VectorRegister AbsExtentX = VectorReplicate(Extent, 0);
	VectorRegister AbsExtentY = VectorReplicate(Extent, 1);
	VectorRegister AbsExtentZ = VectorReplicate(Extent, 2);

	// Calculate the distance (x * x) + (y * y) + (z * z) - w
	VectorRegister DistX_0 = VectorMultiply(OrigX, PlanesX_0);
	VectorRegister DistY_0 = VectorMultiplyAdd(OrigY, PlanesY_0, DistX_0);
	VectorRegister DistZ_0 = VectorMultiplyAdd(OrigZ, PlanesZ_0, DistY_0);
	VectorRegister Distance_0 = VectorSubtract(DistZ_0, PlanesW_0);
	// Now do the push out FMath::Abs(x * x) + FMath::Abs(y * y) + FMath::Abs(z * z)
	VectorRegister PushX_0 = VectorMultiply(AbsExtentX, VectorAbs(PlanesX_0));
	VectorRegister PushY_0 = VectorMultiplyAdd(AbsExtentY, VectorAbs(PlanesY_0), PushX_0);
	VectorRegister PushOut_0 = VectorMultiplyAdd(AbsExtentZ, VectorAbs(PlanesZ_0), PushY_0);

	// Check for completely outside
	if (VectorAnyGreaterThan(Distance_0, PushOut_0))
	{
		return false;
	}

	// Calculate the distance (x * x) + (y * y) + (z * z) - w
	VectorRegister DistX_1 = VectorMultiply(OrigX, PlanesX_1);
	VectorRegister DistY_1 = VectorMultiplyAdd(OrigY, PlanesY_1, DistX_1);
	VectorRegister DistZ_1 = VectorMultiplyAdd(OrigZ, PlanesZ_1, DistY_1);
	VectorRegister Distance_1 = VectorSubtract(DistZ_1, PlanesW_1);
	// Now do the push out FMath::Abs(x * x) + FMath::Abs(y * y) + FMath::Abs(z * z)
	VectorRegister PushX_1 = VectorMultiply(AbsExtentX, VectorAbs(PlanesX_1));
	VectorRegister PushY_1 = VectorMultiplyAdd(AbsExtentY, VectorAbs(PlanesY_1), PushX_1);
	VectorRegister PushOut_1 = VectorMultiplyAdd(AbsExtentZ, VectorAbs(PlanesZ_1), PushY_1);

	// Check for completely outside
	if (VectorAnyGreaterThan(Distance_1, PushOut_1))
	{
		return false;
	}
	return true;
}


class FLandscapeInstanceVertexFactoryVSParameters : public FVertexFactoryShaderParameters
{
	DECLARE_INLINE_TYPE_LAYOUT(FLandscapeInstanceVertexFactoryVSParameters, NonVirtual);
public:
	/**
	* Bind shader constants by name
	* @param	ParameterMap - mapping of named shader constants to indices
	*/
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		TexCoordOffsetParameter.Bind(ParameterMap, TEXT("TexCoordOffset"));
		/*InstanceOffset.Bind(ParameterMap, TEXT("InstanceOffset"));*/
		ClusterInstanceDataBuffer.Bind(ParameterMap, TEXT("ClusterInstanceDataBuffer"));
		ComponentLODBuffer.Bind(ParameterMap, TEXT("ComponentLODBuffer"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimeVS);

		const FLandscapeClusterBatchElementParams* BatchElementParams = (const FLandscapeClusterBatchElementParams*)BatchElement.UserData;

		//UniformBuffer is MutilFrame Resource
		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeComponentClusterUniformBuffer>(), *BatchElementParams->LandscapeComponentClusterUniformBuffer);
		ShaderBindings.Add(ClusterInstanceDataBuffer, BatchElementParams->ClusterInstanceDataBuffer->SRV);
		ShaderBindings.Add(ComponentLODBuffer, BatchElementParams->ComponentLODBuffer->SRV);
	}

protected:
	LAYOUT_FIELD(FShaderParameter, TexCoordOffsetParameter);
	LAYOUT_FIELD(FShaderResourceParameter, ClusterInstanceDataBuffer)
	LAYOUT_FIELD(FShaderResourceParameter, ComponentLODBuffer)
};

class FLandscapeInstanceVertexFactoryPSParameters : public FVertexFactoryShaderParameters
{
	DECLARE_INLINE_TYPE_LAYOUT(FLandscapeInstanceVertexFactoryPSParameters, NonVirtual);
public:
	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* InView,
		const class FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		class FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams
	) const
	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeVFDrawTimePS);

		const FLandscapeClusterBatchElementParams* BatchElementParams = (const FLandscapeClusterBatchElementParams*)BatchElement.UserData;

		ShaderBindings.Add(Shader->GetUniformBufferParameter<FLandscapeComponentClusterUniformBuffer>(), *BatchElementParams->LandscapeComponentClusterUniformBuffer);
	}
};


IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeClusterVertexFactoryMobile, SF_Vertex, FLandscapeInstanceVertexFactoryVSParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FLandscapeClusterVertexFactoryMobile, SF_Pixel, FLandscapeInstanceVertexFactoryPSParameters);
IMPLEMENT_VERTEX_FACTORY_TYPE(FLandscapeClusterVertexFactoryMobile, "/Engine/Private/LandscapeVertexFactory.ush", true, true, true, false, false);


void FLandscapeClusterVertexBuffer::InitRHI() {

	SCOPED_LOADTIMER(FLandscapeClusterVertexBuffer_InitRHI);

	// create a static vertex buffer
	uint32 VertexSize = ClusterQuadSize + 1;
	FRHIResourceCreateInfo CreateInfo;
	void* BufferData = nullptr;
	VertexBufferRHI = RHICreateAndLockVertexBuffer(VertexSize * VertexSize * sizeof(FLandscapeClusterVertex), BUF_Static, CreateInfo, BufferData);
	FLandscapeClusterVertex* Vertex = reinterpret_cast<FLandscapeClusterVertex*>(BufferData);

	for (uint32 y = 0; y < VertexSize; y++){
		for (uint32 x = 0; x < VertexSize; x++){
			Vertex->PositionX = static_cast<uint8>(x);
			Vertex->PositionY = static_cast<uint8>(y);
			Vertex->Black_0 = 0;
			Vertex->Black_1 = 0;
			Vertex++;
		}
	}
	RHIUnlockVertexBuffer(VertexBufferRHI);
}


struct FLandscapeMobileClusterRenderData {

	TArray<TArray<FBoxSphereBounds>> ComponentClusterBounds;

	FLandscapeMobileClusterRenderData(const TArray<uint8>& InPlatformData)
	{
		FMemoryReader MemAr(InPlatformData);

		const auto NumClusterLod = FMath::CeilLogTwo(FLandscapeClusterVertexBuffer::ClusterQuadSize) + 1;

		uint32 SingleLodNumComponentBounds;
		MemAr << SingleLodNumComponentBounds;

		ComponentClusterBounds.AddZeroed(NumClusterLod);
		for (uint32 i = 0; i < static_cast<uint32>(ComponentClusterBounds.Num()); ++i) {
			ComponentClusterBounds[i].AddZeroed(SingleLodNumComponentBounds);
			MemAr.Serialize(ComponentClusterBounds[i].GetData(), SingleLodNumComponentBounds * sizeof(FBoxSphereBounds));
		}
	}

	~FLandscapeMobileClusterRenderData()
	{
		check(IsInRenderingThread());
	}
};


FLandscapeComponentSceneProxyInstanceMobile::FLandscapeComponentSceneProxyInstanceMobile(ULandscapeComponent* InComponent)
	: FLandscapeComponentSceneProxy(InComponent)
	, MobileClusterRenderData(InComponent->ClusterPlatformData.GetRenderData())
	, MobileRenderData(InComponent->PlatformData.GetRenderData())
	, ClusterRenderSystem(nullptr)
{

	check(InComponent->MobileMaterialInterfaces.Num() > 0);
	check(InComponent->MobileWeightmapTextures.Num() > 0);

	check(ComponentSizeQuads + NumSubsections >= FLandscapeClusterVertexBuffer::ClusterQuadSize);
	check(FLandscapeClusterVertexBuffer::ClusterQuadSize > 8);

	//原生代码存在内存泄漏, Mobile会为每个Proxy创建一个FixedGridVertexFactory
	FixedGridVertexFactory = nullptr;

	const uint32 PerComponentClusterSize = (ComponentSizeQuads + NumSubsections) / FLandscapeClusterVertexBuffer::ClusterQuadSize;

	//#TODO: 挪到RenderSystem
	{
		uint32 NumComponents = InComponent->GetLandscapeProxy()->LandscapeComponents.Num();
		check(FMath::IsPowerOfTwo(NumComponents));
		uint32 SqrtSize = FMath::Sqrt(NumComponents);
		ComponentTotalSize = FIntPoint(SqrtSize, SqrtSize);
		
		ComponenLinearStartIndex = (ComponentBase.Y * ComponentTotalSize.X + ComponentBase.X) * PerComponentClusterSize * PerComponentClusterSize;
	}


	WeightmapTextures = InComponent->MobileWeightmapTextures;
	//#TODO: 暂时使用WeightMap, 待替换为HeightMap
	NormalmapTexture = InComponent->MobileWeightmapTextures[0];

}

FLandscapeComponentSceneProxyInstanceMobile::~FLandscapeComponentSceneProxyInstanceMobile() {
	ComponentClusterUniformBuffer.ReleaseResource();

	delete FixedGridVertexFactory;
}

SIZE_T FLandscapeComponentSceneProxyInstanceMobile::GetTypeHash() const {
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

void FLandscapeComponentSceneProxyInstanceMobile::CreateRenderThreadResources() {

	check(HeightmapTexture != nullptr);

	//memory tracker
	LLM_SCOPE(ELLMTag::Landscape); 

	if (IsComponentLevelVisible()){
		//#TODO: 大部分内容不再需要,重新创建一个RenderSystem
		RegisterNeighbors(this); 
	}

	auto FeatureLevel = GetScene().GetFeatureLevel();
	//Initial VertexFactory, ClusterVertexBuffer and ClusterIndexBuffer has been created in sharedBuffers
	{		
		SharedBuffers = FLandscapeComponentSceneProxy::SharedBuffersMap.FindRef(SharedBuffersKey);
		if (SharedBuffers == nullptr) {
			//don't need SOC data
			SharedBuffers = new FLandscapeSharedBuffers(SharedBuffersKey, SubsectionSizeQuads, NumSubsections, FeatureLevel, false, /*NumOcclusionVertices*/0);
			FLandscapeComponentSceneProxy::SharedBuffersMap.Add(SharedBuffersKey, SharedBuffers);

			check(SharedBuffers->ClusterVertexBuffer);
			FLandscapeClusterVertexFactoryMobile* LandscapeVertexFactory = new FLandscapeClusterVertexFactoryMobile(FeatureLevel);
			LandscapeVertexFactory->MobileData.PositionComponent = FVertexStreamComponent(SharedBuffers->ClusterVertexBuffer, 0, sizeof(FLandscapeClusterVertex), VET_UByte4N);
			LandscapeVertexFactory->InitResource();
			SharedBuffers->ClusterVertexFactory = LandscapeVertexFactory;
		}
		SharedBuffers->AddRef();
	}
	

	//#TODO: Remove HeightInfo?
	//Support Virtual Texture Render
	{
		if (UseVirtualTexturing(FeatureLevel))
		{
			MobileRenderData->VertexBuffer->InitResource();
			FLandscapeFixedGridVertexFactoryMobile* LandscapeVertexFactory = new FLandscapeFixedGridVertexFactoryMobile(FeatureLevel);
			LandscapeVertexFactory->MobileData.PositionComponent = FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, Position), sizeof(FLandscapeMobileVertex), VET_UByte4N);
			for (uint32 Index = 0; Index < LANDSCAPE_MAX_ES_LOD_COMP; ++Index){
				LandscapeVertexFactory->MobileData.LODHeightsComponent.Add
				(FVertexStreamComponent(MobileRenderData->VertexBuffer, STRUCT_OFFSET(FLandscapeMobileVertex, LODHeights) + sizeof(uint8) * 4 * Index, sizeof(FLandscapeMobileVertex), VET_UByte4N));
			}
			LandscapeVertexFactory->InitResource();
			FixedGridVertexFactory = LandscapeVertexFactory;

			// Assign LandscapeUniformShaderParameters
			LandscapeUniformShaderParameters.InitResource();

			// Create per Lod uniform buffers
			LandscapeFixedGridUniformShaderParameters.AddDefaulted(MaxLOD + 1);
			for (int32 LodIndex = 0; LodIndex <= MaxLOD; ++LodIndex)
			{
				LandscapeFixedGridUniformShaderParameters[LodIndex].InitResource();
				FLandscapeFixedGridUniformShaderParameters Parameters;
				Parameters.LodValues = FVector4(
					LodIndex,
					0.f,
					(float)((SubsectionSizeVerts >> LodIndex) - 1),
					1.f / (float)((SubsectionSizeVerts >> LodIndex) - 1));
				LandscapeFixedGridUniformShaderParameters[LodIndex].SetContents(Parameters);
			}
		}
	}
}

void FLandscapeComponentSceneProxyInstanceMobile::InitClusterRes() {

	//Save RenderSystem
	ClusterRenderSystem = LandscapeRenderSystems.FindChecked(LandscapeKey);

	const auto NumClusterLod = FMath::CeilLogTwo(FLandscapeClusterVertexBuffer::ClusterQuadSize) + 1;
	const auto NumSectionLod = FMath::CeilLogTwo(SubsectionSizeVerts);
	check(NumSectionLod > NumClusterLod);

	//Transform Bounds from Local To World
	{
		for (uint32 LodIndex = 0; LodIndex < NumClusterLod; ++LodIndex) {
			for (uint32 i = 0; i < ClusterRenderSystem->PerComponentClusterSize * ClusterRenderSystem->PerComponentClusterSize; ++i) {
				uint32 ClusterLinearIndex = ComponenLinearStartIndex + i;
				//now we can use LocalToworld, because SetTransform has been called
				ClusterRenderSystem->ClusterBounds[LodIndex][ClusterLinearIndex] = MobileClusterRenderData->ComponentClusterBounds[LodIndex][i].TransformBy(GetLocalToWorld());
			}
		}
		MobileClusterRenderData.Reset();
	}


	//#TODO: Move To new RenderSystem
	//Calculate about lod parameters
	{
		//面积递减系数为ScreenSizeRatioDivider
		check(HeightmapTexture != nullptr && HeightmapTexture->Resource != nullptr);
		uint32 SectionMaxLod = NumSectionLod - 1;
		const uint32 FirstMip = ((FTexture2DResource*)HeightmapTexture->Resource)->GetCurrentFirstMip();
		check(SectionMaxLod >= FirstMip);

		const uint32 LastClusterLOD = FMath::Min(NumClusterLod - 1, SectionMaxLod);

		//LOD0
		float ScreenSizeRatioDivider = FMath::Max(LandscapeComponent->GetLandscapeProxy()->LOD0DistributionSetting, 1.01f);
		float CurrentScreenSizeRatio = LandscapeComponent->GetLandscapeProxy()->LOD0ScreenSize;
		FLandscapeRenderSystem::FClusterLODSetting& ClusterLodSetting = ClusterRenderSystem->ClusterLODSetting;
		ClusterLodSetting.LastLODIndex = static_cast<int8>(LastClusterLOD);
		ClusterLodSetting.LOD0ScreenSizeSquared = FMath::Square(CurrentScreenSizeRatio);

		//LOD1
		CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
		ClusterLodSetting.LOD1ScreenSizeSquared = FMath::Square(CurrentScreenSizeRatio);

		//Other Lod
		ScreenSizeRatioDivider = FMath::Max(LandscapeComponent->GetLandscapeProxy()->LODDistributionSetting, 1.01f);
		ClusterLodSetting.LODOnePlusDistributionScalarSquared = FMath::Square(ScreenSizeRatioDivider);
		for (uint32 i = 1; i < NumClusterLod; ++i) {
			CurrentScreenSizeRatio /= ScreenSizeRatioDivider;
		}
		ClusterLodSetting.LastLODScreenSizeSquared = FMath::Square(CurrentScreenSizeRatio);
	}


	//Initial UniformBuffer and BatchParameter
	{
		ComponentClusterUniformBuffer.InitResource(); //Content和GPUBuffer在内部是分开的(兼容Descript?)

		ComponentBatchUserData.LandscapeComponentClusterUniformBuffer = &ComponentClusterUniformBuffer;
		ComponentBatchUserData.ClusterInstanceDataBuffer = &ClusterRenderSystem->ClusterInstanceData_GPU;
		ComponentBatchUserData.ComponentLODBuffer = &ClusterRenderSystem->ComponentLODValues_GPU;
	}
}

void FLandscapeComponentSceneProxyInstanceMobile::DestroyRenderThreadResources() {

	FLandscapeComponentSceneProxy::DestroyRenderThreadResources();
}

void FLandscapeComponentSceneProxyInstanceMobile::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const{
	
	SCOPE_CYCLE_COUNTER(STAT_LandscapeDynamicDrawTime);

	check(bRegistered);
	check(Views.Num() == 1);
	const FSceneView* View = Views[0];

	uint32 VisibleInstanceNum = 0;
	uint32 ComponentClusterSize = ClusterRenderSystem->PerComponentClusterSize;
	uint32 ComponentLinearIndex = ClusterRenderSystem->GetComponentLinearIndex(ComponentBase);
	uint32 ClusterLod = ClusterRenderSystem->ComponentLodInt[ComponentLinearIndex];
	uint32 StartInstanceLinearIndexOffset = ComponenLinearStartIndex;

	{
		SCOPE_CYCLE_COUNTER(STAT_LandscapeClusterFrustumCull);
		const TArray<FBoxSphereBounds>& RenderSystemClusterBoundsRef = ClusterRenderSystem->ClusterBounds[ClusterLod];
		for (uint32 LocalClusterY = 0; LocalClusterY < ComponentClusterSize; ++LocalClusterY) {
			for (uint32 LocalClusterX = 0; LocalClusterX < ComponentClusterSize; ++LocalClusterX) {
				uint32 ClusterLinearIndex = ComponenLinearStartIndex + LocalClusterY * ComponentClusterSize + LocalClusterX;
				const auto& ClusterBound = RenderSystemClusterBoundsRef[ClusterLinearIndex];
				bool bIsVisible = false;
			#if WITH_EDITOR
				const bool bUseFastIntersect = View->ViewFrustum.PermutedPlanes.Num() == 8;
				if (!bUseFastIntersect) {
					bIsVisible = View->ViewFrustum.IntersectBox(ClusterBound.Origin, ClusterBound.BoxExtent);
				}
				else
			#endif
				{
					bIsVisible = IntersectBox8Plane(ClusterBound.Origin, ClusterBound.BoxExtent, View->ViewFrustum.PermutedPlanes.GetData());
				}

				if (!bIsVisible) {
					continue;
				}


			#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				if (ViewFamily.EngineShowFlags.Bounds) {
					RenderOnlyBox(Collector.GetPDI(0), ClusterBound);
				}
			#endif

				const auto GlobalClusterBase = ClusterRenderSystem->GetClusteGlobalBase(ComponentBase, FIntPoint(LocalClusterX, LocalClusterY));
				ClusterRenderSystem->ClusterInstanceData_CPU[StartInstanceLinearIndexOffset] = GlobalClusterBase;
				StartInstanceLinearIndexOffset += 1;
				VisibleInstanceNum += 1;
			}
		}
	}

	if(VisibleInstanceNum > 0)
	{
		uint32 CurClusterQuadSize = FLandscapeClusterVertexBuffer::ClusterQuadSize >> ClusterLod;
		uint32 CurClusterVertexSize = CurClusterQuadSize + 1;
		UMaterialInterface* MaterialInterface = nullptr;

		//check material index
		int32 MaterialIndex = LODIndexToMaterialIndex[ClusterLod];
		MaterialInterface = AvailableMaterials[MaterialIndex];
		check(MaterialInterface != nullptr);

		FMeshBatch& MeshBatch = Collector.AllocateMesh();
		MeshBatch.VertexFactory = SharedBuffers->ClusterVertexFactory;
		MeshBatch.MaterialRenderProxy = MaterialInterface->GetRenderProxy();

		MeshBatch.LCI = ComponentLightInfo.Get();
		MeshBatch.ReverseCulling = IsLocalToWorldDeterminantNegative();
		MeshBatch.CastShadow = true;
		MeshBatch.bUseForDepthPass = true;
		MeshBatch.bUseAsOccluder = false; //
		MeshBatch.bUseForMaterial = true;
		MeshBatch.Type = PT_TriangleList;
		MeshBatch.DepthPriorityGroup = SDPG_World;
		MeshBatch.LODIndex = ClusterLod; //not need
		MeshBatch.bDitheredLODTransition = false;
		MeshBatch.bCanApplyViewModeOverrides = true; //兼容WireFrame等
		MeshBatch.bUseWireframeSelectionColoring = IsSelected(); //选中颜色

		// Combined batch element
		FMeshBatchElement& BatchElement = MeshBatch.Elements[0];

		BatchElement.UserData = &ComponentBatchUserData;
		BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
		BatchElement.IndexBuffer = SharedBuffers->ClusterIndexBuffers[ClusterLod];
		BatchElement.NumPrimitives = CurClusterQuadSize * CurClusterQuadSize * 2u;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = CurClusterVertexSize * CurClusterVertexSize - 1;
		BatchElement.NumInstances = VisibleInstanceNum;
		BatchElement.InstancedLODIndex = ClusterLod; //用来传递LOD

		Collector.AddMesh(0, MeshBatch);
	
		//No need to think about Hole
		//ApplyMeshElementModifier(BatchElement, LODIndex);
	}
}

void FLandscapeComponentSceneProxyInstanceMobile::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) 
{
	if (AvailableMaterials.Num() == 0)
	{
		return;
	}

	int32 TotalBatchCount = 1 + LastLOD - FirstLOD;
	TotalBatchCount += (1 + LastVirtualTextureLOD - FirstVirtualTextureLOD) * RuntimeVirtualTextureMaterialTypes.Num();

	StaticBatchParamArray.Empty(TotalBatchCount);
	PDI->ReserveMemoryForMeshes(TotalBatchCount);

	// Add fixed grid mesh batches for runtime virtual texture usage
	for (ERuntimeVirtualTextureMaterialType MaterialType : RuntimeVirtualTextureMaterialTypes)
	{
		const int32 MaterialIndex = LODIndexToMaterialIndex[FirstLOD];

		for (int32 LODIndex = FirstVirtualTextureLOD; LODIndex <= LastVirtualTextureLOD; ++LODIndex)
		{
			FMeshBatch RuntimeVirtualTextureMeshBatch;
			if (GetMeshElementForVirtualTexture(LODIndex, MaterialType, AvailableMaterials[MaterialIndex], RuntimeVirtualTextureMeshBatch, StaticBatchParamArray))
			{
				PDI->DrawMesh(RuntimeVirtualTextureMeshBatch, FLT_MAX);
			}
		}
	}
}


void FLandscapeComponentSceneProxyInstanceMobile::OnTransformChanged() {

	auto FeatureLevel = GetScene().GetFeatureLevel();
	if (UseVirtualTexturing(FeatureLevel)) {
		FLandscapeComponentSceneProxy::OnTransformChanged();
	}


	// Set Lightmap ScaleBias
	int32 PatchExpandCountX = 0;
	int32 PatchExpandCountY = 0;
	int32 DesiredSize = 1; // output by GetTerrainExpandPatchCount but not used below
	const float LightMapRatio = ::GetTerrainExpandPatchCount(StaticLightingResolution, PatchExpandCountX, PatchExpandCountY, ComponentSizeQuads, (NumSubsections * (SubsectionSizeQuads + 1)), DesiredSize, StaticLightingLOD);
	const float LightmapLODScaleX = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountX);
	const float LightmapLODScaleY = LightMapRatio / ((ComponentSizeVerts >> StaticLightingLOD) + 2 * PatchExpandCountY);
	const float LightmapBiasX = PatchExpandCountX * LightmapLODScaleX;
	const float LightmapBiasY = PatchExpandCountY * LightmapLODScaleY;
	const float LightmapScaleX = LightmapLODScaleX * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapScaleY = LightmapLODScaleY * (float)((ComponentSizeVerts >> StaticLightingLOD) - 1) / ComponentSizeQuads;
	const float LightmapExtendFactorX = (float)SubsectionSizeQuads * LightmapScaleX;
	const float LightmapExtendFactorY = (float)SubsectionSizeQuads * LightmapScaleY;

	// cache component's WorldToLocal
	FMatrix LtoW = GetLocalToWorld();
	WorldToLocal = LtoW.Inverse();

	// cache component's LocalToWorldNoScaling
	LocalToWorldNoScaling = LtoW;
	LocalToWorldNoScaling.RemoveScaling();

	uint32 PerComponentClusterSize = (ComponentSizeQuads + NumSubsections) / FLandscapeClusterVertexBuffer::ClusterQuadSize;

	// Set FLandscapeUniformVSParameters for this Component
	FLandscapeComponentClusterUniformBuffer LandscapeClusterParams;

	check(FMath::IsPowerOfTwo(SubsectionSizeVerts));

	LandscapeClusterParams.ClusterParameter = FIntVector4(
		PerComponentClusterSize,
		FLandscapeClusterVertexBuffer::ClusterQuadSize,
		NumSubsections,
		ComponenLinearStartIndex
	);


	LandscapeClusterParams.ComponentBase = ComponentBase;
	LandscapeClusterParams.Size = ComponentTotalSize;
	
	LandscapeClusterParams.HeightmapUVScaleBias = HeightmapScaleBias;
	LandscapeClusterParams.WeightmapUVScaleBias = WeightmapScaleBias;
	LandscapeClusterParams.LocalToWorldNoScaling = LocalToWorldNoScaling;

	LandscapeClusterParams.LandscapeLightmapScaleBias = FVector4(
		LightmapScaleX,
		LightmapScaleY,
		LightmapBiasY,
		LightmapBiasX);

	LandscapeClusterParams.SubsectionSizeVertsLayerUVPan = FVector4(
		SubsectionSizeVerts,
		1.f / (float)SubsectionSizeQuads,
		SectionBase.X,
		SectionBase.Y
	);

	LandscapeClusterParams.SubsectionOffsetParams = FVector4(
		HeightmapSubsectionOffsetU,
		HeightmapSubsectionOffsetV,
		WeightmapSubsectionOffset,
		SubsectionSizeQuads
	);

	LandscapeClusterParams.LightmapSubsectionOffsetParams = FVector4(
		LightmapExtendFactorX,
		LightmapExtendFactorY,
		0,
		0
	);

	LandscapeClusterParams.BlendableLayerMask = FVector4(
		BlendableLayerMask & (1 << 0) ? 1 : 0,
		BlendableLayerMask & (1 << 1) ? 1 : 0,
		BlendableLayerMask & (1 << 2) ? 1 : 0,
		0
	);


	LandscapeClusterParams.HeightmapTexture = HeightmapTexture->TextureReference.TextureReferenceRHI;
	LandscapeClusterParams.HeightmapTextureSampler = TStaticSamplerState<SF_Point>::GetRHI();


	check(XYOffsetmapTexture == nullptr);

	if (NormalmapTexture)
	{
		LandscapeClusterParams.NormalmapTexture = NormalmapTexture->TextureReference.TextureReferenceRHI;
		LandscapeClusterParams.NormalmapTextureSampler = NormalmapTexture->Resource->SamplerStateRHI;
	}
	else
	{
		LandscapeClusterParams.NormalmapTexture = GBlackTexture->TextureRHI;
		LandscapeClusterParams.NormalmapTextureSampler = GBlackTexture->SamplerStateRHI;
	}

	ComponentClusterUniformBuffer.SetContents(LandscapeClusterParams);

	//Because Landscape inherits UPrimitiveComponent, it will not necessarily recreate Proxy when updating Transform
	//check(!bRegistered);

	//Use Dynamic rendering, no need to update PrimitivesNeedingStaticMeshUpdate container
	//GetScene().UpdateCachedRenderStates(this);
}


FBoxSphereBounds FLandscapeComponentSceneProxyInstanceMobile::CalcClusterLocalBounds(
	FIntPoint LocalClusterBase,
	FIntPoint ComponentBase,
	FIntPoint HeightMapMipSize,
	FColor* HeightMapData,
	uint32 SubsectionSizeQuads,
	uint32 InNumSubsections,
	uint8 MipLevel
)
{
	//This Function only called by Gamethread
	check(IsInGameThread());
	const auto ClusterLevels = FMath::CeilLogTwo(FLandscapeClusterVertexBuffer::ClusterQuadSize);

	uint32 ClusterMipQuadSize = FLandscapeClusterVertexBuffer::ClusterQuadSize >> MipLevel;
	uint32 ComponetnQuadSize = SubsectionSizeQuads * InNumSubsections;
	uint32 ComponentMipQuadSize = ((ComponetnQuadSize + InNumSubsections) >> MipLevel) - InNumSubsections;
	uint32 SubSectionMipSizeVerts = (SubsectionSizeQuads + 1) >> MipLevel;
	uint32 SubSectionMipQuadSize = SubSectionMipSizeVerts - 1;

	FIntPoint PerHeightMapComponentSize = HeightMapMipSize / FIntPoint(ComponentMipQuadSize + InNumSubsections, ComponentMipQuadSize + InNumSubsections);
	FIntPoint HeightMapLocalComponentBase = FIntPoint(ComponentBase.X & (PerHeightMapComponentSize.X - 1), ComponentBase.Y & (PerHeightMapComponentSize.Y - 1));
	FIntPoint HeightMapComponentSampleBase = FIntPoint(HeightMapLocalComponentBase.X * (ComponentMipQuadSize + InNumSubsections), HeightMapLocalComponentBase.Y * (ComponentMipQuadSize + InNumSubsections));
	FIntPoint ClusterStartQuadIndex = LocalClusterBase * ClusterMipQuadSize;
	float PerQuadSize = static_cast<float>(SubsectionSizeQuads) / static_cast<float>(SubSectionMipQuadSize);

	//跨两级LOD,处理例如2.9~3.1,理论上最保守
	uint32 NextLevel = FMath::Min(static_cast<uint32>(ClusterLevels - MipLevel), 2u);  
	uint32 NextSubSectionQuadSize = ((SubSectionMipQuadSize + 1) >> NextLevel) - 1;
	uint32 NextClusterMipQuadSize = ClusterMipQuadSize >> NextLevel;
	float NextQuadSize = static_cast<float>(SubsectionSizeQuads) / static_cast<float>(NextSubSectionQuadSize);//求对应LOD的QuadSize


	FVector2D StartClusterPos = FVector2D(static_cast<float>(ClusterStartQuadIndex.X) * PerQuadSize, static_cast<float>(ClusterStartQuadIndex.Y) * PerQuadSize);
	FVector2D EndClusterPos = FVector2D(
		FMath::Min(static_cast<float>(NextClusterMipQuadSize) * NextQuadSize + StartClusterPos.X, static_cast<float>(ComponetnQuadSize)),
		FMath::Min(static_cast<float>(NextClusterMipQuadSize) * NextQuadSize + StartClusterPos.Y, static_cast<float>(ComponetnQuadSize))
	);

	FBox LocalBox = FBox(FVector(StartClusterPos.X, StartClusterPos.Y, 0.f), FVector(EndClusterPos.X, EndClusterPos.Y, 0.f));

	for (uint32 y = 0; y < ClusterMipQuadSize + 1; ++y) {

		uint32 ClusterToComponentPosY = FMath::Min(ClusterStartQuadIndex.Y + y, ComponentMipQuadSize);
		uint32 SectionY = ClusterToComponentPosY >= SubSectionMipSizeVerts ? 1 : 0;
		uint32 ClusterToSectionPosY = ClusterToComponentPosY - SubSectionMipQuadSize * SectionY;
		uint32 SampleHeightY = ClusterToSectionPosY + SectionY * SubSectionMipSizeVerts + HeightMapComponentSampleBase.Y; //采样位置

		for (uint32 x = 0; x < ClusterMipQuadSize + 1; ++x) {

			uint32 ClusterToComponentPosX = FMath::Min(ClusterStartQuadIndex.X + x, ComponentMipQuadSize);
			uint32 SectionX = ClusterToComponentPosX >= SubSectionMipSizeVerts ? 1 : 0;
			uint32 ClusterToSectionPosX = ClusterToComponentPosX - SubSectionMipQuadSize * SectionX;
			uint32 SampleHeightX = ClusterToSectionPosX + SectionX * SubSectionMipSizeVerts; //采样位置

			uint32 HeightMapSampleIndex = HeightMapMipSize.Y * SampleHeightY + SampleHeightX + HeightMapComponentSampleBase.X;
			const auto& HeightValue = HeightMapData[HeightMapSampleIndex];
			float VertexHeight = LandscapeDataAccess::GetLocalHeight(static_cast<uint16>(HeightValue.R << 8u | HeightValue.G));

			LocalBox.Min.Z = FMath::Min(LocalBox.Min.Z, VertexHeight);
			LocalBox.Max.Z = FMath::Max(LocalBox.Max.Z, VertexHeight);
		}
	}

	/*auto WorldBox = LocalBox.TransformBy(InLocalToWorld);*/
	return FBoxSphereBounds(LocalBox);
}


void FLandscapeComponentSceneProxyInstanceMobile::RenderOnlyBox(FPrimitiveDrawInterface* PDI, const FBoxSphereBounds& InBounds) const{
	// Draw the static mesh's bounding box and sphere.
	const ESceneDepthPriorityGroup DrawBoundsDPG = SDPG_World;
	DrawWireBox(PDI, InBounds.GetBox(), FColor(72, 72, 255), DrawBoundsDPG);
	/*DrawCircle(PDI, InBounds.Origin, FVector(1, 0, 0), FVector(0, 1, 0), FColor::Yellow, InBounds.SphereRadius, 32, DrawBoundsDPG);
	DrawCircle(PDI, InBounds.Origin, FVector(1, 0, 0), FVector(0, 0, 1), FColor::Yellow, InBounds.SphereRadius, 32, DrawBoundsDPG);
	DrawCircle(PDI, InBounds.Origin, FVector(0, 1, 0), FVector(0, 0, 1), FColor::Yellow, InBounds.SphereRadius, 32, DrawBoundsDPG);*/
}

bool FLandscapeClusterVertexFactoryMobile::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	return GetMaxSupportedFeatureLevel(Parameters.Platform) == ERHIFeatureLevel::ES3_1 &&
		(Parameters.MaterialParameters.bIsUsedWithLandscape || Parameters.MaterialParameters.bIsSpecialEngineMaterial);
}


void FLandscapeClusterVertexFactoryMobile::InitRHI()
{
	// list of declaration items
	FVertexDeclarationElementList Elements;

	// position decls
	Elements.Add(AccessStreamComponent(MobileData.PositionComponent, 0));

	// create the actual device decls
	InitDeclaration(Elements);
}


TSharedPtr<FLandscapeMobileClusterRenderData, ESPMode::ThreadSafe> FLandscapeComponentClusterDeriveData::GetRenderData()
{
	//#TODO: Compresse Data
	// This function is expected to be called from either the GameThread or via ParallelFor from the GameThread
	check(!IsInRenderingThread());

	TSharedPtr<FLandscapeMobileClusterRenderData, ESPMode::ThreadSafe> RenderData = MakeShareable(new FLandscapeMobileClusterRenderData(UnCompressedClusterLandscapeData));

	return RenderData;
}

//@StarLight code - END LandScapeInstance, Added by yanjianhong
