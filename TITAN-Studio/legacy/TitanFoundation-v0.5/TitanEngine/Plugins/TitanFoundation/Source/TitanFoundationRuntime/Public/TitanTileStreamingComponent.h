#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TitanTileStreamingComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FTitanTileEvent, int32, TileX, int32, TileY);

// Schedules loading/unloading; the game owns actual asset/level I/O.
// Attach to a streaming manager actor and assign TrackingActor to the player.
UCLASS(ClassGroup=(TITAN), meta=(BlueprintSpawnableComponent))
class TITANFOUNDATIONRUNTIME_API UTitanTileStreamingComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UTitanTileStreamingComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming")
    AActor* TrackingActor = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="1.0"))
    float TileSize = 2000.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="0"))
    int32 LoadRadius = 2;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="0"))
    int32 UnloadRadius = 3;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="1"))
    int32 MaxActiveTiles = 64;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="1"))
    int32 LoadsPerTick = 4;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="1"))
    int32 UnloadsPerTick = 8;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="0.05"))
    float UpdateIntervalSeconds = 0.4f;
    UPROPERTY(BlueprintAssignable, Category="TITAN|Streaming")
    FTitanTileEvent OnTileLoadRequested;
    UPROPERTY(BlueprintAssignable, Category="TITAN|Streaming")
    FTitanTileEvent OnTileUnloadRequested;
    UFUNCTION(BlueprintCallable, Category="TITAN|Streaming")
    int32 GetActiveTileCount() const { return ActiveTiles.Num(); }
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
private:
    UPROPERTY(Transient)
    TArray<FIntPoint> ActiveTiles;
    float Accumulator = 0.0f;
};
