#include "TitanLevelStreamingBridgeComponent.h"
#include "TitanWorldOrchestratorComponent.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"

UTitanLevelStreamingBridgeComponent::UTitanLevelStreamingBridgeComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}

void UTitanLevelStreamingBridgeComponent::BeginPlay()
{
    Super::BeginPlay();
    Telemetry.Reset();
    if (!Orchestrator && GetOwner())
    {
        Orchestrator = GetOwner()->FindComponentByClass<UTitanWorldOrchestratorComponent>();
    }
    if (!Orchestrator)
    {
        UE_LOG(LogTemp, Warning, TEXT("TITAN: Level streaming bridge has no orchestrator."));
        SetComponentTickEnabled(false);
        return;
    }
    Orchestrator->OnLoadRequested.AddDynamic(this, &UTitanLevelStreamingBridgeComponent::HandleLoadRequested);
    Orchestrator->OnUnloadRequested.AddDynamic(this, &UTitanLevelStreamingBridgeComponent::HandleUnloadRequested);
}

void UTitanLevelStreamingBridgeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (Orchestrator)
    {
        Orchestrator->OnLoadRequested.RemoveDynamic(this, &UTitanLevelStreamingBridgeComponent::HandleLoadRequested);
        Orchestrator->OnUnloadRequested.RemoveDynamic(this, &UTitanLevelStreamingBridgeComponent::HandleUnloadRequested);
    }
    Pending.Empty();
    InFlightOperations = 0;
    Super::EndPlay(EndPlayReason);
}

FString UTitanLevelStreamingBridgeComponent::CoordinateToken(int32 Coordinate)
{
    // Widen before negation: -MIN_int32 overflows a signed 32-bit integer.
    const int64 Magnitude = Coordinate < 0 ? -static_cast<int64>(Coordinate) : static_cast<int64>(Coordinate);
    return FString::Printf(TEXT("%c%lld"), Coordinate < 0 ? TCHAR('m') : TCHAR('p'),
        static_cast<long long>(Magnitude));
}

FName UTitanLevelStreamingBridgeComponent::MakeLevelName(int32 TileX, int32 TileY) const
{
    return FName(*(LevelPrefix + CoordinateToken(TileX) + TEXT("_") + CoordinateToken(TileY)));
}

void UTitanLevelStreamingBridgeComponent::HandleLoadRequested(int32 X, int32 Y, int64 Bytes)
{
    QueueOperation(X, Y, Bytes, true);
}
void UTitanLevelStreamingBridgeComponent::HandleUnloadRequested(int32 X, int32 Y, int64 Bytes)
{
    QueueOperation(X, Y, Bytes, false);
}

void UTitanLevelStreamingBridgeComponent::QueueOperation(int32 X, int32 Y, int64 Bytes, bool bLoad)
{
    if (!Orchestrator || !GetWorld()) return;
    const FIntPoint Tile(X, Y);
    if (Pending.Contains(Tile))
    {
        UE_LOG(LogTemp, Warning, TEXT("TITAN: ignoring overlapping streaming request %d,%d"), X, Y);
        return;
    }
    Telemetry.Begin(bLoad);
    const FName Name = MakeLevelName(X, Y);
    ULevelStreaming* Stream = UGameplayStatics::GetStreamingLevel(this, Name);
    if (!Stream)
    {
        if (!ReportedMissingLevels.Contains(Name))
        {
            UE_LOG(LogTemp, Warning, TEXT("TITAN: level %s not registered in persistent world"), *Name.ToString());
            ReportedMissingLevels.Add(Name);
        }
        // Missing levels are already unloaded; loads fail and release their reservation.
        CompleteOperation(Tile, bLoad, !bLoad, 0);
        Telemetry.Finish(bLoad, !bLoad, false, 0.0);
        return;
    }

    FPendingOperation Op;
    Op.LevelName = Name;
    Op.bLoading = bLoad;
    Op.Seconds = 0.0f;
    Op.EstimatedMegabytes = static_cast<int32>(FMath::Clamp<int64>(Bytes / (1024LL * 1024LL), 1, 32768));
    Pending.Add(Tile, Op);
    InFlightOperations = Pending.Num();
    if (bLoad)
    {
        Stream->SetShouldBeLoaded(true);
        Stream->SetShouldBeVisible(bMakeLoadedLevelsVisible);
    }
    else
    {
        Stream->SetShouldBeVisible(false);
        Stream->SetShouldBeLoaded(false);
    }
}

void UTitanLevelStreamingBridgeComponent::CompleteOperation(
    const FIntPoint& Tile, bool bLoad, bool bSuccess, int32 EstimatedMegabytes)
{
    if (!Orchestrator) return;
    if (bLoad)
        Orchestrator->AcknowledgeLoaded(Tile.X, Tile.Y, bSuccess,
            bSuccess ? EstimatedMegabytes : 0);
    else
        Orchestrator->AcknowledgeUnloaded(Tile.X, Tile.Y, bSuccess);
}

void UTitanLevelStreamingBridgeComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!Orchestrator || DeltaTime <= 0.0f) return;

    TArray<FIntPoint> Finished;
    for (auto& Entry : Pending)
    {
        FPendingOperation& Op = Entry.Value;
        Op.Seconds += DeltaTime;
        const ULevelStreaming* Stream = UGameplayStatics::GetStreamingLevel(this, Op.LevelName);
        const bool bReady = Op.bLoading
            ? (Stream && Stream->IsLevelLoaded() &&
                (!bMakeLoadedLevelsVisible || Stream->IsLevelVisible()))
            : (!Stream || (!Stream->IsLevelLoaded() && !Stream->IsLevelVisible()));
        const bool bTimedOut = Op.Seconds >= FMath::Max(1.0f, OperationTimeoutSeconds);
        if (bReady || bTimedOut)
        {
            // Explicit ACK preserves scheduler accounting even on failure/timeout.
            CompleteOperation(Entry.Key, Op.bLoading, bReady, Op.EstimatedMegabytes);
            Telemetry.Finish(Op.bLoading, bReady, bTimedOut && !bReady, Op.Seconds);
            if (bTimedOut && !bReady)
            {
                UE_LOG(LogTemp, Warning, TEXT("TITAN: streaming timeout for %s"), *Op.LevelName.ToString());
            }
            Finished.Add(Entry.Key);
        }
    }
    for (const FIntPoint& Tile : Finished) Pending.Remove(Tile);
    InFlightOperations = Pending.Num();
}

FTitanStreamingCounters UTitanLevelStreamingBridgeComponent::GetStreamingCounters() const
{
    const Titan::StreamingSummary Summary = Telemetry.Snapshot();
    FTitanStreamingCounters Result;
    // Saturate values for Blueprint int32, never wrap on long sessions.
    auto ToBP = [](std::uint64_t N) -> int32 {
        return static_cast<int32>(FMath::Min<std::uint64_t>(N, 2147483647ULL));
    };
    Result.LoadRequests = ToBP(Summary.Loads.Started);
    Result.LoadsSucceeded = ToBP(Summary.Loads.Succeeded);
    Result.LoadsFailed = ToBP(Summary.Loads.Failed);
    Result.LoadTimeouts = ToBP(Summary.Loads.TimedOut);
    Result.UnloadRequests = ToBP(Summary.Unloads.Started);
    Result.UnloadsSucceeded = ToBP(Summary.Unloads.Succeeded);
    Result.UnloadsFailed = ToBP(Summary.Unloads.Failed);
    Result.UnloadTimeouts = ToBP(Summary.Unloads.TimedOut);
    Result.PendingCount = Pending.Num();
    Result.LoadP95Ms = static_cast<float>(Summary.Loads.P95Ms);
    Result.UnloadP95Ms = static_cast<float>(Summary.Unloads.P95Ms);
    Result.LoadMeanMs = static_cast<float>(Summary.Loads.MeanMs);
    return Result;
}

void UTitanLevelStreamingBridgeComponent::ResetStreamingCounters()
{
    // Prevent unmatched completions: reset only after all operations finish.
    if (Pending.Num() == 0)
    {
        Telemetry.Reset();
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("TITAN: metrics reset deferred; streaming operations are still pending."));
    }
}
