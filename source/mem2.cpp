/****************************************************************************
 * Visual Boy Advance GX
 *
 * Tantric 2010-2023
 *
 * mem2.cpp
 *
 * MEM2 memory allocator
 ***************************************************************************/

#ifdef HW_RVL

#include <ogc/lwp_heap.h>
#include <ogc/system.h>
#include <stdio.h>

static heap_cntrl mem2_heap;

// romBuffer (vbasupport.cpp's LoadVBAROM()) separately carves ROM_BUFFER_RESERVE
// bytes off the LOW end of Arena2 via raw SYS_GetArena2Lo()/SYS_SetArena2Lo()
// manipulation - a completely different allocator than this LWP heap, sharing
// the same physical Arena2 region with no bounds checking between the two.
// This heap previously reserved a hardcoded 42MB from the HIGH end
// regardless of what romBuffer needed from the LOW end; combined with
// romBuffer's 32MB, that's 74MB requested against Arena2's actual ~51.7MB
// of usable space (Wii MEM2 is 64MB total, but IOS reserves the top
// 12-16MB - see WiiBrew's Memory Map page) - a ~22MB overlap, corrupting
// this heap's own bookkeeping whenever a large-enough ROM (any GBA game;
// GB/GBC ROMs are small enough to stay clear of the overlap) got read into
// romBuffer. Sizing this heap dynamically off the real Arena2 size, minus
// romBuffer's known requirement plus a safety margin, makes the two
// allocations provably non-overlapping instead of two independent guesses.
#define ROM_BUFFER_RESERVE (32 * 1024 * 1024) // must match vbasupport.cpp's romBufferSize (0x2000000)
#define MEM2_SAFETY_MARGIN (1 * 1024 * 1024)  // headroom for IOS/network/USB stack usage fluctuation

u32 InitMem2Manager () 
{
	u32 arena2Size = SYS_GetArena2Size();
	u32 reserved = ROM_BUFFER_RESERVE + MEM2_SAFETY_MARGIN;

	// Should never happen on real hardware, but fail loudly rather than
	// silently wrapping/underflowing into a huge bogus size if it ever did.
	if (reserved >= arena2Size) {
		printf("[mem2] FATAL: Arena2 (%u bytes) too small for romBuffer reserve (%u bytes)\n",
		       (unsigned)arena2Size, (unsigned)reserved);
		reserved = arena2Size / 2; // best-effort fallback so we still boot into something diagnosable
	}

	int size = (int)(arena2Size - reserved);
	void *arena2_hi = SYS_GetArena2Hi();
	void *mem2_heap_ptr = (void*)(((u32)arena2_hi - size) & ~31);
	SYS_SetArena2Hi(mem2_heap_ptr);
	size = __lwp_heap_init(&mem2_heap, mem2_heap_ptr, size, 32);
	return size;
}

void* mem2_malloc(u32 size)
{
	return __lwp_heap_allocate(&mem2_heap, size);
}

bool mem2_free(void *ptr)
{
	return __lwp_heap_free(&mem2_heap, ptr);
}

#endif
