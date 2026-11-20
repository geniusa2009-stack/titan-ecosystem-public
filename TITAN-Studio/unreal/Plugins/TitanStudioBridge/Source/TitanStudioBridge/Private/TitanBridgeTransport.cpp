// TITAN Studio Bridge transport. Original TITAN code.
#include "TitanBridgeTransport.h"

#include "TitanBridgeLog.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace TitanBridge
{
static void SetNonBlocking(int Fd)
{
	fcntl(Fd, F_SETFL, fcntl(Fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
	int One = 1;
	setsockopt(Fd, SOL_SOCKET, SO_NOSIGPIPE, &One, sizeof(One));
#endif
}

bool FTransport::Listen(const FString& Path)
{
	Close();
	SocketPath = Path;
	const FTCHARToUTF8 Utf8(*Path);
	sockaddr_un Addr{};
	Addr.sun_family = AF_UNIX;
	if (Utf8.Length() >= int32(sizeof(Addr.sun_path)))
	{
		UE_LOG(LogTitanBridge, Error, TEXT("Socket path too long (%d bytes): %s"), Utf8.Length(), *Path);
		return false;
	}
	FMemory::Memcpy(Addr.sun_path, Utf8.Get(), Utf8.Length() + 1);
	ListenFd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ListenFd < 0) return false;
	unlink(Utf8.Get());
	const mode_t OldMask = umask(0177);  // socket file mode 0600
	const bool bBound = bind(ListenFd, reinterpret_cast<sockaddr*>(&Addr), sizeof(Addr)) == 0;
	umask(OldMask);
	if (!bBound || listen(ListenFd, 4) != 0)
	{
		UE_LOG(LogTitanBridge, Error, TEXT("Cannot listen on %s (errno %d)"), *Path, errno);
		Close();
		return false;
	}
	SetNonBlocking(ListenFd);
	return true;
}

void FTransport::Close()
{
	DropClient();
	if (ListenFd >= 0) close(ListenFd);
	ListenFd = -1;
	if (!SocketPath.IsEmpty()) unlink(TCHAR_TO_UTF8(*SocketPath));
}

void FTransport::DropClient()
{
	if (ClientFd >= 0) close(ClientFd);
	ClientFd = -1;
	In.Reset();
	Out.Reset();
	OutOffset = 0;
}

void FTransport::Poll(TArray<FString>& OutMessages, bool& bOutConnected, bool& bOutDisconnected)
{
	bOutConnected = bOutDisconnected = false;
	if (ListenFd < 0) return;
	if (ClientFd < 0)
	{
		const int Fd = accept(ListenFd, nullptr, nullptr);
		if (Fd < 0) return;
		uid_t Uid = 0;
		gid_t Gid = 0;
		if (getpeereid(Fd, &Uid, &Gid) != 0 || Uid != geteuid())
		{
			UE_LOG(LogTitanBridge, Warning, TEXT("Rejected a client owned by another user"));
			close(Fd);
			return;
		}
		SetNonBlocking(Fd);
		ClientFd = Fd;
		bOutConnected = true;
	}
	uint8 Buf[65536];
	for (;;)
	{
		const ssize_t N = recv(ClientFd, Buf, sizeof(Buf), 0);
		if (N > 0)
		{
			In.Append(Buf, int32(N));
			continue;
		}
		if (N == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
		{
			DropClient();
			bOutDisconnected = true;
			return;
		}
		break;
	}
	int32 Pos = 0;
	while (In.Num() - Pos >= 4)
	{
		const uint8* P = In.GetData() + Pos;
		const uint32 Len = (uint32(P[0]) << 24) | (uint32(P[1]) << 16) | (uint32(P[2]) << 8) | uint32(P[3]);
		if (Len > kMaxFrame)
		{
			UE_LOG(LogTitanBridge, Warning, TEXT("Oversized frame (%u bytes); dropping client"), Len);
			DropClient();
			bOutDisconnected = true;
			return;
		}
		if (uint32(In.Num() - Pos) < 4 + Len) break;
		OutMessages.Add(FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(P + 4), int32(Len))));
		Pos += int32(4 + Len);
	}
	if (Pos > 0) In.RemoveAt(0, Pos, EAllowShrinking::No);
}

void FTransport::Send(const FString& Json)
{
	if (ClientFd < 0) return;
	const FTCHARToUTF8 Utf8(*Json);
	const uint32 Len = uint32(Utf8.Length());
	const uint8 Hdr[4] = {uint8(Len >> 24), uint8(Len >> 16), uint8(Len >> 8), uint8(Len)};
	Out.Append(Hdr, 4);
	Out.Append(reinterpret_cast<const uint8*>(Utf8.Get()), int32(Len));
	Flush();
}

void FTransport::Flush()
{
	while (ClientFd >= 0 && OutOffset < Out.Num())
	{
		const ssize_t N = send(ClientFd, Out.GetData() + OutOffset, size_t(Out.Num() - OutOffset), 0);
		if (N > 0)
		{
			OutOffset += int32(N);
			TotalSent += N;
			continue;
		}
		if (N < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
		DropClient();
		return;
	}
	if (OutOffset == Out.Num())
	{
		Out.Reset();
		OutOffset = 0;
	}
	else if (Out.Num() - OutOffset > 64 * 1024 * 1024)
	{
		UE_LOG(LogTitanBridge, Warning, TEXT("Client is not reading (64 MiB queued); dropping it"));
		DropClient();
	}
}
}  // namespace TitanBridge
