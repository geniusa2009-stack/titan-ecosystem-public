// TITAN Studio Bridge session. Original TITAN code, written against the supplied UE 5.8.4 source.
#include "TitanBridgeSession.h"

#include "Camera/CameraActor.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Editor.h"
#include "Editor/EditorPerformanceSettings.h"
#include "Editor/TransBuffer.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Light.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "Hash/CityHash.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"  // UE 5.8: no longer reached through JsonSerializer.h
#include "RHIStats.h"
#include "RenderTimer.h"
#include "ScopedTransaction.h"
#include "Selection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ShaderCompiler.h"
#include "TitanBridgeLog.h"
#include "TitanCoordinates.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Widgets/SWindow.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <signal.h>

#define LOCTEXT_NAMESPACE "TitanStudioBridge"

namespace TitanBridge
{
namespace
{
constexpr int32 kInvalidRequest = -32600, kMethodNotFound = -32601, kInvalidParams = -32602;
constexpr int32 kNotNegotiated = 1000, kNoProject = 1001, kNotFound = 1002, kIoError = 1003, kVersionMismatch = 1004,
				kConflict = 1006;
constexpr uint64 kIdMask = (1ull << 53) - 1;
const TCHAR* kTemplateLevel = TEXT("/Engine/Maps/Templates/Template_Default");

TArray<TSharedPtr<FJsonValue>> Arr3(const TitanCoords::V3& V)
{
	auto Clean = [](double D) { return FMath::Abs(D) < 1e-9 ? 0.0 : D; };
	return {MakeShared<FJsonValueNumber>(Clean(V.X)), MakeShared<FJsonValueNumber>(Clean(V.Y)),
			MakeShared<FJsonValueNumber>(Clean(V.Z))};
}

bool ReadV3(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, TitanCoords::V3& Out, FString& Err)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O->TryGetArrayField(Field, A)) return false;
	if (A->Num() != 3)
	{
		Err = FString::Printf(TEXT("%s must be [x, y, z]"), Field);
		return false;
	}
	double V[3];
	for (int32 I = 0; I < 3; ++I)
	{
		if (!(*A)[I].IsValid() || !(*A)[I]->TryGetNumber(V[I]) || !FMath::IsFinite(V[I]))
		{
			Err = FString::Printf(TEXT("%s must contain finite numbers"), Field);
			return false;
		}
	}
	Out = {V[0], V[1], V[2]};
	return true;
}

FString Serialize(const TSharedRef<FJsonObject>& O, bool bPretty = false)
{
	FString Out;
	if (bPretty)
	{
		const auto W = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(O, W);
	}
	else
	{
		const auto W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(O, W);
	}
	return Out;
}

TSharedPtr<FJsonObject> ReadJsonFile(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) return nullptr;
	TSharedPtr<FJsonObject> O;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O)) return nullptr;
	return O;
}

TSharedRef<FJsonObject> MakeEvent(const FString& Name, const TSharedRef<FJsonObject>& Data)
{
	TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	E->SetStringField(TEXT("event"), Name);
	E->SetObjectField(TEXT("data"), Data);
	return E;
}

FString SanitizeName(const FString& In)
{
	FString Out;
	for (TCHAR C : In)
		if (FChar::IsAlnum(C) && C < 128) Out.AppendChar(C);
		else if (C == ' ' || C == '_' || C == '-') Out.AppendChar('_');
	return Out.IsEmpty() ? FString(TEXT("Project")) : Out.Left(48);
}

const TCHAR* ShapeOf(const AActor* A)
{
	const AStaticMeshActor* SM = Cast<AStaticMeshActor>(A);
	const UStaticMesh* Mesh = SM && SM->GetStaticMeshComponent() ? SM->GetStaticMeshComponent()->GetStaticMesh() : nullptr;
	if (!Mesh) return TEXT("none");
	const FString N = Mesh->GetName();
	if (N.Contains(TEXT("Sphere"))) return TEXT("sphere");
	if (N.Contains(TEXT("Cylinder"))) return TEXT("cylinder");
	if (N.Contains(TEXT("Plane")) || N.Contains(TEXT("Floor"))) return TEXT("plane");
	if (N.Contains(TEXT("Cube")) || N.Contains(TEXT("Box"))) return TEXT("cube");
	return TEXT("none");  // arbitrary mesh: Studio shows Unreal's rendered frame for it
}

UStaticMesh* BasicShape(const FString& Shape)
{
	const TCHAR* Path = Shape == TEXT("sphere")	  ? TEXT("/Engine/BasicShapes/Sphere.Sphere")
					  : Shape == TEXT("cylinder") ? TEXT("/Engine/BasicShapes/Cylinder.Cylinder")
					  : Shape == TEXT("plane")	  ? TEXT("/Engine/BasicShapes/Plane.Plane")
												  : TEXT("/Engine/BasicShapes/Cube.Cube");
	return LoadObject<UStaticMesh>(nullptr, Path);
}

// Local transform of an actor in TITAN units (relative to its attach parent, as TITAN transforms are).
void GetLocal(const AActor* A, TitanCoords::V3& Pos, TitanCoords::V3& Rot, TitanCoords::V3& Scale)
{
	const USceneComponent* Root = A->GetRootComponent();
	const FVector L = Root ? Root->GetRelativeLocation() : FVector::ZeroVector;
	const FRotator R = Root ? Root->GetRelativeRotation() : FRotator::ZeroRotator;
	const FVector S = Root ? Root->GetRelativeScale3D() : FVector::OneVector;
	Pos = TitanCoords::UeToTitanPosition({L.X, L.Y, L.Z});
	Rot = TitanCoords::UeRotatorToTitanEuler(R.Pitch, R.Yaw, R.Roll);
	Scale = TitanCoords::UeToTitanScale({S.X, S.Y, S.Z});
}
}  // namespace

FSession::FSession(const FSessionOptions& InOptions) : Options(InOptions), EditorState(MakeShared<FJsonObject>())
{
	StartSeconds = FPlatformTime::Seconds();
}

FSession::~FSession()
{
	EndGesture();
	if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	if (GEngine)
	{
		GEngine->OnActorMoved().Remove(EngineDelegates.IsValidIndex(0) ? EngineDelegates[0] : FDelegateHandle());
		GEngine->OnLevelActorAdded().Remove(EngineDelegates.IsValidIndex(1) ? EngineDelegates[1] : FDelegateHandle());
		GEngine->OnLevelActorDeleted().Remove(EngineDelegates.IsValidIndex(2) ? EngineDelegates[2] : FDelegateHandle());
	}
	USelection::SelectionChangedEvent.Remove(SelectionDelegate);
	FEditorDelegates::PostUndoRedo.Remove(UndoDelegate);
	FEditorDelegates::OnMapOpened.Remove(MapOpenedDelegate);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyDelegate);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupDelegate);
	Frames.Release();
	Transport.Close();
}

uint64 FSession::IdFor(const AActor* Actor)
{
	const FGuid Guid = Actor->GetActorGuid();
	if (Guid.IsValid()) return CityHash64(reinterpret_cast<const char*>(&Guid), sizeof(FGuid)) & kIdMask;
	const FTCHARToUTF8 Path(*Actor->GetPathName());
	return CityHash64(Path.Get(), Path.Length()) & kIdMask;
}

bool FSession::Start()
{
	if (!Transport.Listen(Options.SocketPath)) return false;
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("TitanStudioBridge"), 0.0f, [this](float Dt) { return Tick(Dt); });

	EngineDelegates.Add(GEngine->OnActorMoved().AddLambda([this](AActor* A) { MarkChanged(A); }));
	EngineDelegates.Add(GEngine->OnLevelActorAdded().AddLambda([this](AActor* A) { MarkChanged(A); }));
	EngineDelegates.Add(GEngine->OnLevelActorDeleted().AddLambda([this](AActor*) { MarkFullResync(); }));
	SelectionDelegate = USelection::SelectionChangedEvent.AddLambda([this](UObject*) { if (!bApplyingOwnSelection) bSelectionChanged = true; });
	UndoDelegate = FEditorDelegates::PostUndoRedo.AddLambda([this]() { MarkFullResync(); Frames.Invalidate(); });
	MapOpenedDelegate = FEditorDelegates::OnMapOpened.AddLambda([this](const FString&, bool)
	{
		KnownIds.Reset();
		MarkFullResync();
		Frames.OnWorldChanged();
	});
	PropertyDelegate = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda([this](UObject* O, FPropertyChangedEvent&)
	{
		if (AActor* A = Cast<AActor>(O)) MarkChanged(A);
		else if (UActorComponent* C = Cast<UActorComponent>(O)) MarkChanged(C->GetOwner());
	});
	WorldCleanupDelegate = FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* W, bool, bool)
	{
		if (W == EditorWorld()) Frames.OnWorldChanged();
	});
	UE_LOG(LogTitanBridge, Log, TEXT("TITAN Studio Bridge listening on %s"), *Options.SocketPath);
	return true;
}

void FSession::OnEditorReady()
{
	if (bReadyAnnounced) return;
	bReadyAnnounced = true;
	// The editor drives TITAN Studio's viewport from another process: never throttle in the background.
	if (UEditorPerformanceSettings* Perf = GetMutableDefault<UEditorPerformanceSettings>()) Perf->bThrottleCPUWhenNotForeground = false;
	if (Options.bMinimizeEditor && FSlateApplication::IsInitialized())
		for (const TSharedRef<SWindow>& W : FSlateApplication::Get().GetTopLevelWindows()) W->Minimize();
	// Same readiness line as titan-engine-host; TITAN Studio waits for it on stdout.
	std::printf("TITAN_HOST_READY %s\n", TCHAR_TO_UTF8(*Options.SocketPath));
	std::fflush(stdout);
	UE_LOG(LogTitanBridge, Log, TEXT("TITAN_HOST_READY %s"), *Options.SocketPath);
}

// ---- tick ------------------------------------------------------------------------------------
bool FSession::Tick(float)
{
	const double Now = FPlatformTime::Seconds();
	if (Options.ParentPid > 0 && Now - LastParentCheck > 1.0)
	{
		LastParentCheck = Now;
		if (kill(Options.ParentPid, 0) != 0 && errno == ESRCH)
		{
			UE_LOG(LogTitanBridge, Log, TEXT("TITAN Studio (pid %d) exited; closing the editor"), Options.ParentPid);
			Options.ParentPid = 0;
			RequestEngineExit(TEXT("TITAN Studio exited"));
		}
	}

	TArray<FString> Messages;
	bool bConnected = false, bDisconnected = false;
	const bool bHadClient = Transport.HasClient();
	Transport.Poll(Messages, bConnected, bDisconnected);
	if (bConnected || bDisconnected || (bHadClient && !Transport.HasClient())) bNegotiated = false;

	for (const FString& Text : Messages)
	{
		TArray<TSharedRef<FJsonObject>> Events;
		TSharedPtr<FJsonObject> Req;
		TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Req) || !Req.IsValid())
		{
			Resp->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
			TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetNumberField(TEXT("code"), kInvalidRequest);
			E->SetStringField(TEXT("message"), TEXT("malformed JSON"));
			Resp->SetObjectField(TEXT("error"), E);
		}
		else
		{
			Resp = HandleRequest(Req.ToSharedRef(), Events);
		}
		Send(Resp);
		SendEvents(Events);
		const bool bShutdown = Req.IsValid() && Req->GetStringField(TEXT("method")) == TEXT("engine.shutdown") && !Resp->HasField(TEXT("error"));
		if (bShutdown)
		{
			Transport.Flush();
			RequestEngineExit(TEXT("engine.shutdown from TITAN Studio"));
		}
	}

	// A gesture (drag) that stopped sending updates closes its transaction.
	if (!OpenGestureKey.IsEmpty() && Now - GestureLastTouch > 0.5) EndGesture();

	if (bNegotiated)
	{
		TArray<TSharedRef<FJsonObject>> Events;
		FlushChanges(Events, TEXT("unreal"));
		if (bSelectionChanged)
		{
			bSelectionChanged = false;
			TSharedRef<FJsonObject> Sel = MakeShared<FJsonObject>();
			Sel->SetArrayField(TEXT("ids"), SelectionJson());
			Events.Add(MakeEvent(TEXT("selection.changed"), Sel));
		}
		Frames.Tick(EditorWorld());
		TArray<FCompletedFrame> Done;
		Frames.DrainCompleted(Done);
		for (const FCompletedFrame& F : Done)
		{
			TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
			D->SetNumberField(TEXT("frame"), double(F.Frame));
			D->SetNumberField(TEXT("slot"), F.Slot);
			D->SetNumberField(TEXT("width"), F.Width);
			D->SetNumberField(TEXT("height"), F.Height);
			D->SetNumberField(TEXT("cameraRevision"), F.CameraRev);
			D->SetNumberField(TEXT("captureToReadyMs"), F.CaptureToReadyMs);
			Events.Add(MakeEvent(TEXT("viewport.frame"), D));
		}
		SendEvents(Events);
	}
	else
	{
		Frames.Tick(nullptr);  // keep draining readbacks
		TArray<FCompletedFrame> Ignored;
		Frames.DrainCompleted(Ignored);
	}
	Transport.Flush();
	return true;
}

void FSession::Send(const TSharedRef<FJsonObject>& Message) { Transport.Send(Serialize(Message)); }

void FSession::SendEvents(const TArray<TSharedRef<FJsonObject>>& Events)
{
	if (!bNegotiated) return;  // events go only to negotiated clients
	for (const TSharedRef<FJsonObject>& E : Events) Send(E);
}

// ---- request handling ------------------------------------------------------------------------
TSharedRef<FJsonObject> FSession::HandleRequest(const TSharedRef<FJsonObject>& Req, TArray<TSharedRef<FJsonObject>>& Events)
{
	const double T0 = FPlatformTime::Seconds();
	TSharedRef<FJsonObject> Resp = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonValue> IdValue = Req->TryGetField(TEXT("id"));
	Resp->SetField(TEXT("id"), IdValue.IsValid() ? IdValue : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
	FError Err;
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	FString Method;
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	if (!IdValue.IsValid() || IdValue->Type != EJson::Number || !Req->TryGetStringField(TEXT("method"), Method))
		Err = {kInvalidRequest, TEXT("request needs numeric id and string method")};
	else if (Req->HasField(TEXT("params")) && !Req->TryGetObjectField(TEXT("params"), ParamsPtr))
		Err = {kInvalidParams, TEXT("params must be an object")};
	else if (!bNegotiated && Method != TEXT("session.hello"))
		Err = {kNotNegotiated, TEXT("call session.hello first")};
	else
	{
		if (Method != TEXT("object.update")) EndGesture();
		const TSharedRef<FJsonObject> Params = ParamsPtr ? ParamsPtr->ToSharedRef() : MakeShared<FJsonObject>();
		Dispatch(Method, Params, Result, Events, Err);
	}
	if (Err.Code != 0)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetNumberField(TEXT("code"), Err.Code);
		E->SetStringField(TEXT("message"), Err.Message);
		Resp->SetObjectField(TEXT("error"), E);
		Metrics.Errors++;
	}
	else
	{
		Resp->SetObjectField(TEXT("result"), Result);
	}
	const double Us = (FPlatformTime::Seconds() - T0) * 1e6;
	Metrics.Requests++;
	Metrics.TotalRequestUs += Us;
	Metrics.MaxRequestUs = FMath::Max(Metrics.MaxRequestUs, Us);
	return Resp;
}

bool FSession::Dispatch(const FString& M, const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R,
						TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	auto RequireProject = [&]() { if (!Project) Err = {kNoProject, TEXT("no project is open")}; return Project.IsSet(); };

	if (M == TEXT("session.hello"))
	{
		const TSharedPtr<FJsonObject>* Proto = nullptr;
		double Min = 1, Max = 1;
		if (P->TryGetObjectField(TEXT("protocol"), Proto))
		{
			(*Proto)->TryGetNumberField(TEXT("min"), Min);
			(*Proto)->TryGetNumberField(TEXT("max"), Max);
		}
		if (Min > 1 || Max < 1)
		{
			Err = {kVersionMismatch, TEXT("no common protocol version (server supports 1-1)")};
			return false;
		}
		bNegotiated = true;
		R->SetNumberField(TEXT("protocol"), 1);
		R->SetStringField(TEXT("protocolName"), TEXT("titan.editor"));
		TSharedRef<FJsonObject> Engine = MakeShared<FJsonObject>();
		Engine->SetStringField(TEXT("product"), TEXT("TITAN Engine"));
		Engine->SetStringField(TEXT("version"), FEngineVersion::Current().ToString());
		Engine->SetStringField(TEXT("backend"), TEXT("unreal-editor"));
		Engine->SetStringField(TEXT("platform"), TEXT("macos-arm64"));
		Engine->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"));
		R->SetObjectField(TEXT("engine"), Engine);
		TArray<TSharedPtr<FJsonValue>> Caps;
		for (const TCHAR* C : {TEXT("project"), TEXT("scene"), TEXT("selection"), TEXT("undo"), TEXT("pick"),
							   TEXT("viewport.frames"), TEXT("unreal.levels")})
			Caps.Add(MakeShared<FJsonValueString>(C));
		R->SetArrayField(TEXT("capabilities"), Caps);
		R->SetObjectField(TEXT("state"), StateJson());
		return true;
	}
	if (M == TEXT("engine.status"))
	{
		R->SetNumberField(TEXT("uptimeMs"), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		R->SetNumberField(TEXT("objectCount"), SceneActors().Num());
		R->SetNumberField(TEXT("rssBytes"), double(FPlatformMemory::GetStats().UsedPhysical));
		TSharedRef<FJsonObject> Mx = MakeShared<FJsonObject>();
		Mx->SetNumberField(TEXT("requests"), double(Metrics.Requests));
		Mx->SetNumberField(TEXT("errors"), double(Metrics.Errors));
		Mx->SetNumberField(TEXT("avgRequestUs"), Metrics.Requests ? Metrics.TotalRequestUs / double(Metrics.Requests) : 0.0);
		Mx->SetNumberField(TEXT("maxRequestUs"), Metrics.MaxRequestUs);
		Mx->SetNumberField(TEXT("lastOpenMs"), Metrics.LastOpenMs);
		Mx->SetNumberField(TEXT("lastSaveMs"), Metrics.LastSaveMs);
		R->SetObjectField(TEXT("metrics"), Mx);
		TSharedRef<FJsonObject> U = MakeShared<FJsonObject>();
		U->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
		U->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : TEXT("none"));
		U->SetNumberField(TEXT("frameMs"), FApp::GetDeltaTime() * 1000.0);
		U->SetNumberField(TEXT("gameThreadMs"), FPlatformTime::ToMilliseconds(GGameThreadTime));
		U->SetNumberField(TEXT("renderThreadMs"), FPlatformTime::ToMilliseconds(GRenderThreadTime));
		U->SetNumberField(TEXT("rhiThreadMs"), FPlatformTime::ToMilliseconds(GRHIThreadTime));
		U->SetNumberField(TEXT("gpuMs"), FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
		U->SetNumberField(TEXT("shadersRemaining"), GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0);
		for (const TCHAR* Name : {TEXT("r.Nanite"), TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ReflectionMethod"),
								  TEXT("r.Shadow.Virtual.Enable"), TEXT("r.ScreenPercentage"), TEXT("r.AntiAliasingMethod"),
								  TEXT("r.Streaming.PoolSize"), TEXT("r.RDG.ParallelExecute"), TEXT("r.GPUScene.ParallelUpdate")})
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name)) U->SetStringField(Name, V->GetString());
		const FPlatformMemoryStats Mem = FPlatformMemory::GetStats();
		U->SetNumberField(TEXT("usedPhysicalBytes"), double(Mem.UsedPhysical));
		U->SetNumberField(TEXT("availablePhysicalBytes"), double(Mem.AvailablePhysical));
		// Texture streaming / GPU memory as reported by the RHI (Metal: unified memory, values may be -1).
		FTextureMemoryStats Tex;
		RHIGetTextureMemoryStats(Tex);
		U->SetNumberField(TEXT("streamingTextureBytes"), double(Tex.StreamingMemorySize));
		U->SetNumberField(TEXT("nonStreamingTextureBytes"), double(Tex.NonStreamingMemorySize));
		U->SetNumberField(TEXT("texturePoolBytes"), double(Tex.TexturePoolSize));
		U->SetNumberField(TEXT("totalGraphicsMemoryBytes"), double(Tex.TotalGraphicsMemory));
		const FFrameChannel::FStats FS = Frames.GetStats();
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetNumberField(TEXT("captured"), double(FS.Captured));
		F->SetNumberField(TEXT("delivered"), double(FS.Delivered));
		F->SetNumberField(TEXT("droppedBusy"), double(FS.DroppedBusy));
		F->SetNumberField(TEXT("avgCaptureToReadyMs"), FS.AvgCaptureToReadyMs);
		F->SetNumberField(TEXT("width"), Frames.Width());
		F->SetNumberField(TEXT("height"), Frames.Height());
		U->SetObjectField(TEXT("frames"), F);
		R->SetObjectField(TEXT("unreal"), U);
		return true;
	}
	if (M == TEXT("project.create")) return ProjectCreate(P, R, Ev, Err);
	if (M == TEXT("project.open")) return ProjectOpen(P, R, Ev, Err);
	if (M == TEXT("project.save"))
	{
		if (!RequireProject()) return false;
		const double T0 = FPlatformTime::Seconds();
		UWorld* World = EditorWorld();
		if (!World || !UEditorLoadingAndSavingUtils::SaveMap(World, Project->Level))
		{
			Err = {kIoError, TEXT("Unreal could not save ") + Project->Level};
			return false;
		}
		WriteEditorState();
		Metrics.LastSaveMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		R->SetStringField(TEXT("path"), FPackageName::LongPackageNameToFilename(Project->Level, FPackageName::GetMapPackageExtension()));
		R->SetStringField(TEXT("level"), Project->Level);
		R->SetNumberField(TEXT("saveMs"), Metrics.LastSaveMs);
		R->SetObjectField(TEXT("state"), StateJson());
		Ev.Add(MakeEvent(TEXT("project.state"), StateJson()));
		return true;
	}
	if (M == TEXT("project.close"))
	{
		if (!RequireProject()) return false;
		bool bForce = false;
		P->TryGetBoolField(TEXT("force"), bForce);
		if (IsDirty() && !bForce)
		{
			Err = {kConflict, TEXT("project has unsaved changes (pass force: true to discard)")};
			return false;
		}
		WriteEditorState();
		if (UWorld* W = EditorWorld()) W->GetOutermost()->SetDirtyFlag(false);  // discarding was confirmed
		UEditorLoadingAndSavingUtils::NewBlankMap(false);
		Project.Reset();
		EditorState = MakeShared<FJsonObject>();
		if (GEditor && GEditor->Trans) GEditor->Trans->Reset(LOCTEXT("TitanClose", "TITAN project closed"));
		KnownIds.Reset();
		Changed.Reset();
		bFullResync = false;
		R = StateJson();
		Ev.Add(MakeEvent(TEXT("project.closed"), StateJson()));
		return true;
	}
	if (M == TEXT("scene.query"))
	{
		if (!RequireProject()) return false;
		R = SceneJson();
		return true;
	}
	if (M == TEXT("object.get"))
	{
		if (!RequireProject()) return false;
		AActor* A = FindActor(uint64(P->GetNumberField(TEXT("id"))));
		if (!A)
		{
			Err = {kNotFound, TEXT("object not found")};
			return false;
		}
		R = ActorJson(A);
		const FTransform T = A->GetActorTransform();
		const FMatrix WM = T.ToMatrixWithScale();
		// TITAN world matrix (column-major) = M * UE_world * M^-1 with the cm -> m scale.
		TitanCoords::M3 Lin;
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c) Lin.M[r][c] = WM.M[c][r];  // UE row-vector matrix -> column form
		const TitanCoords::M3 Ax = TitanCoords::AxisMap();
		const TitanCoords::M3 TL = Ax * Lin * Ax.Transposed();
		const TitanCoords::V3 TP = TitanCoords::UeToTitanPosition({WM.M[3][0], WM.M[3][1], WM.M[3][2]});
		TArray<TSharedPtr<FJsonValue>> World;
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
			{
				double V = 0;
				if (c < 3 && r < 3) V = TL.M[r][c];
				else if (c == 3 && r < 3) V = r == 0 ? TP.X : r == 1 ? TP.Y : TP.Z;
				else if (c == 3 && r == 3) V = 1;
				World.Add(MakeShared<FJsonValueNumber>(V));
			}
		R->SetArrayField(TEXT("worldMatrix"), World);
		return true;
	}
	if (M == TEXT("object.update")) return ObjectUpdate(P, R, Ev, Err);
	if (M == TEXT("object.create")) return ObjectCreate(P, R, Ev, Err);
	if (M == TEXT("object.delete")) return ObjectDelete(P, R, Ev, Err);
	if (M == TEXT("selection.set"))
	{
		if (!RequireProject()) return false;
		TArray<AActor*> Actors;
		const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
		if (!P->TryGetArrayField(TEXT("ids"), Ids))
		{
			Err = {kInvalidParams, TEXT("ids must be an array")};
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Ids)
		{
			AActor* A = FindActor(uint64(V->AsNumber()));
			if (!A)
			{
				Err = {kNotFound, FString::Printf(TEXT("object %llu not found"), uint64(V->AsNumber()))};
				return false;
			}
			Actors.AddUnique(A);
		}
		bApplyingOwnSelection = true;
		GEditor->SelectNone(false, true, false);
		for (AActor* A : Actors) GEditor->SelectActor(A, true, false, true);
		GEditor->NoteSelectionChange();
		bApplyingOwnSelection = false;
		bSelectionChanged = false;
		R->SetArrayField(TEXT("ids"), SelectionJson());
		Ev.Add(MakeEvent(TEXT("selection.changed"), R));
		return true;
	}
	if (M == TEXT("scene.pick"))
	{
		if (!RequireProject()) return false;
		TitanCoords::V3 O, D;
		FString E;
		if (!ReadV3(P, TEXT("origin"), O, E) || !ReadV3(P, TEXT("direction"), D, E))
		{
			Err = {kInvalidParams, E.IsEmpty() ? FString(TEXT("origin and direction are required")) : E};
			return false;
		}
		const TitanCoords::V3 UO = TitanCoords::TitanToUePosition(O), UD = TitanCoords::TitanToUeDirection(D);
		FVector Dir(UD.X, UD.Y, UD.Z);
		if (!Dir.Normalize())
		{
			Err = {kInvalidParams, TEXT("direction must be non-zero")};
			return false;
		}
		const FVector Start(UO.X, UO.Y, UO.Z);
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(TitanStudioPick), /*bTraceComplex*/ true);
		UWorld* World = EditorWorld();
		if (World && World->LineTraceSingleByChannel(Hit, Start, Start + Dir * 1.0e7, ECC_Visibility, Q) && Hit.GetActor() &&
			IsSceneActor(Hit.GetActor()))
		{
			R->SetNumberField(TEXT("id"), double(IdFor(Hit.GetActor())));
			R->SetNumberField(TEXT("distance"), Hit.Distance / 100.0);
		}
		else
		{
			R->SetField(TEXT("id"), MakeShared<FJsonValueNull>());
		}
		return true;
	}
	if (M == TEXT("edit.undo") || M == TEXT("edit.redo"))
	{
		if (!RequireProject()) return false;
		const bool bUndo = M == TEXT("edit.undo");
		FString Label;
		if (GEditor->Trans)
			Label = (bUndo ? GEditor->Trans->GetUndoContext(true) : GEditor->Trans->GetRedoContext()).Title.ToString();
		const bool bApplied = bUndo ? GEditor->UndoTransaction() : GEditor->RedoTransaction();
		R->SetBoolField(TEXT("applied"), bApplied);
		R->SetStringField(TEXT("label"), bApplied ? Label : FString());
		if (bApplied)
		{
			MarkFullResync();
			FlushChanges(Ev, bUndo ? TEXT("undo") : TEXT("redo"));
			Frames.Invalidate();
		}
		R->SetObjectField(TEXT("state"), StateJson());
		return true;
	}
	if (M == TEXT("editor.setState"))
	{
		const TSharedPtr<FJsonObject>* Cam = nullptr;
		if (P->TryGetObjectField(TEXT("camera"), Cam)) EditorState->SetObjectField(TEXT("camera"), *Cam);
		return true;
	}
	if (M == TEXT("extensions.list"))
	{
		R->SetArrayField(TEXT("extensions"), TArray<TSharedPtr<FJsonValue>>());
		R->SetStringField(TEXT("platform"), TEXT("macos-arm64"));
		return true;
	}
	if (M == TEXT("viewport.configure")) return ViewportConfigure(P, R, Err);
	if (M == TEXT("viewport.camera")) return ViewportCamera(P, R, Err);
	if (M == TEXT("viewport.render"))
	{
		bool bContinuous = false;
		P->TryGetBoolField(TEXT("continuous"), bContinuous);
		Frames.SetContinuous(bContinuous);
		Frames.Invalidate(1);
		R->SetBoolField(TEXT("continuous"), Frames.IsContinuous());
		return true;
	}
	if (M == TEXT("unreal.setCVar"))
	{
		// Renderer A/B switches for benchmarking. Only renderer ("r.") variables are reachable.
		FString Name, Value;
		if (!P->TryGetStringField(TEXT("name"), Name) || !P->TryGetStringField(TEXT("value"), Value) || !Name.StartsWith(TEXT("r.")))
		{
			Err = {kInvalidParams, TEXT("name (r.*) and value are required")};
			return false;
		}
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (!V)
		{
			Err = {kNotFound, TEXT("unknown console variable ") + Name};
			return false;
		}
		V->Set(*Value, ECVF_SetByConsole);
		Frames.Invalidate();
		R->SetStringField(TEXT("name"), Name);
		R->SetStringField(TEXT("value"), V->GetString());
		return true;
	}
	if (M == TEXT("unreal.window"))
	{
		bool bVisible = true;
		P->TryGetBoolField(TEXT("visible"), bVisible);
		if (FSlateApplication::IsInitialized())
			for (const TSharedRef<SWindow>& W : FSlateApplication::Get().GetTopLevelWindows())
			{
				if (bVisible)
				{
					W->Restore();
					W->BringToFront(true);
				}
				else
				{
					W->Minimize();
				}
			}
		R->SetBoolField(TEXT("visible"), bVisible);
		return true;
	}
	if (M == TEXT("engine.shutdown")) return true;  // Tick() exits after the response is flushed
	Err = {kMethodNotFound, TEXT("not supported by the Unreal backend: ") + M};
	return false;
}

// ---- projects and levels -------------------------------------------------------------------
bool FSession::OpenLevel(const FString& Level, FError& Err)
{
	const FString File = FPackageName::LongPackageNameToFilename(Level, FPackageName::GetMapPackageExtension());
	if (!FPaths::FileExists(File))
	{
		Err = {kIoError, TEXT("level not found: ") + Level};
		return false;
	}
	if (UWorld* W = EditorWorld()) W->GetOutermost()->SetDirtyFlag(false);  // caller checked dirty/force
	if (!FEditorFileUtils::LoadMap(File, /*LoadAsTemplate*/ false, /*bShowProgress*/ false))
	{
		Err = {kIoError, TEXT("Unreal could not load ") + Level};
		return false;
	}
	if (GEditor->Trans) GEditor->Trans->Reset(LOCTEXT("TitanOpen", "TITAN level opened"));
	return true;
}

bool FSession::ProjectCreate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	FString Dir, Name;
	bool bForce = false;
	P->TryGetStringField(TEXT("path"), Dir);
	P->TryGetStringField(TEXT("name"), Name);
	P->TryGetBoolField(TEXT("force"), bForce);
	if (Dir.IsEmpty() || Name.IsEmpty())
	{
		Err = {kInvalidParams, TEXT("path and name are required")};
		return false;
	}
	if (Project && IsDirty() && !bForce)
	{
		Err = {kConflict, TEXT("current project has unsaved changes")};
		return false;
	}
	Dir = FPaths::ConvertRelativePathToFull(Dir);
	if (!IFileManager::Get().MakeDirectory(*Dir, true))
	{
		Err = {kIoError, TEXT("cannot create ") + Dir};
		return false;
	}
	const FString Safe = SanitizeName(Name);
	const FString Level = FString::Printf(TEXT("/Game/TitanProjects/%s_%08x/%s"), *Safe, FCrc::StrCrc32(*Dir), *Safe);

	// Start from Unreal's own template level (floor, sun, sky, fog) and add the TITAN starter objects.
	const double T0 = FPlatformTime::Seconds();
	if (UWorld* W = EditorWorld()) W->GetOutermost()->SetDirtyFlag(false);  // dirty/force checked above
	const FString Template = FPackageName::LongPackageNameToFilename(kTemplateLevel, FPackageName::GetMapPackageExtension());
	if (!(FPaths::FileExists(Template) && FEditorFileUtils::LoadMap(Template, /*LoadAsTemplate*/ true, false)))
		UEditorLoadingAndSavingUtils::NewBlankMap(false);
	UWorld* World = EditorWorld();
	if (!World)
	{
		Err = {kIoError, TEXT("no editor world")};
		return false;
	}
	SeedDefaultObjects(World);
	if (!UEditorLoadingAndSavingUtils::SaveMap(World, Level))
	{
		Err = {kIoError, TEXT("Unreal could not save the new level ") + Level};
		return false;
	}
	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("format"), TEXT("titan.project"));
	Manifest->SetNumberField(TEXT("formatVersion"), 1);
	Manifest->SetStringField(TEXT("name"), Name);
	Manifest->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
	Manifest->SetStringField(TEXT("backend"), TEXT("unreal"));
	Manifest->SetStringField(TEXT("level"), Level);
	if (!FFileHelper::SaveStringToFile(Serialize(Manifest, true) + TEXT("\n"), *(Dir / TEXT("project.titan")),
									   FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Err = {kIoError, TEXT("cannot write project.titan")};
		return false;
	}
	UE_LOG(LogTitanBridge, Log, TEXT("Created TITAN project %s -> %s in %.1f ms"), *Dir, *Level, (FPlatformTime::Seconds() - T0) * 1000.0);
	TSharedRef<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("path"), Dir);
	OpenParams->SetBoolField(TEXT("force"), true);
	return ProjectOpen(OpenParams, R, Ev, Err);
}

bool FSession::ProjectOpen(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	FString Dir, Level;
	bool bForce = false;
	P->TryGetStringField(TEXT("path"), Dir);
	P->TryGetStringField(TEXT("level"), Level);  // optional: open any existing Unreal level directly
	P->TryGetBoolField(TEXT("force"), bForce);
	if (Dir.IsEmpty() && Level.IsEmpty())
	{
		Err = {kInvalidParams, TEXT("path is required")};
		return false;
	}
	if (Project && IsDirty() && !bForce)
	{
		Err = {kConflict, TEXT("current project has unsaved changes")};
		return false;
	}
	FString Name;
	if (!Dir.IsEmpty())
	{
		Dir = FPaths::ConvertRelativePathToFull(Dir);
		TSharedPtr<FJsonObject> Manifest = ReadJsonFile(Dir / TEXT("project.titan"));
		if (!Manifest || Manifest->GetStringField(TEXT("format")) != TEXT("titan.project"))
		{
			Err = {kIoError, TEXT("not a TITAN project: ") + Dir};
			return false;
		}
		Name = Manifest->GetStringField(TEXT("name"));
		if (Level.IsEmpty() && !Manifest->TryGetStringField(TEXT("level"), Level))
		{
			Err = {kIoError, TEXT("this project was created by the TITAN reference host, not the Unreal backend")};
			return false;
		}
	}
	else
	{
		Name = FPackageName::GetShortName(Level);
	}
	if (!FPackageName::IsValidLongPackageName(Level))
	{
		Err = {kInvalidParams, TEXT("invalid Unreal level path ") + Level};
		return false;
	}
	const double T0 = FPlatformTime::Seconds();
	if (!OpenLevel(Level, Err)) return false;
	Metrics.LastOpenMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	Project = FProject{Dir, Name, Level};
	EditorState = MakeShared<FJsonObject>();
	ReadEditorState();
	Revision++;
	KnownIds.Reset();
	for (AActor* A : SceneActors()) KnownIds.Add(IdFor(A));
	Changed.Reset();
	bFullResync = false;
	Frames.OnWorldChanged();

	TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
	Info->SetStringField(TEXT("format"), TEXT("titan.project"));
	Info->SetStringField(TEXT("name"), Name);
	Info->SetStringField(TEXT("path"), Dir);
	Info->SetStringField(TEXT("level"), Level);
	Info->SetStringField(TEXT("backend"), TEXT("unreal"));
	R->SetObjectField(TEXT("project"), Info);
	R->SetObjectField(TEXT("scene"), SceneJson());
	R->SetObjectField(TEXT("editor"), EditorState);
	R->SetObjectField(TEXT("state"), StateJson());
	R->SetNumberField(TEXT("openMs"), Metrics.LastOpenMs);
	Ev.Add(MakeEvent(TEXT("project.state"), StateJson()));
	return true;
}

void FSession::SeedDefaultObjects(UWorld* World)
{
	struct FSeed
	{
		const TCHAR* Name;
		const TCHAR* Shape;
		TitanCoords::V3 Pos, Scale;
	};
	// TITAN starter objects (positions in TITAN metres), the same layout as Scene::makeDefault.
	const FSeed Seeds[] = {
		{TEXT("Hero Cube"), TEXT("cube"), {0, 0.5, 0}, {1, 1, 1}},
		{TEXT("Sphere"), TEXT("sphere"), {-2.2, 0.6, 0.4}, {1.2, 1.2, 1.2}},
		{TEXT("Pillar"), TEXT("cylinder"), {2.4, 1.0, -0.6}, {0.6, 2.0, 0.6}},
		{TEXT("Crate A"), TEXT("cube"), {-1.0, 0.3, 2.2}, {0.6, 0.6, 0.6}},
		{TEXT("Crate B"), TEXT("cube"), {0.2, 0.3, 2.6}, {0.6, 0.6, 0.6}},
	};
	for (const FSeed& S : Seeds)
	{
		const TitanCoords::V3 L = TitanCoords::TitanToUePosition(S.Pos);
		const TitanCoords::V3 Sc = TitanCoords::TitanToUeScale(S.Scale);
		FActorSpawnParameters Params;
		Params.bNoFail = true;
		AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(FVector(L.X, L.Y, L.Z), FRotator::ZeroRotator, Params);
		if (!A) continue;
		A->GetStaticMeshComponent()->SetStaticMesh(BasicShape(S.Shape));
		// Basic shapes are 100 cm with the pivot at the centre.
		A->SetActorScale3D(FVector(Sc.X, Sc.Y, Sc.Z));
		A->SetActorLabel(S.Name);
	}
	// The blank-map fallback has no lights; the template already has sun and sky.
	bool bHasLight = false;
	for (TActorIterator<ALight> It(World); It; ++It) bHasLight = true;
	if (!bHasLight)
	{
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 500), FRotator(-45, 30, 0));
		if (Sun) Sun->SetActorLabel(TEXT("Sun"));
		ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 300), FRotator::ZeroRotator);
		if (Sky) Sky->SetActorLabel(TEXT("Sky Light"));
	}
}

// ---- objects -------------------------------------------------------------------------------
void FSession::EndGesture()
{
	if (OpenGestureKey.IsEmpty()) return;
	OpenGestureKey.Reset();
	if (GEditor) GEditor->EndTransaction();
}

bool FSession::ObjectUpdate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	if (!Project)
	{
		Err = {kNoProject, TEXT("no project is open")};
		return false;
	}
	AActor* A = FindActor(uint64(P->GetNumberField(TEXT("id"))));
	if (!A)
	{
		Err = {kNotFound, TEXT("object not found")};
		EndGesture();
		return false;
	}
	const TSharedPtr<FJsonObject>* SetPtr = nullptr;
	if (!P->TryGetObjectField(TEXT("set"), SetPtr) || (*SetPtr)->Values.Num() == 0)
	{
		Err = {kInvalidParams, TEXT("'set' must be a non-empty object")};
		EndGesture();
		return false;
	}
	const TSharedPtr<FJsonObject>& Set = *SetPtr;

	// Validate everything before touching the actor, so a rejected edit changes nothing.
	TitanCoords::V3 Pos, Rot, Scale;
	GetLocal(A, Pos, Rot, Scale);
	bool bPos = false, bRot = false, bScale = false, bName = false, bVis = false, bIntensity = false;
	FString Name, E;
	bool bVisible = true;
	double Intensity = 0;
	for (const auto& KV : Set->Values)
	{
		// UE 5.8 keys FJsonObject::Values by UE::FSharedString (FString in older releases); both dereference
		// to a null-terminated TCHAR string.
		const FString Key(*KV.Key);
		if (Key == TEXT("transform"))
		{
			const TSharedPtr<FJsonObject>* T = nullptr;
			if (!Set->TryGetObjectField(TEXT("transform"), T))
			{
				Err = {kInvalidParams, TEXT("transform must be an object")};
				break;
			}
			bPos = ReadV3(*T, TEXT("position"), Pos, E);
			bRot = ReadV3(*T, TEXT("rotation"), Rot, E);
			bScale = ReadV3(*T, TEXT("scale"), Scale, E);
			if (!E.IsEmpty()) Err = {kInvalidParams, E};
			else if (bScale && (Scale.X == 0 || Scale.Y == 0 || Scale.Z == 0)) Err = {kInvalidParams, TEXT("scale components must be non-zero")};
		}
		else if (Key == TEXT("name"))
		{
			bName = Set->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty();
			if (!bName) Err = {kInvalidParams, TEXT("name must be non-empty")};
		}
		else if (Key == TEXT("visible"))
		{
			bVis = Set->TryGetBoolField(TEXT("visible"), bVisible);
			if (!bVis) Err = {kInvalidParams, TEXT("visible must be a boolean")};
		}
		else if (Key == TEXT("intensity"))
		{
			bIntensity = Set->TryGetNumberField(TEXT("intensity"), Intensity) && Cast<ALight>(A) != nullptr;
			if (!bIntensity) Err = {kInvalidParams, TEXT("intensity applies to lights only")};
		}
		else if (Key == TEXT("color"))
			Err = {kInvalidParams, TEXT("colour editing is not supported by the Unreal backend yet (edit the material in Unreal)")};
		else
			Err = {kInvalidParams, TEXT("property '") + Key + TEXT("' is not editable")};
		if (Err.Code != 0) break;
	}
	if (Err.Code != 0)
	{
		EndGesture();
		return false;
	}

	// Undo: one Unreal transaction per edit, or one per gesture when Studio sends a coalesce key.
	FString Coalesce;
	P->TryGetStringField(TEXT("coalesce"), Coalesce);
	TUniquePtr<FScopedTransaction> Scoped;
	if (Coalesce.IsEmpty() || Coalesce != OpenGestureKey) EndGesture();
	const FText Label = FText::Format(LOCTEXT("TitanEditFmt", "TITAN Studio: {0} {1}"),
									  FText::FromString(bName ? TEXT("Rename") : bPos || bRot || bScale ? TEXT("Transform") : TEXT("Edit")),
									  FText::FromString(A->GetActorLabel()));
	if (Coalesce.IsEmpty()) Scoped = MakeUnique<FScopedTransaction>(Label);
	else if (OpenGestureKey.IsEmpty())
	{
		GEditor->BeginTransaction(TEXT("TitanStudio"), Label, A);
		OpenGestureKey = Coalesce;
	}
	GestureLastTouch = FPlatformTime::Seconds();

	A->Modify();
	USceneComponent* Root = A->GetRootComponent();
	if (Root && (bPos || bRot || bScale))
	{
		Root->Modify();
		if (bPos)
		{
			const TitanCoords::V3 L = TitanCoords::TitanToUePosition(Pos);
			Root->SetRelativeLocation(FVector(L.X, L.Y, L.Z));
		}
		if (bRot)
		{
			double Pitch = 0, Yaw = 0, Roll = 0;
			TitanCoords::TitanEulerToUeRotator(Rot, Pitch, Yaw, Roll);
			Root->SetRelativeRotation(FRotator(Pitch, Yaw, Roll));
		}
		if (bScale)
		{
			const TitanCoords::V3 S = TitanCoords::TitanToUeScale(Scale);
			Root->SetRelativeScale3D(FVector(S.X, S.Y, S.Z));
		}
		A->PostEditMove(Coalesce.IsEmpty());
	}
	if (bName) A->SetActorLabel(Name);
	if (bVis)
	{
		A->bHiddenEd = !bVisible;
		A->MarkComponentsRenderStateDirty();
	}
	if (bIntensity)
		if (ULightComponent* LC = Cast<ALight>(A)->GetLightComponent())
		{
			LC->Modify();
			LC->SetIntensity(float(FMath::Max(0.0, Intensity)));
		}
	Scoped.Reset();

	MarkChanged(A);
	FlushChanges(Ev, TEXT("update"));
	Frames.Invalidate();
	R->SetObjectField(TEXT("object"), ActorJson(A));
	R->SetNumberField(TEXT("revision"), double(Revision));
	return true;
}

bool FSession::ObjectCreate(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	UWorld* World = EditorWorld();
	if (!Project || !World)
	{
		Err = {kNoProject, TEXT("no project is open")};
		return false;
	}
	FString Type = TEXT("mesh"), Shape = TEXT("cube"), Name;
	P->TryGetStringField(TEXT("type"), Type);
	P->TryGetStringField(TEXT("shape"), Shape);
	if (Type != TEXT("mesh") || !(Shape == TEXT("cube") || Shape == TEXT("sphere") || Shape == TEXT("cylinder") || Shape == TEXT("plane")))
	{
		Err = {kInvalidParams, TEXT("the Unreal backend creates mesh primitives: cube, sphere, cylinder, plane")};
		return false;
	}
	TitanCoords::V3 Pos{0, 0.5, 0}, Scale{1, 1, 1};
	const TSharedPtr<FJsonObject>* T = nullptr;
	FString E;
	if (P->TryGetObjectField(TEXT("transform"), T))
	{
		ReadV3(*T, TEXT("position"), Pos, E);
		ReadV3(*T, TEXT("scale"), Scale, E);
	}
	if (!E.IsEmpty())
	{
		Err = {kInvalidParams, E};
		return false;
	}
	const FScopedTransaction Tx(FText::Format(LOCTEXT("TitanCreate", "TITAN Studio: Create {0}"), FText::FromString(Shape)));
	World->GetCurrentLevel()->Modify();
	const TitanCoords::V3 L = TitanCoords::TitanToUePosition(Pos), S = TitanCoords::TitanToUeScale(Scale);
	FActorSpawnParameters Params;
	Params.bNoFail = true;
	AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(FVector(L.X, L.Y, L.Z), FRotator::ZeroRotator, Params);
	if (!A)
	{
		Err = {kIoError, TEXT("Unreal could not spawn the actor")};
		return false;
	}
	A->GetStaticMeshComponent()->SetStaticMesh(BasicShape(Shape));
	A->SetActorScale3D(FVector(S.X, S.Y, S.Z));
	if (!P->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		Name = FString::Printf(TEXT("%s %d"), *Shape.Left(1).ToUpper().Append(Shape.Mid(1)), SceneActors().Num());
	A->SetActorLabel(Name);
	MarkChanged(A);
	FlushChanges(Ev, TEXT("create"));
	Frames.Invalidate();
	R->SetNumberField(TEXT("id"), double(IdFor(A)));
	R->SetObjectField(TEXT("object"), ActorJson(A));
	return true;
}

bool FSession::ObjectDelete(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, TArray<TSharedRef<FJsonObject>>& Ev, FError& Err)
{
	UWorld* World = EditorWorld();
	if (!Project || !World)
	{
		Err = {kNoProject, TEXT("no project is open")};
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
	if (!P->TryGetArrayField(TEXT("ids"), Ids))
	{
		Err = {kInvalidParams, TEXT("ids must be an array")};
		return false;
	}
	TArray<AActor*> Actors;
	for (const TSharedPtr<FJsonValue>& V : *Ids)
	{
		AActor* A = FindActor(uint64(V->AsNumber()));
		if (!A)
		{
			Err = {kNotFound, TEXT("object not found")};
			return false;
		}
		Actors.AddUnique(A);
	}
	TArray<TSharedPtr<FJsonValue>> Deleted;
	{
		const FScopedTransaction Tx(LOCTEXT("TitanDelete", "TITAN Studio: Delete"));
		GEditor->SelectNone(false, true, false);
		for (AActor* A : Actors)
		{
			Deleted.Add(MakeShared<FJsonValueNumber>(double(IdFor(A))));
			A->Modify();
			World->EditorDestroyActor(A, /*bShouldModifyLevel*/ true);
		}
	}
	MarkFullResync();
	FlushChanges(Ev, TEXT("delete"));
	Frames.Invalidate();
	R->SetArrayField(TEXT("deleted"), Deleted);
	return true;
}

// ---- viewport ------------------------------------------------------------------------------
bool FSession::ViewportConfigure(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, FError& Err)
{
	double Width = 0, Height = 0;
	P->TryGetNumberField(TEXT("width"), Width);
	P->TryGetNumberField(TEXT("height"), Height);
	FString E;
	if (!Frames.Configure(uint32(Width), uint32(Height), E))
	{
		Err = {kInvalidParams, E};
		return false;
	}
	R->SetStringField(TEXT("shm"), Frames.ShmName());
	R->SetNumberField(TEXT("width"), Frames.Width());
	R->SetNumberField(TEXT("height"), Frames.Height());
	R->SetNumberField(TEXT("slots"), TITAN_FRAME_SLOTS);
	R->SetNumberField(TEXT("version"), TITAN_FRAME_VERSION);
	R->SetStringField(TEXT("format"), TEXT("bgra8-srgb"));
	return true;
}

bool FSession::ViewportCamera(const TSharedRef<FJsonObject>& P, TSharedRef<FJsonObject>& R, FError& Err)
{
	FCameraState C;
	FString E;
	if (!ReadV3(P, TEXT("eye"), C.Eye, E) || !ReadV3(P, TEXT("forward"), C.Forward, E))
	{
		Err = {kInvalidParams, E.IsEmpty() ? FString(TEXT("eye and forward are required")) : E};
		return false;
	}
	P->TryGetNumberField(TEXT("fovY"), C.FovYDeg);
	C.FovYDeg = FMath::Clamp(C.FovYDeg, 5.0, 150.0);
	const TArray<TSharedPtr<FJsonValue>>* View = nullptr;
	if (P->TryGetArrayField(TEXT("view"), View) && View->Num() == 16)
		for (int32 I = 0; I < 16; ++I) C.View[I] = float((*View)[I]->AsNumber());
	double Rev = 0;
	P->TryGetNumberField(TEXT("revision"), Rev);
	C.Revision = uint32(Rev);
	Frames.SetCamera(C);
	R->SetNumberField(TEXT("revision"), C.Revision);
	return true;
}

// ---- scene model ---------------------------------------------------------------------------
UWorld* FSession::EditorWorld() const { return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr; }

bool FSession::IsSceneActor(const AActor* A) const
{
	return A && !A->HasAnyFlags(RF_Transient | RF_ClassDefaultObject) && !A->IsTemplate() && A->IsEditable() &&
		   A->IsListedInSceneOutliner() && !A->IsA<AWorldSettings>() && IsValid(A);
}

TArray<AActor*> FSession::SceneActors() const
{
	TArray<AActor*> Out;
	if (UWorld* W = EditorWorld())
		for (TActorIterator<AActor> It(W); It; ++It)
			if (IsSceneActor(*It)) Out.Add(*It);
	return Out;
}

AActor* FSession::FindActor(uint64 Id) const
{
	for (AActor* A : SceneActors())
		if (IdFor(A) == Id) return A;
	return nullptr;
}

TSharedRef<FJsonObject> FSession::ActorJson(AActor* A) const
{
	TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("id"), double(IdFor(A)));
	O->SetStringField(TEXT("name"), A->GetActorLabel());
	const bool bLight = A->IsA<ALight>() || A->IsA<ASkyLight>();
	const TCHAR* Type = bLight ? TEXT("light") : A->IsA<ACameraActor>() ? TEXT("camera") : A->IsA<AStaticMeshActor>() ? TEXT("mesh") : TEXT("group");
	O->SetStringField(TEXT("type"), Type);
	O->SetStringField(TEXT("shape"), ShapeOf(A));
	O->SetStringField(TEXT("unrealClass"), A->GetClass()->GetName());
	const AActor* Parent = A->GetAttachParentActor();
	O->SetNumberField(TEXT("parent"), Parent && IsSceneActor(Parent) ? double(IdFor(Parent)) : 0.0);
	TitanCoords::V3 Pos, Rot, Scale;
	GetLocal(A, Pos, Rot, Scale);
	TSharedRef<FJsonObject> T = MakeShared<FJsonObject>();
	T->SetArrayField(TEXT("position"), Arr3(Pos));
	T->SetArrayField(TEXT("rotation"), Arr3(Rot));
	T->SetArrayField(TEXT("scale"), Arr3(Scale));
	O->SetObjectField(TEXT("transform"), T);
	O->SetArrayField(TEXT("color"), Arr3({0.72, 0.74, 0.77}));
	O->SetBoolField(TEXT("visible"), !A->bHiddenEd);
	if (const ALight* L = Cast<ALight>(A))
		if (const ULightComponent* LC = L->GetLightComponent()) O->SetNumberField(TEXT("intensity"), LC->Intensity);
	return O;
}

TSharedRef<FJsonObject> FSession::SceneJson() const
{
	TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
	S->SetStringField(TEXT("name"), Project ? Project->Name : FString());
	S->SetNumberField(TEXT("revision"), double(Revision));
	TSharedRef<FJsonObject> Env = MakeShared<FJsonObject>();
	Env->SetStringField(TEXT("renderer"), TEXT("unreal"));
	S->SetObjectField(TEXT("environment"), Env);
	TArray<TSharedPtr<FJsonValue>> Objects;
	for (AActor* A : SceneActors()) Objects.Add(MakeShared<FJsonValueObject>(ActorJson(A)));
	S->SetArrayField(TEXT("objects"), Objects);
	S->SetArrayField(TEXT("recipes"), TArray<TSharedPtr<FJsonValue>>());
	return S;
}

bool FSession::IsDirty() const
{
	UWorld* W = EditorWorld();
	return Project && W && W->GetOutermost()->IsDirty();
}

TArray<TSharedPtr<FJsonValue>> FSession::SelectionJson() const
{
	TArray<TSharedPtr<FJsonValue>> Ids;
	if (GEditor)
		for (FSelectionIterator It = GEditor->GetSelectedActorIterator(); It; ++It)
			if (const AActor* A = Cast<AActor>(*It))
				if (IsSceneActor(A)) Ids.Add(MakeShared<FJsonValueNumber>(double(IdFor(A))));
	return Ids;
}

TSharedRef<FJsonObject> FSession::StateJson() const
{
	TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
	S->SetBoolField(TEXT("projectOpen"), Project.IsSet());
	S->SetStringField(TEXT("path"), Project ? Project->Dir : FString());
	S->SetStringField(TEXT("name"), Project ? Project->Name : FString());
	S->SetStringField(TEXT("level"), Project ? Project->Level : FString());
	S->SetBoolField(TEXT("dirty"), IsDirty());
	S->SetNumberField(TEXT("revision"), double(Revision));
	UTransactor* Trans = GEditor ? GEditor->Trans.Get() : nullptr;
	const bool bCanUndo = Project && Trans && Trans->CanUndo();
	const bool bCanRedo = Project && Trans && Trans->CanRedo();
	S->SetBoolField(TEXT("canUndo"), bCanUndo);
	S->SetBoolField(TEXT("canRedo"), bCanRedo);
	S->SetStringField(TEXT("undoLabel"), bCanUndo ? Trans->GetUndoContext(true).Title.ToString() : FString());
	S->SetStringField(TEXT("redoLabel"), bCanRedo ? Trans->GetRedoContext().Title.ToString() : FString());
	S->SetArrayField(TEXT("selection"), SelectionJson());
	return S;
}

void FSession::MarkChanged(const AActor* A)
{
	if (!Project || !A || !IsSceneActor(A)) return;
	Changed.Add(IdFor(A));
	Frames.Invalidate();
}

void FSession::MarkFullResync()
{
	if (Project) bFullResync = true;
}

void FSession::FlushChanges(TArray<TSharedRef<FJsonObject>>& Events, const FString& Reason)
{
	if (!Project || (!bFullResync && Changed.Num() == 0)) return;
	TArray<TSharedPtr<FJsonValue>> Upserted, Removed;
	if (bFullResync)
	{
		TSet<uint64> Now;
		for (AActor* A : SceneActors())
		{
			Now.Add(IdFor(A));
			Upserted.Add(MakeShared<FJsonValueObject>(ActorJson(A)));
		}
		for (uint64 Id : KnownIds)
			if (!Now.Contains(Id)) Removed.Add(MakeShared<FJsonValueNumber>(double(Id)));
		KnownIds = MoveTemp(Now);
	}
	else
	{
		for (uint64 Id : Changed)
		{
			if (AActor* A = FindActor(Id))
			{
				Upserted.Add(MakeShared<FJsonValueObject>(ActorJson(A)));
				KnownIds.Add(Id);
			}
			else if (KnownIds.Remove(Id) > 0)
			{
				Removed.Add(MakeShared<FJsonValueNumber>(double(Id)));
			}
		}
	}
	Changed.Reset();
	bFullResync = false;
	Revision++;
	TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
	D->SetNumberField(TEXT("revision"), double(Revision));
	D->SetStringField(TEXT("reason"), Reason);
	D->SetArrayField(TEXT("upserted"), Upserted);
	D->SetArrayField(TEXT("removed"), Removed);
	D->SetArrayField(TEXT("recipes"), TArray<TSharedPtr<FJsonValue>>());
	D->SetObjectField(TEXT("state"), StateJson());
	Events.Add(MakeEvent(TEXT("scene.changed"), D));
}

void FSession::WriteEditorState() const
{
	if (!Project || Project->Dir.IsEmpty()) return;
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->Values = EditorState->Values;
	J->SetStringField(TEXT("format"), TEXT("titan.editor-state"));
	J->SetArrayField(TEXT("selection"), SelectionJson());
	const FString Path = Project->Dir / TEXT("Saved/editor.json");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const FString Tmp = Path + TEXT(".tmp");
	if (FFileHelper::SaveStringToFile(Serialize(J, true) + TEXT("\n"), *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		IFileManager::Get().Move(*Path, *Tmp, /*bReplace*/ true);
}

void FSession::ReadEditorState()
{
	if (!Project || Project->Dir.IsEmpty()) return;
	TSharedPtr<FJsonObject> J = ReadJsonFile(Project->Dir / TEXT("Saved/editor.json"));
	if (!J) return;
	const TSharedPtr<FJsonObject>* Cam = nullptr;
	if (J->TryGetObjectField(TEXT("camera"), Cam)) EditorState->SetObjectField(TEXT("camera"), *Cam);
	const TArray<TSharedPtr<FJsonValue>>* Sel = nullptr;
	if (J->TryGetArrayField(TEXT("selection"), Sel) && GEditor)
	{
		bApplyingOwnSelection = true;
		GEditor->SelectNone(false, true, false);
		for (const TSharedPtr<FJsonValue>& V : *Sel)
			if (AActor* A = FindActor(uint64(V->AsNumber()))) GEditor->SelectActor(A, true, false, true);
		GEditor->NoteSelectionChange();
		bApplyingOwnSelection = false;
	}
}
}  // namespace TitanBridge

#undef LOCTEXT_NAMESPACE
