// PCGDynamicMeshEmbed.cpp
// 
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGDynamicMeshEmbed.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAccessor.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/StaticMesh.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGDynamicMeshEmbed)

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UPCGDynamicMeshEmbedSettings::UPCGDynamicMeshEmbedSettings()
{
}

TArray<FPCGPinProperties> UPCGDynamicMeshEmbedSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGDynamicMeshEmbed", "InTooltip",
			"Input points with surface normals (from Surface Sampler → Normal filtering). "
			"Each point will be assigned a mesh and embedded into the surface."));
	return Pins;
}

TArray<FPCGPinProperties> UPCGDynamicMeshEmbedSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGDynamicMeshEmbed", "OutTooltip",
			"Modified points with positions embedded into the surface. "
			"Each point has a MeshPath attribute for downstream spawning."));
	return Pins;
}

FPCGElementPtr UPCGDynamicMeshEmbedSettings::CreateElement() const
{
	return MakeShared<FPCGDynamicMeshEmbedElement>();
}



// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

float FPCGDynamicMeshEmbedElement::ComputeSupportFunction(
	const FVector& Direction,
	const FVector& BoundsMin,
	const FVector& BoundsMax)
{
	// For each axis, pick the bound that extends furthest in the direction.
	// This is the support function of the AABB from the origin (mesh pivot).
	//
	// If pivot is at base centre:
	//   BoundsMin ≈ (-HalfX, -HalfY,  0)
	//   BoundsMax ≈ (+HalfX, +HalfY, +Height)
	//
	// On an 80° cliff face with normal ≈ (0.98, 0, 0.17):
	//   X: 0.98 > 0 → use BoundsMax.X (+HalfX) → 0.98 * HalfX
	//   Z: 0.17 > 0 → use BoundsMax.Z (+Height) → 0.17 * Height
	//   = large horizontal embed + modest vertical embed ✓
	//
	// On flat ground with normal = (0, 0, 1):
	//   Z: 1.0 > 0 → use BoundsMax.Z (+Height) → 1.0 * Height
	//   = full height embed downward ✓ (but flat ground should be
	//     filtered out by Density Filter before reaching this node)

	const float SupportX = Direction.X > 0.0f
		? Direction.X * BoundsMax.X
		: Direction.X * BoundsMin.X;

	const float SupportY = Direction.Y > 0.0f
		? Direction.Y * BoundsMax.Y
		: Direction.Y * BoundsMin.Y;

	const float SupportZ = Direction.Z > 0.0f
		? Direction.Z * BoundsMax.Z
		: Direction.Z * BoundsMin.Z;

	return SupportX + SupportY + SupportZ;
}

bool FPCGDynamicMeshEmbedElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGDynamicMeshEmbedElement::Execute);

	const UPCGDynamicMeshEmbedSettings* Settings = Context->GetInputSettings<UPCGDynamicMeshEmbedSettings>();
	check(Settings);

	// ── Validate mesh entries ──

	if (Settings->MeshEntries.Num() == 0)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGDynamicMeshEmbed", "NoMeshes",
			"Dynamic Mesh Embed: No mesh entries configured."));
		return true;
	}

	// ── Load meshes and cache bounding boxes ──

	TArray<FPCGMeshBoundsCache> MeshCache;
	MeshCache.Reserve(Settings->MeshEntries.Num());
	float TotalWeight = 0.0f;

	for (const FPCGEmbedMeshEntry& Entry : Settings->MeshEntries)
	{
		UStaticMesh* LoadedMesh = Entry.Mesh.Get();
		if (!LoadedMesh)
		{
			PCGE_LOG(Warning, GraphAndLog,
				FText::Format(
					NSLOCTEXT("PCGDynamicMeshEmbed", "MeshLoadFail",
						"Dynamic Mesh Embed: Failed to load mesh '{0}', skipping."),
					FText::FromString(Entry.Mesh.ToString())));
			continue;
		}

		FPCGMeshBoundsCache CacheEntry;
		CacheEntry.MeshPath = FSoftObjectPath(LoadedMesh);

		// Get the mesh's local-space bounding box.
		// This respects the pivot point — if pivot is at base centre,
		// BoundsMin.Z ≈ 0 and BoundsMax.Z = full height.
		const FBox MeshBounds = LoadedMesh->GetBoundingBox();
		CacheEntry.BoundsMin = MeshBounds.Min;
		CacheEntry.BoundsMax = MeshBounds.Max;

		TotalWeight += FMath::Max(Entry.Weight, 0.01f);
		CacheEntry.CumulativeWeight = TotalWeight;

		MeshCache.Add(CacheEntry);
	}

	if (MeshCache.Num() == 0)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGDynamicMeshEmbed", "NoValidMeshes",
			"Dynamic Mesh Embed: No valid meshes could be loaded."));
		return true;
	}

	// ── Process each input data set ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGPointData* InputPointData = Cast<UPCGPointData>(Input.Data);
		if (!InputPointData)
		{
			continue;
		}

		const TArray<FPCGPoint>& InputPoints = InputPointData->GetPoints();
		if (InputPoints.Num() == 0)
		{
			continue;
		}

		// Create output point data as a copy of input
		UPCGPointData* OutputPointData = NewObject<UPCGPointData>();
		OutputPointData->InitializeFromData(InputPointData);

		TArray<FPCGPoint>& OutputPoints = OutputPointData->GetMutablePoints();
		OutputPoints = InputPoints; // Deep copy

		// Create the mesh path attribute on the output metadata
		UPCGMetadata* Metadata = OutputPointData->MutableMetadata();

		// Use FindOrCreateAttribute to get/create a string attribute for mesh paths
		FPCGMetadataAttribute<FString>* MeshPathAttr =
			Metadata->FindOrCreateAttribute<FString>(
				Settings->MeshPathAttributeName,
				FString(),         // Default value
				/*bAllowInterpolation=*/false,
				/*bOverrideParent=*/true);

		if (!MeshPathAttr)
		{
			PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGDynamicMeshEmbed", "AttrCreateFail",
				"Dynamic Mesh Embed: Failed to create MeshPath attribute."));
			continue;
		}

		// ── Process each point ──

		const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

		for (int32 PointIdx = 0; PointIdx < OutputPoints.Num(); ++PointIdx)
		{
			FPCGPoint& Point = OutputPoints[PointIdx];

			// ─ 1. Random mesh selection (deterministic using point seed) ─

			// Combine base seed with point seed for deterministic per-point randomness
			const int32 PointSeed = PCGHelpers::ComputeSeed(BaseSeed, Point.Seed);
			FRandomStream Rng(PointSeed);
			const float RollValue = Rng.FRandRange(0.0f, TotalWeight);

			int32 SelectedMeshIdx = 0;
			for (int32 MeshIdx = 0; MeshIdx < MeshCache.Num(); ++MeshIdx)
			{
				if (RollValue <= MeshCache[MeshIdx].CumulativeWeight)
				{
					SelectedMeshIdx = MeshIdx;
					break;
				}
			}

			const FPCGMeshBoundsCache& SelectedMesh = MeshCache[SelectedMeshIdx];

			// ─ 2. Store the mesh path as an attribute ─

			Metadata->InitializeOnSet(Point.MetadataEntry);
			MeshPathAttr->SetValue(Point.MetadataEntry, SelectedMesh.MeshPath.ToString());

			// ─ 3. Extract the surface normal from the point's transform ─
			//      The Surface Sampler orients points so local Z = surface normal

			const FVector SurfaceNormal = Point.Transform.GetRotation().GetUpVector();

			// ─ 4. Compute embed distance via bounding box support function ─

			const float RawEmbed = ComputeSupportFunction(
				SurfaceNormal,
				SelectedMesh.BoundsMin,
				SelectedMesh.BoundsMax);

			// Apply scale if enabled
			float EmbedDistance = RawEmbed * Settings->EmbedMultiplier;

			if (Settings->bScaleProportionalEmbed)
			{
				// Use uniform scale (X component). If non-uniform scale is used,
				// this would need per-axis scaling of the bounds instead.
				const float UniformScale = Point.Transform.GetScale3D().X;
				EmbedDistance *= UniformScale;
			}

			// ─ 5. Offset the point INTO the surface ─
			//      Negative normal direction = into the surface

			const FVector EmbedOffset = -SurfaceNormal * EmbedDistance;
			FVector CurrentPos = Point.Transform.GetLocation();
			CurrentPos += EmbedOffset;
			Point.Transform.SetLocation(CurrentPos);
		}

		// ── Emit output ──

		FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}