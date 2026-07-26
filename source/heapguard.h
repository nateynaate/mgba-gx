#ifndef _HEAPGUARD_H_
#define _HEAPGUARD_H_

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostic build only - see heapguard.cpp for details.
 * Manually re-check a still-live allocation's guard bytes without waiting
 * for free(). Useful for suspect long-lived buffers (e.g. videoBuf,
 * gameScreenPng) - call this right before/after the operation you suspect
 * might be overrunning it. */
void HeapGuardCheckPointer(void *userPtr, size_t size, const char *label);

#ifdef __cplusplus
}
#endif

#endif
