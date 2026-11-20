#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TitanAdaptiveQualityComponent.generated.h"

// Frame-time based quality feedback. Can be disabled in deterministic capture/benchmarks.
UCLASS(ClassGroup=(TITAN), meta=(BlueprintSpawnableComponent))
class TITANFOUNDATIONRUNTIME_API UTitanAdaptiveQualityComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UTitanAdaptiveQualityComponent();
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Performance")
    bool bEnableAdaptation = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Performance", meta=(ClampMin="15.0", ClampMax="240.0"))
    float TargetFPS = 60.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Performance", meta=(ClampMin="50.0", ClampMax="100.0"))
    float MinScreenPercentage = 60.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="TITAN|Performance", meta=(ClampMin="50.0", ClampMax="200.0"))
    float MaxScreenPercentage = 100.0f;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Performance")
    float SmoothedFrameMilliseconds = 16.67f;
    UPROPERTY(BlueprintReadOnly, Category="TITAN|Performance")
    float CurrentScreenPercentage = 100.0f;
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
        FActorComponentTickFunction* ThisTickFunction) override;
private:
    float UpdateElapsed = 0.0f;
};
