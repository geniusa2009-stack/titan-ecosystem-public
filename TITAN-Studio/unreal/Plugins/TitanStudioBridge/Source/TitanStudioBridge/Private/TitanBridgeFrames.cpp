// TITAN Studio Bridge: Unreal-rendered viewport frames for TITAN Studio. Original TITAN code.
#include "TitanBridgeFrames.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "TitanBridgeLog.h"
#include "UObject/Package.h"

#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace TitanBridge
{
namespace
{
uint64 NowNs() { return uint64(FPlatformTime::ToSeconds64(FPlatformTime::Cycles64()) * 1e9); }
uint64 RoundUp(uint64 V, uint64 A) { return (V + A - 1) / A * A; }
}  // namespace

// Everything the render thread touches. Only the render thread reads/writes InFlight and Pool.
struct FFrameChannel::FSharedState
{
	struct FInFlight
	{
		TUniquePtr<FRHIGPUTextureReadback> Readback;
		uint64 Frame = 0;
		uint32 Width = 0, Height = 0, CameraRev = 0;
		uint64 CaptureNs = 0;
		float View[16];
	};
	TArray<FInFlight> InFlight;
	TArray<TUniquePtr<FRHIGPUTextureReadback>> Pool;
	TitanFrameChannelHeader* Header = nullptr;  // set on the game thread only while the RT is flushed
	uint32 NextWriteSlot = 0;
	std::atomic<int32> InFlightCount{0};
	TQueue<FCompletedFrame, EQueueMode::Spsc> Completed;

	void PollRenderThread()
	{
		check(IsInRenderingThread());
		while (InFlight.Num() > 0 && InFlight[0].Readback->IsReady())
		{
			FInFlight Item = MoveTemp(InFlight[0]);
			InFlight.RemoveAt(0);
			InFlightCount.fetch_sub(1);
			int32 PitchPixels = 0, BufferHeight = 0;
			const uint8* Src = static_cast<const uint8*>(Item.Readback->Lock(PitchPixels, &BufferHeight));
			// FRHIGPUTextureReadback keeps its staging texture across reuse ("assume ... same format and size"),
			// so never read more than the locked buffer actually holds.
			const bool bBufferFits = PitchPixels >= int32(Item.Width) && BufferHeight >= int32(Item.Height);
			if (!bBufferFits)
			{
				Item.Readback->Unlock();
				continue;  // drop the frame and the mismatched readback (not returned to the pool)
			}
			if (Src && Header && Item.Width <= Header->max_width && Item.Height <= Header->max_height)
			{
				uint32 Slot = NextWriteSlot % TITAN_FRAME_SLOTS;
				if (Slot == Header->latest_slot) Slot = (Slot + 1) % TITAN_FRAME_SLOTS;
				NextWriteSlot = Slot + 1;
				TitanFrameSlot& S = Header->ring[Slot];
				uint8* Dst = reinterpret_cast<uint8*>(Header) + Header->header_bytes + uint64(Slot) * Header->slot_bytes;
				const uint32 Stride = Item.Width * 4;
				__atomic_store_n(&S.seq, Item.Frame * 2 - 1, __ATOMIC_RELAXED);  // odd: writing
				// Seqlock write side: the odd seq must be visible before any pixel of the new frame. A release
				// store only orders the writes before it, so a reader on ARM64 could otherwise see new pixels
				// with the old (even) seq and accept a torn frame.
				__atomic_thread_fence(__ATOMIC_RELEASE);
				for (uint32 Row = 0; Row < Item.Height; ++Row)
					FMemory::Memcpy(Dst + uint64(Row) * Stride, Src + uint64(Row) * uint64(PitchPixels) * 4, Stride);
				S.frame = Item.Frame;
				S.width = Item.Width;
				S.height = Item.Height;
				S.stride = Stride;
				S.format = TITAN_FRAME_FORMAT_BGRA8_SRGB;
				S.capture_ns = Item.CaptureNs;
				S.ready_ns = NowNs();
				S.gpu_ms = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
				FMemory::Memcpy(S.view, Item.View, sizeof(S.view));
				S.camera_rev = Item.CameraRev;
				__atomic_store_n(&S.seq, Item.Frame * 2, __ATOMIC_RELEASE);  // even: complete
				__atomic_store_n(&Header->latest_slot, Slot, __ATOMIC_RELEASE);
				__atomic_store_n(&Header->latest_frame, Item.Frame, __ATOMIC_RELEASE);
				FCompletedFrame Done;
				Done.Frame = Item.Frame;
				Done.Slot = Slot;
				Done.Width = Item.Width;
				Done.Height = Item.Height;
				Done.CameraRev = Item.CameraRev;
				Done.CaptureToReadyMs = double(S.ready_ns - Item.CaptureNs) / 1e6;
				Completed.Enqueue(Done);
			}
			Item.Readback->Unlock();
			Pool.Add(MoveTemp(Item.Readback));
		}
	}

	void ResetRenderThread()
	{
		check(IsInRenderingThread());
		InFlight.Reset();
		// Pooled readbacks keep a staging texture of the previous channel size, and UE 5.8.4 reuses it as is
		// (RHIGPUReadback.cpp: "Assume for now that every enqueue happens on a texture of the same format and
		// size"). Reusing one after a resize made PollRenderThread read past the staging buffer: SIGSEGV in
		// memmove on the render thread after viewport.configure 320x16 -> 802x828 (sessions 20261109T1422-1424).
		Pool.Reset();
		InFlightCount.store(0);
	}
};

FFrameChannel::FFrameChannel() : Shared(MakeShared<FSharedState, ESPMode::ThreadSafe>()) {}

FFrameChannel::~FFrameChannel() { Release(); }

void FFrameChannel::Release()
{
	// The render thread may still own readbacks that point at the mapping: drain it first.
	TSharedRef<FSharedState, ESPMode::ThreadSafe> S = Shared;
	ENQUEUE_RENDER_COMMAND(TitanFramesReset)([S](FRHICommandListImmediate&) { S->ResetRenderThread(); });
	FlushRenderingCommands();
	Shared->Header = nullptr;
	FCompletedFrame Ignored;
	while (Shared->Completed.Dequeue(Ignored)) {}
	if (Header)
	{
		munmap(Header, MappedBytes);
		shm_unlink(TCHAR_TO_UTF8(*Name));
	}
	Header = nullptr;
	MappedBytes = 0;
	if (ASceneCapture2D* A = CaptureActor.Get())
		if (UWorld* World = A->GetWorld())
		{
			UPackage* Package = World->GetOutermost();
			const bool bWasDirty = Package->IsDirty();
			World->DestroyActor(A);
			if (!bWasDirty) Package->SetDirtyFlag(false);
		}
	CaptureActor.Reset();
	CaptureWorld.Reset();
	Target.Reset();
	W = H = 0;
}

bool FFrameChannel::Configure(uint32 Width, uint32 Height, FString& OutError)
{
	if (Width < 16 || Height < 16 || Width > 8192 || Height > 8192)
	{
		OutError = TEXT("width and height must be 16..8192");
		return false;
	}
	if (Header && Width == W && Height == H) return true;
	Release();

	static uint32 Counter = 0;
	Name = FString::Printf(TEXT("/titan-%d-%u"), FPlatformProcess::GetCurrentProcessId(), ++Counter);  // < 31 chars
	const uint64 SlotBytes = RoundUp(uint64(Width) * Height * 4, 16384);
	MappedBytes = TITAN_FRAME_HEADER_BYTES + SlotBytes * TITAN_FRAME_SLOTS;
	const FTCHARToUTF8 ShmUtf8(*Name);
	const int Fd = shm_open(ShmUtf8.Get(), O_CREAT | O_EXCL | O_RDWR, 0600);
	if (Fd < 0)
	{
		OutError = FString::Printf(TEXT("shm_open failed (errno %d)"), errno);
		return false;
	}
	void* Mem = MAP_FAILED;
	if (ftruncate(Fd, off_t(MappedBytes)) == 0) Mem = mmap(nullptr, MappedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, Fd, 0);
	close(Fd);
	if (Mem == MAP_FAILED)
	{
		shm_unlink(ShmUtf8.Get());
		OutError = FString::Printf(TEXT("cannot map %llu bytes of shared memory (errno %d)"), MappedBytes, errno);
		return false;
	}
	Header = static_cast<TitanFrameChannelHeader*>(Mem);
	FMemory::Memzero(Header, sizeof(TitanFrameChannelHeader));
	Header->magic = TITAN_FRAME_MAGIC;
	Header->version = TITAN_FRAME_VERSION;
	Header->slot_count = TITAN_FRAME_SLOTS;
	Header->max_width = Width;
	Header->max_height = Height;
	Header->header_bytes = TITAN_FRAME_HEADER_BYTES;
	Header->slot_bytes = SlotBytes;
	Header->latest_slot = UINT32_MAX;
	Header->writer_pid = uint32(FPlatformProcess::GetCurrentProcessId());
	Shared->Header = Header;  // render thread is idle (flushed in Release)
	W = Width;
	H = Height;

	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
	RT->RenderTargetFormat = RTF_RGBA8_SRGB;
	RT->ClearColor = FLinearColor::Black;
	RT->InitCustomFormat(Width, Height, PF_B8G8R8A8, /*bInForceLinearGamma*/ false);
	RT->UpdateResourceImmediate(true);
	Target.Reset(RT);
	Invalidate();
	UE_LOG(LogTitanBridge, Log, TEXT("Frame channel %s: %ux%u, %llu bytes"), *Name, Width, Height, MappedBytes);
	return true;
}

void FFrameChannel::SetCamera(const FCameraState& InCamera)
{
	Camera = InCamera;
	ApplyCamera();
	Invalidate();
}

void FFrameChannel::Invalidate(int32 SettleFrames) { SettleRemaining = FMath::Max(SettleRemaining, SettleFrames); }

void FFrameChannel::OnWorldChanged()
{
	CaptureActor.Reset();
	CaptureWorld.Reset();
	Invalidate();
}

bool FFrameChannel::EnsureCaptureActor(UWorld* World)
{
	if (CaptureActor.IsValid() && CaptureWorld.Get() == World) return true;
	UPackage* Package = World->GetOutermost();
	const bool bWasDirty = Package->IsDirty();
	FActorSpawnParameters Params;
	Params.ObjectFlags = RF_Transient;
	Params.bTemporaryEditorActor = true;
	Params.bHideFromSceneOutliner = true;
	Params.bNoFail = true;
	ASceneCapture2D* A = World->SpawnActor<ASceneCapture2D>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (!bWasDirty) Package->SetDirtyFlag(false);  // the transient view is not a user edit
	if (!A) return false;
	A->bIsEditorOnlyActor = true;
	USceneCaptureComponent2D* C = A->GetCaptureComponent2D();
	C->bCaptureEveryFrame = false;
	C->bCaptureOnMovement = false;
	C->bAlwaysPersistRenderingState = true;  // keep TSR / Lumen / VSM history between captures
	C->CaptureSource = SCS_FinalColorLDR;
	C->TextureTarget = Target.Get();
	CaptureActor = A;
	CaptureWorld = World;
	ApplyCamera();
	return true;
}

void FFrameChannel::ApplyCamera()
{
	ASceneCapture2D* A = CaptureActor.Get();
	if (!A || W == 0) return;
	TitanCoords::V3 Loc;
	double Pitch = 0, Yaw = 0;
	TitanCoords::TitanCameraToUe(Camera.Eye, Camera.Forward, Loc, Pitch, Yaw);
	A->SetActorLocationAndRotation(FVector(Loc.X, Loc.Y, Loc.Z), FRotator(Pitch, Yaw, 0.0));
	A->GetCaptureComponent2D()->FOVAngle = float(TitanCoords::HorizontalFovDeg(Camera.FovYDeg, double(W) / double(H)));
}

void FFrameChannel::Tick(UWorld* World)
{
	if (Header)
	{
		TSharedRef<FSharedState, ESPMode::ThreadSafe> S = Shared;
		ENQUEUE_RENDER_COMMAND(TitanFramesPoll)([S](FRHICommandListImmediate&) { S->PollRenderThread(); });
	}
	if (!Header || !World || !Target.IsValid()) return;
	if (!bContinuous && SettleRemaining <= 0) return;
	if (!EnsureCaptureActor(World)) return;
	if (Shared->InFlightCount.load() >= 2)  // GPU or readback is behind; don't queue more work
	{
		Stats.DroppedBusy++;
		return;
	}
	USceneCaptureComponent2D* C = CaptureActor->GetCaptureComponent2D();
	C->CaptureScene();
	SettleRemaining = FMath::Max(0, SettleRemaining - 1);
	Stats.Captured++;

	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	const uint64 Frame = NextFrame++;
	const uint32 FW = W, FH = H, Rev = Camera.Revision;
	const uint64 CaptureNs = NowNs();
	float View[16];
	FMemory::Memcpy(View, Camera.View, sizeof(View));
	Shared->InFlightCount.fetch_add(1);
	TSharedRef<FSharedState, ESPMode::ThreadSafe> S = Shared;
	ENQUEUE_RENDER_COMMAND(TitanFramesReadback)(
		[S, Resource, Frame, FW, FH, Rev, CaptureNs, View](FRHICommandListImmediate& RHICmdList)
		{
			FSharedState::FInFlight Item;
			if (S->Pool.Num() > 0) Item.Readback = S->Pool.Pop();
			else Item.Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("TitanStudioFrame"));
			Item.Readback->EnqueueCopy(RHICmdList, Resource->GetRenderTargetTexture(), FIntVector(0, 0, 0), 0, FIntVector(FW, FH, 1));
			Item.Frame = Frame;
			Item.Width = FW;
			Item.Height = FH;
			Item.CameraRev = Rev;
			Item.CaptureNs = CaptureNs;
			FMemory::Memcpy(Item.View, View, sizeof(View));
			S->InFlight.Add(MoveTemp(Item));
		});
}

void FFrameChannel::DrainCompleted(TArray<FCompletedFrame>& Out)
{
	FCompletedFrame F;
	while (Shared->Completed.Dequeue(F))
	{
		Stats.Delivered++;
		Stats.LastCaptureToReadyMs = F.CaptureToReadyMs;
		Stats.AvgCaptureToReadyMs = Stats.Delivered == 1 ? F.CaptureToReadyMs
														: Stats.AvgCaptureToReadyMs * 0.9 + F.CaptureToReadyMs * 0.1;
		Out.Add(F);
	}
}

FFrameChannel::FStats FFrameChannel::GetStats() const { return Stats; }
}  // namespace TitanBridge
