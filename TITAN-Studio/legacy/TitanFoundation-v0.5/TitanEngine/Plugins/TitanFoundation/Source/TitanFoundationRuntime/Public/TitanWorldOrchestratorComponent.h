#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/UniquePtr.h"
#include "TitanWorldOrchestratorComponent.generated.h"

namespace Titan { class WorldScheduler; }
class AActor;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FTitanWorldRequest, int32, TileX, int32, TileY, int64, ReservedBytes);

// Pluggable streaming planner for UE4.27. Your game/editor handles the asynchronous I/O.
// ACKs are mandatory; requests alone do not cause levels or assets to load/unload.
UCLASS(ClassGroup=(TITAN), meta=(BlueprintSpawnableComponent))
class TITANFOUNDATIONRUNTIME_API UTitanWorldOrchestratorComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UTitanWorldOrchestratorComponent();
    virtual ~UTitanWorldOrchestratorComponent() override;
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World")
    AActor* TrackingActor = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="100.0"))
    float CellSize = 2000.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1",ClampMax="32"))
    int32 LoadRadius = 3;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="2",ClampMax="64"))
    int32 UnloadRadius = 5;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1"))
    int32 EstimatedMegabytesPerTile = 8;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1"))
    int32 MemoryBudgetMegabytes = 512;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1"))
    int32 MaximumTiles = 96;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1"))
    int32 MaxLoadsPerUpdate = 3;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="1"))
    int32 MaxUnloadsPerUpdate = 5;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="0.05"))
    float UpdateInterval = 0.25f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|World", meta=(ClampMin="0.0", ClampMax="5.0"))
    float PredictionSeconds = 0.8f;

    UPROPERTY(BlueprintAssignable, Category="TITAN|World")
    FTitanWorldRequest OnLoadRequested;
    UPROPERTY(BlueprintAssignable, Category="TITAN|World")
    FTitanWorldRequest OnUnloadRequested;

    // Call from your asynchronous streaming completion callbacks.
    UFUNCTION(BlueprintCallable, Category="TITAN|World")
    bool AcknowledgeLoaded(int32 X, int32 Y, bool bSuccess, int32 ActualMegabytes);
    UFUNCTION(BlueprintCallable, Category="TITAN|World")
    bool AcknowledgeUnloaded(int32 X, int32 Y, bool bSuccess);
    UFUNCTION(BlueprintCallable, Category="TITAN|World")
    void GetWorldStatistics(int32& Loading, int32& Resident, int32& Unloading, int64& ReservedBytes) const;
private:
    TUniquePtr<Titan::WorldScheduler> Scheduler;
    FVector PreviousLocation = FVector::ZeroVector;
    bool bHasPreviousLocation = false;
    float ElapsedSeconds = 0.0f;
};
