// PCGDynamicMeshEmbed.h
// Custom PCG node that dynamically computes embed distance per-point based on
// assigned mesh bounding box projected onto the surface normal.
// 
// Place in your project's Source/<Module>/Public/ directory.
// Add "PCG" to your Build.cs PublicDependencyModuleNames if not already there.

#pragma once

#include "CoreMinimal.h"
#include "PCGSettings.h"
#include "PCGElement.h"
#include "Engine/StaticMesh.h"

#include "PCGDynamicMeshEmbed.generated.h"

/**
 * Single mesh entry with selection weight.
 * Mirrors the layout of the built-in PCG weighted mesh spawner entries.
 */
USTRUCT(BlueprintType)
struct PCGEXTENSIONS_API FPCGEmbedMeshEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TSoftObjectPtr<UStaticMesh> Mesh;

	/** Relative probability of this mesh being selected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh", meta = (ClampMin = "0.01"))
	float Weight = 1.0f;
};

/**
 * Cached bounding box data extracted from a loaded static mesh.
 * Stored once at execution start to avoid repeated mesh lookups.
 */
struct FPCGMeshBoundsCache
{
	FSoftObjectPath MeshPath;
	FVector BoundsMin = FVector::ZeroVector;  // Local-space min (e.g. -HalfX, -HalfY, 0 for base pivot)
	FVector BoundsMax = FVector::ZeroVector;  // Local-space max (e.g. +HalfX, +HalfY, +Height)
	float CumulativeWeight = 0.0f;           // For weighted random selection
};

// ─────────────────────────────────────────────
//  Settings (defines the node in the PCG editor)
// ─────────────────────────────────────────────

UCLASS(BlueprintType, ClassGroup = (Procedural))
class PCGEXTENSIONS_API UPCGDynamicMeshEmbedSettings : public UPCGSettings
{
	GENERATED_BODY()

public:
	UPCGDynamicMeshEmbedSettings();

	//~ Begin UPCGSettings interface
	virtual bool UseSeed() const override { return true; }
#if WITH_EDITOR
	virtual FName GetDefaultNodeName() const override { return FName(TEXT("DynamicMeshEmbed")); }
	virtual FText GetDefaultNodeTitle() const override
	{
		return NSLOCTEXT("PCGDynamicMeshEmbed", "NodeTitle", "Dynamic Mesh Embed");
	}
	virtual FText GetNodeTooltipText() const override
	{
		return NSLOCTEXT("PCGDynamicMeshEmbed", "Tooltip",
			"Assigns a random weighted mesh to each point, computes the bounding box projection "
			"onto the surface normal, and offsets the point into the surface by that distance. "
			"Stores the assigned mesh path as an attribute for downstream spawning via "
			"Static Mesh Spawner with 'By Attribute' mesh selector.");
	}
	virtual EPCGSettingsType GetType() const override { return EPCGSettingsType::PointOps; }
#endif

protected:
	virtual TArray<FPCGPinProperties> InputPinProperties() const override;
	virtual TArray<FPCGPinProperties> OutputPinProperties() const override;
	virtual FPCGElementPtr CreateElement() const override;
	//~ End UPCGSettings interface

public:
	// ── Mesh Configuration ──

	/** The meshes to randomly assign, with relative weights. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Meshes",
		meta = (TitleProperty = "Mesh"))
	TArray<FPCGEmbedMeshEntry> MeshEntries;

	// ── Embed Configuration ──

	/**
	 * Multiplier on the computed embed distance.
	 * 1.0 = embed exactly to the bounding box edge (mesh just touches the surface).
	 * 1.1 = embed 10% deeper (recommended — slight over-embed is invisible, floating is obvious).
	 * Values > 1.5 will bury meshes significantly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Embed",
		meta = (ClampMin = "-3.0", ClampMax = "3.0"))
	float EmbedMultiplier = 1.15f;

	/**
	 * If true, the embed distance is multiplied by the point's uniform scale.
	 * Enable this when your Transform Points node applies random scale BEFORE this node.
	 * If scale is applied AFTER this node, leave disabled.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Embed")
	bool bScaleProportionalEmbed = true;

	// ── Output Configuration ──

	/**
	 * Attribute name where the assigned mesh path is stored on each output point.
	 * The downstream Static Mesh Spawner should use Mesh Selector: "By Attribute"
	 * and reference this attribute name.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Output")
	FName MeshPathAttributeName = FName(TEXT("MeshPath"));
};

// ─────────────────────────────────
//  Element (executes the node logic)
// ─────────────────────────────────

class PCGEXTENSIONS_API FPCGDynamicMeshEmbedElement : public IPCGElement
{
protected:
	virtual bool ExecuteInternal(FPCGContext* Context) const override;

private:
	/**
	 * Computes the support function: the maximum extent of the bounding box
	 * from the origin (pivot) in the given direction.
	 *
	 * For direction D and AABB [Min, Max]:
	 *   support(D) = D.X * (D.X > 0 ? Max.X : Min.X)
	 *              + D.Y * (D.Y > 0 ? Max.Y : Min.Y)
	 *              + D.Z * (D.Z > 0 ? Max.Z : Min.Z)
	 *
	 * This naturally handles asymmetric bounds (e.g. pivot at base where Min.Z ≈ 0).
	 */
	static float ComputeSupportFunction(const FVector& Direction,
		const FVector& BoundsMin,
		const FVector& BoundsMax);
};