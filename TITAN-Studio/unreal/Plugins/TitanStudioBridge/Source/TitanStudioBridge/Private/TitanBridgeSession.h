// TITAN Studio Bridge session: serves the titan.editor protocol v1 from inside the Unreal Editor.
// Original TITAN code.
//
// Ownership and threading: one FSession, created by the module after engine init, ticked by FTSTicker
// on the game thread. Every UObject access happens on the game thread; only TitanBridgeFrames touches
// the render thread. Delegates registered here are removed in the destructor.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "TitanBridgeFrames.h"
#include "TitanBridgeTransport.h"

class AActor;
class UWorld;

namespace TitanBridge
{
struct FSessionOptions
{
	FString SocketPath;
	int32 ParentPid = 0;
	bool bMinimizeEditor = false;
};

class FSession
{
public:
	explicit FSession(const FSessionOptions& Options);
	~FSession();

	bool Start();
	// Called once the editor has finished starting (startup map loaded).
	void OnEditorReady();

	// Public for the automation tests: dispatch one request object and collect events.
	TSharedRef<FJsonObject> HandleRequest(const TSharedRef<FJsonObject>& Request, TArray<TSharedRef<FJsonObject>>& OutEvents);
	static uint64 IdFor(const AActor* Actor);

private:
	struct FError
	{
		int32 Code = 0;
		FString Message;
	};
	struct FProject
	{
		FString Dir;
		FString Name;
		FString Level;  // long package name, e.g. /Game/TitanProjects/Demo_1a2b3c4d/Demo
	};

	bool Tick(float DeltaSeconds);
	bool Dispatch(const FString& Method, const TSharedRef<FJsonObject>& Params, TSharedRef<FJsonObject>& Result,
				  TArray<TSharedRef<FJsonObject>>& Events, FError& Err);

	// protocol pieces
	bool ProjectCreate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err);
	bool ProjectOpen(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err);
	bool OpenLevel(const FString& Level, FError& Err);
	bool ObjectUpdate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err);
	bool ObjectCreate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err);
	bool ObjectDelete(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err);
	bool ViewportConfigure(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, FError& Err);
	bool ViewportCamera(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, FError& Err);
	void SeedDefaultObjects(UWorld* World);

	// scene model
	UWorld* EditorWorld() const;
	bool IsSceneActor(const AActor* A) const;
	AActor* FindActor(uint64 Id) const;
	TArray<AActor*> SceneActors() const;
	TSharedRef<FJsonObject> ActorJson(AActor* A) const;
	TSharedRef<FJsonObject> SceneJson() const;
	TSharedRef<FJsonObject> StateJson() const;
	TArray<TSharedPtr<FJsonValue>> SelectionJson() const;
	bool IsDirty() const;

	// change tracking (UE-side edits are echoed to Studio as scene.changed)
	void MarkChanged(const AActor* A);
	void MarkFullResync();
	void FlushChanges(TArray<TSharedRef<FJsonObject>>& Events, const FString& Reason);
	void EndGesture();
	void WriteEditorState() const;
	void ReadEditorState();

	void SendEvents(const TArray<TSharedRef<FJsonObject>>& Events);
	void Send(const TSharedRef<FJsonObject>& Message);

	FSessionOptions Options;
	FTransport Transport;
	FFrameChannel Frames;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<FDelegateHandle> EngineDelegates;
	FDelegateHandle SelectionDelegate, UndoDelegate, MapOpenedDelegate, PropertyDelegate, WorldCleanupDelegate;

	bool bNegotiated = false;
	bool bReadyAnnounced = false;
	TOptional<FProject> Project;
	TSharedRef<FJsonObject> EditorState;
	uint64 Revision = 1;
	TSet<uint64> Changed;
	TSet<uint64> KnownIds;
	bool bFullResync = false;
	bool bSelectionChanged = false;
	bool bApplyingOwnSelection = false;
	FString OpenGestureKey;
	double GestureLastTouch = 0;
	double LastParentCheck = 0;
	double StartSeconds = 0;

	struct FMetrics
	{
		uint64 Requests = 0, Errors = 0;
		double TotalRequestUs = 0, MaxRequestUs = 0, LastOpenMs = 0, LastSaveMs = 0;
	} Metrics;
};
}  // namespace TitanBridge
