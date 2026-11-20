#include "TitanAdaptiveQualityComponent.h"
#include "HAL/IConsoleManager.h"

UTitanAdaptiveQualityComponent::UTitanAdaptiveQualityComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
}
void UTitanAdaptiveQualityComponent::BeginPlay()
{
    Super::BeginPlay();
    if (IConsoleVariable* Setting = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
        CurrentScreenPercentage = Setting->GetFloat();
}
void UTitanAdaptiveQualityComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (DeltaTime <= 0.0f || DeltaTime > 0.25f)
        return; // Avoid paused frames and severe one-off stalls.
    const float Alpha = 1.0f - FMath::Exp(-DeltaTime / 0.65f);
    SmoothedFrameMilliseconds = FMath::Lerp(
        SmoothedFrameMilliseconds, DeltaTime * 1000.0f, Alpha);
    UpdateElapsed += DeltaTime;
    if (!bEnableAdaptation || UpdateElapsed < 2.0f)
        return;
    UpdateElapsed = 0.0f;
    const float TargetMs = 1000.0f / FMath::Clamp(TargetFPS, 15.0f, 240.0f);
    const float MinPercent = FMath::Clamp(MinScreenPercentage, 50.0f, 100.0f);
    const float MaxPercent = FMath::Clamp(MaxScreenPercentage, MinPercent, 200.0f);
    // Hysteresis prevents constant oscillation around target frame time.
    float Next = CurrentScreenPercentage;
    if (SmoothedFrameMilliseconds > TargetMs * 1.14f)
        Next -= 5.0f;
    else if (SmoothedFrameMilliseconds < TargetMs * 0.84f)
        Next += 2.0f;
    Next = FMath::Clamp(Next, MinPercent, MaxPercent);
    if (FMath::Abs(Next - CurrentScreenPercentage) > 0.01f)
    {
        if (IConsoleVariable* Setting = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage")))
        {
            Setting->Set(Next, ECVF_SetByCode);
            CurrentScreenPercentage = Next;
        }
    }
}
