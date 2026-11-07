// The Windows thread block (TEB) code compiled for Windows reaches through the GS register:
// its stack bounds (__chkstk), its TLS slots (gs:[0x58] - each DLL's thread-local data), the
// last error, the process block. Linux leaves GS free on x86-64, so each thread that calls
// into a DLL gets one of these, GS pointed at it (ensureThread).
#pragma once

#include <cstdint>

namespace ffxvk {

struct PeImage;

// a DLL's TLS slot (its index): each thread's copy of its template made as the thread enters
int registerTls(PeImage* img);
// this thread's TEB set up and GS on it (once a thread; cheap after)
void ensureThread();
uint32_t& lastError();

} // namespace ffxvk
