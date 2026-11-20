// TITAN Studio Bridge: Unreal-rendered viewport frames for TITAN Studio. Original TITAN code.
//
// Thread ownership:
//   * Game thread: render target, capture actor, CaptureScene(), camera, draining completed frames.
//   * Render thread: GPU readbacks (FRHIGPUTextureReadback) and copying pixels into shared memory.
//   * The two sides share only FSharedState (thread-safe queue + atomics) and the mapped memory, whose
//     lifetime is ended only after FlushRenderingCommands().
#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "TitanCoordinates.h"
#include "TitanFrameChannel.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtr.h"

class ASceneCapture2D;
class UTextureRenderTarget2D;
class UWorld;

namespace TitanBridge
{
struct FCompletedFrame
{
	uint64 Frame = 0;
	uint32 Slot = 0;
	uint32 Width = 0;
	uint32 Height = 0;
	uint32 CameraRev = 0;
	double CaptureToReadyMs = 0;
};

struct FCameraState
{
	TitanCoords::V3 Eye{0, 1.6, 6};
	TitanCoords::V3 Forward{0, -0.2, -1};
	double FovYDeg = 50.0;
	float View[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	uint32 Revision = 0;
};

class FFrameChannel
{
public:
	FFrameChannel();
	~FFrameChannel();

	// (Re)creates the shared memory and render target. Returns false with OutError on failure.
	bool Configure(uint32 Width, uint32 Height, FString& OutError);
	void Release();

	void SetCamera(const FCameraState& Camera);
	void SetContinuous(bool bOn) { bContinuous = bOn; }
	// Request frames: after a scene or camera change, keep capturing for a while so Unreal's temporal
	// effects (TSR, Lumen, VSM caching) converge.
	void Invalidate(int32 SettleFrames = 45);
	void OnWorldChanged();

	// Game thread, once per engine tick.
	void Tick(UWorld* World);
	void DrainCompleted(TArray<FCompletedFrame>& Out);

	bool IsConfigured() const { return Header != nullptr; }
	const FString& ShmName() const { return Name; }
	uint32 Width() const { return W; }
	uint32 Height() const { return H; }
	bool IsContinuous() const { return bContinuous; }

	struct FStats
	{
		uint64 Captured = 0;
		uint64 Delivered = 0;
		uint64 DroppedBusy = 0;
		double AvgCaptureToReadyMs = 0;
		double LastCaptureToReadyMs = 0;
	};
	FStats GetStats() const;

private:
	struct FSharedState;
	bool EnsureCaptureActor(UWorld* World);
	void ApplyCamera();

	FString Name;
	TitanFrameChannelHeader* Header = nullptr;
	uint64 MappedBytes = 0;
	uint32 W = 0, H = 0;
	TStrongObjectPtr<UTextureRenderTarget2D> Target;
	TWeakObjectPtr<ASceneCapture2D> CaptureActor;
	TWeakObjectPtr<UWorld> CaptureWorld;
	FCameraState Camera;
	bool bContinuous = false;
	int32 SettleRemaining = 0;
	uint64 NextFrame = 1;
	TSharedRef<FSharedState, ESPMode::ThreadSafe> Shared;
	FStats Stats;
};
}  // namespace TitanBridge
