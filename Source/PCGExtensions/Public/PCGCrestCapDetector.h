// PCGCrestCapDetector.h
// Custom PCG node that takes slope rock positions, pushes them upward,
// projects back onto the landscape via line trace, and keeps only
// points that landed on flat ground — identifying cliff crest positions
// for cap rock placement.
//
// Place in your project's Source/<Module>/Public/ directory.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"

#include "PCGCrestCapDetector.generated.h"

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGCrestCapDetectorSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return false; }

#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("CrestCapDetector")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGCrestCapDetector", "NodeTitle", "Crest Cap Detector");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGCrestCapDetector", "Tooltip",
			"Takes slope rock positions, pushes them upward by a configurable distance, "
			"projects back onto the landscape via line trace, and keeps only points that "
			"landed on flat ground. The surviving points mark cliff crest positions where "
			"cap rocks should be placed with absolute rotation to hide hollow undersides.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	/**
	 * How far upward (world Z) to push each point before projecting back down.
	 * Controls how far onto the flat ground the cap rocks extend from the cliff edge.
	 * Too small = cap doesn't cover the hollow undersides of slope rocks below.
	 * Too large = cap rocks appear on open flat ground far from any cliff.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "50.0", ClampMax = "5000.0"))
	float VerticalPushDistance = 400.0f;

	/**
	 * Minimum dot product of the projected surface normal with world-up (0,0,1)
	 * for a point to be considered "flat ground".
	 * 0.85 ≈ slopes gentler than ~32°.
	 * 0.90 ≈ slopes gentler than ~26°.
	 * 0.95 ≈ nearly flat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float FlatnessThreshold = 0.85f;

	/**
	 * How far downward to trace when projecting pushed points onto the landscape.
	 * Should be larger than the tallest cliff in your level.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	/**
	 * Collision channel to use for the line trace.
	 * Default ECC_WorldStatic hits landscapes and static meshes.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	/**
	 * Optional horizontal offset to push points away from the cliff edge.
	 * Applied in the opposite direction of the original surface normal's horizontal
	 * component (i.e. pushing the point further onto the flat, away from the slope).
	 * 0 = no horizontal push (point stays directly above the slope rock).
	 * 200 = point is nudged 200 UU away from the cliff edge onto the flat.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "-2000.0", ClampMax = "2000.0"))
	float HorizontalRetractDistance = 150.0f;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

class PCGEXTENSIONS_API FPCGCrestCapDetectorElement : public IPCGElement
{
	// Line tracing is thread-safe and this node loads no resources, so it stays on the
	// default worker-thread execution (no CanExecuteOnlyOnMainThread override).
protected:
	virtual bool SupportsBasePointDataInputs(FPCGContext* InContext) const override { return true; }
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};