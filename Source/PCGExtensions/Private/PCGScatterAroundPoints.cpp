// PCGScatterAroundPoints.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGScatterAroundPoints.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
#include "Helpers/PCGHelpers.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

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

	const FVector WorldUp(0.0, 0.0, 1.0);
	const int32 BaseSeed = Settings->GetSeed(Context->ExecutionSource.Get());

	for (const FPCGTaggedData& Input : Inputs)
	{
		const UPCGPointData* InputPointData = Cast<UPCGPointData>(Input.Data);
		if (!InputPointData)
		{
			continue;
		}

		const TArray<FPCGPoint>& ParentPoints = InputPointData->GetPoints();
		if (ParentPoints.Num() == 0)
		{
			continue;
		}

		// Create output point data
		UPCGPointData* OutputPointData = NewObject<UPCGPointData>();
		OutputPointData->InitializeFromData(InputPointData);
		TArray<FPCGPoint>& OutputPoints = OutputPointData->GetMutablePoints();
		OutputPoints.Reset();
		OutputPoints.Reserve(ParentPoints.Num() * Settings->PointsPerParent / 2);

		// Track all accepted point positions for global self-pruning
		TArray<FVector> AcceptedPositions;
		AcceptedPositions.Reserve(ParentPoints.Num() * Settings->PointsPerParent / 2);

		// ── For each parent point, scatter children ──

		for (int32 ParentIdx = 0; ParentIdx < ParentPoints.Num(); ++ParentIdx)
		{
			const FPCGPoint& Parent = ParentPoints[ParentIdx];
			const FVector ParentPos = Parent.Transform.GetLocation();

			// Deterministic RNG per parent
			const int32 ParentSeed = PCGHelpers::ComputeSeed(BaseSeed, Parent.Seed + ParentIdx);
			FRandomStream Rng(ParentSeed);

			for (int32 ChildIdx = 0; ChildIdx < Settings->PointsPerParent; ++ChildIdx)
			{
				// ─ 1. Generate random position in a disk around parent ─

				// Random angle
				const float Angle = Rng.FRandRange(0.0f, 2.0f * PI);

				// Random distance with density weighting:
				// Uniform random → sqrt for uniform area distribution → then apply falloff
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

				const float SlopeDot = FVector::DotProduct(SurfaceNormal, WorldUp);

				if (SlopeDot < Settings->MinSlopeDot || SlopeDot > Settings->MaxSlopeDot)
				{
					continue; // Outside slope range
				}

				// ─ 4. Self-pruning ─

				if (MinPruneDist > 0.0f)
				{
					bool bTooClose = false;
					for (const FVector& Existing : AcceptedPositions)
					{
						if (FVector::DistSquared(ProjectedPos, Existing) < MinPruneDistSq)
						{
							bTooClose = true;
							break;
						}
					}
					if (bTooClose)
					{
						continue;
					}
				}

				// ─ 5. Build the output point ─

				FPCGPoint ChildPoint;
				ChildPoint.Seed = PCGHelpers::ComputeSeed(ParentSeed, ChildIdx);
				ChildPoint.Density = 1.0f - FMath::Pow(
					(Distance - InnerRadius) / RadiusRange,
					Settings->FalloffExponent);
				ChildPoint.Density = FMath::Clamp(ChildPoint.Density, 0.0f, 1.0f);

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
					ChildPoint.Transform = FTransform(SurfaceRot, ProjectedPos, FVector::OneVector);
				}
				else
				{
					// ─ FULL TRANSFORM MODE ─
					// Apply rotation, scale, and Z offset directly.
					// Suitable for feeding straight into a Static Mesh Spawner.

					// ─ 6. Compute rotation ─

					FRandomStream RotRng(ChildPoint.Seed + 7919);
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

					FRandomStream ScaleRng(ChildPoint.Seed + 6271);
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

					FRandomStream OffsetRng(ChildPoint.Seed + 4513);
					const float ZOffset = OffsetRng.FRandRange(
						Settings->VerticalOffsetMin, Settings->VerticalOffsetMax);
					const FVector FinalPos = ProjectedPos + FVector(0.0, 0.0, ZOffset);

					// ─ 9. Assemble transform ─

					ChildPoint.Transform = FTransform(FinalRotation, FinalPos, FinalScale);
				}

				// ── Common to both output modes ──

				ChildPoint.SetExtents(FVector(50.0f));
				ChildPoint.Steepness = 1.0f;

				OutputPoints.Add(ChildPoint);
				AcceptedPositions.Add(ProjectedPos);
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