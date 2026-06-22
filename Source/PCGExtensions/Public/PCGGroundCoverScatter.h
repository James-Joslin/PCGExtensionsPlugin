// PCGGroundCoverScatter.h
// Custom PCG node that places ground cover (grass, ferns, small ground plants)
// from a dense input candidate point cloud.
//
// Place in your project's Source/PCGExtensions/Public/ directory.

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
	Perlin            UMETA(DisplayName = "Perlin"),
	Worley_F1         UMETA(DisplayName = "Worley F1"),
	Worley_F2MinusF1  UMETA(DisplayName = "Worley F2-F1"),
	Random            UMETA(DisplayName = "Random")
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
	WeightedAverage  UMETA(DisplayName = "Weighted Average"),
	Max              UMETA(DisplayName = "Max"),
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

	/** Spatial frequency. Lower = larger patterns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Frequency = 0.004f;

	/** Multiplier on the layer output before blending. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float Amplitude = 1.0f;

	/** Added to the layer output before blending. Shifts the baseline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float Bias = 0.0f;

	/** Invert (1 - value) before amplitude/bias. */
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float DensityMultiplier = 1.0f;

	/** Optional mesh-scale bias where this biome dominates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (ClampMin = "0.1", ClampMax = "4.0"))
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float KeepProbability = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution", meta = (ClampMin = "0.0"))
	float MinDistance = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Distribution", meta = (ClampMin = "0.0"))
	float PositionJitter = 25.0f;

	// ── Noise Stack ───────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
	bool bUseNoiseStack = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (EditCondition = "bUseNoiseStack", TitleProperty = "NoiseType"))
	TArray<FPCGGroundNoiseLayer> NoiseLayers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (EditCondition = "bUseNoiseStack"))
	bool bClampNoise = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise", meta = (EditCondition = "bUseNoiseStack", ClampMin = "0.0", ClampMax = "1.0"))
	float NoiseCullThreshold = 0.05f;

	// ── Slope & Surface ───────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlopeDot = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlopeDot = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SlopeAlignAmount = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope & Surface", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float MaxAlignAngleDeg = 40.0f;

	// ── Projection ────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection", meta = (ClampMin = "0.0", ClampMax = "20000.0"))
	float TraceStartHeight = 2000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection", meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	// ── Biome Response ────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome", meta = (TitleProperty = "BiomeAttribute"))
	TArray<FPCGGroundBiomeResponse> BiomeResponses;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	EPCGGroundBiomeCombine BiomeCombineMode = EPCGGroundBiomeCombine::WeightedAverage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Biome")
	bool bBiomeModulatesScale = true;

	// ── Mesh Set ──────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set", meta = (TitleProperty = "Mesh"))
	TArray<FPCGGroundMeshEntry> MeshSet;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh Set")
	FName MeshPathAttributeName = FName(TEXT("MeshPath"));

	// ── Transform Randomisation ───────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector2D TierScaleRange = FVector2D(0.9f, 1.15f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	bool bRandomYaw = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float PitchRollJitterDeg = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	float ZOffset = -2.0f;

	// ── Output ────────────────────────────────────────────────────

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	bool bWriteDensity = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bOutputRejected = false;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class FPCGGroundCoverScatterElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	struct FGroundMeshCache
	{
		FSoftObjectPath MeshPath;
		FVector2D ScaleRange = FVector2D(1.0f, 1.0f);
		float CumulativeWeight = 0.0f;
	};

	static bool BuildMeshCache(const TArray<FPCGGroundMeshEntry>& Entries, TArray<FGroundMeshCache>& OutCache, float& OutTotalWeight);
	static int32 SelectMeshIndex(const TArray<FGroundMeshCache>& Cache, float TotalWeight, FRandomStream& Rng);
	static float ReadFloatAttr(const UPCGMetadata* Meta, FName Name, int64 EntryKey, float Default);
	static void ComputeBiomeFactors(const TArray<FPCGGroundBiomeResponse>& Responses, EPCGGroundBiomeCombine Mode, const UPCGMetadata* Meta, int64 EntryKey, float& OutDensity, float& OutScale);
	static FQuat MakeFoliageRotation(const FVector& SurfaceNormal, float YawDeg, float AlignAmount, float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg);
	static float SampleNoiseStack(const TArray<FPCGGroundNoiseLayer>& Layers, const FVector& Position, bool bClamp);
};