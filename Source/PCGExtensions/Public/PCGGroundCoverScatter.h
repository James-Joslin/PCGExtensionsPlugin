// PCGGroundCoverScatter.h
// Custom PCG node that places ground cover (grass, ferns, small ground plants)
// from a dense input candidate point cloud. Consolidates the whole grass graph
// into one node: landscape projection, slope-band filtering, a multi-layer noise
// stack (Perlin x Worley + fine detail + jitter) for natural clumping/clearings,
// per-biome density/scale response, spatial-hash min-distance thinning,
// weighted mesh assignment, and surface-aligned transform randomisation.
//
// Obstacle exclusion (rocks etc.) is handled upstream by subtracting mesh
// boundaries from the candidate point cloud before it reaches this node.
//
// Designed for HIGH point counts. Min-distance pruning and exclusion lookups are
// accelerated with uniform XY hash grids, so it stays linear-ish where the
// brute-force tree-tier path would be O(N^2) / O(N*M).
//
// Candidate generation (dense Surface Sampler / jittered grid) and any valley /
// volume subtraction are done upstream and fed into the In pin.
//
// Place in your project's Source/<Module>/Public/ directory.
// Requires "PCG" in Build.cs PublicDependencyModuleNames.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "Engine/StaticMesh.h"

#include "PCGGroundCoverScatter.generated.h"

// ─────────────────────────────────────────────
//  Noise stack enums
// ─────────────────────────────────────────────

UENUM(BlueprintType)
enum class EPCGGroundNoiseType : uint8
{
	/** Smooth value noise. Good for broad density patches. */
	Perlin              UMETA(DisplayName = "Perlin"),
	/** Worley distance-to-nearest-feature (cellular). Clump / patch texture. */
	Worley_F1           UMETA(DisplayName = "Worley F1"),
	/** Worley F2 - F1. Ridge / vein patterns between cells. */
	Worley_F2MinusF1    UMETA(DisplayName = "Worley F2-F1"),
	/** Pure per-position hash random. Micro jitter only. */
	Random              UMETA(DisplayName = "Random")
};

UENUM(BlueprintType)
enum class EPCGGroundNoiseBlend : uint8
{
	Multiply   UMETA(DisplayName = "Multiply"),
	Add        UMETA(DisplayName = "Add"),
	Min        UMETA(DisplayName = "Min"),
	Max        UMETA(DisplayName = "Max"),
	Replace    UMETA(DisplayName = "Replace")
};

UENUM(BlueprintType)
enum class EPCGGroundBiomeCombine : uint8
{
	/** Σ(weight·mult) / Σweight. Smooth blends across overlapping biome masks. */
	WeightedAverage  UMETA(DisplayName = "Weighted Average"),
	/** Highest weighted multiplier wins. */
	Max              UMETA(DisplayName = "Max"),
	/** Product of lerp(1, mult, weight). Stacks reductions. */
	Multiply         UMETA(DisplayName = "Multiply")
};

// ─────────────────────────────────────────────
//  One noise layer
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGGroundNoiseLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	EPCGGroundNoiseType NoiseType = EPCGGroundNoiseType::Perlin;

	/** Spatial frequency. Lower = larger patterns. Worley cell grid = 1/Frequency. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Frequency = 0.004f;

	/** Multiplier on the layer output before blending. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float Amplitude = 1.0f;

	/** Added to the layer output before blending. Shifts the baseline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float Bias = 0.0f;

	/** Invert (1 - value) before amplitude/bias. Turns Worley cores into clumps, etc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	bool bInvert = false;

	/** How this layer combines with the accumulated result. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	EPCGGroundNoiseBlend BlendMode = EPCGGroundNoiseBlend::Multiply;

	/** Per-layer seed offset for determinism / decorrelation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	int32 Seed = 0;
};

// ─────────────────────────────────────────────
//  Mesh entry (weighted, with per-mesh scale range)
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGGroundMeshEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Relative probability of this mesh being selected within the set. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh", meta = (ClampMin = "0.01"))
	float Weight = 1.0f;

	/** Per-mesh uniform scale range (multiplied by tier scale and biome scale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	FVector2D ScaleRange = FVector2D(0.8f, 1.2f);
};

// ─────────────────────────────────────────────
//  Per-biome response
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGGroundBiomeResponse
{
	GENERATED_BODY()

	/** Float weight attribute (0–1) on the input points (e.g. "Biome_Meadow"). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	FName BiomeAttribute = FName(TEXT("Biome_Meadow"));

	/** Density scaling where this biome dominates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome",
		meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float DensityMultiplier = 1.0f;

	/** Optional mesh-scale bias where this biome dominates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome",
		meta = (ClampMin = "0.1", ClampMax = "4.0"))
	float ScaleMultiplier = 1.0f;
};

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGGroundCoverScatterSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGGroundCoverScatterSettings();

	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("GroundCoverScatter")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGGroundCoverScatter", "NodeTitle", "Ground Cover Scatter");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGGroundCoverScatter", "Tooltip",
			"Places ground cover (grass / ferns / small plants) from a dense candidate point "
			"cloud. Projects onto the landscape, filters by slope band, shapes density with a "
			"multi-layer noise stack (Perlin x Worley + detail + jitter), modulates per biome, "
			"thins by minimum distance (spatial-hash accelerated), assigns a weighted mesh "
			"(stored as MeshPath), and randomises the transform. Obstacle exclusion is handled "
			"upstream by subtracting mesh boundaries from the point cloud. Built for high "
			"point counts.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Distribution ──────────────────────────────────────────────

	/** Baseline keep probability before noise/biome. 1.0 = keep all that pass filters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float KeepProbability = 1.0f;

	/** Minimum spacing between accepted instances (UU). 0 = no pruning (rely on upstream grid). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution",
		meta = (ClampMin = "0.0"))
	float MinDistance = 40.0f;

	/** Random XY jitter applied to each candidate before projection, to break grid patterns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution",
		meta = (ClampMin = "0.0"))
	float PositionJitter = 25.0f;

	// ── Noise Stack ───────────────────────────────────────────────

	/** Enable the multi-layer noise stack. Disable to rely on biome / upstream density only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	bool bUseNoiseStack = true;

	/** The noise stack, processed in order. See defaults in the constructor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseStack", TitleProperty = "NoiseType"))
	TArray<FPCGGroundNoiseLayer> NoiseLayers;

	/** Clamp the combined noise result to 0–1 before use. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseStack"))
	bool bClampNoise = true;

	/** Candidates whose combined noise density is below this are culled outright. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise",
		meta = (EditCondition = "bUseNoiseStack", ClampMin = "0.0", ClampMax = "1.0"))
	float NoiseCullThreshold = 0.05f;

	// ── Slope & Surface ───────────────────────────────────────────

	/** Min slope dot (surface normal · world-up). Grass typically ~0.5–0.7. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlopeDot = 0.6f;

	/** Max slope dot. 1.0 keeps flat ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlopeDot = 1.0f;

	/** 0 = always upright, 1 = fully aligned to the surface normal. Grass usually 0.6–1.0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlopeAlignAmount = 0.8f;

	/** Cap on how far alignment can tilt a blade from vertical (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface",
		meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxAlignAngleDeg = 40.0f;

	// ── Projection ────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "0.0", ClampMax = "20000.0"))
	float TraceStartHeight = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	// ── Biome Response ────────────────────────────────────────────

	/** One entry per biome this layer responds to. Empty = ignore biomes (uniform). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome",
		meta = (TitleProperty = "BiomeAttribute"))
	TArray<FPCGGroundBiomeResponse> BiomeResponses;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	EPCGGroundBiomeCombine BiomeCombineMode = EPCGGroundBiomeCombine::WeightedAverage;

	/** Apply the per-biome ScaleMultiplier as well as DensityMultiplier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bBiomeModulatesScale = true;

	// ── Mesh Set ──────────────────────────────────────────────────

	/** Weighted mesh variants (grass tufts / ferns). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set",
		meta = (TitleProperty = "Mesh"))
	TArray<FPCGGroundMeshEntry> MeshSet;

	/** Attribute the chosen mesh path is written to. Feed a By-Attribute Static Mesh Spawner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set")
	FName MeshPathAttributeName = FName(TEXT("MeshPath"));

	// ── Transform Randomisation ───────────────────────────────────

	/** Layer-level uniform scale range, applied on top of each mesh's ScaleRange. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector2D TierScaleRange = FVector2D(0.9f, 1.15f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bRandomYaw = true;

	/** Small random pitch/roll tilt for organic variation (degrees). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform",
		meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float PitchRollJitterDeg = 4.0f;

	/** Z offset applied after projection (negative = sink blades slightly into the ground). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	float ZOffset = -2.0f;

	// ── Output ────────────────────────────────────────────────────

	/** Write the final keep value to $Density (useful for downstream density-based culling/LOD). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bWriteDensity = true;

	/** Emit culled candidates on the Rejected pin for debugging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bOutputRejected = false;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGGroundCoverScatterElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	/** Cached, weight-accumulated mesh entry. */
	struct FGroundMeshCache
	{
		FSoftObjectPath MeshPath;
		FVector2D ScaleRange = FVector2D(1.0f, 1.0f);
		float CumulativeWeight = 0.0f;
	};

	// ── Mesh ──
	static bool BuildMeshCache(const TArray<FPCGGroundMeshEntry>& Entries,
		TArray<FGroundMeshCache>& OutCache, float& OutTotalWeight);
	static int32 SelectMeshIndex(const TArray<FGroundMeshCache>& Cache, float TotalWeight, FRandomStream& Rng);

	// ── Attributes ──
	static float ReadFloatAttr(const UPCGMetadata* Meta, FName Name, int64 EntryKey, float Default);

	// ── Biome ──
	static void ComputeBiomeFactors(const TArray<FPCGGroundBiomeResponse>& Responses,
		EPCGGroundBiomeCombine Mode, const UPCGMetadata* Meta, int64 EntryKey,
		float& OutDensity, float& OutScale);

	// ── Transform ──
	static FQuat MakeFoliageRotation(const FVector& SurfaceNormal, float YawDeg,
		float AlignAmount, float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg);

	// ── Noise ──
	static float SampleNoiseStack(const TArray<FPCGGroundNoiseLayer>& Layers,
		const FVector& Position, bool bClamp);
};