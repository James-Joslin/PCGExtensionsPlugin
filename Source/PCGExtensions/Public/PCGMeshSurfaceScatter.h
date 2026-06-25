// PCGMeshSurfaceScatter.h
// Custom PCG node that samples static mesh surfaces (e.g. rocks) and places
// grass / foliage only on flat, wide, non-edge regions. Handles partially
// submerged rocks by either rejecting sample points below terrain or projecting
// them onto the landscape surface (filling the gap that ground-cover exclusion
// zones create around rock footprints).
//
// Key surface-quality filters:
//   • Normal dot-up:        only near-horizontal faces
//   • Edge distance (BFS):  rejects points close to the boundary of the
//                            walkable surface — catches narrow ridges, tips,
//                            and mesh perimeter
//   • Neighbor curvature:   rejects triangles whose neighbors deviate sharply
//                            (pointed or creased geometry)
//
// Mesh access uses GetRenderData() LOD0 vertex/index buffers. Meshes must
// have "Allow CPU Access" enabled in the mesh asset, or a bounding-box
// fallback is used (top face only).
//
// Place in your project's Source/<Module>/Public/ directory.
// Requires "PCG" and "Landscape" in Build.cs PublicDependencyModuleNames.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "Engine/StaticMesh.h"

#include "PCGMeshSurfaceScatter.generated.h"

// ─────────────────────────────────────────────
//  Weighted grass/foliage mesh entry
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGSurfGrassMeshEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Relative selection probability within the set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh", meta = (ClampMin = "0.01"))
	float Weight = 1.0f;

	/** Per-mesh uniform scale range (multiplied by the tier scale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	FVector2D ScaleRange = FVector2D(0.8f, 1.2f);
};

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGMeshSurfaceScatterSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGMeshSurfaceScatterSettings();

	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("MeshSurfaceScatter")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGMeshSurfaceScatter", "NodeTitle", "Mesh Surface Scatter");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGMeshSurfaceScatter", "Tooltip",
			"Samples rock/mesh surfaces and places grass/foliage only on flat, wide, "
			"non-edge areas. Filters by surface normal, edge distance (rejects narrow "
			"ridges, tips, and mesh perimeter), and curvature. Submerged samples can "
			"be projected onto the landscape surface to fill gaps left by ground-cover "
			"exclusion zones around rock footprints.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Source Mesh Identification ─────────────────────────────────

	/**
	 * Attribute on input rock points holding the mesh soft-object path.
	 * Must match the attribute written by DynamicMeshEmbed / RockFormationGenerator.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Mesh")
	FName SourceMeshPathAttribute = FName(TEXT("MeshPath"));

	/**
	 * Fallback: if the In pin is disconnected, search level actors with this tag
	 * and sample every StaticMeshComponent found.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Source Mesh")
	FName ActorTagFallback = FName(TEXT("PCG_RockSurface"));

	// ── Sampling Density ──────────────────────────────────────────

	/** Target grass instances per square metre of eligible (post-filter) mesh surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density",
		meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float PointsPerSquareMeter = 4.0f;

	/** Min spacing between accepted instances (UU). 0 = no pruning. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density",
		meta = (ClampMin = "0.0"))
	float MinDistance = 20.0f;

	// ── Normal / Slope Filter ─────────────────────────────────────

	/**
	 * Minimum dot(triangleNormal, worldUp) for a triangle to be walkable.
	 * 0.7 ≈ 45°, 0.8 ≈ 37°, 0.95 ≈ 18°.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Filter",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinNormalDotUp = 0.7f;

	// ── Edge-Distance Filter (surface-quality core) ───────────────

	/**
	 * Minimum BFS-propagated distance (UU) from the walkable-surface boundary.
	 * Triangles closer than this to the edge of the flat area are ineligible.
	 * Handles narrow ridges, pointed tips, and mesh-perimeter grass that would
	 * overhang the rock edge. Larger values → more conservative culling.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Filter",
		meta = (ClampMin = "0.0", ClampMax = "500.0"))
	float MinEdgeDistance = 30.0f;

	// ── Curvature Filter ──────────────────────────────────────────

	/**
	 * Maximum average angle (degrees) between a walkable triangle's normal and
	 * its walkable neighbours. Triangles exceeding this are reclassified as
	 * boundary (high curvature = pointed / creased / irregular).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Surface Filter",
		meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxNeighborAngleDeg = 25.0f;

	// ── Submerged-Rock Rejection (landscape trace) ────────────────

	/** If true, trace down to the landscape and reject sample points below terrain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape")
	bool bRejectSubmerged = true;

	/** Trace start above the sample point (UU). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged", ClampMin = "0.0", ClampMax = "20000.0"))
	float TraceStartHeight = 3000.0f;

	/** Total trace length downward (UU). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged", ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged"))
	TEnumAsByte<ECollisionChannel> LandscapeTraceChannel = ECC_WorldStatic;

	/**
	 * Minimum height (UU) a sample point must sit above the landscape to survive.
	 * Positive values add a margin so grass doesn't appear right at the soil line.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged", ClampMin = "-50.0", ClampMax = "200.0"))
	float MinHeightAboveLandscape = 5.0f;

	/**
	 * Instead of discarding submerged samples, project them up to the landscape
	 * surface. This fills the grass gap where ground-cover exclusion zones remove
	 * landscape grass inside rock footprints — the projected points cover the
	 * terrain directly above the buried rock geometry.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged"))
	bool bProjectSubmergedToLandscape = true;

	/**
	 * Z offset for landscape-projected grass (UU above the terrain hit).
	 * Small positive values prevent z-fighting with the landscape surface.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape",
		meta = (EditCondition = "bRejectSubmerged", ClampMin = "-20.0", ClampMax = "50.0"))
	float LandscapeProjectionZOffset = 2.0f;

	// ── Noise (density variation across the mesh surface) ─────────

	/** Enable Perlin-noise density modulation on the mesh surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	bool bUseNoise = true;

	/** Noise spatial frequency. Higher = smaller patches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoise", ClampMin = "0.001", ClampMax = "1.0"))
	float NoiseFrequency = 0.06f;

	/** Samples below this noise density are culled. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoise", ClampMin = "0.0", ClampMax = "1.0"))
	float NoiseDensityMin = 0.15f;

	/** Per-noise seed offset for determinism. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	int32 NoiseSeed = 42;

	// ── Grass Mesh Set ────────────────────────────────────────────

	/** Weighted grass/foliage mesh variants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass Mesh",
		meta = (TitleProperty = "Mesh"))
	TArray<FPCGSurfGrassMeshEntry> GrassMeshSet;

	/** Attribute the chosen grass mesh path is written to. Feed a By-Attribute Static Mesh Spawner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grass Mesh")
	FName OutputMeshPathAttribute = FName(TEXT("MeshPath"));

	// ── Landscape Grass (projected-point overrides) ───────────────

	/**
	 * Separate grass/foliage mesh set for points projected onto the landscape
	 * above submerged rock surfaces. Use this to place different vegetation
	 * where rocks meet the soil — e.g. taller wild grass building up around
	 * rock bases vs. short moss on exposed rock faces.
	 *
	 * If empty, the main GrassMeshSet is used for both rock and landscape points.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape Grass",
		meta = (EditCondition = "bRejectSubmerged && bProjectSubmergedToLandscape",
			TitleProperty = "Mesh"))
	TArray<FPCGSurfGrassMeshEntry> LandscapeGrassMeshSet;

	/**
	 * Layer-level scale range for landscape-projected grass, applied on top of
	 * each LandscapeGrassMeshSet entry's per-mesh ScaleRange. Separate from the
	 * rock-surface TierScaleRange so base-of-rock vegetation can be a different
	 * size than rock-top vegetation.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape Grass",
		meta = (EditCondition = "bRejectSubmerged && bProjectSubmergedToLandscape"))
	FVector2D LandscapeTierScaleRange = FVector2D(0.8f, 1.2f);

	/**
	 * Z offset for landscape-projected grass instances (separate from rock-surface
	 * ZOffset). 0 = flush with terrain. Negative sinks into soil.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Landscape Grass",
		meta = (EditCondition = "bRejectSubmerged && bProjectSubmergedToLandscape"))
	float LandscapeGrassZOffset = 0.0f;

	// ── Transform ─────────────────────────────────────────────────

	/** Layer-level uniform scale range applied on top of each mesh entry's ScaleRange. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector2D TierScaleRange = FVector2D(0.7f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bRandomYaw = true;

	/** Align grass to the mesh surface normal (1) vs world-up (0). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlopeAlignAmount = 0.85f;

	/** Cap on how far alignment can tilt from vertical (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform",
		meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxAlignAngleDeg = 35.0f;

	/** Small random pitch/roll tilt (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform",
		meta = (ClampMin = "0.0", ClampMax = "20.0"))
	float PitchRollJitterDeg = 4.0f;

	/** Z offset after placement (negative sinks blades into the rock surface). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	float ZOffset = -1.5f;

	// ── Output ────────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bWriteDensity = true;

	/** Emit rejected sample points on the Rejected pin for debug visualisation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bOutputRejected = false;

	/** Log per-rock sampling stats (eligible area, point counts). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bLogStats = false;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGMeshSurfaceScatterElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	// ── Mesh topology ──

	struct FTriData
	{
		FVector V0, V1, V2;        // world-space positions
		FVector Normal;            // world-space face normal
		FVector Centroid;
		float Area;                // world-space area (sq UU)
		float DotUp;               // dot(Normal, WorldUp)
		bool bWalkable;            // passes MinNormalDotUp
		bool bCurvatureOK;         // passes MaxNeighborAngleDeg
		float EdgeDist;            // BFS distance from walkable boundary (UU)
		float CumulativeArea;      // for area-weighted sampling (filled after filtering)
	};

	struct FEdgeKey
	{
		int32 A, B; // sorted vertex indices (A < B)
		bool operator==(const FEdgeKey& O) const { return A == O.A && B == O.B; }
	};

	friend FORCEINLINE uint32 GetTypeHash(const FEdgeKey& K)
	{
		return HashCombine(::GetTypeHash(K.A), ::GetTypeHash(K.B));
	}

	// ── Grass mesh cache (same pattern as GroundCoverScatter) ──

	struct FGrassMeshCache
	{
		FSoftObjectPath MeshPath;
		FVector2D ScaleRange = FVector2D(1.0f, 1.0f);
		float CumulativeWeight = 0.0f;
	};

	static bool BuildGrassMeshCache(const TArray<FPCGSurfGrassMeshEntry>& Entries,
		TArray<FGrassMeshCache>& OutCache, float& OutTotalWeight);
	static int32 SelectGrassMeshIndex(const TArray<FGrassMeshCache>& Cache,
		float TotalWeight, FRandomStream& Rng);

	// ── Mesh topology building ──

	static bool BuildTopology(UStaticMesh* Mesh, const FTransform& WorldTransform,
		float MinNormalDotUp, float MaxNeighborAngleDeg,
		TArray<FTriData>& OutTris);

	// ── Attributes ──

	static FString ReadStringAttr(const UPCGMetadata* Meta, FName Name, int64 EntryKey);

	// ── Transform ──

	static FQuat MakeSurfaceRotation(const FVector& SurfaceNormal, float YawDeg,
		float AlignAmount, float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg);

	// ── Sampling ──

	static FVector RandomBarycentric(FRandomStream& Rng);
};