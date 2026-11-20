// TITAN Studio Bridge: serves the titan.editor v1 protocol from inside the Unreal Editor so TITAN
// Studio can drive the real Unreal Engine 5.8.4 runtime. Original TITAN code.
//
// Activation (the bridge stays dormant in ordinary editor sessions):
//   -TitanSocket=<path>     Unix socket to listen on (or env TITAN_BRIDGE_SOCKET)
//   -TitanParentPid=<pid>   exit the editor when TITAN Studio exits
//   -TitanMinimizeEditor    minimise Unreal's own windows once ready (Studio shows Unreal's frames)
// When ready the bridge prints "TITAN_HOST_READY <socket>" on stdout, like titan-engine-host.
#include "CoreMinimal.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "TitanBridgeLog.h"
#include "TitanBridgeSession.h"

DEFINE_LOG_CATEGORY(LogTitanBridge);

class FTitanStudioBridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		TitanBridge::FSessionOptions Options;
		if (!FParse::Value(FCommandLine::Get(), TEXT("TitanSocket="), Options.SocketPath))
			Options.SocketPath = FPlatformMisc::GetEnvironmentVariable(TEXT("TITAN_BRIDGE_SOCKET"));
		if (Options.SocketPath.IsEmpty())
		{
			UE_LOG(LogTitanBridge, Log, TEXT("TITAN Studio Bridge dormant (no -TitanSocket)"));
			return;
		}
		FParse::Value(FCommandLine::Get(), TEXT("TitanParentPid="), Options.ParentPid);
		Options.bMinimizeEditor = FParse::Param(FCommandLine::Get(), TEXT("TitanMinimizeEditor"));
		Session = MakeUnique<TitanBridge::FSession>(Options);
		if (!Session->Start())
		{
			Session.Reset();
			return;
		}
		if (GIsRunning) Session->OnEditorReady();
		else ReadyHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]() { if (Session) Session->OnEditorReady(); });
		// Tear down while GEditor, the editor world, the rendering thread and the RHI are all still alive.
		// ShutdownModule() runs after ShutdownRenderingThread() (LaunchEngineLoop.cpp, FEngineLoop::Exit), too
		// late to release GPU readbacks, the render target and the transient capture actor safely.
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([this]() { Session.Reset(); });
#else
		UE_LOG(LogTitanBridge, Warning, TEXT("TITAN Studio Bridge: transport not implemented on this platform"));
#endif
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(ReadyHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		Session.Reset();  // normally already released in OnEnginePreExit
	}

private:
	TUniquePtr<TitanBridge::FSession> Session;
	FDelegateHandle ReadyHandle;
	FDelegateHandle PreExitHandle;
};

IMPLEMENT_MODULE(FTitanStudioBridgeModule, TitanStudioBridge)
