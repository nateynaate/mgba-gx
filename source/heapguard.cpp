/****************************************************************************
 * mGBA-GX - heapguard.cpp
 *
 * DIAGNOSTIC BUILD ONLY - not for release.
 *
 * Wraps every malloc/free/realloc/calloc call in the entire binary (via
 * linker --wrap, so this catches allocations from mGBA's own prebuilt
 * library too, not just our own code) with guard-byte "redzones" before
 * and after each allocation. If anything overflows a heap buffer by even
 * one byte, the corrupted redzone is detected the moment that specific
 * allocation is freed - pinpointing which allocation was overrun, instead
 * of finding out only later when some unrelated malloc() call stumbles
 * into already-trashed free-list metadata (which is what the PC-inside-
 * _malloc_r crash we were chasing looks like).
 *
 * HOW TO ENABLE (add to the project's makefile, e.g. makefile.wii):
 *   LDFLAGS += -Wl,--wrap=malloc -Wl,--wrap=free -Wl,--wrap=realloc -Wl,--wrap=calloc
 * and add this file to the SOURCES for the diagnostic build.
 *
 * Every guard violation prints via printf (routed to USB Gecko if enabled
 * in vbagx.cpp's USBGeckoOutput(), otherwise stdout) with the allocation's
 * requested size and the pointer, so cross-referencing against the .map
 * file / recent code path tells us which malloc() call is being overrun.
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

extern "C" {

void *__real_malloc(size_t size);
void  __real_free(void *ptr);
void *__real_realloc(void *ptr, size_t size);
void *__real_calloc(size_t nmemb, size_t size);

/* Redzone layout per allocation:
 *   [ HEADER (16B, 32-byte aligned) ][ user data (size bytes) ][ FOOTER (16B) ]
 * HEADER holds a magic canary + the requested size (for the footer-offset
 * calc and for realloc's old-size lookup) + a second magic word.
 * FOOTER is just a repeated canary pattern, sized to comfortably catch
 * both small scalar overflows and off-by-a-few-words overflows without
 * being enormous - 16 bytes is plenty for a diagnostic build. */
#define GUARD_MAGIC_HEAD 0xDEADC0DEu
#define GUARD_MAGIC_FOOT 0xFEEDFACEu
#define GUARD_HEADER_SIZE 16
#define GUARD_FOOTER_SIZE 16

struct GuardHeader {
    uint32_t magicHead;
    uint32_t size;
    uint32_t magicHead2;
    uint32_t reserved;
};

static inline uint8_t *FooterPtr(void *userPtr, size_t size)
{
    return (uint8_t *)userPtr + size;
}

static void ReportCorruption(const char *who, void *userPtr, size_t size)
{
    printf("\n[HEAPGUARD] *** CORRUPTION DETECTED in %s ***\n", who);
    printf("[HEAPGUARD] allocation ptr=%p size=%u\n", userPtr, (unsigned)size);

    struct GuardHeader *hdr = (struct GuardHeader *)((uint8_t *)userPtr - GUARD_HEADER_SIZE);
    printf("[HEAPGUARD] header: magicHead=%08x (want %08x) magicHead2=%08x (want %08x)\n",
           hdr->magicHead, GUARD_MAGIC_HEAD, hdr->magicHead2, GUARD_MAGIC_HEAD);

    uint8_t *foot = FooterPtr(userPtr, size);
    printf("[HEAPGUARD] footer bytes: %02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x %02x %02x %02x %02x %02x %02x (want all ce/fa/ed/fe pattern)\n",
           foot[0],foot[1],foot[2],foot[3],foot[4],foot[5],foot[6],foot[7],
           foot[8],foot[9],foot[10],foot[11],foot[12],foot[13],foot[14],foot[15]);
    printf("[HEAPGUARD] ^ if header is intact but footer is trashed: overflow past the END\n");
    printf("[HEAPGUARD] ^ if header is trashed: underflow / wrote before the START\n\n");
}

static void CheckGuards(void *userPtr, const char *who)
{
    if (!userPtr) return;
    struct GuardHeader *hdr = (struct GuardHeader *)((uint8_t *)userPtr - GUARD_HEADER_SIZE);

    if (hdr->magicHead != GUARD_MAGIC_HEAD || hdr->magicHead2 != GUARD_MAGIC_HEAD) {
        ReportCorruption(who, userPtr, hdr->size);
        return;
    }

    uint8_t *foot = FooterPtr(userPtr, hdr->size);
    static const uint8_t pattern[4] = { 0xCE, 0xFA, 0xED, 0xFE }; /* GUARD_MAGIC_FOOT bytes */
    for (int i = 0; i < GUARD_FOOTER_SIZE; i++) {
        if (foot[i] != pattern[i % 4]) {
            ReportCorruption(who, userPtr, hdr->size);
            return;
        }
    }
}

void *__wrap_malloc(size_t size)
{
    size_t total = GUARD_HEADER_SIZE + size + GUARD_FOOTER_SIZE;
    uint8_t *base = (uint8_t *)__real_malloc(total);
    if (!base) return NULL;

    struct GuardHeader *hdr = (struct GuardHeader *)base;
    hdr->magicHead  = GUARD_MAGIC_HEAD;
    hdr->size       = (uint32_t)size;
    hdr->magicHead2 = GUARD_MAGIC_HEAD;
    hdr->reserved   = 0;

    void *userPtr = base + GUARD_HEADER_SIZE;
    uint8_t *foot = FooterPtr(userPtr, size);
    static const uint8_t pattern[4] = { 0xCE, 0xFA, 0xED, 0xFE };
    for (int i = 0; i < GUARD_FOOTER_SIZE; i++) foot[i] = pattern[i % 4];

    return userPtr;
}

void __wrap_free(void *ptr)
{
    if (!ptr) return;
    CheckGuards(ptr, "free()");
    uint8_t *base = (uint8_t *)ptr - GUARD_HEADER_SIZE;
    __real_free(base);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (!ptr) return __wrap_malloc(size);
    if (size == 0) { __wrap_free(ptr); return NULL; }

    CheckGuards(ptr, "realloc() [old block, before resize]");

    struct GuardHeader *oldHdr = (struct GuardHeader *)((uint8_t *)ptr - GUARD_HEADER_SIZE);
    size_t oldSize = oldHdr->size;

    void *newPtr = __wrap_malloc(size);
    if (!newPtr) return NULL;

    size_t copySize = oldSize < size ? oldSize : size;
    memcpy(newPtr, ptr, copySize);
    __wrap_free(ptr);
    return newPtr;
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size; /* diagnostic build only - not guarding overflow of this multiply */
    void *p = __wrap_malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* Call this periodically (e.g. once per frame from mgba_emuMain, or on
 * Home-menu entry) to sweep specific known-live pointers you're suspicious
 * of, without waiting for their free(). Pass any pointer previously
 * returned by __wrap_malloc/realloc/calloc and its requested size. */
void HeapGuardCheckPointer(void *userPtr, size_t size, const char *label)
{
    if (!userPtr) return;
    struct GuardHeader *hdr = (struct GuardHeader *)((uint8_t *)userPtr - GUARD_HEADER_SIZE);
    if (hdr->size != size) {
        printf("[HEAPGUARD] size mismatch checking '%s': tracked=%u passed=%u\n",
               label, hdr->size, (unsigned)size);
    }
    CheckGuards(userPtr, label);
}

} /* extern "C" */
