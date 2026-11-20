#include "TitanWorldOrchestratorComponent.h"
#include "TitanWorldScheduler.h"
#include "GameFramework/Actor.h"
#include <algorithm>

UTitanWorldOrchestratorComponent::UTitanWorldOrchestratorComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}
UTitanWorldOrchestratorComponent::~UTitanWorldOrchestratorComponent() = default;
void UTitanWorldOrchestratorComponent::BeginPlay()
{
    Super::BeginPlay();
    Titan::StreamConfig C;
    C.CellWorldSize = FMath::Max(100.0f, CellSize);
    C.LoadRadius = FMath::Clamp(LoadRadius,1,32);
    C.UnloadRadius = FMath::Clamp(UnloadRadius,C.LoadRadius,64);
    C.PredictionSeconds = FMath::Clamp(PredictionSeconds,0.0f,5.0f);
    C.MaxTiles = static_cast<size_t>(FMath::Clamp(MaximumTiles,1,8192));
    C.EstimatedTileBytes = static_cast<size_t>(FMath::Clamp(EstimatedMegabytesPerTile,1,4096)) * 1024ULL * 1024ULL;
    C.MaxResidentBytes = static_cast<size_t>(FMath::Clamp(MemoryBudgetMegabytes,1,32768)) * 1024ULL * 1024ULL;
    C.MaxInFlightLoads = static_cast<size_t>(FMath::Clamp(MaxLoadsPerUpdate,1,64));
    C.MaxLoadsPerTick = C.MaxInFlightLoads;
    C.MaxUnloadsPerTick = static_cast<size_t>(FMath::Clamp(MaxUnloadsPerUpdate,1,128));
    Scheduler = MakeUnique<Titan::WorldScheduler>(C);
    ElapsedSeconds = FMath::Max(UpdateInterval,0.05f);
}
void UTitanWorldOrchestratorComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaTime,TickType,TickFunction);
    if (!Scheduler || DeltaTime <= 0.0f) return;
    const AActor* Target = TrackingActor ? TrackingActor : GetOwner();
    if (!Target) return;
    const FVector Location = Target->GetActorLocation();
    ElapsedSeconds += DeltaTime;
    const float Interval = FMath::Max(UpdateInterval,0.05f);
    if (ElapsedSeconds < Interval) return;
    const FVector Velocity = bHasPreviousLocation && ElapsedSeconds > KINDA_SMALL_NUMBER
        ? (Location - PreviousLocation) / ElapsedSeconds : FVector::ZeroVector;
    PreviousLocation = Location;
    bHasPreviousLocation = true;
    ElapsedSeconds = 0.0f;
    const auto Commands = Scheduler->Tick(Location.X,Location.Y,Velocity.X,Velocity.Y);
    for (const auto& Command:Commands) {
        const int64 Bytes = static_cast<int64>(Command.EstimatedBytes);
        if (Command.Operation == Titan::StreamOp::Load)
            OnLoadRequested.Broadcast(Command.Key.X,Command.Key.Y,Bytes);
        else OnUnloadRequested.Broadcast(Command.Key.X,Command.Key.Y,Bytes);
    }
}
bool UTitanWorldOrchestratorComponent::AcknowledgeLoaded(int32 X,int32 Y,bool bSuccess,int32 ActualMegabytes)
{
    if (!Scheduler) return false;
    const std::size_t Bytes = ActualMegabytes <= 0 ? 0 :
        static_cast<std::size_t>(FMath::Min(ActualMegabytes,32768)) * 1024ULL * 1024ULL;
    return Scheduler->CompleteLoad(Titan::Cell(X,Y),bSuccess,Bytes);
}
bool UTitanWorldOrchestratorComponent::AcknowledgeUnloaded(int32 X,int32 Y,bool bSuccess)
{
    return Scheduler && Scheduler->CompleteUnload(Titan::Cell(X,Y),bSuccess);
}
void UTitanWorldOrchestratorComponent::GetWorldStatistics(int32& Loading,int32& Resident,
    int32& Unloading,int64& ReservedBytes) const
{
    Loading=Resident=Unloading=0;
    ReservedBytes=0;
    if (!Scheduler) return;
    const auto S=Scheduler->Stats();
    Loading=static_cast<int32>(S.Loading);
    Resident=static_cast<int32>(S.Resident);
    Unloading=static_cast<int32>(S.Unloading);
    ReservedBytes=static_cast<int64>(S.ReservedBytes);
}
