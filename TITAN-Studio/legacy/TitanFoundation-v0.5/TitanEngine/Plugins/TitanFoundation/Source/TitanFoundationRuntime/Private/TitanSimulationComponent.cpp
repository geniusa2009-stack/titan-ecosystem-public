#include "TitanSimulationComponent.h"
#include "Math/UnrealMathUtility.h"

UTitanSimulationComponent::UTitanSimulationComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PrePhysics;
}
void UTitanSimulationComponent::BeginPlay()
{
    Super::BeginPlay();
    ResetSimulation();
}
void UTitanSimulationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Handles.Empty();
    Clock.Reset();
    Simulation.Reset();
    LogicalEntityCount = 0;
    Super::EndPlay(EndPlayReason);
}
void UTitanSimulationComponent::ResetSimulation()
{
    Simulation.Reset(new Titan::EntityWorld());
    const double step = FMath::Clamp(static_cast<double>(FixedStepSeconds), 0.001, 0.1);
    const int steps = FMath::Clamp(static_cast<int>(MaxCatchUpSteps), 1, 16);
    Clock.Reset(new Titan::FixedClock(step, steps));
    Handles.Empty();
    LogicalEntityCount = 0;
    NextHandle = 1;
}
int32 UTitanSimulationComponent::SpawnLogicalEntity(FVector Position, FVector Velocity)
{
    if (!Simulation) ResetSimulation();
    if (NextHandle <= 0 || NextHandle == MAX_int32) return INDEX_NONE;
    Titan::Entity e = Simulation->Create();
    Simulation->Set<Titan::Transform>(e, Titan::Transform{{Position.X, Position.Y, Position.Z}});
    Simulation->Set<Titan::Velocity>(e, Titan::Velocity{{Velocity.X, Velocity.Y, Velocity.Z}});
    int32 id = NextHandle++;
    Handles.Add(id, e);
    LogicalEntityCount = static_cast<int32>(Simulation->AliveCount());
    return id;
}
bool UTitanSimulationComponent::DestroyLogicalEntity(int32 Handle)
{
    if (!Simulation) return false;
    Titan::Entity* e = Handles.Find(Handle);
    if (!e) return false;
    const bool removed = Simulation->Destroy(*e);
    Handles.Remove(Handle);
    LogicalEntityCount = static_cast<int32>(Simulation->AliveCount());
    return removed;
}
bool UTitanSimulationComponent::GetLogicalPosition(int32 Handle, FVector& OutPosition) const
{
    OutPosition = FVector::ZeroVector;
    if (!Simulation) return false;
    const Titan::Entity* e = Handles.Find(Handle);
    if (!e) return false;
    const Titan::Transform* t = Simulation->Get<Titan::Transform>(*e);
    if (!t) return false;
    OutPosition = FVector(t->Position.X, t->Position.Y, t->Position.Z);
    return true;
}
void UTitanSimulationComponent::StepLogicalWorld(double dt)
{
    Simulation->Each<Titan::Transform, Titan::Velocity>([dt](Titan::Entity,
        Titan::Transform& t, Titan::Velocity& v) {
        t.Position.X += v.Linear.X * dt;
        t.Position.Y += v.Linear.Y * dt;
        t.Position.Z += v.Linear.Z * dt;
    });
}
void UTitanSimulationComponent::TickComponent(float DeltaTime, ELevelTick TickType,
    FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (bSimulateAutomatically && Simulation && Clock)
        Clock->Advance(static_cast<double>(FMath::Max(DeltaTime, 0.0f)),
            [this](double dt) { StepLogicalWorld(dt); });
}
