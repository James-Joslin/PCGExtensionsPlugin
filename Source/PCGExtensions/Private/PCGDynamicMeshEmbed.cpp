// PCGDynamicMeshEmbed.cpp
// 
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGDynamicMeshEmbed.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Async/PCGAsyncLoadingContext.h"
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
//  Element -- Async resource loading
// ─────────────────────────────────────────────

bool FPCGDynamicMeshEmbedElement::PrepareDataInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGDynamicMeshEmbedElement::PrepareData);

	const UPCGDynamicMeshEmbedSettings* Settings = Context->GetInputSettings<UPCGDynamicMeshEmbedSettings>();
	check(Settings);

	FPCGDynamicMeshEmbedContext* ThisContext = static_cast<FPCGDynamicMeshEmbedContext*>(Context);

	if (!ThisContext->WasLoadRequested())
	{
		TArray<FSoftObjectPath> ToLoad;
		ToLoad.Reserve(Settings->MeshEntries.Num());
		for (const FPCGEmbedMeshEntry& Entry : Settings->MeshEntries)
		{
			if (!Entry.Mesh.IsNull())
			{
				ToLoad.Add(Entry.Mesh.ToSoftObjectPath());
			}
		}

		// Async load suspends the task (returns false) until streaming completes; on resume
		// WasLoadRequested() is true so we skip straight to caching below. A synchronous load
		// returns true immediately and falls through to caching on this same pass.
		if (!ThisContext->RequestResourceLoad(ThisContext, MoveTemp(ToLoad), !Settings->bSynchronousLoad))
		{
			return false;
		}
	}

	// ── Resolve mesh bounds on the game thread and cache them on the context. ──
	// Reading UStaticMesh bounds off the game thread is unsafe, so it happens here in the
	// PrepareData phase rather than in ExecuteInternal (which runs on a worker thread).
	if (!ThisContext->bCacheBuilt)
	{
		TArray<FSoftObjectPath> FailedPaths;
		PCGExtScatterCommon::BuildMeshBoundsCache(
			Settings->MeshEntries, ThisContext->MeshCache, ThisContext->TotalWeight, &FailedPaths);

		for (const FSoftObjectPath& Failed : FailedPaths)
		{
			PCGE_LOG(Warning, GraphAndLog,
				FText::Format(
					NSLOCTEXT("PCGDynamicMeshEmbed", "MeshLoadFail",
						"Dynamic Mesh Embed: Failed to load mesh '{0}', skipping."),
					FText::FromString(Failed.ToString())));
		}

		ThisContext->bCacheBuilt = true;
	}

	return true;
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

	FPCGDynamicMeshEmbedContext* ThisContext = static_cast<FPCGDynamicMeshEmbedContext*>(Context);

	// Mesh bounds were resolved on the game thread during PrepareData; here we only read them.
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& MeshCache = ThisContext->MeshCache;
	const float TotalWeight = ThisContext->TotalWeight;

	if (MeshCache.Num() == 0)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGDynamicMeshEmbed", "NoValidMeshes",
			"Dynamic Mesh Embed: No valid mesh entries configured or loaded."));
		return true;
	}

	// ── Process each input data set ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGBasePointData* InputPointData = Cast<UPCGBasePointData>(Input.Data);
		if (!InputPointData)
		{
			continue;
		}

		const int32 NumPoints = InputPointData->GetNumPoints();
		if (NumPoints == 0)
		{
			continue;
		}

		// 1:1 copy-and-modify: duplicate the input (points + parented metadata) and
		// edit it in place through value ranges.
		UPCGBasePointData* OutputPointData = CastChecked<UPCGBasePointData>(InputPointData->DuplicateData(Context));

		// Create the mesh-path attribute as a SoftObjectPath so the Static Mesh Spawner
		// "By Attribute" selector consumes it natively.
		UPCGMetadata* Metadata = OutputPointData->MutableMetadata();
		Metadata->CreateSoftObjectPathAttribute(
			Settings->MeshPathAttributeName, FSoftObjectPath(), /*bAllowsInterpolation=*/false);

		FPCGMetadataAttribute<FSoftObjectPath>* MeshPathAttr =
			Metadata->GetMutableTypedAttribute<FSoftObjectPath>(Settings->MeshPathAttributeName);

		if (!MeshPathAttr)
		{
			PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGDynamicMeshEmbed", "AttrCreateFail",
				"Dynamic Mesh Embed: Failed to create MeshPath attribute."));
			continue;
		}

		// ── Process each point ──

		const TConstPCGValueRange<int32> SeedRange = OutputPointData->GetConstSeedValueRange();
		TPCGValueRange<FTransform> TransformRange = OutputPointData->GetTransformValueRange();
		TPCGValueRange<int64> MetadataEntryRange = OutputPointData->GetMetadataEntryValueRange();

		for (int32 PointIdx = 0; PointIdx < NumPoints; ++PointIdx)
		{
			// ─ 1. Random mesh selection (deterministic using point seed) ─

			const int32 PointSeed = PCGHelpers::ComputeSeed(BaseSeed, SeedRange[PointIdx]);
			FRandomStream Rng(PointSeed);
			const float RollValue = Rng.FRandRange(0.0f, TotalWeight);

			// Fall back to the last bucket so float rounding can't bias toward mesh 0.
			int32 SelectedMeshIdx = MeshCache.Num() - 1;
			for (int32 MeshIdx = 0; MeshIdx < MeshCache.Num(); ++MeshIdx)
			{
				if (RollValue <= MeshCache[MeshIdx].CumulativeWeight)
				{
					SelectedMeshIdx = MeshIdx;
					break;
				}
			}

			const PCGExtScatterCommon::FMeshBoundsCache& SelectedMesh = MeshCache[SelectedMeshIdx];

			// ─ 2. Store the mesh path as an attribute (preserving inherited entries) ─

			Metadata->InitializeOnSet(MetadataEntryRange[PointIdx]);
			MeshPathAttr->SetValue(MetadataEntryRange[PointIdx], SelectedMesh.MeshPath);

			// ─ 3. Extract the surface normal from the point's transform ─
			//      The Surface Sampler orients points so local Z = surface normal

			FTransform& PointTransform = TransformRange[PointIdx];
			const FVector SurfaceNormal = PointTransform.GetRotation().GetUpVector();

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
				const float UniformScale = PointTransform.GetScale3D().X;
				EmbedDistance *= UniformScale;
			}

			// ─ 5. Offset the point INTO the surface ─
			//      Negative normal direction = into the surface

			const FVector EmbedOffset = -SurfaceNormal * EmbedDistance;
			PointTransform.SetLocation(PointTransform.GetLocation() + EmbedOffset);
		}

		// ── Emit output ──

		FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}