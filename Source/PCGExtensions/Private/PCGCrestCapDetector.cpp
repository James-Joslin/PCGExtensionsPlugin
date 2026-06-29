// PCGCrestCapDetector.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGCrestCapDetector.h"

#include "PCGContext.h"
#include "Data/PCGBasePointData.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(PCGCrestCapDetector)

// ─────────────────────────────────────────────
//  Settings
// ─────────────────────────────────────────────

TArray<FPCGPinProperties> UPCGCrestCapDetectorSettings::InputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultInputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGCrestCapDetector", "InTooltip",
			"Slope rock positions (output from Layer 1A's Static Mesh Spawner). "
			"Each point will be pushed upward and checked for flat ground above."));
	return Pins;
}

TArray<FPCGPinProperties> UPCGCrestCapDetectorSettings::OutputPinProperties() const
{
	TArray<FPCGPinProperties> Pins;
	Pins.Emplace(PCGPinConstants::DefaultOutputLabel,
		EPCGDataType::Point,
		/*bAllowMultipleConnections=*/true,
		/*bAllowMultipleData=*/true,
		NSLOCTEXT("PCGCrestCapDetector", "OutTooltip",
			"Points positioned on flat ground at cliff crests. "
			"Feed into Dynamic Mesh Embed → Transform Points (Absolute Rotation) → Spawner."));
	return Pins;
}

FPCGElementPtr UPCGCrestCapDetectorSettings::CreateElement() const
{
	return MakeShared<FPCGCrestCapDetectorElement>();
}

// ─────────────────────────────────────────────
//  Element — Execution
// ─────────────────────────────────────────────

bool FPCGCrestCapDetectorElement::ExecuteInternal(FPCGContext* Context) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FPCGCrestCapDetectorElement::Execute);

	const UPCGCrestCapDetectorSettings* Settings =
		Context->GetInputSettings<UPCGCrestCapDetectorSettings>();
	check(Settings);

	// ── Get the world for line tracing ──

	UWorld* World = nullptr;
	if (UObject* SourceObj = Context->ExecutionSource.GetObject())
	{
		World = SourceObj->GetWorld();
	}

	if (!World)
	{
		PCGE_LOG(Error, GraphAndLog, NSLOCTEXT("PCGCrestCapDetector", "NoWorld",
			"Crest Cap Detector: Could not get UWorld for line tracing."));
		return true;
	}

	// ── Process each input data set ──

	TArray<FPCGTaggedData> Inputs = Context->InputData.GetAllInputs();
	TArray<FPCGTaggedData>& Outputs = Context->OutputData.TaggedData;

	FCollisionQueryParams QueryParams;
	QueryParams.bTraceComplex = false;
	QueryParams.bReturnPhysicalMaterial = false;

	const FVector WorldUp(0.0, 0.0, 1.0);

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

		const TConstPCGValueRange<FTransform> InTransforms = InputPointData->GetConstTransformValueRange();

		// ─ First pass: run the push + line trace + flatness test, recording survivors ─
		// We keep the input index (for native-prop + metadata copy) plus the computed
		// new transform and density so we can overwrite those properties after copying.

		TArray<int32> KeptReadIndices;
		TArray<FTransform> KeptTransforms;
		TArray<float> KeptDensities;
		KeptReadIndices.Reserve(NumPoints / 3); // Rough estimate -- most points won't be crests
		KeptTransforms.Reserve(NumPoints / 3);
		KeptDensities.Reserve(NumPoints / 3);

		for (int32 PointIdx = 0; PointIdx < NumPoints; ++PointIdx)
		{
			const FTransform& InputTransform = InTransforms[PointIdx];
			const FVector OriginalPos = InputTransform.GetLocation();

			// ─ 1. Get the original surface normal at this slope rock ─
			const FVector SlopeNormal = InputTransform.GetRotation().GetUpVector();

			// ─ 2. Compute push direction ─
			// Push upward in Z, and optionally retract horizontally away from the cliff edge.
			// The horizontal retract direction is the opposite of the slope normal's
			// horizontal component (i.e. away from the cliff face, onto the flat).

			FVector PushOffset = FVector(0.0, 0.0, Settings->VerticalPushDistance);

			// TODO(audit): the horizontal-retract DIRECTION contradicts the header doc.
			// The header says the offset is applied in the OPPOSITE direction of the slope
			// normal's horizontal component, but the code below pushes ALONG it (i.e. in the
			// +HorizontalNormal direction, toward the cliff face).
			if (FMath::Abs(Settings->HorizontalRetractDistance) > KINDA_SMALL_NUMBER)
			{
				// The slope normal's horizontal component points away from the cliff.
				// On a cliff face with normal (0.98, 0, 0.17), horizontal is (0.98, 0, 0).
				// We push in this direction to move the point further onto the flat ground.
				FVector HorizontalNormal(SlopeNormal.X, SlopeNormal.Y, 0.0);
				const float HorizLen = HorizontalNormal.Size();
				if (HorizLen > KINDA_SMALL_NUMBER)
				{
					HorizontalNormal /= HorizLen;
					PushOffset += HorizontalNormal * Settings->HorizontalRetractDistance;
				}
			}

			const FVector PushedPos = OriginalPos + PushOffset;

			// ─ 3. Line trace downward to find the landscape surface ─

			const FVector TraceStart = PushedPos;
			const FVector TraceEnd = PushedPos - FVector(0.0, 0.0, Settings->TraceDistance);

			FHitResult HitResult;
			const bool bHit = World->LineTraceSingleByChannel(
				HitResult,
				TraceStart,
				TraceEnd,
				Settings->TraceChannel,
				QueryParams);

			if (!bHit)
			{
				// No ground found below — skip this point
				continue;
			}

			// ─ 4. Check if the hit surface is flat enough ─

			const FVector& HitNormal = HitResult.ImpactNormal;
			const float Flatness = FVector::DotProduct(HitNormal, WorldUp);

			if (Flatness < Settings->FlatnessThreshold)
			{
				// Still on a slope — not a crest, skip
				continue;
			}

			// ─ 5. This is a crest point! Record the projected transform and density ─

			FTransform CrestTransform = InputTransform; // Preserve scale, etc.

			// Set position to the hit point on the flat ground
			CrestTransform.SetLocation(HitResult.ImpactPoint);

			// Set rotation to match the flat surface normal
			// (so downstream Normal To Density / embed reads the flat normal, not the old slope)
			// TODO(audit): FRotationMatrix::MakeFromZ(HitNormal) discards the point's original
			// yaw -- the forward direction is chosen arbitrarily from Z alone. MakeFromZX (passing
			// the old forward vector as the secondary axis) could preserve the forward heading if
			// desired.
			const FQuat NewRotation = FRotationMatrix::MakeFromZ(HitNormal).ToQuat();
			CrestTransform.SetRotation(NewRotation);

			KeptReadIndices.Add(PointIdx);
			KeptTransforms.Add(CrestTransform);
			// Density based on how flat the surface is (1.0 = perfectly flat)
			KeptDensities.Add(Flatness);
		}

		const int32 KeptCount = KeptReadIndices.Num();

		// Only emit if we found any crest points; allocate the output only once we know.
		if (KeptCount == 0)
		{
			continue;
		}

		// ─ Second pass: build the filtered output ─
		// Allocate, copy all native props + metadata for the kept points, then overwrite
		// only the transform and density that this node changes.

		UPCGBasePointData* OutputPointData = FPCGContext::NewPointData_AnyThread(Context);
		OutputPointData->InitializeFromData(InputPointData);
		OutputPointData->SetNumPoints(KeptCount);

		TArray<int32> WriteIndices;
		WriteIndices.SetNumUninitialized(KeptCount);
		for (int32 w = 0; w < KeptCount; ++w)
		{
			WriteIndices[w] = w;
		}

		InputPointData->CopyPointsTo(OutputPointData, KeptReadIndices, WriteIndices);

		TPCGValueRange<FTransform> OutT = OutputPointData->GetTransformValueRange();
		TPCGValueRange<float> OutD = OutputPointData->GetDensityValueRange();
		for (int32 w = 0; w < KeptCount; ++w)
		{
			OutT[w] = KeptTransforms[w];
			OutD[w] = KeptDensities[w];
		}

		FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
		Output.Data = OutputPointData;
	}

	return true;
}