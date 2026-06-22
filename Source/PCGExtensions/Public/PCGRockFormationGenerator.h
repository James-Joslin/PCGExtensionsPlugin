// PCGRockFormationGenerator.h
// Custom PCG node that generates multi-tier rock formations on flat ground.
// Creates foundation clusters, upper tier rocks, and fill rocks for overhangs.
//
// Place in your project's Source/PCGExtensions/Public/ directory.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "Engine/StaticMesh.h"

#include "PCGRockFormationGenerator.generated.h"

// ─────────────────────────────────────────────
//  Mesh entry (reused from DynamicMeshEmbed)
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGFormationMeshEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh", meta = (ClampMin = "0.01"))
	float Weight = 1.0f;
};

// ─────────────────────────────────────────────
//  Per-tier configuration
// ─────────────────────────────────────────────

USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGFormationTierConfig
{
	GENERATED_BODY()

	/** Meshes available for this tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshes", meta = (TitleProperty = "Mesh"))
	TArray<FPCGFormationMeshEntry> MeshEntries;

	/** Min rocks to place per parent rock from the tier below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0", ClampMax = "12"))
	int32 MinRocksPerParent = 2;

	/** Max rocks to place per parent rock from the tier below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0", ClampMax = "12"))
	int32 MaxRocksPerParent = 5;

	/**
	 * Probability that a parent rock generates children at this tier.
	 * 1.0 = always, 0.5 = 50% chance per parent.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PlacementProbability = 0.8f;

	/** Min horizontal distance from parent centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0"))
	float MinRadius = 50.0f;

	/** Max horizontal distance from parent centre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0"))
	float MaxRadius = 500.0f;

	/**
	 * Min height as fraction of parent bbox height (0 = base, 1 = top).
	 * Allows rocks to be placed at ground level (0) or elevated (0.5+).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float MinHeightFraction = 0.0f;

	/**
	 * Max height fraction. Values > 1.0 allow rocks above the parent top.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Placement", meta = (ClampMin = "0.0", ClampMax = "1.5"))
	float MaxHeightFraction = 0.7f;

	/** Min uniform scale for rocks at this tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scale", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float MinScale = 0.4f;

	/** Max uniform scale for rocks at this tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scale", meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float MaxScale = 0.8f;

	/**
	 * Degrees of inward tilt toward the formation centre.
	 * Creates the leaning-against-each-other look.
	 * 0 = no tilt, 15 = noticeable lean inward.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rotation", meta = (ClampMin = "0.0", ClampMax = "45.0"))
	float MaxInwardTilt = 10.0f;

	/** Z offset applied after placement (negative = sink). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Offset")
	float VerticalOffset = -100.0f;
};

// ─────────────────────────────────────────────
//  Internal: placed rock tracking
// ─────────────────────────────────────────────

// Internal structurally managed struct; does not require reflective macros
struct FFormationPlacedRock
{
	FVector Position = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector Scale = FVector::OneVector;
	FVector BoundsMin = FVector::ZeroVector;  // Local-space mesh bounds
	FVector BoundsMax = FVector::ZeroVector;
	FSoftObjectPath MeshPath;
	int32 Tier = 0;
	bool bIsFillRock = false;

	/** Get the world-space AABB top Z (approximate, ignoring rotation for speed). */
	float GetWorldTopZ() const
	{
		return Position.Z + BoundsMax.Z * Scale.Z;
	}

	/** Get the world-space AABB bottom Z. */
	float GetWorldBottomZ() const
	{
		return Position.Z + BoundsMin.Z * Scale.Z;
	}

	/** Get the world-space horizontal half-extents. */
	float GetWorldHalfExtentX() const { return FMath::Max(FMath::Abs(BoundsMax.X), FMath::Abs(BoundsMin.X)) * Scale.X; }
	float GetWorldHalfExtentY() const { return FMath::Max(FMath::Abs(BoundsMax.Y), FMath::Abs(BoundsMin.Y)) * Scale.Y; }

	/** Check if a world XY point falls within this rock's horizontal footprint. */
	bool ContainsXY(const FVector& Point, float Tolerance = 50.0f) const
	{
		const float HalfX = GetWorldHalfExtentX() + Tolerance;
		const float HalfY = GetWorldHalfExtentY() + Tolerance;
		return FMath::Abs(Point.X - Position.X) <= HalfX &&
			FMath::Abs(Point.Y - Position.Y) <= HalfY;
	}
};

// Cached mesh data
struct FFormationMeshCache
{
	FSoftObjectPath MeshPath;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::ZeroVector;
	float CumulativeWeight = 0.0f;
};

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGRockFormationGeneratorSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("RockFormationGenerator")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGRockFormationGenerator", "NodeTitle", "Rock Formation Generator");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGRockFormationGenerator", "Tooltip",
			"Generates multi-tier rock formations. Creates foundation clusters of large "
			"overlapping rocks, places smaller rocks on/around them in upper tiers, and "
			"fills overhangs with support rocks. Each tier enforces smaller bounding boxes "
			"than the tier below. Fill rocks match the tier they support from, not the "
			"overhanging tier.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;

public:
	// ── Foundation Cluster (Tier 0) ──

	/** Meshes for foundation rocks. Should be the largest in the formation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (TitleProperty = "Mesh"))
	TArray<FPCGFormationMeshEntry> FoundationMeshEntries;

	/** Min foundation rocks per formation. 1 = single base, 4 = dense cluster. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (ClampMin = "1", ClampMax = "6"))
	int32 MinFoundationRocks = 1;

	/** Max foundation rocks per formation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (ClampMin = "1", ClampMax = "6"))
	int32 MaxFoundationRocks = 3;

	/** How close foundation rocks cluster together. Smaller = more overlap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (ClampMin = "0.0"))
	float FoundationClusterRadius = 300.0f;

	/** Scale range for foundation rocks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (ClampMin = "0.1"))
	float FoundationMinScale = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation", meta = (ClampMin = "0.1"))
	float FoundationMaxScale = 1.5f;

	/** Z sink for foundation rocks into flat ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 0 - Foundation")
	float FoundationVerticalOffset = -200.0f;

	// ── Tier 1 (Mid) ──

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 1 - Mid")
	FPCGFormationTierConfig Tier1Config;

	// ── Tier 2 (Cap) ──

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tier 2 - Cap")
	FPCGFormationTierConfig Tier2Config;

	// ── Overhang Detection ──

	/** Number of sample points on the bottom face of each rock for overhang detection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang", meta = (ClampMin = "4", ClampMax = "16"))
	int32 OverhangSampleCount = 6;

	/** Tolerance for considering a sample point "supported". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang", meta = (ClampMin = "0.0"))
	float OverhangTolerance = 80.0f;

	/** Maximum number of fill rocks per overhanging rock. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang", meta = (ClampMin = "0", ClampMax = "6"))
	int32 MaxFillsPerRock = 2;

	/** Scale range for fill rocks (relative to their tier's scale). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Overhang", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float FillScaleMultiplier = 0.8f;

	// ── Output ──

	/** Attribute name for the assigned mesh path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FName MeshPathAttributeName = FName(TEXT("MeshPath"));

	/** Attribute name for the fill rock flag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FName FillFlagAttributeName = FName(TEXT("bIsFillRock"));
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class FPCGRockFormationGeneratorElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	static bool CacheTierMeshes(const TArray<FPCGFormationMeshEntry>& Entries, TArray<FFormationMeshCache>& OutCache, float& OutTotalWeight);
	static const FFormationMeshCache& SelectRandomMesh(const TArray<FFormationMeshCache>& Cache, float TotalWeight, FRandomStream& Rng);
	static void GenerateTierRocks(const FPCGFormationTierConfig& Config, const TArray<FFormationMeshCache>& MeshCache, float MeshTotalWeight, const TArray<FFormationPlacedRock>& ParentRocks, const FVector& FormationCentre, int32 TierIndex, int32 BaseSeed, TArray<FFormationPlacedRock>& OutRocks);
	static void GenerateFillRocks(const TArray<FFormationPlacedRock>& AllRocks, int32 OverhangSampleCount, float OverhangTolerance, int32 MaxFillsPerRock, float FillScaleMultiplier, const TArray<FFormationMeshCache>& FillMeshCache, float FillMeshTotalWeight, int32 FillTierIndex, int32 TargetTierIndex, int32 BaseSeed, TArray<FFormationPlacedRock>& OutFillRocks);
};