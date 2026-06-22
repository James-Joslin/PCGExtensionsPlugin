// PCGGroundCoverScatter.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGGroundCoverScatter.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSpatialData.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "CollisionQueryParams.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGGroundCoverScatter)

namespace
{
	const FName RejectedLabel(TEXT("Rejected"));

	// ── Deterministic per-cell hash → two floats in [0,1). Used by Worley + Random. ──
	FORCEINLINE FVector2D CellRandom2(int32 CX, int32 CY, int32 Seed)
	{
		uint32 H = uint32(CX) * 374761393u + uint32(CY) * 668265263u + uint32(Seed) * 2246822519u;
		H = (H ^ (H >> 13)) * 1274126177u;
		const uint32 HX = H ^ (H >> 16);
		H = (H * 2654435761u) ^ (H >> 15);
		const uint32 HY = H ^ (H >> 16);
		return FVector2D(float(HX & 0xFFFFFFu) / 16777216.0f, float(HY & 0xFFFFFFu) / 16777216.0f);
	}

	// ── 2D Worley: F1 = nearest feature distance, F2 = second nearest (in cell units). ──
	void Worley2D(const FVector2D& P, int32 Seed, float& OutF1, float& OutF2)
	{
		const int32 CX = FMath::FloorToInt(P.X);
		const int32 CY = FMath::FloorToInt(P.Y);
		float F1 = BIG_NUMBER;
		float F2 = BIG_NUMBER;

		for (int32 dx = -1; dx <= 1; ++dx)
		{
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				const int32 NX = CX + dx;
				const int32 NY = CY + dy;
				const FVector2D Feature = FVector2D(NX, NY) + CellRandom2(NX, NY, Seed);
				const float D = FVector2D::Distance(P, Feature);
				if (D < F1) { F2 = F1; F1 = D; }
				else if (D < F2) { F2 = D; }
			}
		}
		OutF1 = F1;
		OutF2 = F2;
	}

	// ── Per-position hash random in [0,1] (quantised to ~1 UU). ──
	FORCEINLINE float PosRandom01(const FVector& Pos, int32 Seed)
	{
		const FVector2D R = CellRandom2(FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y), Seed);
		return R.X;
	}

	// ── Uniform XY hash grid for min-distance pruning. Cell size must be >= query radius. ──
	struct FMinDistGrid
	{
		float CellSize = 100.0f;
		TMap<FIntPoint, TArray<FVector>> Cells;

		FORCEINLINE FIntPoint CellOf(const FVector& P) const
		{
			return FIntPoint(FMath::FloorToInt(P.X / CellSize), FMath::FloorToInt(P.Y / CellSize));
		}
		FORCEINLINE void Add(const FVector& P) { Cells.FindOrAdd(CellOf(P)).Add(P); }

		bool HasWithin(const FVector& P, float RadiusSq) const
		{
			const FIntPoint C = CellOf(P);
			for (int32 dx = -1; dx <= 1; ++dx)
			{
				for (int32 dy = -1; dy <= 1; ++dy)
				{
					if (const TArray<FVector>* Arr = Cells.Find(FIntPoint(C.X + dx, C.Y + dy)))
					{
						for (const FVector& Q : *Arr)
						{
							if (FVector::DistSquared(P, Q) < RadiusSq)
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}
	};
}

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

UPCGGroundCoverScatterSettings::UPCGGroundCoverScatterSettings()
{
	// Default noise stack: large Perlin patches × medium Worley clumps + fine Perlin + micro jitter.
	NoiseLayers.Reset();

	FPCGGroundNoiseLayer L0;
	L0.NoiseType = EPCGGroundNoiseType::Perlin;
	L0.Frequency = 0.004f; L0.Amplitude = 1.0f; L0.BlendMode = EPCGGroundNoiseBlend::Multiply; L0.Seed = 0;
	NoiseLayers.Add(L0);

	FPCGGroundNoiseLayer L1;
	L1.NoiseType = EPCGGroundNoiseType::Worley_F1;
	L1.Frequency = 0.03f; L1.Amplitude = 1.0f; L1.bInvert = true; // invert → dense at cell cores
	L1.BlendMode = EPCGGroundNoiseBlend::Multiply; L1.Seed = 1;
	NoiseLayers.Add(L1);

	FPCGGroundNoiseLayer L2;
	L2.NoiseType = EPCGGroundNoiseType::Perlin;
	L2.Frequency = 0.1f; L2.Amplitude = 0.3f; L2.BlendMode = EPCGGroundNoiseBlend::Add; L2.Seed = 2;
	NoiseLayers.Add(L2);

	FPCGGroundNoiseLayer L3;
	L3.NoiseType = EPCGGroundNoiseType::Random;
	L3.Frequency = 0.0f; L3.Amplitude = 0.1f; L3.BlendMode = EPCGGroundNoiseBlend::Add; L3.Seed = 3;
	NoiseLayers.Add(L3);
}

TArray<FPCGPinProperties> UPCGGroundCoverScatterSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "InTooltip",
			"Dense candidate points (Surface Sampler / jittered grid). Biome weight attributes "
			"(e.g. Biome_Meadow) are read from these points if present."));

	return Pins;
}

TArray<FPCGPinProperties> UPCGGroundCoverScatterSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;

	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "OutTooltip",
			"Placed ground-cover instances with a MeshPath attribute. Feed a By-Attribute "
			"Static Mesh Spawner."));

	Pins.Emplace(RejectedLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGGroundCoverScatter", "RejTooltip",
			"Culled candidates (slope / noise / spacing). Debug only."));

	return Pins;
}

FPCGElementPtr UPCGGroundCoverScatterSettings::CreateElement() const
{
	return MakeShared<FPCGGroundCoverScatterElement>();
}

// ─────────────────────────────────────────────
//  Helpers — mesh
// ─────────────────────────────────────────────

bool FPCGGroundCoverScatterElement::BuildMeshCache(
	const TArray<FPCGGroundMeshEntry>& Entries,
	TArray<FGroundMeshCache>& OutCache, float& OutTotalWeight)
{
	OutCache.Reset();
	OutTotalWeight = 0.0f;

	for (const FPCGGroundMeshEntry& Entry : Entries)
	{
		UStaticMesh* LoadedMesh = Entry.Mesh.LoadSynchronous();
		if (!LoadedMesh)
		{
			continue;
		}

		FGroundMeshCache CacheEntry;
		CacheEntry.MeshPath = FSoftObjectPath(LoadedMesh);
		CacheEntry.ScaleRange = Entry.ScaleRange;
		OutTotalWeight += FMath::Max(Entry.Weight, 0.01f);
		CacheEntry.CumulativeWeight = OutTotalWeight;
		OutCache.Add(CacheEntry);
	}

	return OutCache.Num() > 0;
}

int32 FPCGGroundCoverScatterElement::SelectMeshIndex(
	const TArray<FGroundMeshCache>& Cache, float TotalWeight, FRandomStream& Rng)
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

// ─────────────────────────────────────────────
//  Helpers — attributes
// ─────────────────────────────────────────────

float FPCGGroundCoverScatterElement::ReadFloatAttr(
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

// ─────────────────────────────────────────────
//  Helpers — biome
// ─────────────────────────────────────────────

void FPCGGroundCoverScatterElement::ComputeBiomeFactors(
	const TArray<FPCGGroundBiomeResponse>& Responses, EPCGGroundBiomeCombine Mode,
	const UPCGMetadata* Meta, int64 EntryKey, float& OutDensity, float& OutScale)
{
	OutDensity = 1.0f;
	OutScale = 1.0f;

	if (Responses.Num() == 0 || !Meta)
	{
		return;
	}

	switch (Mode)
	{
	case EPCGGroundBiomeCombine::WeightedAverage:
	{
		float SumW = 0.0f, SumWD = 0.0f, SumWS = 0.0f;
		for (const FPCGGroundBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			SumW += W;
			SumWD += W * R.DensityMultiplier;
			SumWS += W * R.ScaleMultiplier;
		}
		const float Residual = FMath::Max(0.0f, 1.0f - SumW);
		const float Denom = SumW + Residual;
		if (Denom > KINDA_SMALL_NUMBER)
		{
			OutDensity = (SumWD + Residual) / Denom;
			OutScale = (SumWS + Residual) / Denom;
		}
		break;
	}
	case EPCGGroundBiomeCombine::Max:
	{
		float BestW = 0.0f;
		for (const FPCGGroundBiomeResponse& R : Responses)
		{
			const float W = FMath::Clamp(ReadFloatAttr(Meta, R.BiomeAttribute, EntryKey, 0.0f), 0.0f, 1.0f);
			if (W > BestW)
			{
				BestW = W;
				OutDensity = R.DensityMultiplier;
				OutScale = R.ScaleMultiplier;
			}
		}
		OutDensity = FMath::Lerp(1.0f, OutDensity, BestW);
		OutScale = FMath::Lerp(1.0f, OutScale, BestW);
		break;
	}
	case EPCGGroundBiomeCombine::Multiply:
	{
		for (const FPCGGroundBiomeResponse& R : Responses)
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

// ─────────────────────────────────────────────
//  Helpers — transform
// ─────────────────────────────────────────────

FQuat FPCGGroundCoverScatterElement::MakeFoliageRotation(
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

// ─────────────────────────────────────────────
//  Helpers — noise stack
// ─────────────────────────────────────────────

float FPCGGroundCoverScatterElement::SampleNoiseStack(
	const TArray<FPCGGroundNoiseLayer>& Layers, const FVector& Position, bool bClamp)
{
	float Result = 1.0f;

	for (const FPCGGroundNoiseLayer& L : Layers)
	{
		float V = 0.0f;
		switch (L.NoiseType)
		{
		case EPCGGroundNoiseType::Perlin:
		{
			const FVector SeedOffset(L.Seed * 13.13f, L.Seed * 7.77f, L.Seed * 3.33f);
			const float N = FMath::PerlinNoise3D(Position * L.Frequency + SeedOffset);
			V = FMath::Clamp(0.5f * (N + 1.0f), 0.0f, 1.0f);
			break;
		}
		case EPCGGroundNoiseType::Worley_F1:
		{
			float F1, F2;
			Worley2D(FVector2D(Position.X, Position.Y) * L.Frequency, L.Seed, F1, F2);
			V = FMath::Clamp(F1, 0.0f, 1.0f); // 0 at feature centre → 1 at cell edge
			break;
		}
		case EPCGGroundNoiseType::Worley_F2MinusF1:
		{
			float F1, F2;
			Worley2D(FVector2D(Position.X, Position.Y) * L.Frequency, L.Seed, F1, F2);
			V = FMath::Clamp(F2 - F1, 0.0f, 1.0f); // ridge lines between cells
			break;
		}
		case EPCGGroundNoiseType::Random:
		{
			V = PosRandom01(Position, L.Seed);
			break;
		}
		}

		if (L.bInvert)
		{
			V = 1.0f - V;
		}
		V = V * L.Amplitude + L.Bias;

		switch (L.BlendMode)
		{
		case EPCGGroundNoiseBlend::Multiply: Result *= V; break;
		case EPCGGroundNoiseBlend::Add:      Result += V; break;
		case EPCGGroundNoiseBlend::Min:      Result = FMath::Min(Result, V); break;
		case EPCGGroundNoiseBlend::Max:      Result = FMath::Max(Result, V); break;
		case EPCGGroundNoiseBlend::Replace:  Result = V; break;
		}
	}

	return bClamp ? FMath::Clamp(Result, 0.0f, 1.0f) : Result;
}

// ─────────────────────────────────────────────
//  Element — execution
// ─────────────────────────────────────────────

bool FPCGGroundCoverScatterElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGGroundCoverScatterElement::Execute);

	const UPCGGroundCoverScatterSettings* Settings =
		Context->GetInputSettings<UPCGGroundCoverScatterSettings>();
	check(Settings);

	// ── World for line tracing ──
	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}
	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGGroundCoverScatter", "NoWorld",
			"Ground Cover Scatter: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Mesh cache ──
	TArray<FGroundMeshCache> MeshCache;
	float MeshTotalWeight = 0.0f;
	if (!BuildMeshCache(Settings->MeshSet, MeshCache, MeshTotalWeight))
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGGroundCoverScatter", "NoMeshes",
			"Ground Cover Scatter: No valid meshes in Mesh Set."));
		return true;
	}


	float ExclCellSize = 500.0f;
	TMap<FIntPoint, TArray<int32>> ExclGrid;
	auto ExclCellOf = [ExclCellSize](float X, float Y)
		{
			return FIntPoint(FMath::FloorToInt(X / ExclCellSize), FMath::FloorToInt(Y / ExclCellSize));
		};


	// ── Validate config ──
	const float MinSlope = FMath::Min(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MaxSlope = FMath::Max(Settings->MinSlopeDot, Settings->MaxSlopeDot);
	const float MinDist = FMath::Max(Settings->MinDistance, 0.0f);
	const float MinDistSq = MinDist * MinDist;
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());
	const FVector WorldUp(0.0, 0.0, 1.0);

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	// Min-distance acceleration grid (cell size must be >= MinDistance for the 3x3 lookup).
	FMinDistGrid SpacingGrid;
	SpacingGrid.CellSize = FMath::Max(MinDist, 1.0f);

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

	// ── Outputs ──
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;
	const TArray<FPCGTaggedData> CandidateInputs =
		Context->InputData.GetInputsByPin(PCGPinConstants::DefaultInputLabel);

	UPCGPointData* OutData = nullptr;
	UPCGPointData* RejData = nullptr;
	FPCGMetadataAttribute<FString>* MeshPathAttr = nullptr;

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

		// Lazily create outputs from the first valid input (schema source).
		if (!OutData)
		{
			OutData = NewObject<UPCGPointData>();
			OutData->InitializeFromData(InData);
			UPCGMetadata* InitMeta = OutData->MutableMetadata();
			MeshPathAttr = InitMeta->FindOrCreateAttribute<FString>(
				Settings->MeshPathAttributeName, FString(),
				/*bAllowInterpolation=*/false, /*bOverrideParent=*/true);

			if (Settings->bOutputRejected)
			{
				RejData = NewObject<UPCGPointData>();
				RejData->InitializeFromData(InData);
			}
		}

		TArray<FPCGPoint>& OutPoints = OutData->GetMutablePoints();
		TArray<FPCGPoint>* RejPoints = RejData ? &RejData->GetMutablePoints() : nullptr;
		UPCGMetadata* OutMeta = OutData->MutableMetadata();

		OutPoints.Reserve(OutPoints.Num() + InPoints.Num());

		for (int32 Idx = 0; Idx < InPoints.Num(); ++Idx)
		{
			const FPCGPoint& Cand = InPoints[Idx];
			const FVector CandPos = Cand.Transform.GetLocation();

			const int32 PointSeed = PCGHelpers::ComputeSeed(BaseSeed, Cand.Seed + Idx);
			FRandomStream Rng(PointSeed);

			auto Reject = [&]()
				{
					if (RejPoints)
					{
						RejPoints->Add(Cand);
					}
				};

			// 1. Optional XY jitter (break grid regularity), then project.
			FVector SampleXY = CandPos;
			if (Settings->PositionJitter > 0.0f)
			{
				const float Ang = Rng.FRandRange(0.0f, 2.0f * PI);
				const float Rad = Rng.FRandRange(0.0f, Settings->PositionJitter);
				SampleXY.X += FMath::Cos(Ang) * Rad;
				SampleXY.Y += FMath::Sin(Ang) * Rad;
			}

			FVector ProjPos, Normal;
			if (!Project(SampleXY, CandPos.Z, ProjPos, Normal))
			{
				continue; // no surface — drop silently
			}

			// 2. Slope band.
			const float SlopeDot = FVector::DotProduct(Normal, WorldUp);
			if (SlopeDot < MinSlope || SlopeDot > MaxSlope)
			{
				Reject();
				continue;
			}

			// 3. Noise stack → density.
			float NoiseDensity = 1.0f;
			if (Settings->bUseNoiseStack && Settings->NoiseLayers.Num() > 0)
			{
				NoiseDensity = SampleNoiseStack(Settings->NoiseLayers, ProjPos, Settings->bClampNoise);
				if (NoiseDensity < Settings->NoiseCullThreshold)
				{
					Reject();
					continue;
				}
			}

			// 4. Biome response.
			float BiomeDensity = 1.0f, BiomeScale = 1.0f;
			ComputeBiomeFactors(Settings->BiomeResponses, Settings->BiomeCombineMode,
				InMeta, Cand.MetadataEntry, BiomeDensity, BiomeScale);


			const float Keep = FMath::Clamp(
				Settings->KeepProbability * NoiseDensity * BiomeDensity, 0.0f, 1.0f);

			if (Rng.FRand() > Keep)
			{
				Reject();
				continue;
			}

			// 7. Min-distance prune (spatial-hash accelerated).
			if (MinDist > 0.0f)
			{
				if (SpacingGrid.HasWithin(ProjPos, MinDistSq))
				{
					Reject();
					continue;
				}
			}

			// 8. Build the instance.
			FPCGPoint NewPoint = Cand;
			NewPoint.Seed = PointSeed;
			NewPoint.Density = Settings->bWriteDensity ? Keep : Cand.Density;
			NewPoint.Steepness = 1.0f;
			NewPoint.SetExtents(FVector(8.0f));

			const int32 MeshIdx = SelectMeshIndex(MeshCache, MeshTotalWeight, Rng);
			const FGroundMeshCache& Chosen = MeshCache[MeshIdx];

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

			// Fresh metadata entry for the mesh path.
			NewPoint.MetadataEntry = PCGInvalidEntryKey;
			OutMeta->InitializeOnSet(NewPoint.MetadataEntry);
			if (MeshPathAttr)
			{
				MeshPathAttr->SetValue(NewPoint.MetadataEntry, Chosen.MeshPath.ToString());
			}

			OutPoints.Add(NewPoint);
			if (MinDist > 0.0f)
			{
				SpacingGrid.Add(ProjPos);
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
	if (RejData && RejData->GetPoints().Num() > 0)
	{
		FPCGTaggedData& T = Outputs.Emplace_GetRef();
		T.Data = RejData;
		T.Pin = RejectedLabel;
	}

	return true;
}