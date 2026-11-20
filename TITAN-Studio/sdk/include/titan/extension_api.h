/* TITAN Extensions SDK — native extension ABI, version 1. Original TITAN code.
 *
 * A native extension is a shared library plus a `titan-extension.json` manifest. The
 * host validates the manifest (identity, versions, platforms, dependencies, capabilities)
 * BEFORE loading the library, then resolves `titan_extension_entry`.
 *
 * The ABI is plain C with JSON strings so extensions can be built with any compiler/STL.
 *
 * SECURITY: native extensions run inside the host process with full process privileges.
 * The manifest is a compatibility contract, NOT a sandbox. Only load reviewed extensions.
 */
#ifndef TITAN_EXTENSION_API_H
#define TITAN_EXTENSION_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TITAN_EXTENSION_API_VERSION 1u

#if defined(_WIN32)
#define TITAN_EXTENSION_EXPORT __declspec(dllexport)
#else
#define TITAN_EXTENSION_EXPORT __attribute__((visibility("default")))
#endif

enum TitanLogLevel { TITAN_LOG_DEBUG = 0, TITAN_LOG_INFO = 1, TITAN_LOG_WARNING = 2, TITAN_LOG_ERROR = 3 };

/* Services the host offers to an extension. Valid from initialize() until shutdown(). */
typedef struct TitanHostApi {
    uint32_t api_version;
    const char* engine_version;
    void* host_context;
    void (*log)(void* host_context, int level, const char* message);
} TitanHostApi;

typedef struct TitanExtensionApi {
    uint32_t api_version; /* must equal TITAN_EXTENSION_API_VERSION */
    const char* id;       /* must equal the manifest id */
    /* Returns 0 on success. */
    int (*initialize)(const TitanHostApi* host);
    void (*shutdown)(void);
    /* Handles `command` with UTF-8 JSON params. Returns a UTF-8 JSON document that remains
     * valid until the next invoke() call or shutdown(). Errors are reported in-band as
     * {"error": {"message": "..."}}. Must not throw across the ABI boundary. */
    const char* (*invoke)(const char* command, const char* params_json);
} TitanExtensionApi;

typedef const TitanExtensionApi* (*TitanExtensionEntryFn)(void);

#define TITAN_EXTENSION_ENTRY_SYMBOL "titan_extension_entry"

#ifdef __cplusplus
}
#endif

#endif /* TITAN_EXTENSION_API_H */
