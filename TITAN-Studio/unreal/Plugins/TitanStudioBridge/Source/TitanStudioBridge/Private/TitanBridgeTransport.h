// TITAN Studio Bridge transport. Original TITAN code.
// Unix domain socket (mode 0600, same-UID peers only), framing = 4-byte big-endian length + UTF-8 JSON,
// identical to the TITAN reference host. Non-blocking on both directions; game thread only.
#pragma once

#include "CoreMinimal.h"

namespace TitanBridge
{
class FTransport
{
public:
	~FTransport() { Close(); }

	bool Listen(const FString& Path);
	void Close();

	// Accepts a pending client (one at a time; a new client replaces a disconnected one), reads all
	// available bytes and returns complete frames. bOutConnected/bOutDisconnected report edges.
	void Poll(TArray<FString>& OutMessages, bool& bOutConnected, bool& bOutDisconnected);
	void Send(const FString& Json);
	void Flush();

	bool HasClient() const { return ClientFd >= 0; }
	const FString& GetPath() const { return SocketPath; }
	int64 BytesSent() const { return TotalSent; }
	int64 BytesQueued() const { return int64(Out.Num()) - OutOffset; }

	static constexpr uint32 kMaxFrame = 16u * 1024u * 1024u;  // same cap as titan::protocol::FrameDecoder

private:
	void DropClient();

	FString SocketPath;
	int ListenFd = -1;
	int ClientFd = -1;
	TArray<uint8> In;
	TArray<uint8> Out;
	int32 OutOffset = 0;
	int64 TotalSent = 0;
};
}  // namespace TitanBridge
