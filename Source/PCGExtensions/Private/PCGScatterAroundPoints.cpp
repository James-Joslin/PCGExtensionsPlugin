// PCGScatterAroundPoints.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGScatterAroundPoints.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

#include "PCGExtScatterCommon.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGScatterAroundPoints)

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGScatterAroundPointsSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGScatterAroundPoints", "InTooltip",
			"Parent points to scatter around (e.g. output from a large rock spawner). "
			"Child points will be generated in a disk around each parent."));
	return Pins;
}

TArray<FPCGPinProperties> UPCGScatterAroundPointsSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGScatterAroundPoints", "OutTooltip",
			"Scattered child points projected onto the landscape, filtered, pruned, "
			"and randomised. Ready for a Static Mesh Spawner or Dynamic Mesh Embed."));
	return Pins;
}

FPCGElementPtr UPCGScatterAroundPointsSettings::CreateElement() const
{
	return MakeShared<FPCGScatterAroundPointsElement>();
}

// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

bool FPCGScatterAroundPointsElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGScatterAroundPointsElement::Execute);

	const UPCGScatterAroundPointsSettings* Settings =
		Context->GetInputSettings<UPCGScatterAroundPointsSettings>();
	check(Settings);

	// ── Get the world for line tracing ──

	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}

	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGScatterAroundPoints", "NoWorld",
			"Scatter Around Points: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Validate settings ──

	const float InnerRadius = FMath::Max(Settings->InnerRadius, 0.0f);
	const float OuterRadius = FMath::Max(Settings->OuterRadius, InnerRadius + 10.0f);
	const float RadiusRange = OuterRadius - InnerRadius;
	const float MinPruneDist = Settings->MinDistanceBetweenPoints;
	const float MinPruneDistSq = MinPruneDist * MinPruneDist;

	// ── Process each input data set ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGBasePointData* InputPointData = Cast<UPCGBasePointData>(Input.Data);
		if (!InputPointData)
		{
			continue;
		}

		const int32 NumParents = InputPointData->GetNumPoints();
		if (NumParents == 0)
		{
			continue;
		}

		const TConstPCGValueRange<FTransform> ParentTransforms = InputPointData->GetConstTransformValueRange();
		const TConstPCGValueRange<int32> ParentSeeds = InputPointData->GetConstSeedValueRange();

		// Accumulate accepted children into POD arrays during the trace/filter loop;
		// the output point data is sized and filled only after the loop (the number of
		// survivors isn't known up front).
		const int32 ReserveHint = NumParents * Settings->PointsPerParent / 2;
		TArray<FTransform> AcceptedTransforms;
		TArray<int32> AcceptedSeeds;
		TArray<float> AcceptedDensities;
		AcceptedTransforms.Reserve(ReserveHint);
		AcceptedSeeds.Reserve(ReserveHint);
		AcceptedDensities.Reserve(ReserveHint);

		// XY hash grid for global self-pruning (replaces the former O(n^2) linear scan).
		PCGExtScatterCommon::FMinDistGrid PruneGrid;
		PruneGrid.CellSize = FMath::Max(MinPruneDist, 1.0f);

		// ── For each parent point, scatter children ──

		for (int32 ParentIdx = 0; ParentIdx < NumParents; ++ParentIdx)
		{
			const FVector ParentPos = ParentTransforms[ParentIdx].GetLocation();

			// Deterministic RNG per parent
			const int32 ParentSeed = PCGHelpers::ComputeSeed(BaseSeed, ParentSeeds[ParentIdx] + ParentIdx);
			FRandomStream Rng(ParentSeed);

			for (int32 ChildIdx = 0; ChildIdx < Settings->PointsPerParent; ++ChildIdx)
			{
				// ─ 1. Generate random position in a disk around parent ─

				// Random angle
				const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);

				// Random distance with density weighting:
				// Uniform random → sqrt for uniform area distribution → then apply falloff
				// TODO(audit): the comment claims sqrt(U) area-uniform sampling, but the code
				// below never applies sqrt() to T → the radial distribution is biased toward the
				// centre independently of the falloff. Distribution left unchanged, you probably 
				// want to fix this.
				const float T = Rng.FRandRange(0.0f, 1.0f);

				// Density falloff: probability decreases with distance
				// We use inverse CDF sampling: closer distances are more likely
				const float FalloffT = 1.0f - FMath::Pow(T, 1.0f / Settings->FalloffExponent);
				const float Distance = InnerRadius + FalloffT * RadiusRange;

				const float OffsetX = FMath::Cos(Angle) * Distance;
				const float OffsetY = FMath::Sin(Angle) * Distance;

				const FVector ScatteredXY(
					ParentPos.X + OffsetX,
					ParentPos.Y + OffsetY,
					ParentPos.Z);

				// ─ 2. Project onto landscape via line trace ─

				const FVector TraceStart(ScatteredXY.X, ScatteredXY.Y,
					ParentPos.Z + Settings->TraceStartHeight);
				const FVector TraceEnd(ScatteredXY.X, ScatteredXY.Y,
					ParentPos.Z - Settings->TraceDistance);

				FHitResult HitResult;
				const bool bHit = World->LineTraceSingleByChannel(
					HitResult, TraceStart, TraceEnd,
					Settings->TraceChannel, QueryParams);

				if (!bHit)
				{
					continue; // No ground found
				}

				const FVector ProjectedPos = HitResult.ImpactPoint;
				const FVector SurfaceNormal = HitResult.ImpactNormal;

				// ─ 3. Slope filter ─

				const float SlopeDot = FVector::DotProduct(SurfaceNormal, PCGExtScatterCommon::WorldUp);

				if (SlopeDot < Settings->MinSlopeDot || SlopeDot > Settings->MaxSlopeDot)
				{
					continue; // Outside slope range
				}

				// ─ 4. Self-pruning ─

				if (MinPruneDist > 0.0f && PruneGrid.HasWithin(ProjectedPos, MinPruneDistSq))
				{
					continue;
				}

				// ─ 5. Build the child (accumulated into POD arrays) ─

				const int32 ChildSeed = PCGHelpers::ComputeSeed(ParentSeed, ChildIdx);

				// TODO(audit): the falloff is double-counted -- it already thins the child *count*
				// via the inverse-CDF distance sampling above, and here it attenuates per-child
				// *density* as well. Edge children are therefore both rarer and fainter. 
				// Left unchanged (this is the historical behavior).
				float ChildDensity = 1.0f - FMath::Pow(
					(Distance - InnerRadius) / RadiusRange,
					Settings->FalloffExponent);
				ChildDensity = FMath::Clamp(ChildDensity, 0.0f, 1.0f);

				FTransform ChildTransform;

				if (Settings->bProjectionOutputForEmbed)
				{
					// ─ RAW PROJECTION MODE ─
					// Output clean projected data for downstream Dynamic Mesh Embed.
					// Position = on the landscape surface, no offset.
					// Rotation = surface normal orientation, no randomisation.
					// Scale = (1,1,1), no randomisation.
					// The embed node will read the true surface normal from GetUpVector()
					// and compute the correct embed distance.
					// Apply rotation, scale, and Z offset via a Transform Points node
					// AFTER the Dynamic Mesh Embed.

					const FQuat SurfaceRot = FRotationMatrix::MakeFromZ(SurfaceNormal).ToQuat();
					ChildTransform = FTransform(SurfaceRot, ProjectedPos, FVector::OneVector);
				}
				else
				{
					// ─ FULL TRANSFORM MODE ─
					// Apply rotation, scale, and Z offset directly.
					// Suitable for feeding straight into a Static Mesh Spawner.

					// ─ 6. Compute rotation ─

					FRandomStream RotRng(ChildSeed + 7919);
					const FRotator RandomRot(
						RotRng.FRandRange(Settings->RotationMin.Pitch, Settings->RotationMax.Pitch),
						RotRng.FRandRange(Settings->RotationMin.Yaw, Settings->RotationMax.Yaw),
						RotRng.FRandRange(Settings->RotationMin.Roll, Settings->RotationMax.Roll));

					FQuat FinalRotation;
					if (Settings->bAbsoluteRotation)
					{
						FinalRotation = RandomRot.Quaternion();
					}
					else
					{
						const FQuat SurfaceRot = FRotationMatrix::MakeFromZ(SurfaceNormal).ToQuat();
						FinalRotation = SurfaceRot * RandomRot.Quaternion();
					}

					// ─ 7. Compute scale ─

					FRandomStream ScaleRng(ChildSeed + 6271);
					FVector FinalScale;
					if (Settings->bUniformScale)
					{
						const float S = ScaleRng.FRandRange(Settings->ScaleMin.X, Settings->ScaleMax.X);
						FinalScale = FVector(S);
					}
					else
					{
						FinalScale.X = ScaleRng.FRandRange(Settings->ScaleMin.X, Settings->ScaleMax.X);
						FinalScale.Y = ScaleRng.FRandRange(Settings->ScaleMin.Y, Settings->ScaleMax.Y);
						FinalScale.Z = ScaleRng.FRandRange(Settings->ScaleMin.Z, Settings->ScaleMax.Z);
					}

					// ─ 8. Apply vertical offset ─

					FRandomStream OffsetRng(ChildSeed + 4513);
					const float ZOffset = OffsetRng.FRandRange(
						Settings->VerticalOffsetMin, Settings->VerticalOffsetMax);
					const FVector FinalPos = ProjectedPos + FVector(0.0, 0.0, ZOffset);

					// ─ 9. Assemble transform ─

					ChildTransform = FTransform(FinalRotation, FinalPos, FinalScale);
				}

				// ── Accept: record in POD arrays + register for self-pruning ──

				AcceptedTransforms.Add(ChildTransform);
				AcceptedSeeds.Add(ChildSeed);
				AcceptedDensities.Add(ChildDensity);
				PruneGrid.Add(ProjectedPos);
			}
		}

		// ── Materialize the accepted children into a fresh point data set ──

		const int32 NumChildren = AcceptedTransforms.Num();
		if (NumChildren == 0)
		{
			continue;
		}

		UPCGBasePointData* OutputPointData = FPCGContext::NewPointData_AnyThread(Context);
		OutputPointData->InitializeFromData(InputPointData);
		OutputPointData->SetNumPoints(NumChildren, /*bInitializeValues=*/false);
		OutputPointData->AllocateProperties(
			EPCGPointNativeProperties::Transform |
			EPCGPointNativeProperties::Seed |
			EPCGPointNativeProperties::Density |
			EPCGPointNativeProperties::Steepness |
			EPCGPointNativeProperties::BoundsMin |
			EPCGPointNativeProperties::BoundsMax);

		// Ranges must be fetched AFTER SetNumPoints/AllocateProperties (those invalidate them).
		TPCGValueRange<FTransform> OutTransforms = OutputPointData->GetTransformValueRange(/*bAllocate=*/false);
		TPCGValueRange<int32> OutSeeds = OutputPointData->GetSeedValueRange(/*bAllocate=*/false);
		TPCGValueRange<float> OutDensities = OutputPointData->GetDensityValueRange(/*bAllocate=*/false);
		TPCGValueRange<float> OutSteepness = OutputPointData->GetSteepnessValueRange(/*bAllocate=*/false);
		TPCGValueRange<FVector> OutBoundsMin = OutputPointData->GetBoundsMinValueRange(/*bAllocate=*/false);
		TPCGValueRange<FVector> OutBoundsMax = OutputPointData->GetBoundsMaxValueRange(/*bAllocate=*/false);

		for (int32 ChildIdx = 0; ChildIdx < NumChildren; ++ChildIdx)
		{
			OutTransforms[ChildIdx] = AcceptedTransforms[ChildIdx];
			OutSeeds[ChildIdx] = AcceptedSeeds[ChildIdx];
			OutDensities[ChildIdx] = AcceptedDensities[ChildIdx];
			OutSteepness[ChildIdx] = 1.0f;
			// Former FPCGPoint::SetExtents(FVector(50)) → symmetric ±50 bounds.
			OutBoundsMin[ChildIdx] = FVector(-50.0);
			OutBoundsMax[ChildIdx] = FVector(50.0);
		}

		FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}