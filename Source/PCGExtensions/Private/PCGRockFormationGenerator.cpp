// PCGRockFormationGenerator.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGRockFormationGenerator.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Async/PCGAsyncLoadingContext.h"
#include "Engine/StaticMesh.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGRockFormationGenerator)

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGRockFormationGeneratorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGRockFormationGenerator", "InTooltip",
			"Sparse points on flat ground. Each point becomes the centre of a rock formation."));
	return Pins;
}

TArray<FPCGPinProperties> UPCGRockFormationGeneratorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGRockFormationGenerator", "OutTooltip",
			"All formation rocks (foundation + tiers + fills) with MeshPath attributes. "
			"Feed directly into a Static Mesh Spawner with 'By Attribute' mesh selector."));
	return Pins;
}

FPCGElementPtr UPCGRockFormationGeneratorSettings::CreateElement() const
{
	return MakeShared<FPCGRockFormationGeneratorElement>();
}

// ─────────────────────────────────────────────
//  Element -- Async resource loading
// ─────────────────────────────────────────────

bool FPCGRockFormationGeneratorElement::PrepareDataInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGRockFormationGeneratorElement::PrepareData);

	const UPCGRockFormationGeneratorSettings* Settings =
		Context->GetInputSettings<UPCGRockFormationGeneratorSettings>();
	check(Settings);

	FPCGRockFormationGeneratorContext* ThisContext =
		static_cast<FPCGRockFormationGeneratorContext*>(Context);

	if (!ThisContext->WasLoadRequested())
	{
		// Collect soft paths across all three mesh sets (Foundation + Tier 1 + Tier 2).
		TArray<FSoftObjectPath> ToLoad;

		auto CollectPaths = [&ToLoad](const TArray<FPCGFormationMeshEntry>& Entries)
		{
			for (const FPCGFormationMeshEntry& Entry : Entries)
			{
				if (!Entry.Mesh.IsNull())
				{
					ToLoad.Add(Entry.Mesh.ToSoftObjectPath());
				}
			}
		};

		CollectPaths(Settings->FoundationMeshEntries);
		CollectPaths(Settings->Tier1Config.MeshEntries);
		CollectPaths(Settings->Tier2Config.MeshEntries);

		// Async load suspends the task (returns false) until streaming completes; on resume
		// WasLoadRequested() is true so we skip straight to caching below. A synchronous load
		// returns true immediately and falls through to caching on this same pass. The context
		// holds the streamable handle so the meshes stay resident through Execute.
		if (!ThisContext->RequestResourceLoad(ThisContext, MoveTemp(ToLoad), !Settings->bSynchronousLoad))
		{
			return false;
		}
	}

	// ── Resolve per-tier mesh bounds on the game thread and cache them on the context. ──
	// BuildMeshBoundsCache reads UStaticMesh::GetBoundingBox(), which is unsafe off the game
	// thread, so it happens here in the PrepareData phase rather than in ExecuteInternal
	// (which runs on a worker thread and only reads these caches).
	if (!ThisContext->bCacheBuilt)
	{
		if (!PCGExtScatterCommon::BuildMeshBoundsCache(
			Settings->FoundationMeshEntries,
			ThisContext->FoundationCache,
			ThisContext->FoundationTotalWeight))
		{
			PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGRockFormationGenerator", "NoFoundation",
				"Rock Formation Generator: No valid foundation meshes."));
			return true;
		}

		ThisContext->bHasTier1 = PCGExtScatterCommon::BuildMeshBoundsCache(
			Settings->Tier1Config.MeshEntries,
			ThisContext->Tier1Cache,
			ThisContext->Tier1TotalWeight);
		ThisContext->bHasTier2 = PCGExtScatterCommon::BuildMeshBoundsCache(
			Settings->Tier2Config.MeshEntries,
			ThisContext->Tier2Cache,
			ThisContext->Tier2TotalWeight);

		ThisContext->bCacheBuilt = true;
	}

	return true;
}

// ─────────────────────────────────────────────
//  Element — Helpers
// ─────────────────────────────────────────────

const PCGExtScatterCommon::FMeshBoundsCache& FPCGRockFormationGeneratorElement::SelectRandomMesh(
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& Cache,
	float TotalWeight,
	FRandomStream& Rng)
{
	const float Roll = Rng.FRandRange(0.0f, TotalWeight);
	for (int32 i = 0; i < Cache.Num(); ++i)
	{
		if (Roll <= Cache[i].CumulativeWeight)
		{
			return Cache[i];
		}
	}
	return Cache.Last();
}

void FPCGRockFormationGeneratorElement::GenerateTierRocks(
	const FPCGFormationTierConfig& Config,
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& MeshCache,
	float MeshTotalWeight,
	const TArray<FFormationPlacedRock>& ParentRocks,
	const FVector& FormationCentre,
	int32 TierIndex,
	int32 BaseSeed,
	TArray<FFormationPlacedRock>& OutRocks)
{
	if (MeshCache.Num() == 0) return;

	for (int32 ParentIdx = 0; ParentIdx < ParentRocks.Num(); ++ParentIdx)
	{
		const FFormationPlacedRock& Parent = ParentRocks[ParentIdx];
		if (Parent.bIsFillRock) continue; // Fill rocks don't generate children

		const int32 ParentSeed = PCGHelpers::ComputeSeed(BaseSeed, ParentIdx + TierIndex * 1000);
		FRandomStream Rng(ParentSeed);

		// Probability check
		if (Rng.FRand() > Config.PlacementProbability) continue;

		const int32 RockCount = Rng.RandRange(Config.MinRocksPerParent, Config.MaxRocksPerParent);
		const float ParentHeight = Parent.BoundsMax.Z * Parent.Scale.Z;

		for (int32 RockIdx = 0; RockIdx < RockCount; ++RockIdx)
		{
			const PCGExtScatterCommon::FMeshBoundsCache& SelectedMesh = SelectRandomMesh(MeshCache, MeshTotalWeight, Rng);

			// ─ Scale ─
			const float Scale = Rng.FRandRange(Config.MinScale, Config.MaxScale);

			// ─ Position: radial offset from parent ─
			const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);
			const float Radius = Rng.FRandRange(Config.MinRadius, Config.MaxRadius);
			const float HeightFrac = Rng.FRandRange(Config.MinHeightFraction, Config.MaxHeightFraction);
			const float HeightZ = Parent.Position.Z + ParentHeight * HeightFrac;

			FVector RockPos(
				Parent.Position.X + FMath::Cos(Angle) * Radius,
				Parent.Position.Y + FMath::Sin(Angle) * Radius,
				HeightZ + Config.VerticalOffset);

			// ─ Rotation: random yaw only ─
			// TODO(audit): the advertised "inward tilt toward formation centre" (leaning
			// rock look, driven by Config.MaxInwardTilt) is currently a no-op -- Rock.Rotation
			// below is only ever the yaw quaternion. The original tilt computation
			// (TowardCentre / TiltAngle / RockRot / YawQuat / TiltQuat / FinalRot) was dead
			// code whose result was never stored, so it has been removed to satisfy -Werror.
			// Reimplementing the lean is a behavioral/visual
			const float Yaw = Rng.FRandRange(0.0f, 360.0f);

			// ─ Store placed rock ─
			FFormationPlacedRock Rock;
			Rock.Position = RockPos;
			Rock.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion(); // Simplified: yaw only for now
			Rock.Scale = FVector(Scale);
			Rock.BoundsMin = SelectedMesh.BoundsMin;
			Rock.BoundsMax = SelectedMesh.BoundsMax;
			Rock.MeshPath = SelectedMesh.MeshPath;
			Rock.Tier = TierIndex;
			Rock.bIsFillRock = false;

			OutRocks.Add(Rock);
		}
	}
}

void FPCGRockFormationGeneratorElement::GenerateFillRocks(
	const TArray<FFormationPlacedRock>& AllRocks,
	int32 OverhangSampleCount,
	float OverhangTolerance,
	int32 MaxFillsPerRock,
	float FillScaleMultiplier,
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& FillMeshCache,
	float FillMeshTotalWeight,
	int32 FillTierIndex,
	int32 TargetTierIndex,
	int32 BaseSeed,
	TArray<FFormationPlacedRock>& OutFillRocks)
{
	if (FillMeshCache.Num() == 0) return;

	// Collect all rocks at the support tier (the tier below the overhanging tier)
	TArray<const FFormationPlacedRock*> SupportRocks;
	for (const FFormationPlacedRock& Rock : AllRocks)
	{
		if (Rock.Tier == FillTierIndex)
		{
			SupportRocks.Add(&Rock);
		}
	}

	int32 FillSeedCounter = 0;

	// Check each rock at the target tier (the overhanging tier)
	for (const FFormationPlacedRock& UpperRock : AllRocks)
	{
		if (UpperRock.Tier != TargetTierIndex) continue;
		if (UpperRock.bIsFillRock) continue;

		int32 FillsPlaced = 0;
		const float HalfX = UpperRock.GetWorldHalfExtentX();
		const float HalfY = UpperRock.GetWorldHalfExtentY();
		const float BottomZ = UpperRock.GetWorldBottomZ();

		// Sample points on the bottom face of the upper rock
		FRandomStream SampleRng(PCGHelpers::ComputeSeed(BaseSeed, FillSeedCounter++));

		for (int32 SampleIdx = 0; SampleIdx < OverhangSampleCount && FillsPlaced < MaxFillsPerRock; ++SampleIdx)
		{
			// Sample point on the bottom face
			const float SampleX = UpperRock.Position.X + SampleRng.FRandRange(-HalfX * 0.8f, HalfX * 0.8f);
			const float SampleY = UpperRock.Position.Y + SampleRng.FRandRange(-HalfY * 0.8f, HalfY * 0.8f);
			const FVector SamplePoint(SampleX, SampleY, BottomZ);

			// Check if this point is supported by any rock at the support tier
			bool bSupported = false;
			for (const FFormationPlacedRock* Support : SupportRocks)
			{
				if (Support->ContainsXY(SamplePoint, OverhangTolerance) &&
					Support->GetWorldTopZ() >= BottomZ - OverhangTolerance * 2.0f)
				{
					bSupported = true;
					break;
				}
			}

			if (!bSupported)
			{
				// Place a fill rock at this gap position
				// Fill is at the SUPPORT tier level (below the overhanging rock)
				const PCGExtScatterCommon::FMeshBoundsCache& FillMesh = SelectRandomMesh(
					FillMeshCache, FillMeshTotalWeight, SampleRng);

				// Find the nearest support rock to estimate ground level
				float NearestSupportZ = BottomZ - 200.0f; // Fallback
				float NearestDistSq = TNumericLimits<float>::Max();
				for (const FFormationPlacedRock* Support : SupportRocks)
				{
					const float DistSq = FVector::DistSquaredXY(
						SamplePoint, Support->Position);
					if (DistSq < NearestDistSq)
					{
						NearestDistSq = DistSq;
						NearestSupportZ = Support->Position.Z;
					}
				}

				FFormationPlacedRock FillRock;
				FillRock.Position = FVector(SampleX, SampleY, NearestSupportZ);
				FillRock.Rotation = FRotator(0.0f, SampleRng.FRandRange(0.0f, 360.0f), 0.0f).Quaternion();
				FillRock.Scale = FVector(FillScaleMultiplier *
					SampleRng.FRandRange(0.6f, 1.0f));
				FillRock.BoundsMin = FillMesh.BoundsMin;
				FillRock.BoundsMax = FillMesh.BoundsMax;
				FillRock.MeshPath = FillMesh.MeshPath;
				FillRock.Tier = FillTierIndex; // Fill matches the support tier
				FillRock.bIsFillRock = true;

				OutFillRocks.Add(FillRock);
				FillsPlaced++;
			}
		}
	}
}

// ─────────────────────────────────────────────
//  Element — Main Execution
// ─────────────────────────────────────────────

bool FPCGRockFormationGeneratorElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGRockFormationGeneratorElement::Execute);

	const UPCGRockFormationGeneratorSettings* Settings =
		Context->GetInputSettings<UPCGRockFormationGeneratorSettings>();
	check(Settings);

	FPCGRockFormationGeneratorContext* ThisContext =
		static_cast<FPCGRockFormationGeneratorContext*>(Context);

	// ── Read tier mesh caches resolved on the game thread during PrepareData ──
	// GetBoundingBox() already ran in PrepareDataInternal; here we only read the caches.

	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& FoundationCache = ThisContext->FoundationCache;
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& Tier1Cache = ThisContext->Tier1Cache;
	const TArray<PCGExtScatterCommon::FMeshBoundsCache>& Tier2Cache = ThisContext->Tier2Cache;
	const float FoundationTotalWeight = ThisContext->FoundationTotalWeight;
	const float Tier1TotalWeight = ThisContext->Tier1TotalWeight;
	const float Tier2TotalWeight = ThisContext->Tier2TotalWeight;
	const bool bHasTier1 = ThisContext->bHasTier1;
	const bool bHasTier2 = ThisContext->bHasTier2;

	if (FoundationCache.Num() == 0)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGRockFormationGenerator", "NoFoundation",
			"Rock Formation Generator: No valid foundation meshes."));
		return true;
	}

	// ── Process inputs ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGBasePointData* InputPointData = Cast<UPCGBasePointData>(Input.Data);
		if (!InputPointData) continue;

		const int32 NumInputPoints = InputPointData->GetNumPoints();
		if (NumInputPoints == 0) continue;

		const TConstPCGValueRange<FTransform> InputTransforms = InputPointData->GetConstTransformValueRange();
		const TConstPCGValueRange<int32> InputSeeds = InputPointData->GetConstSeedValueRange();

		// Accumulate every generated rock for ALL input points first, so the output
		// point count is known before allocation. Determinism is preserved: each input
		// point seeds its own FRandomStream exactly as before.
		TArray<FFormationPlacedRock> OutputRocks;

		// ── For each input point, generate a formation ──

		for (int32 FormationIdx = 0; FormationIdx < NumInputPoints; ++FormationIdx)
		{
			const FVector FormationCentre = InputTransforms[FormationIdx].GetLocation();
			const int32 FormationSeed = PCGHelpers::ComputeSeed(BaseSeed, InputSeeds[FormationIdx] + FormationIdx);
			FRandomStream FormationRng(FormationSeed);

			// All placed rocks for this formation (for overhang detection)
			TArray<FFormationPlacedRock> FormationRocks;

			// ── Phase 1: Foundation Cluster (Tier 0) ──

			const int32 FoundationCount = FormationRng.RandRange(
				Settings->MinFoundationRocks, Settings->MaxFoundationRocks);

			TArray<FFormationPlacedRock> FoundationRocks;
			FoundationRocks.Reserve(FoundationCount);

			for (int32 FIdx = 0; FIdx < FoundationCount; ++FIdx)
			{
				const PCGExtScatterCommon::FMeshBoundsCache& Mesh = SelectRandomMesh(
					FoundationCache, FoundationTotalWeight, FormationRng);

				const float Scale = FormationRng.FRandRange(
					Settings->FoundationMinScale, Settings->FoundationMaxScale);

				// Cluster offset (first rock at centre, rest around it)
				FVector Offset = FVector::ZeroVector;
				if (FIdx > 0)
				{
					const float Angle = FormationRng.FRandRange(0.0f, 2.0f * PI);
					const float Dist = FormationRng.FRandRange(
						Settings->FoundationClusterRadius * 0.3f,
						Settings->FoundationClusterRadius);
					Offset = FVector(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist, 0.0f);
				}

				FFormationPlacedRock Rock;
				Rock.Position = FormationCentre + Offset +
					FVector(0.0f, 0.0f, Settings->FoundationVerticalOffset);
				Rock.Rotation = FRotator(0.0f, FormationRng.FRandRange(0.0f, 360.0f), 0.0f).Quaternion();
				Rock.Scale = FVector(Scale);
				Rock.BoundsMin = Mesh.BoundsMin;
				Rock.BoundsMax = Mesh.BoundsMax;
				Rock.MeshPath = Mesh.MeshPath;
				Rock.Tier = 0;
				Rock.bIsFillRock = false;

				FoundationRocks.Add(Rock);
				FormationRocks.Add(Rock);
			}

			// ── Phase 2: Tier 1 (Mid) ──

			TArray<FFormationPlacedRock> Tier1Rocks;
			if (bHasTier1)
			{
				GenerateTierRocks(
					Settings->Tier1Config,
					Tier1Cache, Tier1TotalWeight,
					FoundationRocks,
					FormationCentre,
					1,
					FormationSeed + 100,
					Tier1Rocks);

				FormationRocks.Append(Tier1Rocks);
			}

			// ── Phase 3: Tier 2 (Cap) ──

			TArray<FFormationPlacedRock> Tier2Rocks;
			if (bHasTier2 && Tier1Rocks.Num() > 0)
			{
				GenerateTierRocks(
					Settings->Tier2Config,
					Tier2Cache, Tier2TotalWeight,
					Tier1Rocks,
					FormationCentre,
					2,
					FormationSeed + 200,
					Tier2Rocks);

				FormationRocks.Append(Tier2Rocks);
			}

			// ── Phase 4: Overhang Detection + Fill ──

			TArray<FFormationPlacedRock> FillRocks;

			// Check Tier 1 overhangs against Tier 0 → fill with Tier 0 meshes
			if (Tier1Rocks.Num() > 0)
			{
				GenerateFillRocks(
					FormationRocks,
					Settings->OverhangSampleCount,
					Settings->OverhangTolerance,
					Settings->MaxFillsPerRock,
					Settings->FillScaleMultiplier,
					FoundationCache, FoundationTotalWeight,  // Fill uses Tier 0 meshes
					0,  // Fill tier = 0 (support tier)
					1,  // Target tier = 1 (overhanging tier)
					FormationSeed + 300,
					FillRocks);
			}

			// Check Tier 2 overhangs against Tier 1 → fill with Tier 1 meshes
			if (Tier2Rocks.Num() > 0 && bHasTier1)
			{
				GenerateFillRocks(
					FormationRocks,
					Settings->OverhangSampleCount,
					Settings->OverhangTolerance,
					Settings->MaxFillsPerRock,
					Settings->FillScaleMultiplier,
					Tier1Cache, Tier1TotalWeight,  // Fill uses Tier 1 meshes
					1,  // Fill tier = 1 (support tier)
					2,  // Target tier = 2 (overhanging tier)
					FormationSeed + 400,
					FillRocks);
			}

			FormationRocks.Append(FillRocks);

			// ── Phase 5: Accumulate rocks (seed resolved against the running output count) ──
			// Seed derivation is identical to the legacy path: ComputeSeed(FormationSeed,
			// <running output index>). Accumulating in the same order keeps determinism.
			for (FFormationPlacedRock& Rock : FormationRocks)
			{
				Rock.Seed = PCGHelpers::ComputeSeed(FormationSeed, OutputRocks.Num());
				OutputRocks.Add(Rock);
			}
		}

		if (OutputRocks.Num() == 0) continue;

		// ── Build the output point data (generate pattern) ──

		const int32 TotalRockCount = OutputRocks.Num();

		UPCGBasePointData* OutputPointData = FPCGContext::NewPointData_AnyThread(Context);
		OutputPointData->InitializeFromData(InputPointData);
		OutputPointData->SetNumPoints(TotalRockCount, /*bInitializeValues=*/false);
		OutputPointData->AllocateProperties(
			EPCGPointNativeProperties::Transform |
			EPCGPointNativeProperties::Seed |
			EPCGPointNativeProperties::Density |
			EPCGPointNativeProperties::Steepness |
			EPCGPointNativeProperties::BoundsMin |
			EPCGPointNativeProperties::BoundsMax |
			EPCGPointNativeProperties::MetadataEntry);

		// Create metadata attributes. MeshPath is a SoftObjectPath so the Static Mesh
		// Spawner "By Attribute" selector consumes it natively; keep the bool fill flag.
		UPCGMetadata* Metadata = OutputPointData->MutableMetadata();
		Metadata->CreateSoftObjectPathAttribute(
			Settings->MeshPathAttributeName, FSoftObjectPath(), /*bAllowsInterpolation=*/false);
		FPCGMetadataAttribute<FSoftObjectPath>* MeshPathAttr =
			Metadata->GetMutableTypedAttribute<FSoftObjectPath>(Settings->MeshPathAttributeName);
		FPCGMetadataAttribute<bool>* FillFlagAttr =
			Metadata->FindOrCreateAttribute<bool>(
				Settings->FillFlagAttributeName, false, false, true);

		if (!MeshPathAttr || !FillFlagAttr)
		{
			PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGRockFormationGenerator", "AttrFail",
				"Rock Formation Generator: Failed to create output attributes."));
			continue;
		}

		TPCGValueRange<FTransform> TransformRange = OutputPointData->GetTransformValueRange();
		TPCGValueRange<int32> SeedRange = OutputPointData->GetSeedValueRange();
		TPCGValueRange<float> DensityRange = OutputPointData->GetDensityValueRange();
		TPCGValueRange<float> SteepnessRange = OutputPointData->GetSteepnessValueRange();
		TPCGValueRange<FVector> BoundsMinRange = OutputPointData->GetBoundsMinValueRange();
		TPCGValueRange<FVector> BoundsMaxRange = OutputPointData->GetBoundsMaxValueRange();
		TPCGValueRange<int64> MetadataEntryRange = OutputPointData->GetMetadataEntryValueRange();

		for (int32 i = 0; i < TotalRockCount; ++i)
		{
			const FFormationPlacedRock& Rock = OutputRocks[i];

			TransformRange[i] = FTransform(Rock.Rotation, Rock.Position, Rock.Scale);
			SeedRange[i] = Rock.Seed;
			DensityRange[i] = 1.0f;
			SteepnessRange[i] = 1.0f;
			// Write actual mesh local AABB -- handles base-pivot offset correctly (asymmetric).
			// Downstream consumers (GroundCover exclusion, debug vis) read the asymmetric box.
			BoundsMinRange[i] = Rock.BoundsMin;
			BoundsMaxRange[i] = Rock.BoundsMax;

			// Store mesh path + fill flag via the metadata-entry range.
			MetadataEntryRange[i] = PCGInvalidEntryKey;
			Metadata->InitializeOnSet(MetadataEntryRange[i]);
			MeshPathAttr->SetValue(MetadataEntryRange[i], Rock.MeshPath);
			FillFlagAttr->SetValue(MetadataEntryRange[i], Rock.bIsFillRock);
		}

		FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}