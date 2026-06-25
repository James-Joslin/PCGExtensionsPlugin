// PCGMeshSurfaceScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGMeshSurfaceScatter.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "CollisionQueryParams.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "LandscapeProxy.h"
#include "EngineUtils.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGMeshSurfaceScatter)

namespace
{
	const FName RejectedLabel(TEXT("Rejected"));
	const FVector WorldUp(0.0, 0.0, 1.0);

	// Square-centimetre → square-metre conversion (UU are cm in UE).
	constexpr double SqCmToSqM = 1.0 / (100.0 * 100.0);

	// ── Uniform XY hash grid for min-distance pruning (same as GroundCoverScatter). ──
	struct FMinDistGrid
	{
		float CellSize = 100.0f;
		TMap<FIntPoint, TArray<FVector>> Cells;

		FORCEINLINE FIntPoint CellOf(const FVector& P) const
		{
			return FIntPoint(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize));
		}
		FORCEINLINE void Add(const FVector& P) { Cells.FindOrAdd(CellOf(P)).Add(P); }

		bool HasWithin(const FVector& P, float RadiusSq) const
		{
			const FIntPoint C = CellOf(P);
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					if (const TArray<FVector>* Arr = Cells.Find(FIntPoint(C.X + dx, C.Y + dy)))
					{
						for (const FVector& Q : *Arr)
						{
							if (FVector::DistSquared(P, Q) < RadiusSq)
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}
	};

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

	Pins.Emplace(RejectedLabel,
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
		UStaticMesh* Loaded = Entry.Mesh.LoadSynchronous();
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
	const FPCGMetadataAttributeBase* Base = Meta->GetConstAttribute(Name);
	if (!Base || Base->GetTypeId() != PCG::Private::MetadataTypes<FString>::Id)
	{
		return FString();
	}
	const auto* Attr = static_cast<const FPCGMetadataAttribute<FString>*>(Base);
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
		Tri.DotUp = FVector::DotProduct(Tri.Normal, WorldUp);

		// Accept both winding directions — the "up" face is whichever has a positive DotUp.
		// If the mesh has inverted winding, the normal points down; use the flipped version.
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
	TArray<FDistEntry> Heap;
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
		FDistEntry Current;
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
	FVector DesiredUp = WorldUp;

	if (AlignAmount > 0.0f)
	{
		const float NDot = FMath::Clamp(FVector::DotProduct(WorldUp, N), -1.0f, 1.0f);
		const float FullAngle = FMath::Acos(NDot);
		if (FullAngle > KINDA_SMALL_NUMBER)
		{
			const float MaxRad = FMath::DegreesToRadians(MaxAlignDeg);
			float Alpha = AlignAmount;
			if (FullAngle * Alpha > MaxRad)
			{
				Alpha = MaxRad / FullAngle;
			}
			DesiredUp = FMath::Lerp(WorldUp, N, Alpha).GetSafeNormal();
		}
	}

	const FQuat AlignQuat = FQuat::FindBetweenNormals(WorldUp, DesiredUp);
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
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGMeshSurfaceScatter", "NoWorld",
			"Mesh Surface Scatter: Could not get UWorld."));
		return true;
	}

	// ── Grass mesh cache ──
	TArray<FGrassMeshCache> GrassCache;
	float GrassTotalWeight = 0.0f;
	if (!BuildGrassMeshCache(Settings->GrassMeshSet, GrassCache, GrassTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGMeshSurfaceScatter", "NoGrassMesh",
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
		// Use ToPointData() instead of Cast<UPCGPointData> — rock graph outputs
		// may arrive as composite/union data that silently fails a direct cast.
		const UPCGSpatialData* SpatialData = Cast<UPCGSpatialData>(Tagged.Data);
		if (!SpatialData)
		{
			continue;
		}
		const UPCGPointData* PtData = SpatialData->ToPointData(Context);
		if (!PtData)
		{
			continue;
		}
		const TArray<FPCGPoint>& Points = PtData->GetPoints();
		if (Points.Num() == 0)
		{
			continue;
		}
		bHasInputPin = true;
		const UPCGMetadata* Meta = PtData->ConstMetadata();

		for (const FPCGPoint& P : Points)
		{
			const FString MeshPath = ReadStringAttr(Meta, Settings->SourceMeshPathAttribute, P.MetadataEntry);
			if (MeshPath.IsEmpty())
			{
				continue;
			}
			UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(MeshPath).TryLoad());
			if (!Mesh)
			{
				continue;
			}
			RockInputs.Add({ Mesh, P.Transform, P.Seed });
		}
	}

	// ── Fallback: find actors by tag ──
	if (!bHasInputPin && !Settings->ActorTagFallback.IsNone())
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
				RockInputs.Add({ Mesh, SMC->GetComponentTransform(), 0 });
			}
		}
	}

	if (RockInputs.Num() == 0)
	{
		PCGE_LOG(Warning, GraphAndLog, NSLOCTEXT("PCGMeshSurfaceScatter", "NoRocks",
			"Mesh Surface Scatter: No rock meshes found to sample."));
		return true;
	}

	// ── Prepare outputs ──
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	UPCGPointData* OutData = NewObject<UPCGPointData>();
	OutData->InitializeFromData(nullptr);
	TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
	UPCGMetadata* OutMeta = OutData->MutableMetadata();

	FPCGMetadataAttribute<FString>* MeshPathAttr =
		OutMeta->FindOrCreateAttribute<FString>(
			Settings->OutputMeshPathAttribute, FString(),
			/*bAllowInterpolation=*/false, /*bOverrideParent=*/true);

	UPCGPointData* RejData = nullptr;
	TArray<FPCGPoint>* RejPoints = nullptr;
	if (Settings->bOutputRejected)
	{
		RejData = NewObject<UPCGPointData>();
		RejData->InitializeFromData(nullptr);
		RejPoints = &RejData->GetMutablePoints();
	}

	// ── Landscape trace setup ──
	FCollisionQueryParams TraceParams;
	TraceParams.bTraceComplex = false;
	TraceParams.bReturnPhysicalMaterial = false;

	// ── Min-distance grid (global across all rocks so grass from adjacent rocks doesn't overlap) ──
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	FMinDistGrid SpacingGrid;
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
		const double AreaSqM = TotalEligibleArea * SqCmToSqM;
		const int32 TargetCount = FMath::Max(1, FMath::RoundToInt(AreaSqM * Settings->PointsPerSquareMeter));

		const int32 RockSeed = PCGHelpers::ComputeSeed(BaseSeed, Rock.Seed + RockIdx);
		FRandomStream Rng(RockSeed);

		int32 RockAccepted = 0, RockSubmerged = 0, RockProjected = 0;

		for (int32 SampleIdx = 0; SampleIdx < TargetCount; ++SampleIdx)
		{
			// 4a. Area-weighted triangle selection.
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
				FVector LandscapeNormal = WorldUp;
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
							if (RejPoints)
							{
								FPCGPoint Rej;
								Rej.Transform.SetLocation(SamplePos);
								Rej.Density = 0.0f;
								RejPoints->Add(Rej);
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
					if (RejPoints)
					{
						FPCGPoint Rej;
						Rej.Transform.SetLocation(SamplePos);
						Rej.Density = NoiseVal;
						RejPoints->Add(Rej);
					}
					continue;
				}
			}

			// 7. Min-distance pruning.
			if (MinDist > 0.0f && SpacingGrid.HasWithin(SamplePos, MinDistSq))
			{
				if (RejPoints)
				{
					FPCGPoint Rej;
					Rej.Transform.SetLocation(SamplePos);
					Rej.Density = 0.0f;
					RejPoints->Add(Rej);
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

			FPCGPoint NewPoint;
			NewPoint.Seed = PointSeed;
			NewPoint.Density = Settings->bWriteDensity ? 1.0f : 1.0f;
			NewPoint.Steepness = 1.0f;
			NewPoint.SetExtents(FVector(8.0f));
			NewPoint.Transform = FTransform(Rot, FinalPos, FVector(FinalScale));

			NewPoint.MetadataEntry = PCGInvalidEntryKey;
			OutMeta->InitializeOnSet(NewPoint.MetadataEntry);
			if (MeshPathAttr)
			{
				MeshPathAttr->SetValue(NewPoint.MetadataEntry, Chosen.MeshPath.ToString());
			}

			OutPoints.Add(NewPoint);
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
				TotalEligibleArea * SqCmToSqM, TargetCount,
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

	// ── Emit ──
	if (OutPoints.Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = OutData;
		T.Pin = PCGPinConstants::DefaultOutputLabel;
	}
	if (RejData && RejPoints && RejPoints->Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = RejData;
		T.Pin = RejectedLabel;
	}

	return true;
}