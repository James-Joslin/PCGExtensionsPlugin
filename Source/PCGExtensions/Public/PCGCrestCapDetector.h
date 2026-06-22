// PCGCrestCapDetector.h
// Custom PCG node that takes slope rock positions, pushes them upward,
// projects back onto the landscape via line trace, and keeps only
// points that landed on flat ground — identifying cliff crest positions
// for cap rock placement.
//
// Place in your project's Source/PCGExtensions/Public/ directory.

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
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "50.0", ClampMax = "5000.0"))
	float VerticalPushDistance = 400.0f;

	/**
	 * Minimum dot product of the projected surface normal with world-up (0,0,1)
	 * for a point to be considered "flat ground".
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float FlatnessThreshold = 0.85f;

	/**
	 * How far downward to trace when projecting pushed points onto the landscape.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float TraceDistance = 10000.0f;

	/**
	 * Collision channel to use for the line trace.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection")
	TEnumAsByte<ECollisionChannel> TraceChannel = ECC_WorldStatic;

	/**
	 * Optional horizontal offset to push points away from the cliff edge.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detection",
		meta = (ClampMin = "-2000.0", ClampMax = "2000.0"))
	float HorizontalRetractDistance = 150.0f;
};

// ─────────────────────────────────────────────
//  Element
// ─────────────────────────────────────────────

// Note: FPCGElement classes generally do not require module export macros unless 
// you intend to inherit from them or instantiate them directly outside of this plugin.
class FPCGCrestCapDetectorElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;
};