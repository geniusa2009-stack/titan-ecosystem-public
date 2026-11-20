// Engine-side automation tests for the TITAN Studio Bridge. Original TITAN code.
// Run: UnrealEditor TitanHost.uproject -ExecCmds="Automation RunTests TITAN; Quit" -unattended -nosplash
#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "TitanCoordinates.h"
#include "TitanFrameChannel.h"

#if WITH_DEV_AUTOMATION_TESTS

// Our plain-C++ rotator convention must match Unreal's FRotationMatrix / FMatrix::Rotator exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTitanCoordinatesMatchUnreal, "TITAN.Bridge.CoordinatesMatchUnreal",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTitanCoordinatesMatchUnreal::RunTest(const FString&)
{
	FRandomStream Rng(0x7174A9);
	double MaxMatrixError = 0, MaxRoundTripError = 0;
	for (int32 I = 0; I < 2000; ++I)
	{
		const FRotator R(Rng.FRandRange(-89.0, 89.0), Rng.FRandRange(-180.0, 180.0), Rng.FRandRange(-180.0, 180.0));
		const FMatrix UE = FRotationMatrix(R);
		const TitanCoords::M3 Ours = TitanCoords::UeRotatorToMatrix(R.Pitch, R.Yaw, R.Roll);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FVector A = UE.GetScaledAxis(EAxis::Type(Axis + 1));  // X, Y, Z
			MaxMatrixError = FMath::Max(MaxMatrixError, FMath::Abs(A.X - Ours.M[0][Axis]));
			MaxMatrixError = FMath::Max(MaxMatrixError, FMath::Abs(A.Y - Ours.M[1][Axis]));
			MaxMatrixError = FMath::Max(MaxMatrixError, FMath::Abs(A.Z - Ours.M[2][Axis]));
		}
		// UE -> TITAN -> UE must describe the same orientation (compare as quaternions).
		const TitanCoords::V3 E = TitanCoords::UeRotatorToTitanEuler(R.Pitch, R.Yaw, R.Roll);
		double P = 0, Y = 0, Rl = 0;
		TitanCoords::TitanEulerToUeRotator(E, P, Y, Rl);
		const double Dot = FMath::Abs(R.Quaternion() | FRotator(P, Y, Rl).Quaternion());
		MaxRoundTripError = FMath::Max(MaxRoundTripError, 1.0 - Dot);
	}
	TestTrue(FString::Printf(TEXT("rotator matrix matches FRotationMatrix (max error %g)"), MaxMatrixError), MaxMatrixError < 1e-9);
	TestTrue(FString::Printf(TEXT("UE->TITAN->UE orientation round trip (max 1-|q.q| %g)"), MaxRoundTripError), MaxRoundTripError < 1e-9);

	// Positions: 1 TITAN metre right/up/back = (0, 100, 0) / (0, 0, 100) / (-100, 0, 0) cm.
	const TitanCoords::V3 U = TitanCoords::TitanToUePosition({1.5, 2.5, 3.5});
	TestEqual(TEXT("TITAN->UE position"), FVector(U.X, U.Y, U.Z), FVector(-350.0, 150.0, 250.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTitanFrameChannelLayout, "TITAN.Bridge.FrameChannelLayout",
								 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTitanFrameChannelLayout::RunTest(const FString&)
{
	TestTrue(TEXT("header fits in the reserved page"), sizeof(TitanFrameChannelHeader) <= TITAN_FRAME_HEADER_BYTES);
	TestEqual(TEXT("slot size"), int32(sizeof(TitanFrameSlot)), 160);
	return true;
}

#endif
