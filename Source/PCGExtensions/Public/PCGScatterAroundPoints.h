// PCGScatterAroundPoints.h
// Custom PCG node that scatters child points around parent positions with
// distance-based density falloff, landscape projection, self-pruning,
// slope filtering, and configurable rotation/scale randomisation.
//
// Replaces the chain: Copy Points → Create Attribute → Transform Points →
// Distance → Density Remap → Density Filter → Project Points →
// Normal To Density → Density Filter → Transform Points
//
// Place in your project's Source/<Module>/Public/ directory.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGScatterAroundPoints.generated.h"

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGScatterAroundPointsSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("ScatterAroundPoints")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGScatterAroundPoints", "NodeTitle", "Scatter Around Points");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGScatterAroundPoints", "Tooltip",
			"Scatters child points around each input parent position in a disk pattern. "
			"Density falls off with distance from parent. Points are projected onto the landscape "
			"via line trace, filtered by slope, self-pruned by minimum distance, and randomised "
			"in rotation and scale. Outputs points ready for a Static Mesh Spawner.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Output Mode ──

	/**
	 * When enabled, outputs raw projected points for downstream Dynamic Mesh Embed:
	 * - Position: on the landscape surface (no Z offset applied)
	 * - Rotation: encodes the true surface normal (no random rotation applied)
	 * - Scale: (1,1,1) (no random scale applied)
	 *
	 * The rotation, scale, and Z offset settings on this node are IGNORED.
	 * Apply them via a Transform Points node AFTER the Dynamic Mesh Embed.
	 *
	 * When disabled (default), the node applies rotation, scale, and Z offset
	 * directly — suitable for feeding straight into a Static Mesh Spawner
	 * without a Dynamic Mesh Embed in between.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output Mode")
	bool bProjectionOutputForEmbed = false;

	// ── Scatter Distribution ──

	/** Number of child points to attempt per parent point. Not all will survive filtering. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter",
		meta = (ClampMin = "1", ClampMax = "200"))
	int32 PointsPerParent = 12;

	/** Minimum scatter radius from parent (UU). Points won't be placed closer than this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter",
		meta = (ClampMin = "0.0"))
	float InnerRadius = 100.0f;

	/** Maximum scatter radius from parent (UU). Points won't be placed further than this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scatter",
		meta = (ClampMin = "10.0"))
	float OuterRadius = 800.0f;

	// ── Density Falloff ──

	/**
	 * Controls the density falloff curve from inner to outer radius.
	 * 1.0 = linear falloff (gentle thinning)
	 * 2.0 = quadratic falloff (dense near parent, sparse at edges)
	 * 0.5 = square root falloff (more uniform distribution)
	 * 3.0+ = very aggressive falloff (tight clusters)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Density",
		meta = (ClampMin = "0.1", ClampMax = "5.0"))
	float FalloffExponent = 2.0f;

	// ── Landscape Projection ──

	/** How far downward to trace when projecting scattered points onto the landscape. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	/** Collision channel for the landscape projection trace. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	/**
	 * Height above the parent point to start the trace from.
	 * Should be enough to clear any terrain above the scatter area.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Projection",
		meta = (ClampMin = "100.0", ClampMax = "10000.0"))
	float TraceStartHeight = 2000.0f;

	// ── Slope Filtering ──

	/**
	 * Minimum slope steepness to keep (dot product of surface normal with world-up).
	 * 0.0 = vertical cliff, 1.0 = flat ground.
	 * Set to 0.0 to keep points on all slopes.
	 * Set to 0.3 to exclude near-vertical surfaces.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope Filter",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinSlopeDot = 0.0f;

	/**
	 * Maximum slope steepness to keep.
	 * Set to 1.0 to keep everything including flat ground.
	 * Set to 0.85 to exclude flat ground (medium rocks only on slopes).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Slope Filter",
		meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxSlopeDot = 1.0f;

	// ── Self-Pruning ──

	/**
	 * Minimum distance between surviving scattered points.
	 * Points too close to an already-placed point are removed.
	 * 0 = no pruning.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pruning",
		meta = (ClampMin = "0.0"))
	float MinDistanceBetweenPoints = 80.0f;

	// ── Rotation ──

	/** If true, rotation is in world space. If false, relative to surface normal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rotation")
	bool bAbsoluteRotation = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rotation")
	FRotator RotationMin = FRotator(-5.0f, 0.0f, -5.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Rotation")
	FRotator RotationMax = FRotator(5.0f, 360.0f, 5.0f);

	// ── Scale ──

	/** If true, only ScaleMin.X and ScaleMax.X are used (uniform scaling). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scale")
	bool bUniformScale = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scale")
	FVector ScaleMin = FVector(0.3f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scale")
	FVector ScaleMax = FVector(0.8f);

	// ── Vertical Offset ──

	/** Additional Z offset after projection (negative = sink into ground). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Offset",
		meta = (ClampMin = "-5000.0", ClampMax = "5000.0"))
	float VerticalOffsetMin = -200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Offset",
		meta = (ClampMin = "-5000.0", ClampMax = "5000.0"))
	float VerticalOffsetMax = -400.0f;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGScatterAroundPointsElement : public IPCGElement
{
	// Line tracing is thread-safe and this node loads no resources, so it stays on the
	// default worker-thread execution (no CanExecuteOnlyOnMainThread override).
protected:
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};