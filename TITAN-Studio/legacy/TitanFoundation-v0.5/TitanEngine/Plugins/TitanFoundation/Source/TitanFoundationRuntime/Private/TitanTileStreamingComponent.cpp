#include "TitanTileStreamingComponent.h"
#include "TitanSpatialPlanner.h"
#include "GameFramework/Actor.h"
#include <vector>

UTitanTileStreamingComponent::UTitanTileStreamingComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
}
void UTitanTileStreamingComponent::BeginPlay()
{
    Super::BeginPlay();
    // Emit initial tile requests on first tick rather than during BeginPlay.
    Accumulator = FMath::Max(UpdateIntervalSeconds, 0.05f);
}
void UTitanTileStreamingComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    Accumulator += DeltaTime;
    const float Interval = FMath::Max(UpdateIntervalSeconds, 0.05f);
    if (Accumulator < Interval)
        return;
    Accumulator = 0.0f;
    const AActor* Target = TrackingActor ? TrackingActor : GetOwner();
    if (!Target)
        return;
    const FVector Position = Target->GetActorLocation();
    TitanSpatial::Config Config;
    Config.TileWorldSize = FMath::Max(TileSize, 1.0f);
    Config.LoadRadius = FMath::Clamp(LoadRadius, 0, 32);
    Config.UnloadRadius = FMath::Clamp(UnloadRadius, Config.LoadRadius, 64);
    Config.MaxActive = static_cast<std::size_t>(FMath::Max(MaxActiveTiles, 1));
    Config.MaxLoadsPerUpdate = static_cast<std::size_t>(FMath::Clamp(LoadsPerTick, 1, 64));
    Config.MaxUnloadsPerUpdate = static_cast<std::size_t>(FMath::Clamp(UnloadsPerTick, 1, 64));
    std::vector<TitanSpatial::Tile> Current;
    Current.reserve(ActiveTiles.Num());
    for (const FIntPoint& Entry : ActiveTiles)
        Current.push_back(TitanSpatial::Tile(Entry.X, Entry.Y));
    const TitanSpatial::PlanResult Result = TitanSpatial::Plan(
        Config, Position.X, Position.Y, Current);
    for (const TitanSpatial::Tile& Tile : Result.ToUnload)
    {
        const FIntPoint Key(Tile.X, Tile.Y);
        ActiveTiles.RemoveSingle(Key);
        OnTileUnloadRequested.Broadcast(Tile.X, Tile.Y);
    }
    for (const TitanSpatial::Tile& Tile : Result.ToLoad)
    {
        const FIntPoint Key(Tile.X, Tile.Y);
        if (!ActiveTiles.Contains(Key))
        {
            ActiveTiles.Add(Key);
            OnTileLoadRequested.Broadcast(Tile.X, Tile.Y);
        }
    }
}
