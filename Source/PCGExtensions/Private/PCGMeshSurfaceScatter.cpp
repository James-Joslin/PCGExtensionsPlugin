// PCGMeshSurfaceScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGMeshSurfaceScatter.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Async/PCGAsyncLoadingContext.h"
#include "PCGExtScatterCommon.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGMeshSurfaceScatter)

// Shared scatter helpers (FMinDistGrid / WorldUp / RejectedPinLabel) live in
// PCGExtScatterCommon.h -- see PCGExtScatterCommon::* below. FDistEntry and the
// SqCmToSqM constant are unique to this node; they sit in a file-named namespace
// (rather than an anonymous one) so the UBT Unity build can't collide them with a
// same-named helper in a sibling .cpp concatenated into the same translation unit.
namespace PCGMeshSurfaceScatter
{
	// Square-centimetre → square-metre conversion (UU are cm in UE).
	constexpr double SqCmToSqM = 1.0 / (100.0 * 100.0);

	// Priority-queue entry for Dijkstra-style BFS.
	struct FDistEntry
	{
		int32 TriIdx;
		float Dist;
		bool operator<(const FDistEntry& O) const { return Dist > O.Dist; } // min-heap via TArray::Heapify
	};
}

// ─────────────────────────────────────────────
//  Settings — construction & pins
// ─────────────────────────────────────────────

UPCGMeshSurfaceScatterSettings::UPCGMeshSurfaceScatterSettings()
{
}

TArray<FPCGPinProperties> UPCGMeshSurfaceScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Spatial,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGMeshSurfaceScatter", "InTooltip",
			"Rock spawner output points (with MeshPath attributes and transforms). "
			"Each point's mesh surface is sampled for grass placement. Accepts composite/"
			"union data from rock graphs. If disconnected, falls back to searching level "
			"actors by ActorTagFallback."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGMeshSurfaceScatterSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGMeshSurfaceScatter", "OutTooltip",
			"Grass/foliage instances on rock surfaces with MeshPath attribute. "
			"Feed a By-Attribute Static Mesh Spawner."));

	Pins.Emplace(PCGExtScatterCommon::RejectedPinLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGMeshSurfaceScatter", "RejTooltip",
			"Rejected samples (submerged, edge, noise, spacing). Debug only."));

	return Pins;
}

FPCGElementPtr UPCGMeshSurfaceScatterSettings::CreateElement() const
{
	return MakeShared<FPCGMeshSurfaceScatterElement>();
}

// ─────────────────────────────────────────────
//  Helpers — grass mesh cache
// ─────────────────────────────────────────────

bool FPCGMeshSurfaceScatterElement::BuildGrassMeshCache(
	const TArray<FPCGSurfGrassMeshEntry>& Entries,
	TArray<FGrassMeshCache>& OutCache, float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGSurfGrassMeshEntry& Entry : Entries)
	{
		// Resident after PrepareDataInternal's async load -- never sync-loads off-thread.
		UStaticMesh* Loaded = Entry.Mesh.Get();
		if (!Loaded)
		{
			continue;
		}
		FGrassMeshCache C;
		C.MeshPath = FSoftObjectPath(Loaded);
		C.ScaleRange = Entry.ScaleRange;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		C.CumulativeWeight = OutTotalWeight;
		OutCache.Add(C);
	}
	return OutCache.Num() > 0;
}

int32 FPCGMeshSurfaceScatterElement::SelectGrassMeshIndex(
	const TArray<FGrassMeshCache>& Cache, float TotalWeight, FRandomStream& Rng)
{
	const float Roll = Rng.FRandRange(0.0f, TotalWeight);
	for (int32 i = 0; i < Cache.Num(); ++i)
	{
		if (Roll <= Cache[i].CumulativeWeight)
		{
			return i;
		}
	}
	return Cache.Num() - 1;
}

// ─────────────────────────────────────────────
//  Helpers — attributes
// ─────────────────────────────────────────────

FString FPCGMeshSurfaceScatterElement::ReadStringAttr(
	const UPCGMetadata* Meta, FName Name, int64 EntryKey)
{
	if (!Meta || Name.IsNone() || !Meta->HasAttribute(Name))
	{
		return FString();
	}
	// GetConstTypedAttribute returns null on a type mismatch, so we avoid the
	// PCG::Private::MetadataTypes<T>::Id comparison entirely.
	const FPCGMetadataAttribute<FString>* Attr = Meta->GetConstTypedAttribute<FString>(Name);
	if (!Attr)
	{
		return FString();
	}
	return Attr->GetValueFromItemKey(EntryKey);
}

// ─────────────────────────────────────────────
//  Helpers — mesh topology building
// ─────────────────────────────────────────────

bool FPCGMeshSurfaceScatterElement::BuildTopology(
	UStaticMesh* Mesh, const FTransform& WorldTransform,
	float MinNormalDotUp, float MaxNeighborAngleDeg,
	TArray<FTriData>& OutTris)
{
	OutTris.Reset();

	// ── Access LOD0 render data ──
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
#if WITH_EDITOR
		UE_LOG(LogTemp, Warning,
			TEXT("[MeshSurfaceScatter] Mesh '%s' has no render data — skipping."),
			*Mesh->GetPathName());
#endif
		return false;
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	const uint32 NumIndices = LOD.IndexBuffer.GetNumIndices();

	if (NumVerts == 0 || NumIndices < 3)
	{
#if WITH_EDITOR
		// Most common cause: "Allow CPU Access" is off in the mesh asset.
		// Without it the vertex buffer is GPU-only and reads back zero vertices.
		if (NumVerts == 0)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[MeshSurfaceScatter] Mesh '%s' returned 0 vertices from LOD0. "
					"Enable 'Allow CPU Access' in the mesh asset settings."),
				*Mesh->GetPathName());
		}
#endif
		return false;
	}

	// ── Extract vertices → world space ──
	TArray<FVector> Verts;
	Verts.SetNum(NumVerts);
	for (uint32 i = 0; i < NumVerts; ++i)
	{
		const FVector3f LocalPos = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
		Verts[i] = WorldTransform.TransformPosition(FVector(LocalPos));
	}

	// ── Extract indices ──
	TArray<uint32> Indices;
	LOD.IndexBuffer.GetCopy(Indices);

	const int32 NumTris = Indices.Num() / 3;
	if (NumTris == 0)
	{
		return false;
	}

	// ── Build triangle data ──
	OutTris.SetNum(NumTris);
	for (int32 T = 0; T < NumTris; ++T)
	{
		FTriData& Tri = OutTris[T];
		Tri.V0 = Verts[Indices[T * 3 + 0]];
		Tri.V1 = Verts[Indices[T * 3 + 1]];
		Tri.V2 = Verts[Indices[T * 3 + 2]];
		Tri.Centroid = (Tri.V0 + Tri.V1 + Tri.V2) / 3.0;

		const FVector Cross = FVector::CrossProduct(Tri.V1 - Tri.V0, Tri.V2 - Tri.V0);
		const float CrossLen = Cross.Size();
		Tri.Area = CrossLen * 0.5f;
		Tri.Normal = (CrossLen > KINDA_SMALL_NUMBER) ? (Cross / CrossLen) : FVector::UpVector;
		Tri.DotUp = FVector::DotProduct(Tri.Normal, PCGExtScatterCommon::WorldUp);

		// Accept both winding directions — the "up" face is whichever has a positive DotUp.
		// If the mesh has inverted winding, the normal points down; use the flipped version.
		// TODO(audit): this unconditional flip also treats overhang undersides (true
		// downward-facing faces) as walkable up-faces, so grass can be placed on the
		// underside of overhangs. Decide whether to flip only for inverted-winding
		// meshes (e.g. via mesh winding/orientation) or to keep the current behaviour.
		if (Tri.DotUp < 0.0f)
		{
			Tri.Normal = -Tri.Normal;
			Tri.DotUp = -Tri.DotUp;
		}

		Tri.bWalkable = (Tri.DotUp >= MinNormalDotUp) && (Tri.Area > 1.0f); // ≥1 sq cm
		Tri.bCurvatureOK = true;
		Tri.EdgeDist = 0.0f;
		Tri.CumulativeArea = 0.0f;
	}

	// ── Build edge adjacency: edge(sorted vertex pair) → list of triangle indices ──
	TMap<FEdgeKey, TArray<int32>> EdgeToTris;
	EdgeToTris.Reserve(NumTris * 3);

	auto MakeEdge = [](uint32 A, uint32 B) -> FEdgeKey
		{
			return (A < B) ? FEdgeKey{ (int32)A, (int32)B } : FEdgeKey{ (int32)B, (int32)A };
		};

	for (int32 T = 0; T < NumTris; ++T)
	{
		const uint32 I0 = Indices[T * 3 + 0];
		const uint32 I1 = Indices[T * 3 + 1];
		const uint32 I2 = Indices[T * 3 + 2];
		EdgeToTris.FindOrAdd(MakeEdge(I0, I1)).Add(T);
		EdgeToTris.FindOrAdd(MakeEdge(I1, I2)).Add(T);
		EdgeToTris.FindOrAdd(MakeEdge(I0, I2)).Add(T);
	}

	// ── Curvature check: reclassify walkable triangles with high-curvature neighbours ──
	const float MaxAngleCos = FMath::Cos(FMath::DegreesToRadians(MaxNeighborAngleDeg));

	// Build per-triangle walkable-neighbor lists (needed for BFS too).
	TArray<TArray<int32>> WalkableNeighbors;
	WalkableNeighbors.SetNum(NumTris);

	for (auto& Pair : EdgeToTris)
	{
		const TArray<int32>& SharedTris = Pair.Value;
		if (SharedTris.Num() != 2)
		{
			continue;
		}
		const int32 TA = SharedTris[0];
		const int32 TB = SharedTris[1];
		if (OutTris[TA].bWalkable && OutTris[TB].bWalkable)
		{
			WalkableNeighbors[TA].Add(TB);
			WalkableNeighbors[TB].Add(TA);
		}
	}

	// Curvature pass: for each walkable triangle, check average dot with walkable neighbours.
	for (int32 T = 0; T < NumTris; ++T)
	{
		FTriData& Tri = OutTris[T];
		if (!Tri.bWalkable || WalkableNeighbors[T].Num() == 0)
		{
			continue;
		}

		float SumDot = 0.0f;
		for (const int32 N : WalkableNeighbors[T])
		{
			SumDot += FVector::DotProduct(Tri.Normal, OutTris[N].Normal);
		}
		const float AvgDot = SumDot / WalkableNeighbors[T].Num();
		if (AvgDot < MaxAngleCos)
		{
			Tri.bCurvatureOK = false;
			// Demote to non-walkable for the boundary-detection step.
			// This means neighbouring walkable triangles will see a boundary here.
			Tri.bWalkable = false;
		}
	}

	// After curvature reclassification, rebuild walkable-neighbor lists and pass to BFS.
	WalkableNeighbors.Reset();
	WalkableNeighbors.SetNum(NumTris);
	for (auto& Pair : EdgeToTris)
	{
		const TArray<int32>& SharedTris = Pair.Value;
		if (SharedTris.Num() != 2)
		{
			continue;
		}
		const int32 TA = SharedTris[0];
		const int32 TB = SharedTris[1];
		if (OutTris[TA].bWalkable && OutTris[TB].bWalkable)
		{
			WalkableNeighbors[TA].Add(TB);
			WalkableNeighbors[TB].Add(TA);
		}
	}

	// ── BFS edge-distance computation ──

	// Identify boundary-adjacent walkable triangles: those sharing an edge with a
	// non-walkable triangle or a mesh-boundary edge (only 1 triangle on the edge).
	TArray<PCGMeshSurfaceScatter::FDistEntry> Heap;
	for (int32 T = 0; T < NumTris; ++T)
	{
		if (!OutTris[T].bWalkable)
		{
			OutTris[T].EdgeDist = 0.0f;
			continue;
		}
		OutTris[T].EdgeDist = BIG_NUMBER;
	}

	for (auto& Pair : EdgeToTris)
	{
		const TArray<int32>& SharedTris = Pair.Value;

		// Mesh-boundary edge (only 1 triangle): if walkable, it's at the edge.
		if (SharedTris.Num() == 1)
		{
			const int32 T = SharedTris[0];
			if (OutTris[T].bWalkable && OutTris[T].EdgeDist > 0.0f)
			{
				OutTris[T].EdgeDist = 0.0f;
				Heap.Add({ T, 0.0f });
			}
			continue;
		}

		// Shared edge between walkable and non-walkable → the walkable one is boundary.
		if (SharedTris.Num() == 2)
		{
			const int32 TA = SharedTris[0];
			const int32 TB = SharedTris[1];
			const bool WA = OutTris[TA].bWalkable;
			const bool WB = OutTris[TB].bWalkable;
			if (WA && !WB && OutTris[TA].EdgeDist > 0.0f)
			{
				OutTris[TA].EdgeDist = 0.0f;
				Heap.Add({ TA, 0.0f });
			}
			if (WB && !WA && OutTris[TB].EdgeDist > 0.0f)
			{
				OutTris[TB].EdgeDist = 0.0f;
				Heap.Add({ TB, 0.0f });
			}
		}
	}

	// Dijkstra propagation through walkable adjacency.
	Heap.Heapify();

	while (Heap.Num() > 0)
	{
		PCGMeshSurfaceScatter::FDistEntry Current;
		Heap.HeapPop(Current);

		if (Current.Dist > OutTris[Current.TriIdx].EdgeDist)
		{
			continue; // stale entry
		}

		for (const int32 N : WalkableNeighbors[Current.TriIdx])
		{
			const float StepDist = FVector::Dist(OutTris[Current.TriIdx].Centroid, OutTris[N].Centroid);
			const float NewDist = Current.Dist + StepDist;
			if (NewDist < OutTris[N].EdgeDist)
			{
				OutTris[N].EdgeDist = NewDist;
				Heap.HeapPush({ N, NewDist });
			}
		}
	}

	return true;
}

// ─────────────────────────────────────────────
//  Helpers — transform
// ─────────────────────────────────────────────

FQuat FPCGMeshSurfaceScatterElement::MakeSurfaceRotation(
	const FVector& SurfaceNormal, float YawDeg,
	float AlignAmount, float MaxAlignDeg,
	float JitterPitchDeg, float JitterRollDeg)
{
	const FVector N = SurfaceNormal.GetSafeNormal();
	FVector DesiredUp = PCGExtScatterCommon::WorldUp;

	if (AlignAmount > 0.0f)
	{
		const float NDot = FMath::Clamp(FVector::DotProduct(PCGExtScatterCommon::WorldUp, N), -1.0f, 1.0f);
		const float FullAngle = FMath::Acos(NDot);
		if (FullAngle > KINDA_SMALL_NUMBER)
		{
			const float MaxRad = FMath::DegreesToRadians(MaxAlignDeg);
			float Alpha = AlignAmount;
			if (FullAngle * Alpha > MaxRad)
			{
				Alpha = MaxRad / FullAngle;
			}
			DesiredUp = FMath::Lerp(PCGExtScatterCommon::WorldUp, N, Alpha).GetSafeNormal();
		}
	}

	const FQuat AlignQuat = FQuat::FindBetweenNormals(PCGExtScatterCommon::WorldUp, DesiredUp);
	const FQuat YawQuat(DesiredUp, FMath::DegreesToRadians(YawDeg));
	const FQuat JitterQuat = FRotator(JitterPitchDeg, 0.0f, JitterRollDeg).Quaternion();

	return YawQuat * AlignQuat * JitterQuat;
}

// ─────────────────────────────────────────────
//  Helpers — sampling
// ─────────────────────────────────────────────

FVector FPCGMeshSurfaceScatterElement::RandomBarycentric(FRandomStream& Rng)
{
	// Uniform random point in a triangle via Osada et al.'s square-root mapping.
	const float U = Rng.FRand();
	const float V = Rng.FRand();
	const float SqU = FMath::Sqrt(U);
	return FVector(1.0f - SqU, SqU * (1.0f - V), SqU * V);
}

// ─────────────────────────────────────────────
//  Element -- Async resource loading
// ─────────────────────────────────────────────

bool FPCGMeshSurfaceScatterElement::PrepareDataInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGMeshSurfaceScatterElement::PrepareData);

	const UPCGMeshSurfaceScatterSettings* Settings =
		Context->GetInputSettings<UPCGMeshSurfaceScatterSettings>();
	check(Settings);

	FPCGMeshSurfaceScatterContext* ThisContext = static_cast<FPCGMeshSurfaceScatterContext*>(Context);

	// Determine whether the In pin carries point data -- this gates both the per-point
	// rock-mesh gather (below) and the actor-tag fallback discovery. Reading input via
	// ToBasePointData here mirrors what ExecuteInternal does, so the two phases agree.
	const TArray<FPCGTaggedData> Inputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	bool bHasInputPin = false;

	if (!ThisContext->WasLoadRequested())
	{
		TArray<FSoftObjectPath> ToLoad;

		// (i) Static grass sets -- both the main and the landscape-projected variants.
		auto GatherSet = [&ToLoad](const TArray<FPCGSurfGrassMeshEntry>& Entries)
			{
				for (const FPCGSurfGrassMeshEntry& Entry : Entries)
				{
					if (!Entry.Mesh.IsNull())
					{
						ToLoad.AddUnique(Entry.Mesh.ToSoftObjectPath());
					}
				}
			};
		GatherSet(Settings->GrassMeshSet);
		GatherSet(Settings->LandscapeGrassMeshSet);

		// (ii) Data-dependent rock meshes -- one soft path per input point's
		//      SourceMeshPathAttribute. Mirrors the Static Mesh Spawner's
		//      "by attribute" gather (PCGStaticMeshSpawner.cpp ~601).
		for (const FPCGTaggedData& Tagged : Inputs)
		{
			const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Tagged.Data);
			if (!SpatialData)
			{
				continue;
			}
			const UPCGBasePointData* PtData = SpatialData->ToBasePointData(Context);
			if (!PtData)
			{
				continue;
			}
			const int32 NumPoints = PtData->GetNumPoints();
			if (NumPoints == 0)
			{
				continue;
			}
			bHasInputPin = true;

			const UPCGMetadata* Meta = PtData->ConstMetadata();
			const TConstPCGValueRange<int64> MetadataEntryRange = PtData->GetConstMetadataEntryValueRange();

			for (int32 PointIdx = 0; PointIdx < NumPoints; ++PointIdx)
			{
				const FString MeshPath = ReadStringAttr(Meta, Settings->SourceMeshPathAttribute, MetadataEntryRange[PointIdx]);
				if (MeshPath.IsEmpty())
				{
					continue;
				}
				ToLoad.AddUnique(FSoftObjectPath(MeshPath));
			}
		}

		// (iii) Actor-tag fallback discovery -- GAME-THREAD-ONLY. When there's no input-pin
		//       data, walk the world for tagged actors' StaticMeshComponents. TActorIterator
		//       must not run off the game thread, so the discovery happens here in PrepareData
		//       and the results are cached on the context for ExecuteInternal to consume.
		if (!bHasInputPin && !Settings->ActorTagFallback.IsNone() && !ThisContext->bFallbackGathered)
		{
			UWorld* World = nullptr;
			if (UObject* SourceObj = Context->ExecutionSource.GetObject())
			{
				World = SourceObj->GetWorld();
			}

			if (World)
			{
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!Actor->ActorHasTag(Settings->ActorTagFallback))
					{
						continue;
					}
					TArray<UStaticMeshComponent*> SMCs;
					Actor->GetComponents<UStaticMeshComponent>(SMCs);
					for (UStaticMeshComponent* SMC : SMCs)
					{
						UStaticMesh* Mesh = SMC->GetStaticMesh();
						if (!Mesh)
						{
							continue;
						}
						FPCGMeshSurfaceScatterContext::FFallbackRock Rock;
						Rock.Mesh = Mesh;
						Rock.WorldTransform = SMC->GetComponentTransform();
						ThisContext->FallbackRocks.Add(Rock);

						// Defensive: these meshes are referenced by a spawned component so they
						// are already resident, but add their soft paths to the load set so the
						// streamable handle keeps them alive through Execute.
						ToLoad.AddUnique(FSoftObjectPath(Mesh));
					}
				}
			}

			ThisContext->bFallbackGathered = true;
		}

		// Async load suspends the task (returns false) until streaming completes; the context
		// holds the streamable handle so the meshes (grass + rocks) stay resident through
		// Execute. A synchronous load returns true immediately.
		if (!ThisContext->RequestResourceLoad(ThisContext, MoveTemp(ToLoad), !Settings->bSynchronousLoad))
		{
			return false;
		}
	}

	return true;
}

// ─────────────────────────────────────────────
//  Element — main execution
// ─────────────────────────────────────────────

bool FPCGMeshSurfaceScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGMeshSurfaceScatterElement::Execute);

	const UPCGMeshSurfaceScatterSettings* Settings =
		Context->GetInputSettings<UPCGMeshSurfaceScatterSettings>();
	check(Settings);

	// ── World ──
	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}
	if (!World)
	{
		PCGE_LOG_C(Error, GraphAndLog, Context, NSLOCTEXT("PCGMeshSurfaceScatter", "NoWorld",
			"Mesh Surface Scatter: Could not get UWorld."));
		return true;
	}

	// ── Grass mesh cache ──
	TArray<FGrassMeshCache> GrassCache;
	float GrassTotalWeight = 0.0f;
	if (!BuildGrassMeshCache(Settings->GrassMeshSet, GrassCache, GrassTotalWeight))
	{
		PCGE_LOG_C(Error, GraphAndLog, Context, NSLOCTEXT("PCGMeshSurfaceScatter", "NoGrassMesh",
			"Mesh Surface Scatter: No valid grass meshes configured."));
		return true;
	}

	// ── Landscape grass cache (optional, falls back to main cache if empty) ──
	TArray<FGrassMeshCache> LandscapeGrassCache;
	float LandscapeGrassTotalWeight = 0.0f;
	const bool bHasLandscapeGrass = BuildGrassMeshCache(
		Settings->LandscapeGrassMeshSet, LandscapeGrassCache, LandscapeGrassTotalWeight);

	// ── Collect rock mesh inputs ──
	// Each entry: mesh ptr + world transform (from the rock point).
	struct FRockInput
	{
		UStaticMesh* Mesh;
		FTransform WorldTransform;
		int32 Seed;
	};
	TArray<FRockInput> RockInputs;

	const TArray<FPCGTaggedData> Inputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	bool bHasInputPin = false;
	for (const FPCGTaggedData& Tagged : Inputs)
	{
		// Use ToBasePointData() instead of Cast<UPCGBasePointData> -- rock graph outputs
		// may arrive as composite/union data that silently fails a direct cast.
		const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Tagged.Data);
		if (!SpatialData)
		{
			continue;
		}
		const UPCGBasePointData* PtData = SpatialData->ToBasePointData(Context);
		if (!PtData)
		{
			continue;
		}
		const int32 NumPoints = PtData->GetNumPoints();
		if (NumPoints == 0)
		{
			continue;
		}
		bHasInputPin = true;
		const UPCGMetadata* Meta = PtData->ConstMetadata();

		const TConstPCGValueRange<FTransform> TransformRange = PtData->GetConstTransformValueRange();
		const TConstPCGValueRange<int32> SeedRange = PtData->GetConstSeedValueRange();
		const TConstPCGValueRange<int64> MetadataEntryRange = PtData->GetConstMetadataEntryValueRange();

		for (int32 PointIdx = 0; PointIdx < NumPoints; ++PointIdx)
		{
			const FString MeshPath = ReadStringAttr(Meta, Settings->SourceMeshPathAttribute, MetadataEntryRange[PointIdx]);
			if (MeshPath.IsEmpty())
			{
				continue;
			}
			// Already resident -- PrepareDataInternal async-loaded every per-point rock
			// path, so ResolveObject() returns the loaded mesh without a sync load.
			UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(MeshPath).ResolveObject());
			if (!Mesh)
			{
				continue;
			}
			RockInputs.Add({ Mesh, TransformRange[PointIdx], SeedRange[PointIdx] });
		}
	}

	// ── Fallback: actors-by-tag discovery resolved during PrepareData ──
	// The TActorIterator world walk is game-thread-only, so it ran in PrepareDataInternal
	// and cached its results on the context. Here (on a worker thread) we just resolve the
	// already-resident meshes via .Get() and feed them into RockInputs.
	if (!bHasInputPin && !Settings->ActorTagFallback.IsNone())
	{
		FPCGMeshSurfaceScatterContext* ThisContext = static_cast<FPCGMeshSurfaceScatterContext*>(Context);
		for (const FPCGMeshSurfaceScatterContext::FFallbackRock& Rock : ThisContext->FallbackRocks)
		{
			// Resident after PrepareData's async load -- .Get() returns without a sync load.
			UStaticMesh* Mesh = Rock.Mesh.Get();
			if (!Mesh)
			{
				continue;
			}
			RockInputs.Add({ Mesh, Rock.WorldTransform, 0 });
		}
	}

	if (RockInputs.Num() == 0)
	{
		PCGE_LOG_C(Warning, GraphAndLog, Context, NSLOCTEXT("PCGMeshSurfaceScatter", "NoRocks",
			"Mesh Surface Scatter: No rock meshes found to sample."));
		return true;
	}

	// ── Prepare outputs ──
	// This node generates a variable number of points, so we accumulate plain-old-data
	// during sampling and build the UPCGBasePointData (SetNumPoints + AllocateProperties
	// + value-range fill) once the counts are known.
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	// Accepted-point accumulators.
	struct FAcceptedPoint
	{
		FTransform Transform;
		int32 Seed;
		FSoftObjectPath MeshPath;
	};
	TArray<FAcceptedPoint> AcceptedPoints;

	// Rejected-point accumulators (debug only).
	struct FRejectedPoint
	{
		FVector Location;
		float Density;
	};
	TArray<FRejectedPoint> RejectedPoints;
	const bool bOutputRejected = Settings->bOutputRejected;

	// ── Landscape trace setup ──
	FCollisionQueryParams TraceParams;
	TraceParams.bTraceComplex = false;
	TraceParams.bReturnPhysicalMaterial = false;

	// ── Min-distance grid (global across all rocks so grass from adjacent rocks doesn't overlap) ──
	// TODO(audit): FMinDistGrid bins by XY only, so a point on an overhead surface is
	// suppressed by a projected base point directly below it (same XY, different Z). On
	// stacked/overhanging geometry this thins out the upper surface. Decide
	// whether the spacing test should be fully 3D (cell + Z bucket) or stay XY-projected.
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	PCGExtScatterCommon::FMinDistGrid SpacingGrid;
	SpacingGrid.CellSize = FMath::Max(MinDist, 1.0f);

	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	// ── Process each rock mesh ──
	int32 TotalSampled = 0, TotalAccepted = 0, TotalSubmerged = 0, TotalProjected = 0;

	for (int32 RockIdx = 0; RockIdx < RockInputs.Num(); ++RockIdx)
	{
		const FRockInput& Rock = RockInputs[RockIdx];

		// 1. Build mesh topology (vertices, triangles, adjacency, walkable, curvature, BFS edge dist).
		TArray<FTriData> Tris;
		if (!BuildTopology(Rock.Mesh, Rock.WorldTransform,
			Settings->MinNormalDotUp, Settings->MaxNeighborAngleDeg, Tris))
		{
			continue;
		}

		// 2. Filter to eligible triangles and build area-weighted CDF.
		float TotalEligibleArea = 0.0f;
		TArray<int32> EligibleTris;
		EligibleTris.Reserve(Tris.Num());

		for (int32 T = 0; T < Tris.Num(); ++T)
		{
			FTriData& Tri = Tris[T];
			if (!Tri.bWalkable)
			{
				continue;
			}
			if (Tri.EdgeDist < Settings->MinEdgeDistance)
			{
				continue;
			}
			TotalEligibleArea += Tri.Area;
			Tri.CumulativeArea = TotalEligibleArea;
			EligibleTris.Add(T);
		}

		if (EligibleTris.Num() == 0 || TotalEligibleArea < 1.0f)
		{
			continue;
		}

		// 3. Determine sample count from area × density.
		const double AreaSqM = TotalEligibleArea * PCGMeshSurfaceScatter::SqCmToSqM;
		const int32 TargetCount = FMath::Max(1, FMath::RoundToInt(AreaSqM * Settings->PointsPerSquareMeter));

		const int32 RockSeed = PCGHelpers::ComputeSeed(BaseSeed, Rock.Seed + RockIdx);
		FRandomStream Rng(RockSeed);

		int32 RockAccepted = 0, RockSubmerged = 0, RockProjected = 0;

		for (int32 SampleIdx = 0; SampleIdx < TargetCount; ++SampleIdx)
		{
			// 4a. Area-weighted triangle selection.
			// TODO(audit) perf: this is a linear scan of the area CDF per sample --
			// O(samples * eligibleTris). CumulativeArea is monotonic, so Algo::UpperBound
			// over the eligible-tri CDF would make this O(samples * log(eligibleTris)).
			// Left as-is here to keep the migration behaviour-preserving.
			const float AreaRoll = Rng.FRandRange(0.0f, TotalEligibleArea);
			int32 ChosenEligible = 0;
			for (int32 E = 0; E < EligibleTris.Num(); ++E)
			{
				if (AreaRoll <= Tris[EligibleTris[E]].CumulativeArea)
				{
					ChosenEligible = E;
					break;
				}
			}
			const FTriData& Tri = Tris[EligibleTris[ChosenEligible]];

			// 4b. Random point within the triangle (uniform barycentric).
			const FVector Bary = RandomBarycentric(Rng);
			FVector SamplePos = Tri.V0 * Bary.X + Tri.V1 * Bary.Y + Tri.V2 * Bary.Z;
			FVector SampleNormal = Tri.Normal;
			bool bProjectedToLandscape = false;

			// 5. Submerged handling — multi-trace to landscape. Points below terrain
			//    are either projected onto the landscape surface (filling the gap that
			//    ground-cover exclusion zones create) or rejected outright.
			if (Settings->bRejectSubmerged)
			{
				const FVector TraceStart(SamplePos.X, SamplePos.Y,
					SamplePos.Z + Settings->TraceStartHeight);
				const FVector TraceEnd(SamplePos.X, SamplePos.Y,
					SamplePos.Z - Settings->TraceDistance);
				TArray<FHitResult> Hits;
				World->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd,
					Settings->LandscapeTraceChannel, TraceParams);

				// Find the landscape hit by actor type — capture both Z and normal.
				float LandscapeZ = -BIG_NUMBER;
				FVector LandscapeNormal = PCGExtScatterCommon::WorldUp;
				for (const FHitResult& H : Hits)
				{
					AActor* HitActor = H.GetActor();
					if (HitActor && HitActor->IsA<ALandscapeProxy>())
					{
						LandscapeZ = H.ImpactPoint.Z;
						LandscapeNormal = H.ImpactNormal;
						break;
					}
				}

				if (LandscapeZ > -BIG_NUMBER + 1.0f)
				{
					// Landscape found. Check if sample sits at or below terrain.
					if (SamplePos.Z < LandscapeZ + Settings->MinHeightAboveLandscape)
					{
						++RockSubmerged;

						if (Settings->bProjectSubmergedToLandscape)
						{
							// Project to landscape: keep XY, snap Z to terrain,
							// use landscape normal for grass alignment.
							SamplePos.Z = LandscapeZ + Settings->LandscapeProjectionZOffset;
							SampleNormal = LandscapeNormal;
							bProjectedToLandscape = true;
							++RockProjected;
							// Fall through to noise → spacing → output.
						}
						else
						{
							if (bOutputRejected)
							{
								RejectedPoints.Add({ SamplePos, 0.0f });
							}
							continue;
						}
					}
				}
				// No landscape found below → treat as fully exposed (floating rock, etc.).
			}

			// 6. Noise density modulation.
			if (Settings->bUseNoise)
			{
				const FVector SeedOffset(Settings->NoiseSeed * 13.13f,
					Settings->NoiseSeed * 7.77f,
					Settings->NoiseSeed * 3.33f);
				const float N = FMath::PerlinNoise3D(SamplePos * Settings->NoiseFrequency + SeedOffset);
				const float NoiseVal = FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
				if (NoiseVal < Settings->NoiseDensityMin)
				{
					if (bOutputRejected)
					{
						RejectedPoints.Add({ SamplePos, NoiseVal });
					}
					continue;
				}
			}

			// 7. Min-distance pruning.
			if (MinDist > 0.0f && SpacingGrid.HasWithin(SamplePos, MinDistSq))
			{
				if (bOutputRejected)
				{
					RejectedPoints.Add({ SamplePos, 0.0f });
				}
				continue;
			}

			// 8. Build the output point.
			const int32 PointSeed = PCGHelpers::ComputeSeed(RockSeed, SampleIdx);
			FRandomStream PointRng(PointSeed);

			// Select grass mesh from the appropriate cache: landscape-projected
			// points use the landscape set (if populated), otherwise fall back.
			const bool bUseLandscapeCache = bProjectedToLandscape && bHasLandscapeGrass;
			const TArray<FGrassMeshCache>& ActiveCache = bUseLandscapeCache
				? LandscapeGrassCache : GrassCache;
			const float ActiveTotalWeight = bUseLandscapeCache
				? LandscapeGrassTotalWeight : GrassTotalWeight;

			const int32 GrassIdx = SelectGrassMeshIndex(ActiveCache, ActiveTotalWeight, PointRng);
			const FGrassMeshCache& Chosen = ActiveCache[GrassIdx];

			// Scale: landscape-projected points use LandscapeTierScaleRange.
			const FVector2D& ActiveTierScale = bProjectedToLandscape
				? Settings->LandscapeTierScaleRange : Settings->TierScaleRange;
			const float TierScale = PointRng.FRandRange(ActiveTierScale.X, ActiveTierScale.Y);
			const float MeshScale = PointRng.FRandRange(Chosen.ScaleRange.X, Chosen.ScaleRange.Y);
			const float FinalScale = TierScale * MeshScale;

			const float YawDeg = Settings->bRandomYaw ? PointRng.FRandRange(0.0f, 360.0f) : 0.0f;
			const float JitterP = PointRng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const float JitterR = PointRng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const FQuat Rot = MakeSurfaceRotation(SampleNormal, YawDeg,
				Settings->SlopeAlignAmount, Settings->MaxAlignAngleDeg, JitterP, JitterR);

			// Z offset: landscape-projected points use LandscapeGrassZOffset.
			const float ActiveZOffset = bProjectedToLandscape
				? Settings->LandscapeGrassZOffset : Settings->ZOffset;
			const FVector FinalPos = SamplePos + SampleNormal * ActiveZOffset;

			// TODO(audit): bWriteDensity currently has no effect -- density is written as a
			// constant 1.0f either way (the original `bWriteDensity ? 1.0f : 1.0f` no-op).
			// The computed noise value (NoiseVal) is local to the noise block above and not
			// in scope here, so no value is fabricated. Decide what density should
			// be written when bWriteDensity is true (e.g. plumb NoiseVal through), and what
			// it should be when false.
			FAcceptedPoint Accepted;
			Accepted.Transform = FTransform(Rot, FinalPos, FVector(FinalScale));
			Accepted.Seed = PointSeed;
			Accepted.MeshPath = Chosen.MeshPath;
			AcceptedPoints.Add(Accepted);

			if (MinDist > 0.0f)
			{
				SpacingGrid.Add(SamplePos);
			}
			++RockAccepted;
		}

		TotalSampled += TargetCount;
		TotalAccepted += RockAccepted;
		TotalSubmerged += RockSubmerged;
		TotalProjected += RockProjected;

#if WITH_EDITOR
		if (Settings->bLogStats)
		{
			UE_LOG(LogTemp, Log,
				TEXT("[MeshSurfaceScatter] Rock %d: mesh=%s  tris=%d  eligible=%d  "
					"area=%.1f sqm  target=%d  accepted=%d  submerged=%d  projected=%d"),
				RockIdx, *Rock.Mesh->GetName(),
				Tris.Num(), EligibleTris.Num(),
				TotalEligibleArea * PCGMeshSurfaceScatter::SqCmToSqM, TargetCount,
				RockAccepted, RockSubmerged, RockProjected);
		}
#endif
	}

#if WITH_EDITOR
	if (Settings->bLogStats)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[MeshSurfaceScatter] TOTAL: rocks=%d  sampled=%d  accepted=%d  submerged=%d  projected=%d"),
			RockInputs.Num(), TotalSampled, TotalAccepted, TotalSubmerged, TotalProjected);
	}
#endif

	// ── Emit accepted points (generate pattern) ──
	if (AcceptedPoints.Num() > 0)
	{
		const int32 NumAccepted = AcceptedPoints.Num();

		UPCGBasePointData* OutData = FPCGContext::NewPointData_AnyThread(Context);
		OutData->SetNumPoints(NumAccepted, /*bInitializeValues=*/false);
		OutData->AllocateProperties(
			EPCGPointNativeProperties::Transform |
			EPCGPointNativeProperties::Seed |
			EPCGPointNativeProperties::MetadataEntry);

		// Uniform native properties (same for every accepted point).
		OutData->SetDensity(1.0f);
		OutData->SetSteepness(1.0f);
		OutData->SetExtents(FVector(8.0f));

		UPCGMetadata* OutMeta = OutData->MutableMetadata();

		// Soft-object-path attribute so the By-Attribute Static Mesh Spawner consumes
		// the mesh path natively (was previously stored as an FString).
		OutMeta->CreateSoftObjectPathAttribute(
			Settings->OutputMeshPathAttribute, FSoftObjectPath(), /*bAllowsInterpolation=*/false);
		FPCGMetadataAttribute<FSoftObjectPath>* MeshPathAttr =
			OutMeta->GetMutableTypedAttribute<FSoftObjectPath>(Settings->OutputMeshPathAttribute);

		TPCGValueRange<FTransform> TransformRange = OutData->GetTransformValueRange();
		TPCGValueRange<int32> SeedRange = OutData->GetSeedValueRange();
		TPCGValueRange<int64> MetadataEntryRange = OutData->GetMetadataEntryValueRange();

		for (int32 i = 0; i < NumAccepted; ++i)
		{
			const FAcceptedPoint& Accepted = AcceptedPoints[i];
			TransformRange[i] = Accepted.Transform;
			SeedRange[i] = Accepted.Seed;

			MetadataEntryRange[i] = PCGInvalidEntryKey;
			OutMeta->InitializeOnSet(MetadataEntryRange[i]);
			if (MeshPathAttr)
			{
				MeshPathAttr->SetValue(MetadataEntryRange[i], Accepted.MeshPath);
			}
		}

		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = OutData;
		T.Pin = PCGPinConstants::DefaultOutputLabel;
	}

	// ── Emit rejected points (debug only; no mesh attribute) ──
	if (bOutputRejected && RejectedPoints.Num() > 0)
	{
		const int32 NumRejected = RejectedPoints.Num();

		UPCGBasePointData* RejData = FPCGContext::NewPointData_AnyThread(Context);
		RejData->SetNumPoints(NumRejected, /*bInitializeValues=*/false);
		RejData->AllocateProperties(
			EPCGPointNativeProperties::Transform |
			EPCGPointNativeProperties::Density);

		RejData->SetSteepness(1.0f);

		TPCGValueRange<FTransform> RejTransformRange = RejData->GetTransformValueRange();
		TPCGValueRange<float> RejDensityRange = RejData->GetDensityValueRange();

		for (int32 i = 0; i < NumRejected; ++i)
		{
			RejTransformRange[i] = FTransform(RejectedPoints[i].Location);
			RejDensityRange[i] = RejectedPoints[i].Density;
		}

		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = RejData;
		T.Pin = PCGExtScatterCommon::RejectedPinLabel;
	}

	return true;
}