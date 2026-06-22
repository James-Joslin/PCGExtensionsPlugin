// PCGRockFormationGenerator.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGRockFormationGenerator.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Helpers/PCGHelpers.h"
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
//  Element — Helpers
// ─────────────────────────────────────────────

bool FPCGRockFormationGeneratorElement::CacheTierMeshes(
	const TArray<FPCGFormationMeshEntry>& Entries,
	TArray<FFormationMeshCache>& OutCache,
	float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGFormationMeshEntry& Entry : Entries)
	{
		UStaticMesh* LoadedMesh = Entry.Mesh.LoadSynchronous();
		if (!LoadedMesh) continue;

		FFormationMeshCache Cache;
		Cache.MeshPath = FSoftObjectPath(LoadedMesh);
		const FBox Bounds = LoadedMesh->GetBoundingBox();
		Cache.BoundsMin = Bounds.Min;
		Cache.BoundsMax = Bounds.Max;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		Cache.CumulativeWeight = OutTotalWeight;
		OutCache.Add(Cache);
	}

	return OutCache.Num() > 0;
}

const FFormationMeshCache& FPCGRockFormationGeneratorElement::SelectRandomMesh(
	const TArray<FFormationMeshCache>& Cache,
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
	const TArray<FFormationMeshCache>& MeshCache,
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
			const FFormationMeshCache& SelectedMesh = SelectRandomMesh(MeshCache, MeshTotalWeight, Rng);

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

			// ─ Rotation: random yaw + inward tilt toward formation centre ─
			const float Yaw = Rng.FRandRange(0.0f, 360.0f);
			const float InwardTilt = Rng.FRandRange(0.0f, Config.MaxInwardTilt);

			// Compute tilt direction: toward formation centre
			FVector TowardCentre = FormationCentre - RockPos;
			TowardCentre.Z = 0.0f;
			const float TiltAngle = (TowardCentre.Size() > KINDA_SMALL_NUMBER)
				? FMath::Atan2(TowardCentre.Y, TowardCentre.X) * (180.0f / PI)
				: 0.0f;

			// Build rotation: yaw + tilt toward centre
			FRotator RockRot(InwardTilt, Yaw, 0.0f);
			// Apply tilt direction
			const FQuat YawQuat = FRotator(0.0f, TiltAngle, 0.0f).Quaternion();
			const FQuat TiltQuat = FRotator(InwardTilt, 0.0f, 0.0f).Quaternion();
			const FQuat FinalRot = FRotator(0.0f, Yaw, 0.0f).Quaternion() *
				YawQuat * TiltQuat * YawQuat.Inverse();

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
	const TArray<FFormationMeshCache>& FillMeshCache,
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
				const FFormationMeshCache& FillMesh = SelectRandomMesh(
					FillMeshCache, FillMeshTotalWeight, SampleRng);

				// Determine fill rock height: roughly where the support tier sits
				float FillZ = BottomZ - FillMesh.BoundsMax.Z * FillScaleMultiplier * 0.5f;

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

	// ── Cache all tier meshes ──

	TArray<FFormationMeshCache> FoundationCache, Tier1Cache, Tier2Cache;
	float FoundationTotalWeight = 0.0f, Tier1TotalWeight = 0.0f, Tier2TotalWeight = 0.0f;

	if (!CacheTierMeshes(Settings->FoundationMeshEntries, FoundationCache, FoundationTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGRockFormationGenerator", "NoFoundation",
			"Rock Formation Generator: No valid foundation meshes."));
		return true;
	}

	const bool bHasTier1 = CacheTierMeshes(
		Settings->Tier1Config.MeshEntries, Tier1Cache, Tier1TotalWeight);
	const bool bHasTier2 = CacheTierMeshes(
		Settings->Tier2Config.MeshEntries, Tier2Cache, Tier2TotalWeight);

	// ── Process inputs ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGPointData* InputPointData = Cast<UPCGPointData>(Input.Data);
		if (!InputPointData) continue;

		const TArray<FPCGPoint>& InputPoints = InputPointData->GetPoints();
		if (InputPoints.Num() == 0) continue;

		// Create output
		UPCGPointData* OutputPointData = NewObject<UPCGPointData>();
		OutputPointData->InitializeFromData(InputPointData);
		TArray<FPCGPoint>& OutputPoints = OutputPointData->GetMutablePoints();
		OutputPoints.Reset();

		// Create metadata attributes
		UPCGMetadata* Metadata = OutputPointData->MutableMetadata();
		FPCGMetadataAttribute<FString>* MeshPathAttr =
			Metadata->FindOrCreateAttribute<FString>(
				Settings->MeshPathAttributeName, FString(), false, true);
		FPCGMetadataAttribute<bool>* FillFlagAttr =
			Metadata->FindOrCreateAttribute<bool>(
				Settings->FillFlagAttributeName, false, false, true);

		if (!MeshPathAttr || !FillFlagAttr)
		{
			PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGRockFormationGenerator", "AttrFail",
				"Rock Formation Generator: Failed to create output attributes."));
			continue;
		}

		// ── For each input point, generate a formation ──

		for (int32 FormationIdx = 0; FormationIdx < InputPoints.Num(); ++FormationIdx)
		{
			const FPCGPoint& InputPoint = InputPoints[FormationIdx];
			const FVector FormationCentre = InputPoint.Transform.GetLocation();
			const int32 FormationSeed = PCGHelpers::ComputeSeed(BaseSeed, InputPoint.Seed + FormationIdx);
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
				const FFormationMeshCache& Mesh = SelectRandomMesh(
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

			// ── Phase 5: Convert to PCG Points ──

			for (const FFormationPlacedRock& Rock : FormationRocks)
			{
				FPCGPoint Point;
				Point.Transform = FTransform(Rock.Rotation, Rock.Position, Rock.Scale);
				Point.Seed = PCGHelpers::ComputeSeed(FormationSeed,
					OutputPoints.Num());
				Point.Density = 1.0f;
				Point.SetExtents(FVector(50.0f));
				Point.Steepness = 1.0f;

				// Set mesh path attribute
				Metadata->InitializeOnSet(Point.MetadataEntry);
				MeshPathAttr->SetValue(Point.MetadataEntry, Rock.MeshPath.ToString());
				FillFlagAttr->SetValue(Point.MetadataEntry, Rock.bIsFillRock);

				OutputPoints.Add(Point);
			}
		}

		if (OutputPoints.Num() > 0)
		{
			FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
			Output.Data = OutputPointData;
		}
	}

	return true;
}