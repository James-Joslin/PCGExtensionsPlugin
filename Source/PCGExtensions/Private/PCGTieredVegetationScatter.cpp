// PCGTieredVegetationScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGTieredVegetationScatter.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "CollisionQueryParams.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGTieredVegetationScatter)

namespace
{
	const FName CompanionsLabel(TEXT("Companions"));
	const FName RejectedLabel(TEXT("Rejected"));
}

// ─────────────────────────────────────────────
//  Settings — pins
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGTieredVegetationScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "InTooltip",
			"Candidate points to place this tier from (Surface Sampler / jittered grid / "
			"Poisson). Do valley/volume subtraction upstream. Biome weight attributes "
			"(e.g. Biome_Meadow) read from these points if present."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGTieredVegetationScatterSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "OutTooltip",
			"Placed instances with a MeshPath attribute. Feed a By-Attribute Static Mesh "
			"Spawner, and/or this tier's Exclusion Sources for the next tier down."));

	Pins.Emplace(CompanionsLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "CompTooltip",
			"Optional understory ring points (transform-ready, mesh-less). Assign shrub "
			"meshes downstream or pipe into Scatter Around Points."));

	Pins.Emplace(RejectedLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGTieredVegetationScatter", "RejTooltip",
			"Culled candidates (slope / exclusion / noise / density). Debug only."));

	return Pins;
}

FPCGElementPtr UPCGTieredVegetationScatterSettings::CreateElement() const
{
	return MakeShared<FPCGTieredVegetationScatterElement>();
}

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────

bool FPCGTieredVegetationScatterElement::BuildMeshCache(
	const TArray<FPCGVegMeshEntry>& Entries,
	TArray<FVegMeshCache>& OutCache,
	float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGVegMeshEntry& Entry : Entries)
	{
		UStaticMesh* LoadedMesh = Entry.Mesh.LoadSynchronous();
		if (!LoadedMesh)
		{
			continue;
		}

		FVegMeshCache CacheEntry;
		CacheEntry.MeshPath = FSoftObjectPath(LoadedMesh);
		CacheEntry.ScaleRange = Entry.ScaleRange;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		CacheEntry.CumulativeWeight = OutTotalWeight;
		OutCache.Add(CacheEntry);
	}

	return OutCache.Num() > 0;
}

int32 FPCGTieredVegetationScatterElement::SelectMeshIndex(
	const TArray<FVegMeshCache>& Cache, float TotalWeight, FRandomStream& Rng)
{
	const float Roll = Rng.FRandRange(0.0f, TotalWeight);
	for (int32 i = 0; i < Cache.Num(); ++i)
	{
		if (Roll <= Cache[i].CumulativeWeight)
		{
			return i;
		}
	}
	return Cache.Num() - 1;
}

float FPCGTieredVegetationScatterElement::ReadFloatAttr(
	const UPCGMetadata* Meta, FName Name, int64 EntryKey, float Default)
{
	if (!Meta || Name.IsNone() || !Meta->HasAttribute(Name))
	{
		return Default;
	}

	const FPCGMetadataAttributeBase* Base = Meta->GetConstAttribute(Name);
	if (!Base || Base->GetTypeId() != PCG::Private::MetadataTypes<float>::Id)
	{
		return Default;
	}

	const FPCGMetadataAttribute<float>* Attr = static_cast<const FPCGMetadataAttribute<float>*>(Base);
	return Attr->GetValueFromItemKey(EntryKey);
}

FString FPCGTieredVegetationScatterElement::ReadStringAttr(
	const UPCGMetadata* Meta, FName Name, int64 EntryKey)
{
	if (!Meta || Name.IsNone() || !Meta->HasAttribute(Name))
	{
		return FString();
	}

	const FPCGMetadataAttributeBase* Base = Meta->GetConstAttribute(Name);
	if (!Base || Base->GetTypeId() != PCG::Private::MetadataTypes<FString>::Id)
	{
		return FString();
	}

	const FPCGMetadataAttribute<FString>* Attr = static_cast<const FPCGMetadataAttribute<FString>*>(Base);
	return Attr->GetValueFromItemKey(EntryKey);
}

void FPCGTieredVegetationScatterElement::ComputeBiomeFactors(
	const TArray<FPCGVegBiomeResponse>& Responses,
	EPCGVegBiomeCombine Mode,
	const UPCGMetadata* Meta,
	int64 EntryKey,
	float& OutDensity,
	float& OutScale)
{
	OutDensity = 1.0f;
	OutScale = 1.0f;

	if (Responses.Num() == 0 || !Meta)
	{
		return;
	}

	switch (Mode)
	{
	case EPCGVegBiomeCombine::WeightedAverage:
	{
		float SumW = 0.0f, SumWD = 0.0f, SumWS = 0.0f;
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			SumW += W;
			SumWD += W * R.DensityMultiplier;
			SumWS += W * R.ScaleMultiplier;
		}
		// Residual weight (where no biome dominates) stays neutral at 1.0.
		const float Residual = FMath::Max(0.0f, 1.0f - SumW);
		const float Denom = SumW + Residual; // == max(1, SumW)
		if (Denom > KINDA_SMALL_NUMBER)
		{
			OutDensity = (SumWD + Residual * 1.0f) / Denom;
			OutScale = (SumWS + Residual * 1.0f) / Denom;
		}
		break;
	}
	case EPCGVegBiomeCombine::Max:
	{
		float BestW = 0.0f;
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			if (W > BestW)
			{
				BestW = W;
				OutDensity = R.DensityMultiplier;
				OutScale = R.ScaleMultiplier;
			}
		}
		// Fade toward neutral when even the dominant biome is weak.
		OutDensity = FMath::Lerp(1.0f, OutDensity, BestW);
		OutScale = FMath::Lerp(1.0f, OutScale, BestW);
		break;
	}
	case EPCGVegBiomeCombine::Multiply:
	{
		for (const FPCGVegBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			OutDensity *= FMath::Lerp(1.0f, R.DensityMultiplier, W);
			OutScale *= FMath::Lerp(1.0f, R.ScaleMultiplier, W);
		}
		break;
	}
	}

	OutDensity = FMath::Max(0.0f, OutDensity);
	OutScale = FMath::Max(0.01f, OutScale);
}

FQuat FPCGTieredVegetationScatterElement::MakeFoliageRotation(
	const FVector& SurfaceNormal, float YawDeg, float AlignAmount,
	float MaxAlignDeg, float JitterPitchDeg, float JitterRollDeg)
{
	const FVector WorldUp(0.0, 0.0, 1.0);
	const FVector N = SurfaceNormal.GetSafeNormal();

	FVector DesiredUp = WorldUp;
	if (AlignAmount > 0.0f)
	{
		const float NDot = FMath::Clamp(FVector::DotProduct(WorldUp, N), -1.0f, 1.0f);
		const float FullAngle = FMath::Acos(NDot);
		if (FullAngle > KINDA_SMALL_NUMBER)
		{
			const float MaxRad = FMath::DegreesToRadians(MaxAlignDeg);
			float Alpha = AlignAmount;
			if (FullAngle * Alpha > MaxRad)
			{
				Alpha = MaxRad / FullAngle;
			}
			DesiredUp = FMath::Lerp(WorldUp, N, Alpha).GetSafeNormal();
		}
	}

	const FQuat AlignQuat = FQuat::FindBetweenNormals(WorldUp, DesiredUp);
	const FQuat YawQuat(DesiredUp, FMath::DegreesToRadians(YawDeg));
	const FQuat JitterQuat = FRotator(JitterPitchDeg, 0.0f, JitterRollDeg).Quaternion();

	return YawQuat * AlignQuat * JitterQuat;
}

float FPCGTieredVegetationScatterElement::Perlin01(const FVector& Position, float Frequency, int32 Seed)
{
	// Offset the sample domain by the seed so different tiers/layers decorrelate.
	const FVector SeedOffset(Seed * 13.13f, Seed * 7.77f, Seed * 3.33f);
	const float N = FMath::PerlinNoise3D(Position * Frequency + SeedOffset);
	return FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
}

// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

bool FPCGTieredVegetationScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGTieredVegetationScatterElement::Execute);

	const UPCGTieredVegetationScatterSettings* Settings =
		Context->GetInputSettings<UPCGTieredVegetationScatterSettings>();
	check(Settings);

	// ── World for line tracing ──
	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}
	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGTieredVegetationScatter", "NoWorld",
			"Tiered Vegetation Scatter: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Mesh cache ──
	TArray<FVegMeshCache> MeshCache;
	float MeshTotalWeight = 0.0f;
	if (!BuildMeshCache(Settings->MeshSet, MeshCache, MeshTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGTieredVegetationScatter", "NoMeshes",
			"Tiered Vegetation Scatter: No valid meshes in Mesh Set."));
		return true;
	}

	// ── Validate config ──
	const float MinSlope = FMath::Min(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MaxSlope = FMath::Max(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	const float CompMinDist = FMath::Max(Settings->CompanionMinDistance, 0.0f);
	const float CompMinDistSq = CompMinDist * CompMinDist;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());
	const FVector WorldUp(0.0, 0.0, 1.0);

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	// ── Outputs (accumulated across all input data sets) ──
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	const TArray<FPCGTaggedData> CandidateInputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	UPCGPointData* OutData = nullptr;
	UPCGPointData* CompData = nullptr;
	UPCGPointData* RejData = nullptr;
	FPCGMetadataAttribute<FString>* MeshPathAttr = nullptr;

	TArray<FVector> AcceptedPositions;   // for primary min-distance pruning
	TArray<FVector> CompanionPositions;  // for companion min-distance pruning

	// Local projection lambda.
	auto Project = [&](const FVector& XY, double RefZ, FVector& OutPos, FVector& OutNormal) -> bool
		{
			const FVector Start(XY.X, XY.Y, RefZ + Settings->TraceStartHeight);
			const FVector End(XY.X, XY.Y, RefZ - Settings->TraceDistance);
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, Start, End, Settings->TraceChannel, QueryParams))
			{
				OutPos = Hit.ImpactPoint;
				OutNormal = Hit.ImpactNormal;
				return true;
			}
			return false;
		};

	for (const FPCGTaggedData& Input : CandidateInputs)
	{
		const UPCGPointData* InData = Cast<UPCGPointData>(Input.Data);
		if (!InData)
		{
			continue;
		}

		const TArray<FPCGPoint>& InPoints = InData->GetPoints();
		if (InPoints.Num() == 0)
		{
			continue;
		}

		const UPCGMetadata* InMeta = InData->ConstMetadata();

		// Lazily create outputs from the first valid input (for metadata schema).
		if (!OutData)
		{
			OutData = NewObject<UPCGPointData>();
			OutData->InitializeFromData(InData);
			UPCGMetadata* OutMeta = OutData->MutableMetadata();
			MeshPathAttr = OutMeta->FindOrCreateAttribute<FString>(
				Settings->MeshPathAttributeName, FString(),
				/*bAllowInterpolation=*/false, /*bOverrideParent=*/true);

			if (Settings->bGenerateCompanions)
			{
				CompData = NewObject<UPCGPointData>();
				CompData->InitializeFromData(InData);
			}
			if (Settings->bOutputRejected)
			{
				RejData = NewObject<UPCGPointData>();
				RejData->InitializeFromData(InData);
			}
		}

		TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
		TArray<FPCGPoint>* CompPoints = CompData ? &CompData->GetMutablePoints() : nullptr;
		TArray<FPCGPoint>* RejPoints = RejData ? &RejData->GetMutablePoints() : nullptr;
		UPCGMetadata* OutMeta = OutData->MutableMetadata();

		for (int32 Idx = 0; Idx < InPoints.Num(); ++Idx)
		{
			const FPCGPoint& Cand = InPoints[Idx];
			const FVector CandPos = Cand.Transform.GetLocation();

			const int32 PointSeed = PCGHelpers::ComputeSeed(BaseSeed, Cand.Seed + Idx);
			FRandomStream Rng(PointSeed);

			// 1. Project onto the landscape.
			FVector ProjPos, Normal;
			if (!Project(CandPos, CandPos.Z, ProjPos, Normal))
			{
				continue; // no surface — silently drop (not even a "reject")
			}

			auto Reject = [&]()
				{
					if (RejPoints)
					{
						FPCGPoint R = Cand;
						R.Transform.SetLocation(ProjPos);
						RejPoints->Add(R);
					}
				};

			// 2. Slope band.
			const float SlopeDot = FVector::DotProduct(Normal, WorldUp);
			if (SlopeDot < MinSlope || SlopeDot > MaxSlope)
			{
				Reject();
				continue;
			}

			// 3. Noise cluster gate.
			float Noise = 1.0f;
			if (Settings->bUseNoiseMask)
			{
				Noise = Perlin01(ProjPos, Settings->NoiseFrequency, Settings->NoiseSeed);
				if (Settings->bInvertNoise)
				{
					Noise = 1.0f - Noise;
				}
				if (Noise < Settings->NoiseThreshold)
				{
					Reject();
					continue;
				}
			}

			// 4. Biome response.
			float BiomeDensity = 1.0f, BiomeScale = 1.0f;
			ComputeBiomeFactors(Settings->BiomeResponses, Settings->BiomeCombineMode,
				InMeta, Cand.MetadataEntry, BiomeDensity, BiomeScale);

			// 6. Density roll.
			const float Keep = FMath::Clamp(
				Settings->KeepProbability * Noise * BiomeDensity, 0.0f, 1.0f);
			if (Rng.FRand() > Keep)
			{
				Reject();
				continue;
			}

			// 7. Min-distance prune.
			if (MinDist > 0.0f)
			{
				bool bTooClose = false;
				for (const FVector& P : AcceptedPositions)
				{
					if (FVector::DistSquared(ProjPos, P) < MinDistSq)
					{
						bTooClose = true;
						break;
					}
				}
				if (bTooClose)
				{
					Reject();
					continue;
				}
			}

			// 8. Build the instance.
			FPCGPoint NewPoint;
			NewPoint.Seed = PointSeed;
			NewPoint.Density = Keep;
			NewPoint.Steepness = 1.0f;
			NewPoint.SetExtents(FVector(50.0f));

			const int32 MeshIdx = SelectMeshIndex(MeshCache, MeshTotalWeight, Rng);
			const FVegMeshCache& Chosen = MeshCache[MeshIdx];

			const float TierScale = Rng.FRandRange(Settings->TierScaleRange.X, Settings->TierScaleRange.Y);
			const float MeshScale = Rng.FRandRange(Chosen.ScaleRange.X, Chosen.ScaleRange.Y);
			float FinalScale = TierScale * MeshScale;
			if (Settings->bBiomeModulatesScale)
			{
				FinalScale *= BiomeScale;
			}

			const float YawDeg = Settings->bRandomYaw ? Rng.FRandRange(0.0f, 360.0f) : 0.0f;
			const float JitterP = Rng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const float JitterR = Rng.FRandRange(-Settings->PitchRollJitterDeg, Settings->PitchRollJitterDeg);
			const FQuat Rot = MakeFoliageRotation(Normal, YawDeg,
				Settings->SlopeAlignAmount, Settings->MaxAlignAngleDeg, JitterP, JitterR);

			const FVector FinalPos = ProjPos + FVector(0.0, 0.0, Settings->ZOffset);
			NewPoint.Transform = FTransform(Rot, FinalPos, FVector(FinalScale));

			// Write the mesh path attribute on a fresh metadata entry.
			NewPoint.MetadataEntry = PCGInvalidEntryKey;
			OutMeta->InitializeOnSet(NewPoint.MetadataEntry);
			if (MeshPathAttr)
			{
				MeshPathAttr->SetValue(NewPoint.MetadataEntry, Chosen.MeshPath.ToString());
			}

			OutPoints.Add(NewPoint);
			AcceptedPositions.Add(ProjPos);

			// 9. Companions.
			if (Settings->bGenerateCompanions && CompPoints)
			{
				const int32 NComp = Rng.RandRange(
					FMath::Min(Settings->CompanionsPerPrimaryMin, Settings->CompanionsPerPrimaryMax),
					FMath::Max(Settings->CompanionsPerPrimaryMin, Settings->CompanionsPerPrimaryMax));

				const float RMin = FMath::Min(Settings->CompanionRadiusMin, Settings->CompanionRadiusMax);
				const float RMax = FMath::Max(Settings->CompanionRadiusMin, Settings->CompanionRadiusMax);

				for (int32 c = 0; c < NComp; ++c)
				{
					const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);
					const float Radius = Rng.FRandRange(RMin, RMax);
					const FVector CompXY(
						ProjPos.X + FMath::Cos(Angle) * Radius,
						ProjPos.Y + FMath::Sin(Angle) * Radius,
						ProjPos.Z);

					FVector CompPos, CompNormal;
					if (!Project(CompXY, ProjPos.Z, CompPos, CompNormal))
					{
						continue;
					}

					const float CompSlope = FVector::DotProduct(CompNormal, WorldUp);
					if (CompSlope < MinSlope || CompSlope > MaxSlope)
					{
						continue;
					}

					//if (Exclusions.Num() > 0 &&
					//	ExclusionFactorAt(Exclusions, FVector2D(CompPos.X, CompPos.Y),
					//		Settings->ExclusionFalloffDistance) <= 0.0f)
					//{
					//	continue;
					//}

					if (CompMinDist > 0.0f)
					{
						bool bClose = false;
						for (const FVector& P : CompanionPositions)
						{
							if (FVector::DistSquared(CompPos, P) < CompMinDistSq)
							{
								bClose = true;
								break;
							}
						}
						if (bClose)
						{
							continue;
						}
					}

					FPCGPoint Comp;
					Comp.Seed = PCGHelpers::ComputeSeed(PointSeed, c + 1);
					Comp.Density = 1.0f;
					Comp.Steepness = 1.0f;
					Comp.SetExtents(FVector(30.0f));

					FRandomStream CompRng(Comp.Seed);
					const float CompScale = CompRng.FRandRange(
						Settings->CompanionScaleRange.X, Settings->CompanionScaleRange.Y);
					const float CYaw = Settings->bRandomYaw ? CompRng.FRandRange(0.0f, 360.0f) : 0.0f;
					const FQuat CRot = MakeFoliageRotation(CompNormal, CYaw,
						Settings->SlopeAlignAmount, Settings->MaxAlignAngleDeg, 0.0f, 0.0f);

					Comp.Transform = FTransform(CRot,
						CompPos + FVector(0.0, 0.0, Settings->CompanionZOffset),
						FVector(CompScale));

					CompPoints->Add(Comp);
					CompanionPositions.Add(CompPos);
				}
			}
		}
	}

	// ── Emit ──
	if (OutData && OutData->GetPoints().Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = OutData;
		T.Pin = PCGPinConstants::DefaultOutputLabel;
	}
	if (CompData && CompData->GetPoints().Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = CompData;
		T.Pin = CompanionsLabel;
	}
	if (RejData && RejData->GetPoints().Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = RejData;
		T.Pin = RejectedLabel;
	}

	return true;
}