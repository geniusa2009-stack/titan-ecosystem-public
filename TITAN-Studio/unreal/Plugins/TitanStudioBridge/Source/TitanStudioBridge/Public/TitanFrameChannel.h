// TITAN frame channel: shared-memory ring that carries rendered viewport frames from the engine process
// (TitanStudioBridge inside Unreal) to TITAN Studio. Original TITAN code.
//
// Single source of truth for the layout: included by the Unreal plugin and by TITAN Studio
// (studio/CMakeLists.txt adds this directory to the include path).
//
// Layout: [TitanFrameChannelHeader][slot 0 pixels][slot 1 pixels][slot 2 pixels]
//   * POSIX shared memory object (shm_open, mode 0600), name announced by viewport.configure.
//   * Pixels are BGRA8, sRGB-encoded (Unreal SCS_FinalColorLDR), rows of `stride` bytes, top row first.
//   * Per-slot seqlock: the writer sets `seq` odd before writing and even (= 2 * frame number) after.
//     A reader copies or uploads the pixels and re-reads `seq`; if it changed, the frame is dropped.
//   * The writer never writes the slot named by `latest_slot`, so the newest frame stays readable.
// Frame notification travels on the protocol socket as the event "viewport.frame".
//
// Ordering contract (both sides must follow it exactly):
//   writer: seq = 2F-1 (relaxed); release fence; pixels + metadata; seq = 2F (release);
//           latest_slot = slot (release); latest_frame = F (release).
//   reader: slot = latest_slot (acquire); s1 = seq (acquire), reject if 0 or odd; copy metadata + pixels;
//           acquire fence; s2 = seq (relaxed); accept only if s2 == s1.
//   The fences are compiler barriers and arm64 `dmb ish`. A release *store* of the odd seq would not stop
//   the pixel stores after it from becoming visible first, and an acquire *load* of s2 would not stop the
//   pixel loads before it from being satisfied late; either gap lets a torn frame pass with s1 == s2.
// Slot reuse: with 3 slots and `latest_slot` skipped, a reader that holds slot L can be lapped once the
//   writer has published two newer frames; that rewrite changes seq (frame numbers only grow, so values
//   never repeat: no ABA) and the reader drops the copy. `viewport.configure` with a new size creates a
//   new shared-memory object with a new name, so a mapping's geometry never changes after creation; the
//   reader validates it once at attach and uses its own copy.
// Language level: pixels are copied with plain memcpy on both sides, which the C++ memory model calls a
//   data race when a copy overlaps a rewrite. As in Linux seqcount readers, a racing copy is never used:
//   it is discarded by the s1 != s2 check, the metadata it reads is bounds-checked against the cached
//   geometry before any access, and it is only copied into a private buffer (no pointers are followed),
//   so torn values cannot cause an invalid access. Across processes the standard does not apply anyway;
//   the guarantees come from the fences above and the hardware memory model.
#pragma once

#include <stdint.h>

#define TITAN_FRAME_MAGIC 0x4E415254u  /* "TRAN" little-endian: TITAN frames */
#define TITAN_FRAME_VERSION 1u
#define TITAN_FRAME_SLOTS 3u
#define TITAN_FRAME_FORMAT_BGRA8_SRGB 1u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TitanFrameSlot
{
	uint64_t seq;            /* seqlock, see above */
	uint64_t frame;          /* monotonically increasing frame number */
	uint32_t width;
	uint32_t height;
	uint32_t stride;         /* bytes per row */
	uint32_t format;         /* TITAN_FRAME_FORMAT_* */
	uint64_t capture_ns;     /* engine monotonic clock when the capture was enqueued */
	uint64_t ready_ns;       /* engine monotonic clock when the pixels were written */
	double gpu_ms;           /* GPU time of the engine frame that produced it (0 if unknown) */
	float view[16];          /* TITAN view matrix used for this frame (column-major) */
	uint32_t camera_rev;     /* viewport.camera revision that produced the frame */
	uint32_t reserved[9];
} TitanFrameSlot;

typedef struct TitanFrameChannelHeader
{
	uint32_t magic;
	uint32_t version;
	uint32_t slot_count;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t header_bytes;   /* offset of slot 0 pixels */
	uint64_t slot_bytes;     /* bytes reserved per slot */
	uint32_t latest_slot;    /* slot holding the newest complete frame, or UINT32_MAX */
	uint32_t writer_pid;
	uint64_t latest_frame;
	TitanFrameSlot ring[TITAN_FRAME_SLOTS]; /* not "slots": that is a Qt keyword macro */
	uint8_t reserved[256];
} TitanFrameChannelHeader;

#ifdef __cplusplus
}
static_assert(sizeof(TitanFrameSlot) == 160, "frame slot layout changed: bump TITAN_FRAME_VERSION");
#endif

/* Header is padded to a page so pixel data is page-aligned. */
#define TITAN_FRAME_HEADER_BYTES 16384u
