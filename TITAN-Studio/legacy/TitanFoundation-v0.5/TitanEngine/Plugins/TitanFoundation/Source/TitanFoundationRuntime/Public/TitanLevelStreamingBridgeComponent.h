#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TitanStreamingTelemetry.h"
#include "TitanLevelStreamingBridgeComponent.generated.h"

class UTitanWorldOrchestratorComponent;

/** Snapshot of actual UE4.27 streaming operation durations, not GPU frame time. */
USTRUCT(BlueprintType)
struct FTitanStreamingCounters
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 LoadRequests = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 LoadsSucceeded = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 LoadsFailed = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 LoadTimeouts = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 UnloadRequests = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 UnloadsSucceeded = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 UnloadsFailed = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 UnloadTimeouts = 0;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") float LoadP95Ms = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") float UnloadP95Ms = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") float LoadMeanMs = 0.0f;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Streaming") int32 PendingCount = 0;
};

/**
 * Drives UE4.27 persistent-world sublevels from TITAN's scheduler.
 * Sublevels must already be registered in the persistent level.
 * No LevelInstance creation or World Partition features are assumed.
 */
UCLASS(ClassGroup=(TITAN), meta=(BlueprintSpawnableComponent))
class TITANFOUNDATIONRUNTIME_API UTitanLevelStreamingBridgeComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UTitanLevelStreamingBridgeComponent();

    // Keep this actor component on the same actor as the orchestrator, or assign it explicitly.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming")
    UTitanWorldOrchestratorComponent* Orchestrator = nullptr;

    // Example names: Tile_p0_p0, Tile_m1_p2 (p = positive/nonnegative, m = negative).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming")
    FString LevelPrefix = TEXT("Tile_");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming", meta=(ClampMin="1.0", ClampMax="600.0"))
    float OperationTimeoutSeconds = 60.0f;

    // We request visibility as well as load, and confirm both before ACK.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Streaming")
    bool bMakeLoadedLevelsVisible = true;

    // These are budget estimates, not measurements of actual UE process RAM.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="TITAN|Streaming")
    int32 InFlightOperations = 0;

    UFUNCTION(BlueprintCallable, Category="TITAN|Streaming|Diagnostics")
    FTitanStreamingCounters GetStreamingCounters() const;

    UFUNCTION(BlueprintCallable, Category="TITAN|Streaming|Diagnostics")
    void ResetStreamingCounters();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UFUNCTION(BlueprintPure, Category="TITAN|Streaming")
    FName MakeLevelName(int32 TileX, int32 TileY) const;

private:
    struct FPendingOperation
    {
        FName LevelName;
        bool bLoading = true;
        float Seconds = 0.0f;
        int32 EstimatedMegabytes = 0;
    };

    TMap<FIntPoint, FPendingOperation> Pending;
    Titan::StreamingTelemetry Telemetry;
    TSet<FName> ReportedMissingLevels;

    UFUNCTION()
    void HandleLoadRequested(int32 TileX, int32 TileY, int64 ReservedBytes);
    UFUNCTION()
    void HandleUnloadRequested(int32 TileX, int32 TileY, int64 ReservedBytes);

    void QueueOperation(int32 TileX, int32 TileY, int64 ReservedBytes, bool bLoad);
    void CompleteOperation(const FIntPoint& Tile, bool bLoad, bool bSuccess, int32 EstimatedMegabytes);
    static FString CoordinateToken(int32 Coordinate);
};
