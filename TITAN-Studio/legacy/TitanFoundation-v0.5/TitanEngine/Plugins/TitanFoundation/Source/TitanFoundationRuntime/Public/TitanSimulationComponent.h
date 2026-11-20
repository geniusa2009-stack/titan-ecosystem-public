#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TitanECS.h"
#include "TitanFixedClock.h"
#include "TitanSimulationComponent.generated.h"

/**
 * Opt-in runtime testbed for original TITAN C++ ECS. Does NOT replace UE actors or physics.
 * This moves logical Transform/Velocity components only, not SceneComponents.
 */
UCLASS(ClassGroup=(TITAN), meta=(BlueprintSpawnableComponent))
class TITANFOUNDATIONRUNTIME_API UTitanSimulationComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UTitanSimulationComponent();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|ECS", meta=(ClampMin="0.001", ClampMax="0.1"))
    float FixedStepSeconds = 0.01666667f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|ECS", meta=(ClampMin="1", ClampMax="16"))
    int32 MaxCatchUpSteps = 5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|ECS")
    bool bSimulateAutomatically = true;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="TITAN|ECS")
    int32 LogicalEntityCount = 0;

    UFUNCTION(BlueprintCallable, Category="TITAN|ECS")
    int32 SpawnLogicalEntity(FVector Position, FVector Velocity);

    UFUNCTION(BlueprintCallable, Category="TITAN|ECS")
    bool DestroyLogicalEntity(int32 Handle);

    UFUNCTION(BlueprintCallable, Category="TITAN|ECS")
    bool GetLogicalPosition(int32 Handle, FVector& OutPosition) const;

    /** Clears all logical entities; safe to call from Blueprint. */
    UFUNCTION(BlueprintCallable, Category="TITAN|ECS")
    void ResetSimulation();

private:
    TUniquePtr<Titan::EntityWorld> Simulation;
    TUniquePtr<Titan::FixedClock> Clock;
    TMap<int32, Titan::Entity> Handles;
    int32 NextHandle = 1;
    void StepLogicalWorld(double FixedSeconds);
};
