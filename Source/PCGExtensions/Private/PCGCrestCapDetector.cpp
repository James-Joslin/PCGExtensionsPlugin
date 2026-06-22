// PCGCrestCapDetector.cpp
//
// Place in your project's Source/<Module>/Private/ directory.

#include "PCGCrestCapDetector.h"

#include "PCGContext.h"
#include "PCGPoint.h"
#include "Data/PCGPointData.h"
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

		// Create output — we'll selectively add only crest points
		UPCGPointData* OutputPointData = NewObject<UPCGPointData>();
		OutputPointData->InitializeFromData(InputPointData);
		TArray<FPCGPoint>& OutputPoints = OutputPointData->GetMutablePoints();
		OutputPoints.Reset(); // Start empty, add only surviving points
		OutputPoints.Reserve(InputPoints.Num() / 3); // Rough estimate — most points won't be crests

		for (const FPCGPoint& InputPoint : InputPoints)
		{
			const FVector OriginalPos = InputPoint.Transform.GetLocation();

			// ─ 1. Get the original surface normal at this slope rock ─
			const FVector SlopeNormal = InputPoint.Transform.GetRotation().GetUpVector();

			// ─ 2. Compute push direction ─
			// Push upward in Z, and optionally retract horizontally away from the cliff edge.
			// The horizontal retract direction is the opposite of the slope normal's
			// horizontal component (i.e. away from the cliff face, onto the flat).

			FVector PushOffset = FVector(0.0, 0.0, Settings->VerticalPushDistance);

			if (Settings->HorizontalRetractDistance > 0.0f)
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

			// ─ 5. This is a crest point! Create output point at the projected position ─

			FPCGPoint CrestPoint = InputPoint; // Copy attributes, seed, etc.

			// Set position to the hit point on the flat ground
			CrestPoint.Transform.SetLocation(HitResult.ImpactPoint);

			// Set rotation to match the flat surface normal
			// (so downstream Normal To Density / embed reads the flat normal, not the old slope)
			const FQuat NewRotation = FRotationMatrix::MakeFromZ(HitNormal).ToQuat();
			CrestPoint.Transform.SetRotation(NewRotation);

			// Update density based on how flat the surface is (1.0 = perfectly flat)
			CrestPoint.Density = Flatness;

			OutputPoints.Add(CrestPoint);
		}

		// Only emit if we found any crest points
		if (OutputPoints.Num() > 0)
		{
			FPCGTaggedData& Output = Outputs.Add_GetRef(Input);
			Output.Data = OutputPointData;
		}
	}

	return true;
}