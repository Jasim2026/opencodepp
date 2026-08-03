// src/abi/version.cpp -- exported ABI version symbol.
//
// Gives opencodepp_shared a real exported symbol (dlopen-friendly) and seeds
// the build while engine sources are added phase by phase. Consumers normally
// use the header's static-inline `opencode_abi_version()`; this extern "C"
// twin exists so a host can query the version without including the header.
#include <cstdint>

#define OPENCODE_ABI_HEADER_ONLY /* omit the header's static-inline twin */
#include "opencode/opencode.h"

extern "C" {

uint32_t opencode_abi_version(void) { return OPENCODE_ABI_VERSION; }

} /* extern "C" */
