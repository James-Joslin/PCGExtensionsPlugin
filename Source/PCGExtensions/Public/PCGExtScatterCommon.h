// PCGExtScatterCommon.h
//
// Shared helpers for the scatter nodes (GroundCover / MeshSurface / TieredVegetation /
// ScatterAroundPoints). These were previously duplicated inside per-file anonymous
// namespaces, which collide under UBT Unity builds (the .cpp files concatenate into one
// translation unit, so two anonymous-namespace `FMinDistGrid`/`RejectedLabel` definitions
// become an ODR/C2084 redefinition). Hoisting them here under a named namespace with
// `inline` linkage fixes that and keeps a single source of truth.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "Engine/StaticMesh.h"

namespace PCGExtScatterCommon
{
	/** World up axis, shared by every scatter node's slope/alignment math. */
	inline const FVector WorldUp(0.0, 0.0, 1.0);

	/** Common output-pin labels. */
	inline const FName RejectedPinLabel(TEXT("Rejected"));
	inline const FName CompanionsPinLabel(TEXT("Companions"));

	/**
	 * Uniform XY hash grid for min-distance pruning.
	 * CellSize must be >= the query radius so the 3x3 neighbour scan is exhaustive.
	 * Stored positions are 3D but binned by XY only.
	 */
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

	/**
	 * Bounds + weight for one mesh, resolved on the game thread during PrepareData and read
	 * (off-thread) during Execute. Shared by the mesh-bounds scatter nodes so the bounds-cache
	 * type and its build loop live in one place.
	 */
	struct FMeshBoundsCache
	{
		FSoftObjectPath MeshPath;
		FVector BoundsMin = FVector::ZeroVector;  // Local-space min (e.g. -HalfX, -HalfY, 0 for base pivot)
		FVector BoundsMax = FVector::ZeroVector;  // Local-space max (e.g. +HalfX, +HalfY, +Height)
		float CumulativeWeight = 0.0f;            // Running weight sum, for weighted random selection
	};

	/**
	 * Build an FMeshBoundsCache array from any entry type exposing `.Mesh` (TSoftObjectPtr<UStaticMesh>)
	 * and `.Weight` (float). Meshes must already be resident (async-loaded in PrepareData) -- this only
	 * calls .Get(), never sync-loads. Null/unloaded entries are skipped; their paths are appended to
	 * OutFailedPaths when provided so the caller can warn. Returns true if any entry was cached.
	 *
	 * MUST run on the game thread: UStaticMesh::GetBoundingBox() is not thread-safe.
	 */
	template <typename EntryT>
	bool BuildMeshBoundsCache(
		const TArray<EntryT>& Entries,
		TArray<FMeshBoundsCache>& OutCache,
		float& OutTotalWeight,
		TArray<FSoftObjectPath>* OutFailedPaths = nullptr)
	{
		OutCache.Reset();
		OutCache.Reserve(Entries.Num());
		OutTotalWeight = 0.0f;

		for (const EntryT& Entry : Entries)
		{
			UStaticMesh* LoadedMesh = Entry.Mesh.Get();
			if (!LoadedMesh)
			{
				if (OutFailedPaths) { OutFailedPaths->Add(Entry.Mesh.ToSoftObjectPath()); }
				continue;
			}

			FMeshBoundsCache CacheEntry;
			CacheEntry.MeshPath = Entry.Mesh.ToSoftObjectPath();
			const FBox MeshBounds = LoadedMesh->GetBoundingBox();
			CacheEntry.BoundsMin = MeshBounds.Min;
			CacheEntry.BoundsMax = MeshBounds.Max;
			OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
			CacheEntry.CumulativeWeight = OutTotalWeight;
			OutCache.Add(CacheEntry);
		}

		return OutCache.Num() > 0;
	}

	// NOTE(audit): The weighted *selection* step is still duplicated across every scatter node
	// (DynamicMeshEmbed, GroundCover, MeshSurface, RockFormation, TieredVegetation): each picks via
	// `Roll = Rng.FRandRange(0, TotalWeight); for (i) if (Roll <= Cache[i].CumulativeWeight)`.
	// This predates the 5.7 migration and spans untouched code (incl. nodes whose caches carry no
	// bounds), so a shared WeightedPick() is left for the repo owner to consolidate.
}
