/* **********************************************************
 * Copyright (c) 2010-2012 Google, Inc.  All rights reserved.
 * Copyright (c) 2008-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/***************************************************************************
 * alloc.c: Dr. Memory heap tracking
 */

/* Malloc/free replacement w/ own routines vs using original versions: 
 * 
 * Pro original: 
 * * Must use original versions to apply to replayed execution 
 *   Note that the headers can function as mini-redzones for replay as well
 * * Don't have to duplicate all of the flags, alignment, features of 
 *   Windows heap allocators that apps might be depending on! 
 *   Windows heaps are more complex than Unix: multiple heaps, 
 *   extra features like zeroing, etc. 
 *   And never know whether some other part of system is going to 
 *   make calls into heap subsystem beyond just malloc+free. 
 * 
 * Con original: 
 * * Alloc code is instrumented but must ignore its accesses to headers 
 * * Harder to delay frees 
 * * Don't know header flags: but can find sizes by storing in redzone 
 *   or (replay-compatible) calling malloc_usable_size() (Linux only)
 *   (RtlSizeHeap returns asked-for size)
 */

/*
System call and library routines we need to watch:

* NtMapViewOfSection => addressable and defined, whether image or data file
* NtUnmapViewOfSection => no longer addressable
* NtAllocateVirtualMemory: if for VirtualAlloc, then addressable and defined;
  if for HeapAlloc, then neither.  I'm using my reserve-then-commit heuristic
  to distinguish, combined w/ the currently-in-heap-routine var.
* MapUserPhysicalPages: NYI for now

* kernel32!Heap* calls ntdll!Rtl*Heap; can probably ignore everything
  the kernel32 routines do

* RtlAllocateHeap => addressable; if pass HEAP_ZERO_MEMORY then defined as well.
* RtlReAllocateHeap => transfer current addressability and definedness state
* RtlFreeHeap => no longer defined or addressable
* alloca => just adjusts esp so we need do nothing special except for its
  guard page probes

* RtlCreateHeap, RtlDestroyHeap: to suppress false positives where
  heap routines that aren't exported (like RtlpHeapIsLocked) read from
  heap headers, we track the heap regions.  We also track all exported
  heap routine entry and exits to suppress their heap reads and
  writes.  We also use these to distinguish VirtualAlloc from HeapAlloc.
  How track heaps?  RtlCreateHeap => single reservation, so can
  use return value and query to find the extent: but can have subheaps
  chained together, so we watch the syscalls for both reserving and freeing.

  FIXME: verify that RtlDestroyHeap calls NtFreeVirtualMemory and I
  catch its heap region removal: for sub-heaps have to be there,
  unless reverse-engineer heap format.  Ask Sam if he did sub-heaps.

* RtlCompactHeap: should be able to ignore:
  "compacts the heap by coalescing adjacent free blocks of memory and
  decommitting large free blocks of memory"
* RtlExtendHeap: no msdn pages on it: probably just increases maximum size of
  heap itself, not of individual allocations
* RtlWalkHeap: could use that plus GetProcessHeaps to walk all heaps at
  init time

* LocalAlloc: LMEM_ZEROINIT = 0x0040
  FIXME: LMEM_MOVEABLE is strange: LocalLock locks it to an address, and LocalReAlloc 
  makes it moveable again: what's the purpose? 
  It looks like it ends up calling RtlAllocateHeap, and LocalReAlloc calls
  RtlReAllocateHeap, so we can probably just watch the Rtl routines.
  FIXME: I don't see how 0x40 is translated to 0x8 for zeroing.
  I do see 0x140000 being or-ed in: so it asks for +x, and some secret flag.
* ditto with GlobalAlloc

* STL strings use custom allocation but there is a flag to turn that
  off at compile time?

* TLS slots: keep unaddressable until kernel32!TlsAlloc.
  Or, at unaddressable exception time, check the bit.

* outer layers sometimes add debug features including their own redzones:
  so we need to intercept the outermost layer (PR 476805)

* cygwin malloc uses its own heap (PR 476805, PR 595798)

* various additional heap routines (PR 406323: memalign, valloc, pvalloc, etc.,
  PR 595802: _recalloc, _aligned_offset_malloc, etc.)

msvcrt!malloc =>
msvcrt!_heap_alloc+0xd1:
77c2c3c3 ff15f410c177     call  dword ptr [msvcrt!_imp__HeapAlloc (77c110f4)]
0:001> U @@(*(int *)0x77c110f4)
ntdll!RtlAllocateHeap:

HeapAlloc: http://msdn2.microsoft.com/en-us/library/aa366597.aspx
RtlAllocateHeap is presumably identical since we have forwarders:
  LPVOID WINAPI HeapAlloc(
    __in          HANDLE hHeap,
    __in          DWORD dwFlags,
    __in          SIZE_T dwBytes
  );
flag to pay attention to is HEAP_ZERO_MEMORY = 0x00000008
If the function succeeds, the return value is a pointer to the allocated memory block.
 */

/* When RtlAllocateHeap/malloc pad the asked-for size to a certain alignment,
 * better to use the size the app asked for since it shouldn't write to the
 * extra space anyway.  RtlSizeHeap returns the asked-for space, but
 * malloc_usable_size returns the padded size.
 */

#include "dr_api.h"
#include "drmgr.h"
#include "drwrap.h"
#include "alloc.h"
#include "alloc_private.h"
#include "alloc_replace.h"
#include "heap.h"
#include "callstack.h"
#include "redblack.h"
#ifdef USE_DRSYMS
# include "drsyms.h"
# include "symcache.h"
#endif
#ifdef LINUX
# include "sysnum_linux.h"
# include <sys/mman.h>
#else
# include "windefs.h"
# include "../wininc/crtdbg.h"
#endif
#include "asm_utils.h"
#include <string.h>

#define DR_MC_GPR (DR_MC_INTEGER | DR_MC_CONTROL)

#ifdef LINUX
typedef struct {
    unsigned long addr;
    unsigned long len;
    unsigned long prot;
    unsigned long flags;
    unsigned long fd;
    unsigned long offset;
} mmap_arg_struct_t;
#endif

/* Options currently all have 0 as default value */
alloc_options_t alloc_ops;

#ifdef WINDOWS
/* system calls we want to intercept */
int sysnum_mmap = -1;
int sysnum_mapcmf = -1;
int sysnum_munmap = -1;
int sysnum_valloc = -1;
int sysnum_vfree = -1;
int sysnum_cbret = -1;
int sysnum_continue = -1;
int sysnum_setcontext = -1;
int sysnum_RaiseException = -1;
int sysnum_UserConnectToServer = -1;
#endif

#ifdef STATISTICS
/* XXX: we used to have stats on #wraps and #flushes but no longer
 * since that's inside drwrap
 */
uint num_mallocs;
uint num_large_mallocs;
uint num_frees;
#endif

/* points at the per-malloc API to use */
malloc_interface_t malloc_interface;

#ifdef WINDOWS
/* i#607 part A: is msvcr*d.dll present, yet we do not have symbols? */
static bool dbgcrt_nosyms;
#endif

#ifdef LINUX
byte *
get_brk(bool pre_us)
{
    if (pre_us && alloc_ops.replace_malloc)
        return alloc_replace_orig_brk();
    return (byte *) raw_syscall(SYS_brk, 1, 0);
}

byte *
set_brk(byte *new_val)
{
    return (byte *) raw_syscall(SYS_brk, 1, (ptr_int_t) new_val);
}
#endif

static void
alloc_hook(void *wrapcxt, INOUT void **user_data);

static void
handle_alloc_post(void *wrapcxt, void *user_data);

#ifdef WINDOWS
static void
alloc_wrap_exception(void *wrapcxt, void OUT **user_data);

static void
alloc_wrap_Ki(void *wrapcxt, void OUT **user_data);
#endif

static dr_emit_flags_t
alloc_event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                        bool for_trace, bool translating, OUT void **user_data);

static dr_emit_flags_t
alloc_event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *inst,
                      bool for_trace, bool translating, void *user_data);

static bool
malloc_lock_held_by_self(void);

static void
malloc_wrap_init(void);

/***************************************************************************
 * PER-THREAD DATA
 */

/* all our data is callback-private */
int cls_idx_alloc = -1;

#define MAX_HEAP_NESTING 12

typedef struct _cls_alloc_t {
    /* communicating from pre to post alloc routines */
#ifdef LINUX
    app_pc sbrk;
#else
    ptr_int_t auxarg; /* heap or blocktype or generic additional arg */
#endif
    uint alloc_flags;
    size_t alloc_size;
    size_t realloc_old_size;
    size_t realloc_replace_size;
    app_pc alloc_base;
    bool syscall_this_process;
    /* we need to split these to handle cases like exception inside RtlFreeHeap */
    bool expect_sys_to_fail;
    bool expect_lib_to_fail;
    uint valloc_type;
    bool valloc_commit;
    app_pc munmap_base;
    /* indicates thread is inside a heap creation routine */
    int in_heap_routine;
    /* at what value of in_heap_routine did we adjust heap routine args?
     * (we only allow one level of recursion to do so)
     */
    int in_heap_adjusted;
    bool in_realloc;
    /* record which heap routine */
    app_pc last_alloc_routine[MAX_HEAP_NESTING];
    void *last_alloc_info[MAX_HEAP_NESTING];
    bool ignored_alloc;
    app_pc alloc_being_freed; /* handles post-pre-free actions */
    /* record which outer layer was used to allocate (i#123) */
    uint allocator;
    /* present the outer layer as the top of the allocation call stack,
     * regardless of how many inner layers we went through (i#913)
     */
    app_pc outer_retaddr;
    reg_t outer_xbp;
    reg_t outer_xsp;
    reg_t xbp_tmp;
    reg_t xsp_tmp;
#ifdef WINDOWS
    /* avoid deliberate mismatches from _DebugHeapDelete<*> being used instead of
     * operator delete* (i#722,i#655)
     */
    bool ignore_next_mismatch;
#endif
    bool in_calloc;
    bool malloc_from_calloc;
#ifdef WINDOWS
    bool in_create; /* are we inside RtlCreateHeap */
    bool malloc_from_realloc;
    bool heap_tangent; /* not a callback but a heap tangent (i#301) */
    HANDLE heap_handle; /* used to identify the Heap of new allocations */

    bool in_seh; /* track whether handling an exception */
#endif
} cls_alloc_t;

static void
alloc_context_init(void *drcontext, bool new_depth)
{
    cls_alloc_t *data;
    if (new_depth) {
        data = (cls_alloc_t *) thread_alloc(drcontext, sizeof(*data), HEAPSTAT_MISC);
        drmgr_set_cls_field(drcontext, cls_idx_alloc, data);
    } else
        data = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    memset(data, 0, sizeof(*data));
}

static void
alloc_context_exit(void *drcontext, bool thread_exit)
{
    if (thread_exit) {
        cls_alloc_t *data = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
        thread_free(drcontext, data, sizeof(*data), HEAPSTAT_MISC);
    }
    /* else, nothing to do: we leave the struct for re-use on next callback */
}

/***************************************************************************
 * MALLOC ROUTINES
 */

/* We need to track multiple sets of library routines and multiple layers
 * (xref PR 476805, DRi#284) so we need a hashtable of entry points
 */
#define ALLOC_ROUTINE_TABLE_HASH_BITS 6
static hashtable_t alloc_routine_table;
static void *alloc_routine_lock; /* protects alloc_routine_table */

/* Itanium ABI manglings */
/* operator new(unsigned int) */
#define MANGLED_NAME_NEW                    "_Znwj"
/* operator new[](unsigned int) */
#define MANGLED_NAME_NEW_ARRAY              "_Znaj"
/* operator new(unsigned int, std::nothrow_t const&) */
#define MANGLED_NAME_NEW_NOTHROW            "_ZnwjRKSt9nothrow_t"
/* operator new[](unsigned int, std::nothrow_t const&) */
#define MANGLED_NAME_NEW_ARRAY_NOTHROW      "_ZnajRKSt9nothrow_t"
/* operator new(std::size_t, void* __p) */
#define MANGLED_NAME_NEW_PLACEMENT          "_ZnwjPv"
/* operator new[](std::size_t, void* __p) */
#define MANGLED_NAME_NEW_ARRAY_PLACEMENT    "_ZnajPv"
/* operator delete(void*) */
#define MANGLED_NAME_DELETE                 "_ZdlPv"
/* operator delete[](void*) */
#define MANGLED_NAME_DELETE_ARRAY           "_ZdaPv"
/* operator delete(void*, std::nothrow_t const&) */
#define MANGLED_NAME_DELETE_NOTHROW         "_ZdlPvRKSt9nothrow_t"
/* operator delete[](void*, std::nothrow_t const&) */
#define MANGLED_NAME_DELETE_ARRAY_NOTHROW   "_ZdaPvRKSt9nothrow_t"
/* operator delete(void*, void*) */
#define MANGLED_NAME_DELETE_PLACEMENT       "_ZdlPvS_"
/* operator delete[](void*, void*) */
#define MANGLED_NAME_DELETE_ARRAY_PLACEMENT "_ZdaPvS_"

static inline bool
routine_needs_post_wrap(routine_type_t type, heapset_type_t set_type)
{
    /* i#674: don't bother to do post wrap for delete since there are
     * a lot of call sites there.  we do need post for new to
     * distinguish placement new.  an alternative would be to get
     * function params from drsyms: but I'm not sure I trust that
     * as there could be some global new overload that doesn't
     * call malloc that would mess us up anyway.
     * We do need to intercept post so we can have in_heap_routine set
     * for operator delete if using MSVC debug crt (i#26).
     */
#ifdef WINDOWS
    if (type == HEAP_ROUTINE_DebugHeapDelete)
        return false;
#endif
    return !is_delete_routine(type) IF_WINDOWS(|| set_type == HEAPSET_CPP_DBG);
}

typedef struct _possible_alloc_routine_t {
    const char *name;
    routine_type_t type;
} possible_alloc_routine_t;

static const possible_alloc_routine_t possible_libc_routines[] = {
    /* when non-exported routines are added here, add to the regex list in
     * find_alloc_routines() to reduce # symbol lookups (i#315)
     */
    { "malloc_usable_size", HEAP_ROUTINE_SIZE_USABLE },
#ifdef WINDOWS
    { "_msize", HEAP_ROUTINE_SIZE_REQUESTED },
#endif
    { "malloc", HEAP_ROUTINE_MALLOC },
    { "realloc", HEAP_ROUTINE_REALLOC }, 
    { "free", HEAP_ROUTINE_FREE },
    { "calloc", HEAP_ROUTINE_CALLOC },
    /* for cfree we ignore 2 extra args if there are any, as glibc itself does */
    { "cfree", HEAP_ROUTINE_FREE },
    /* XXX PR 406323: not supported yet */
    { "posix_memalign", HEAP_ROUTINE_NOT_HANDLED },
    { "memalign", HEAP_ROUTINE_NOT_HANDLED },
    { "valloc", HEAP_ROUTINE_NOT_HANDLED },
    { "pvalloc", HEAP_ROUTINE_NOT_HANDLED },
    /* we do not change args or return val for these: we simply allow
     * them to access heap headers.  returned stats will be inflated
     * by redzones: oh well.
     */
    { "mallopt",              HEAP_ROUTINE_STATS },
    { "mallinfo",             HEAP_ROUTINE_STATS },
    { "malloc_stats",         HEAP_ROUTINE_STATS },
    { "malloc_trim",          HEAP_ROUTINE_STATS },
    { "malloc_get_state",     HEAP_ROUTINE_STATS },
    /* XXX PR 406323: not supported yet */
    { "malloc_set_state",     HEAP_ROUTINE_NOT_HANDLED },
    { "independent_calloc",   HEAP_ROUTINE_NOT_HANDLED },
    { "independent_comalloc", HEAP_ROUTINE_NOT_HANDLED },
    /* FIXME PR 406323: memalign, valloc, pvalloc, etc. */
#ifdef WINDOWS
    /* the _impl versions are sometimes called directly (i#31)
     * XXX: there are also _base versions but they always call _impl?
     */
    { "malloc_impl", HEAP_ROUTINE_MALLOC },
    { "realloc_impl", HEAP_ROUTINE_REALLOC }, 
    { "free_impl", HEAP_ROUTINE_FREE },
    { "calloc_impl", HEAP_ROUTINE_CALLOC },
    /* for VS2010 I see this (but no other _*_impl: looking at the crt
     * sources confirms it), as well as a layer of _*_crt routines
     * that just call _impl: perhaps replacing the prior _base
     * versions (i#940)
     */
    { "_calloc_impl", HEAP_ROUTINE_CALLOC },
    /* for cygwin */
    { "sbrk", HEAP_ROUTINE_SBRK },
    /* FIXME PR 595802: _recalloc, _aligned_offset_malloc, etc. */
#endif
};
#define POSSIBLE_LIBC_ROUTINE_NUM \
    (sizeof(possible_libc_routines)/sizeof(possible_libc_routines[0]))

#define OPERATOR_ENTRIES 4 /* new, new[], delete, delete[] */

static const possible_alloc_routine_t possible_cpp_routines[] = {
#ifdef USE_DRSYMS
    /* XXX: currently drsyms does NOT include function params, which is what
     * we want here as we want to include all overloads in symcache but
     * be able to easily enumerate them from function name only.
     * Once including params is an option in drsyms we need to ensure
     * we turn it off for these lookups.
     */
    /* XXX i#633: do we need to handle this?
     * cs2bug!`operator new'::`6'::`dynamic atexit destructor for 'nomem'' ( void )
     */
    /* We distinguish HEAP_ROUTINE_*_NOTHROW, as well as placement new, after
     * we look these symbols up 
     */
    { "operator new",      HEAP_ROUTINE_NEW },
    { "operator new[]",    HEAP_ROUTINE_NEW_ARRAY },
    { "operator delete",   HEAP_ROUTINE_DELETE },
    { "operator delete[]", HEAP_ROUTINE_DELETE_ARRAY },
    /* These are the names we store in the symcache to distinguish from regular
     * operators for i#882.
     * This means we are storing something different from the actual symbol names.
     * We assume that these 4 (== OPERATOR_ENTRIE) entries immediately
     * follow the 4 above!
     */
    { "operator new nothrow",      HEAP_ROUTINE_NEW_NOTHROW },
    { "operator new[] nothrow",    HEAP_ROUTINE_NEW_ARRAY_NOTHROW },
    { "operator delete nothrow",   HEAP_ROUTINE_DELETE_NOTHROW },
    { "operator delete[] nothrow", HEAP_ROUTINE_DELETE_ARRAY_NOTHROW },
# ifdef WINDOWS
    /* This is the name we store in the symcache for i#722.
     * This means we are storing something different from the actual symbol names.
     */
    { "std::_DebugHeapDelete<>", HEAP_ROUTINE_DebugHeapDelete },
# endif
#else
    /* Until we have drsyms on Linux/Cygwin for enumeration, we look up
     * the standard Itanium ABI/VS manglings for the standard operators.
     * XXX: we'll miss overloads that add more args.
     * XXX: we assume drsyms will find and de-mangle exports
     * in stripped modules so that when we have drsyms we can ignore
     * these manglings.
     */
# ifdef LINUX
    { MANGLED_NAME_NEW,                  HEAP_ROUTINE_NEW },
    { MANGLED_NAME_NEW_ARRAY,            HEAP_ROUTINE_NEW_ARRAY },
    { MANGLED_NAME_NEW_NOTHROW,          HEAP_ROUTINE_NEW_NOTHROW },
    { MANGLED_NAME_NEW_ARRAY_NOTHROW,    HEAP_ROUTINE_NEW_ARRAY_NOTHROW },
    { MANGLED_NAME_DELETE,               HEAP_ROUTINE_DELETE },
    { MANGLED_NAME_DELETE_ARRAY,         HEAP_ROUTINE_DELETE_ARRAY },
    { MANGLED_NAME_DELETE_NOTHROW,       HEAP_ROUTINE_DELETE_NOTHROW },
    { MANGLED_NAME_DELETE_ARRAY_NOTHROW, HEAP_ROUTINE_DELETE_ARRAY_NOTHROW },
# else
    /* operator new(unsigned int) */
    { "??2@YAPAXI@Z",       HEAP_ROUTINE_NEW },
    /* operator new(unsigned int,int,char const *,int) */
    { "??2@YAPAXIHPBDH@Z",  HEAP_ROUTINE_NEW },
    /* operator new[](unsigned int) */
    { "??_U@YAPAXI@Z",      HEAP_ROUTINE_NEW_ARRAY },
    /* operator new[](unsigned int,int,char const *,int) */
    { "??_U@YAPAXIHPBDH@Z", HEAP_ROUTINE_NEW_ARRAY },
    /* operator delete(void *) */
    { "??3@YAXPAX@Z",       HEAP_ROUTINE_DELETE },
    /* operator delete[](void *) */
    { "??_V@YAXPAX@Z",      HEAP_ROUTINE_DELETE_ARRAY },
    /* XXX: we don't support nothrow operators w/o USE_DRSYMS */
# endif
#endif /* USE_DRSYMS */
};
# define POSSIBLE_CPP_ROUTINE_NUM \
    (sizeof(possible_cpp_routines)/sizeof(possible_cpp_routines[0]))

static const char *
translate_routine_name(const char *name)
{
#ifndef USE_DRSYMS
    /* temporary until we have online syms */
    /* could add to table but doesn't seem worth adding a whole new field */
    if (strcmp(name, IF_WINDOWS_ELSE("??2@YAPAXI@Z", MANGLED_NAME_NEW)) == 0 ||
        strcmp(name, IF_WINDOWS_ELSE("??2@YAPAXIHPBDH@Z",
                                     MANGLED_NAME_NEW_NOTHROW)) == 0)
        return "operator new";
    else if (strcmp(name, IF_WINDOWS_ELSE("??_U@YAPAXI@Z",
                                          MANGLED_NAME_NEW_ARRAY)) == 0 ||
             strcmp(name, IF_WINDOWS_ELSE("??_U@YAPAXIHPBDH@Z",
                                           MANGLED_NAME_NEW_ARRAY_NOTHROW)) == 0)
        return "operator new[]";
    if (strcmp(name, IF_WINDOWS_ELSE("??3@YAXPAX@Z", MANGLED_NAME_DELETE)) == 0
        IF_LINUX(|| strcmp(name, MANGLED_NAME_DELETE_NOTHROW) == 0))
        return "operator delete";
    else if (strcmp(name, IF_WINDOWS_ELSE("??_V@YAXPAX@Z",
                                          MANGLED_NAME_DELETE_ARRAY)) == 0
             IF_LINUX(|| strcmp(name,  MANGLED_NAME_DELETE_ARRAY_NOTHROW) == 0))
        return "operator delete[]";
#endif
    return name;
}

#ifdef WINDOWS
static const possible_alloc_routine_t possible_crtdbg_routines[] = {
    { "_msize_dbg", HEAP_ROUTINE_SIZE_REQUESTED_DBG },
    { "_malloc_dbg", HEAP_ROUTINE_MALLOC_DBG },
    { "_realloc_dbg", HEAP_ROUTINE_REALLOC_DBG }, 
    { "_free_dbg", HEAP_ROUTINE_FREE_DBG },
    { "_calloc_dbg", HEAP_ROUTINE_CALLOC_DBG },
    /* _nh_malloc_dbg is called directly by debug operator new (i#500) */
    { "_nh_malloc_dbg", HEAP_ROUTINE_MALLOC_DBG },
    /* the _impl versions are sometimes called directly (i#31, i#606)
     * XXX: there are also _base versions but they always call _impl?
     */
    { "_malloc_dbg_impl", HEAP_ROUTINE_MALLOC_DBG },
    { "_realloc_dbg_impl", HEAP_ROUTINE_REALLOC_DBG }, 
    { "_free_dbg_impl", HEAP_ROUTINE_FREE_DBG },
    { "_calloc_dbg_impl", HEAP_ROUTINE_CALLOC_DBG },
    /* to control the debug crt options (i#51) */
    { "_CrtSetDbgFlag", HEAP_ROUTINE_SET_DBG },
    /* the dbgflag only controls full-heap scans: to disable checks on
     * malloc and free we disable the reporting routines.  this is a hack
     * and may suppress other errors we might want to see: but the
     * alternative is to completely replace _dbg (see i#51 notes below).
     */
    { "_CrtDbgReport", HEAP_ROUTINE_DBG_NOP_FALSE },
    /* I've seen two instances of _CrtDbgReportW but usually both end up
     * calling _CrtDbgReportWV so ok to replace just one
     */
    { "_CrtDbgReportW", HEAP_ROUTINE_DBG_NOP_FALSE },
    { "_CrtDbgReportV", HEAP_ROUTINE_DBG_NOP_FALSE },
    { "_CrtDbgReportWV", HEAP_ROUTINE_DBG_NOP_FALSE },
    { "_CrtDbgBreak", HEAP_ROUTINE_DBG_NOP_FALSE },
    /* We avoid perf problems (i#977) by turning this check into a nop.
     * Note that this mostly obviates the need to nop _CrtDbgReport*.
     * XXX: there are some non-assert uses of this.  If we ever find
     * a problem with those, we may want to instead nop out RtlValidateHeap,
     * but that may have its own non-debug-check uses.
     * This is an export.
     */
    { "_CrtIsValidHeapPointer", HEAP_ROUTINE_DBG_NOP_TRUE },
    /* We avoid perf problems w/ heap header accesses by treating
     * as a heap header (it directly calls _nh_malloc_dbg: i#997).
     * This is an export.
     */
    { "_getptd", HEAP_ROUTINE_GETPTD },
    /* FIXME PR 595802: _recalloc_dbg, _aligned_offset_malloc_dbg, etc. */
};
#define POSSIBLE_CRTDBG_ROUTINE_NUM \
    (sizeof(possible_crtdbg_routines)/sizeof(possible_crtdbg_routines[0]))

/* for i#51 so we can disable crtdbg checks */
#define CRTDBG_FLAG_NAME "crtDbgFlag"
#define CRTDBG_FLAG_NAME_ALT "_crtDbgFlag"

static const possible_alloc_routine_t possible_rtl_routines[] = {
    { "RtlSizeHeap", RTL_ROUTINE_SIZE },
    { "RtlAllocateHeap", RTL_ROUTINE_MALLOC },
    { "RtlReAllocateHeap", RTL_ROUTINE_REALLOC },
    { "RtlFreeHeap", RTL_ROUTINE_FREE },
# ifdef X64
    /* i#1032: In 64-bit Windows 7, NtdllpFreeStringRoutine, is pointed
     * at by RtlFreeStringRoutine, is directly called from many places to free
     * the string allocated via RTL heap routines, so it should be treated as
     * a heap routine.
     */
    { "RtlFreeStringRoutine", RTL_ROUTINE_FREE_STRING },
# endif
    { "RtlValidateHeap", RTL_ROUTINE_VALIDATE },
    { "RtlCreateHeap", RTL_ROUTINE_CREATE },
    { "RtlDestroyHeap", RTL_ROUTINE_DESTROY },
    { "RtlGetUserInfoHeap", RTL_ROUTINE_GETINFO },
    { "RtlSetUserValueHeap", RTL_ROUTINE_SETINFO },
    { "RtlSetUserFlagsHeap", RTL_ROUTINE_SETFLAGS },
    { "RtlSetHeapInformation", RTL_ROUTINE_HEAPINFO },
    { "RtlCreateActivationContext", RTL_ROUTINE_CREATE_ACTCXT},
    /* XXX: i#297: investigate these new routines */
    { "RtlMultipleAllocateHeap", HEAP_ROUTINE_NOT_HANDLED_NOTIFY },
    { "RtlMultipleFreeHeap", HEAP_ROUTINE_NOT_HANDLED_NOTIFY },
    /* i#318: These are very basic pool allocators provided by ntdll and used by
     * low-level Windows libraries such as AUDIOSES.DLL.
     * FIXME: We cannot use HEAP_ROUTINE_NOT_HANDLED because that causes us to
     * interpret the backing vallocs as heaps, which turns any usage of this
     * layer into unaddrs.  Ideally we'd just log these calls.
     */
#if 0
    { "RtlCreateMemoryZone", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlAllocateMemoryZone", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlResetMemoryZone", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlDestroyMemoryZone", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlCreateMemoryBlockLookaside", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlAllocateMemoryBlockLookaside", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlResetMemoryBlockLookaside", HEAP_ROUTINE_NOT_HANDLED },
    { "RtlDestroyMemoryBlockLookaside", HEAP_ROUTINE_NOT_HANDLED },
#endif
    /* kernel32!LocalFree calls these.  these call RtlEnterCriticalSection
     * and ntdll!RtlpCheckHeapSignature and directly touch heap headers.
     */
    { "RtlLockHeap", RTL_ROUTINE_LOCK },
    { "RtlUnlockHeap", RTL_ROUTINE_UNLOCK },
    /* called by kernel32!Heap32First and touches heap headers.
     * XXX: kernel32!Heap32{First,Next} itself reads a header!
     */
    { "RtlEnumProcessHeaps", RTL_ROUTINE_QUERY },
    { "RtlQueryHeapInformation", RTL_ROUTINE_QUERY },
    { "RtlGetProcessHeaps", RTL_ROUTINE_QUERY },
    { "RtlQueryProcessHeapInformation", RTL_ROUTINE_QUERY },
    /* RtlpHeapIsLocked is a non-exported routine that is called directly
     * from LdrShutdownProcess: so we treat the latter as a heap routine
     * XXX: now that we have online symbols can we replace w/ RtlpHeapIsLocked?
     */
    { "LdrShutdownProcess", RTL_ROUTINE_SHUTDOWN },
};
#define POSSIBLE_RTL_ROUTINE_NUM \
    (sizeof(possible_rtl_routines)/sizeof(possible_rtl_routines[0]))

#endif /* WINDOWS */

struct _alloc_routine_set_t;
typedef struct _alloc_routine_set_t alloc_routine_set_t;

/* Each entry in the alloc_routine_table */
struct _alloc_routine_entry_t {
    app_pc pc;
    routine_type_t type;
    const char *name;
    alloc_routine_set_t *set;
    /* Once we have an API for custom allocators (PR 406756) will we need a
     * separate name field, or we'll just call them by their type names?
     */
    /* Do we care about the post wrapper?  If not we can save a lot (b/c our
     * call site method causes a lot of instrumentation when there's high fan-in)
     */
    bool intercept_post;
};

/* Set of malloc routines */
struct _alloc_routine_set_t {
    heapset_type_t type;
    /* Array of entries for all routines in the set.  We could save space by
     * union-ing Rtl and libc but it's not worth it.
     */
    alloc_routine_entry_t *func[HEAP_ROUTINE_COUNT];
    /* Whether redzones are used: we don't for msvcrtdbg (i#26) */
    bool use_redzone;
    /* Let user store a field per malloc set */
    void *client;
    /* For easy cleanup */
    uint refcnt;
    /* For alloc_ops.replace_realloc */
    byte *realloc_replacement;
    /* For simpler removal on module unload */
    app_pc modbase;
    /* For i#643 */
    bool check_mismatch;
    /* For i#939, let wrap/replace store a field per malloc set */
    void *user_data;
    /* For i#964 we connect crt and dbgcrt */
    struct _alloc_routine_set_t *set_libc;
};

void *
alloc_routine_set_get_user_data(alloc_routine_entry_t *e)
{
    ASSERT(e != NULL, "invalid param");
    return e->set->user_data;
}

/* The set for the dynamic libc lib */
static alloc_routine_set_t *set_dyn_libc;

/* lock is held when this is called */
static void
alloc_routine_entry_free(void *p)
{
    alloc_routine_entry_t *e = (alloc_routine_entry_t *) p;
    if (e->set != NULL) {
        ASSERT(e->set->refcnt > 0, "invalid refcnt");
        e->set->refcnt--;
        if (e->set->refcnt == 0) {
            client_remove_malloc_routine(e->set->client);
            malloc_interface.malloc_set_exit(e->set->type, e->pc, e->set->user_data,
                                             e->set->set_libc == NULL ? NULL :
                                             e->set->set_libc->user_data);
            global_free(e->set, sizeof(*e->set), HEAPSTAT_HASHTABLE);
        }
    }
    global_free(e, sizeof(*e), HEAPSTAT_HASHTABLE);
}

#ifdef WINDOWS
static bool
replace_crtdbg_routine(app_pc pc)
{
    alloc_routine_entry_t *e;
    bool res = false;
    if (!alloc_ops.disable_crtdbg)
        return false;
    dr_mutex_lock(alloc_routine_lock);
    e = hashtable_lookup(&alloc_routine_table, (void *)pc);
    if (e != NULL &&
        (e->type == HEAP_ROUTINE_DBG_NOP_FALSE || e->type == HEAP_ROUTINE_DBG_NOP_TRUE))
        res = true;
    dr_mutex_unlock(alloc_routine_lock);
    return res;
}
#endif

#if defined(WINDOWS) || defined(DEBUG)
static const char *
get_alloc_routine_name(app_pc pc)
{
    alloc_routine_entry_t *e;
    const char *name = "<not found>";
    dr_mutex_lock(alloc_routine_lock);
    e = hashtable_lookup(&alloc_routine_table, (void *)pc);
    if (e != NULL)
        name = e->name;
    dr_mutex_unlock(alloc_routine_lock);
    return name;
}
#endif

/* Rather than requiring the caller to hold the hashtable lock, we
 * return a copy of the data.  These entries have lifetimes equal to
 * the app's libraries, so really no entry should become invalid while
 * processing an app call into that library, but our call into the
 * size routine could fail if the app library is racily unloaded.
 * Today we only use this for -conservative.
 */
static bool
get_alloc_entry(app_pc pc, alloc_routine_entry_t *entry)
{
    alloc_routine_entry_t *e;
    bool found = false;
    dr_mutex_lock(alloc_routine_lock);
    e = hashtable_lookup(&alloc_routine_table, (void *)pc);
    if (e != NULL) {
        memcpy(entry, e, sizeof(*entry));
        found = true;
    }
    dr_mutex_unlock(alloc_routine_lock);
    return found;
}

static alloc_routine_entry_t *
size_func_in_set(alloc_routine_set_t *set)
{
    if (set == NULL)
        return NULL;
#ifdef WINDOWS
    if (set->type == HEAPSET_RTL)
        return set->func[RTL_ROUTINE_SIZE];
#endif
    /* prefer usable to requested unless -prefer_msize */
    if (alloc_ops.prefer_msize && set->func[HEAP_ROUTINE_SIZE_REQUESTED] != NULL)
        return set->func[HEAP_ROUTINE_SIZE_REQUESTED];
    else if (set->func[HEAP_ROUTINE_SIZE_USABLE] != NULL)
        return set->func[HEAP_ROUTINE_SIZE_USABLE];
    else if (set->func[HEAP_ROUTINE_SIZE_REQUESTED] != NULL)
        return set->func[HEAP_ROUTINE_SIZE_REQUESTED];
#ifdef WINDOWS
    if (set->func[HEAP_ROUTINE_SIZE_REQUESTED_DBG] != NULL)
        return set->func[HEAP_ROUTINE_SIZE_REQUESTED_DBG];
#endif
    return NULL;
}

static alloc_routine_entry_t *
malloc_func_in_set(alloc_routine_set_t *set)
{
    if (set == NULL)
        return NULL;
#ifdef WINDOWS
    if (set->type == HEAPSET_RTL)
        return set->func[RTL_ROUTINE_MALLOC];
    else if (set->type == HEAPSET_LIBC_DBG)
        return set->func[HEAP_ROUTINE_MALLOC_DBG];
#endif
    if (set->type == HEAPSET_CPP IF_WINDOWS(|| set->type == HEAPSET_CPP_DBG))
        return set->func[HEAP_ROUTINE_NEW];
    else
        return set->func[HEAP_ROUTINE_MALLOC];
}

static alloc_routine_entry_t *
realloc_func_in_set(alloc_routine_set_t *set)
{
    if (set == NULL)
        return NULL;
#ifdef WINDOWS
    if (set->type == HEAPSET_RTL)
        return set->func[RTL_ROUTINE_REALLOC];
    else if (set->type == HEAPSET_LIBC_DBG)
        return set->func[HEAP_ROUTINE_REALLOC_DBG];
#endif
    if (set->type == HEAPSET_CPP IF_WINDOWS(|| set->type == HEAPSET_CPP_DBG))
        return NULL;
    return set->func[HEAP_ROUTINE_REALLOC];
}

static alloc_routine_entry_t *
free_func_in_set(alloc_routine_set_t *set)
{
    if (set == NULL)
        return NULL;
#ifdef WINDOWS
    if (set->type == HEAPSET_RTL)
        return set->func[RTL_ROUTINE_FREE];
    else if (set->type == HEAPSET_LIBC_DBG)
        return set->func[HEAP_ROUTINE_FREE_DBG];
#endif
    if (set->type == HEAPSET_CPP IF_WINDOWS(|| set->type == HEAPSET_CPP_DBG))
        return set->func[HEAP_ROUTINE_DELETE];
    return set->func[HEAP_ROUTINE_FREE];
}

/***************************************************************************
 * REALLOC REPLACEMENT
 */

/* Our wrap strategy does not handle realloc well as by the time we see
 * the results, another malloc can use the freed memory, leading to races.
 * Rather than serialize all alloc routines, we replace realloc with
 * an equivalent series of malloc and free calls, which also solves
 * the problem of delaying any free that realloc performs.
 */
static byte *gencode_start, *gencode_cur;
static void *gencode_lock;
/* We need room for one routine per realloc: one per module in some cases */
#define GENCODE_SIZE (2*PAGE_SIZE)

/* To handle module unloads we have a free list (i#545), protected by gencode_lock */
static byte *gencode_free;
#ifdef WINDOWS
static byte *gencode_free_dbg;
static byte *gencode_free_Rtl;
#endif

/* In alloc_unopt.c b/c we do not want these optimized, and for
 * gcc < 4.4 we have no control (volatile is not good enough).
 */
extern void * marker_malloc(size_t size);
extern size_t marker_size(void *ptr);
extern void marker_free(void *ptr);
extern void *replace_realloc_template(void *p, size_t newsz);

#ifdef WINDOWS
extern void * marker_malloc_dbg(size_t size, int type, const char *file, int line);
extern size_t marker_size_dbg(void *ptr, int type);
extern void marker_free_dbg(void *ptr, int type);
extern void *replace_realloc_template_dbg(void *p, size_t newsz, int type);
extern PVOID NTAPI marker_RtlAllocateHeap(HANDLE heap, DWORD flags, SIZE_T size);
extern ULONG NTAPI marker_RtlSizeHeap(HANDLE heap, ULONG flags, PVOID block);
extern bool NTAPI marker_RtlFreeHeap(HANDLE heap, ULONG flags, PVOID block);
extern void * NTAPI
replace_realloc_template_Rtl(HANDLE heap, ULONG flags, PVOID p, SIZE_T newsz);
#endif

/* Some malloc sets do not have a requested-size query, or don't
 * have any size query (e.g., ld-linux.so.2), so we retrieve the
 * size ourselves
 */
static size_t
replace_realloc_size_app(void *p)
{
    return 0;
}

static void
replace_realloc_size_pre(void *wrapcxt, OUT void **user_data)
{
    cls_alloc_t *pt = (cls_alloc_t *)
        drmgr_get_cls_field(dr_get_current_drcontext(), cls_idx_alloc);
    *user_data = (void *) pt;
    pt->alloc_base = (byte *) drwrap_get_arg(wrapcxt, 0);
    LOG(2, "replace_realloc_size_pre "PFX"\n", pt->alloc_base);
}

static void
replace_realloc_size_post(void *wrapcxt, void *user_data)
{
    cls_alloc_t *pt = (cls_alloc_t *) user_data;
    dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_INTEGER);
    ASSERT(mc->xax == 0, "replace_realloc_size_app always returns 0");
    /* should never fail for our uses */
    mc->xax = malloc_size(pt->alloc_base);
    LOG(2, "replace_realloc_size_post "PFX" => "PIFX"\n", pt->alloc_base, mc->xax);
    drwrap_set_mcontext(wrapcxt);
}

#ifdef X64
static byte *
generate_jmp_ind_stub(void *drcontext, app_pc tgt_pc, byte *epc)
{
    instr_t *instr;
    /* assuming %rax is dead, mov pc => %rax; jmp %rax */
    ASSERT(tgt_pc != NULL, "wrong target pc for call stub");
    instr = INSTR_CREATE_mov_imm(drcontext,
                                 opnd_create_reg(DR_REG_XAX),
                                 OPND_CREATE_INTPTR(tgt_pc));
    epc = instr_encode(drcontext, instr, epc);
    instr_destroy(drcontext, instr);
    instr = INSTR_CREATE_jmp_ind(drcontext, opnd_create_reg(DR_REG_XAX));
    epc = instr_encode(drcontext, instr, epc);
    instr_destroy(drcontext, instr);
    return epc;
}
#endif

static void
generate_realloc_replacement(alloc_routine_set_t *set)
{
    void *drcontext = dr_get_current_drcontext();
    byte *epc_start, *dpc, *epc, *func_start;
    bool success = true;
    instr_t inst;
    alloc_routine_entry_t *set_malloc = malloc_func_in_set(set);
    alloc_routine_entry_t *set_size = size_func_in_set(set);
    alloc_routine_entry_t *set_free = free_func_in_set(set);
    alloc_routine_entry_t *set_realloc = realloc_func_in_set(set);
#ifdef X64
    byte *malloc_stub_pc, *size_stub_pc, *free_stub_pc;
#endif
    byte *size_func;
    uint found_calls = 0;
    byte **free_list = NULL;
    ASSERT(alloc_ops.replace_realloc, "should not get here");
    ASSERT(set != NULL, "invalid param");
    /* if no set_size (e.g., ld-linux.so.2) or only usable size (which
     * would lead to unaddrs on memcpy) we arrange to query our
     * hashtable
     */
    if (set_size == NULL || set_size->type == HEAP_ROUTINE_SIZE_USABLE)
        size_func = (byte *) replace_realloc_size_app;
    else
        size_func = set_size->pc;
    ASSERT(set_realloc != NULL && set_malloc != NULL &&
           size_func != NULL && set_free != NULL, "set incomplete");

    /* copy by decoding template, replacing mark_ call targets along the way. */
    dr_mutex_lock(gencode_lock);
    /* we keep read-only to work around DRi#404 */
    if (!dr_memory_protect(gencode_start, GENCODE_SIZE,
                           DR_MEMPROT_READ|DR_MEMPROT_WRITE|DR_MEMPROT_EXEC)) {
        ASSERT(false, "failed to unprotect realloc gencode");
        dr_mutex_unlock(gencode_lock);
        return;
    }
#ifdef WINDOWS
    if (set->type == HEAPSET_RTL) {
        dpc = (byte *) replace_realloc_template_Rtl;
        free_list = &gencode_free_Rtl;
    } else if (set->type == HEAPSET_LIBC_DBG) {
        dpc = (byte *) replace_realloc_template_dbg;
        free_list = &gencode_free_dbg;
    } else {
#endif
        dpc = (byte *) replace_realloc_template;
        free_list = &gencode_free;
#ifdef WINDOWS
    }
#endif

    /* i#545: we use a free list to re-use sequences from unloaded modules */
    if (*free_list != NULL) {
        epc_start = *free_list;
        *free_list = *((byte **)free_list);
    } else
        epc_start = gencode_cur;
    epc = epc_start;

#ifdef X64
    /* create stubs for indirect jumps */
    malloc_stub_pc = epc;
    epc = generate_jmp_ind_stub(drcontext, set_malloc->pc, epc);
    size_stub_pc = epc;
    epc = generate_jmp_ind_stub(drcontext, size_func, epc);
    free_stub_pc = epc;
    epc = generate_jmp_ind_stub(drcontext, set_free->pc, epc);
#endif
    func_start = epc;


    instr_init(drcontext, &inst);
    do {
        instr_reset(drcontext, &inst);
        dpc = decode(drcontext, dpc, &inst);
        ASSERT(dpc != NULL, "invalid instr in realloc template");
        if (epc == func_start && instr_get_opcode(&inst) == OP_jmp) {
            /* skip jmp in ILT */
            ASSERT(opnd_is_pc(instr_get_target(&inst)), "decoded jmp should have pc tgt");
            dpc = opnd_get_pc(instr_get_target(&inst));
            continue;
        }
        /* XXX: for x64 we will have to consider reachability */
        if (instr_get_opcode(&inst) == OP_call) {
            opnd_t tgt = instr_get_target(&inst);
            app_pc pc, tgt_pc;
            found_calls++;
            ASSERT(opnd_is_pc(tgt), "invalid call");
            pc = opnd_get_pc(tgt);
            if (pc == (app_pc) marker_malloc
                IF_WINDOWS(|| pc == (app_pc) marker_malloc_dbg
                           || pc == (app_pc) marker_RtlAllocateHeap))
                tgt_pc = IF_X64_ELSE(malloc_stub_pc, set_malloc->pc);
            else if (pc == (app_pc) marker_size
                     IF_WINDOWS(|| pc == (app_pc) marker_size_dbg
                                || pc == (app_pc) marker_RtlSizeHeap))
                tgt_pc = IF_X64_ELSE(size_stub_pc, size_func);
            else if (pc == (app_pc) marker_free
                     IF_WINDOWS(|| pc == (app_pc) marker_free_dbg
                                || pc == (app_pc) marker_RtlFreeHeap))
                tgt_pc = IF_X64_ELSE(free_stub_pc, set_free->pc);
            else /* force re-encode */
                tgt_pc = pc;
            instr_set_target(&inst, opnd_create_pc(tgt_pc));
        }
        epc = instr_encode(drcontext, &inst, epc);
        ASSERT(epc != NULL, "failed to encode realloc template");
        if (epc + MAX_INSTR_SIZE >= gencode_start + GENCODE_SIZE) {
            ASSERT(false, "alloc gencode too small");
            success = false;
            break;
        }
        if (dpc == NULL || epc == NULL) { /* be defensive */
            success = false;
            break;
        }
        /* I assume there's only one ret */
    } while (!instr_is_return(&inst));
    if (found_calls < 4) { /* PIC base call makes 5 on linux */
        NOTIFY_ERROR("Dr. Memory compiled incorrectly: realloc template optimized?"NL);
        dr_abort();
    }
    instr_reset(drcontext, &inst);
    if (!dr_memory_protect(gencode_start, GENCODE_SIZE,
                           DR_MEMPROT_READ|DR_MEMPROT_EXEC)) {
        ASSERT(false, "failed to re-protect realloc gencode");
    }
    if (epc_start == gencode_cur) /* else using free list */
        gencode_cur = epc;
    dr_mutex_unlock(gencode_lock);

    if (success) {
        set->realloc_replacement = func_start;
        if (!drwrap_replace(set_realloc->pc, set->realloc_replacement, false))
            ASSERT(false, "failed to replace realloc");
        LOG(1, "replacement realloc @"PFX"\n", func_start);
    } else {
        /* if we fail consequences are races and non-delayed-frees: not fatal */
        LOG(1, "WARNING: replacement realloc failed\n");
    }
    return;
}

/***************************************************************************
 * MALLOC WRAPPING
 */

#ifdef WINDOWS
/* we replace app _CrtDbgReport* with this routine */
static ptr_int_t
replaced_nop_false_routine(void)
{
    return 0;
}

static ptr_int_t
replaced_nop_true_routine(void)
{
    return 1;
}
#endif

static app_pc
lookup_symbol_or_export(const module_data_t *mod, const char *name)
{
#ifdef USE_DRSYMS
    app_pc res;
    if (mod->full_path != NULL) {
        res = lookup_symbol(mod, name);
        if (res != NULL)
            return res;
    }
    res = (app_pc) dr_get_proc_address(mod->handle, name);
    if (res != NULL && op_use_symcache)
        symcache_add(mod, name, res - mod->start);
    return res;
#else
    return (app_pc) dr_get_proc_address(mod->handle, name);
#endif
}

/* caller must hold alloc routine lock */
static alloc_routine_entry_t *
add_alloc_routine(app_pc pc, routine_type_t type, const char *name,
                  alloc_routine_set_t *set, app_pc modbase, const char *modname)
{
    alloc_routine_entry_t *e;
    IF_DEBUG(bool is_new;)
    e = hashtable_lookup(&alloc_routine_table, (void *)pc);
    if (e != NULL) {
        /* this happens w/ things like cfree which maps to free in libc */
        LOG(1, "alloc routine %s "PFX" is already intercepted\n", name, pc);
        /* i#643: operator collapse makes distinguishing impossible.
         * I was unable to determine what causes Chromium Release build to
         * collapse its operators: could not repro in a small sample project.
         *
         * XXX: we don't check for only delete vs delete[] mismatch
         * b/c some free() calls end up going here even though "free"
         * lookup finds the routine the deletes call: the deletes and
         * some free()-ish calls (ones that line up w/ malloc) all go
         * to the same point.  But that's hard to detect so we just
         * say "all bets are off" when plain==[].
         */
        if (type != e->type) {
            if ((type == HEAP_ROUTINE_FREE ||
                 type == HEAP_ROUTINE_DELETE ||
                 type == HEAP_ROUTINE_DELETE_ARRAY) &&
                (e->type == HEAP_ROUTINE_FREE ||
                 e->type == HEAP_ROUTINE_DELETE ||
                 e->type == HEAP_ROUTINE_DELETE_ARRAY)) {
                e->set->check_mismatch = false;
                /* i#643: some optimized libs have identical operator stubs */
                WARN("WARNING: free/delete/delete[] are collapsed together,"
                     " disabling mismatch detection for %s\n",
                     modname);
            } else if ((type == HEAP_ROUTINE_NEW ||
                        type == HEAP_ROUTINE_NEW_ARRAY) &&
                       (e->type == HEAP_ROUTINE_NEW ||
                        e->type == HEAP_ROUTINE_NEW_ARRAY)) {
                e->set->check_mismatch = false;
                /* i#643: some optimized libs have identical operator stubs */
                WARN("WARNING: new == new[] =>"
                     " disabling mismatch detection for %s\n",
                     modname);
            }
        }
        return e;
    }
    e = global_alloc(sizeof(*e), HEAPSTAT_HASHTABLE);
    e->pc = pc;
    e->type = type;
    ASSERT(e->type < HEAP_ROUTINE_COUNT, "invalid type");
    e->name = name;
    e->set = set;
    e->intercept_post = routine_needs_post_wrap(type, set->type);
    if (e->set != NULL) {
        e->set->refcnt++;
        e->set->func[e->type] = e;
        e->set->modbase = modbase;
    } else
        ASSERT(false, "set is required w/ new module unload scheme");
    IF_DEBUG(is_new = )
        hashtable_add(&alloc_routine_table, (void *)pc, (void *)e);
    ASSERT(is_new, "alloc entry should not already exist");
    /* there could be a race on unload where passing e is unsafe but
     * we live w/ it
     * XXX: for -conservative we should do a lookup
     */
    malloc_interface.malloc_intercept(pc, type, e);
    return e;
}

#if defined(WINDOWS) && defined(USE_DRSYMS)
static void
disable_crtdbg(const module_data_t *mod)
{
    static const int zero = 0;
    int *crtdbg_flag_ptr;
    if (!alloc_ops.disable_crtdbg)
        return;
    /* i#51: we do not want crtdbg checks when our tool is present
     * (the checks overlap, better to have our tool report it than
     * crt, etc.).  This dbgflag only controls full-heap scans: to
     * disable checks on malloc and free we also disable the
     * reporting routines, which is a hack and may suppress other
     * errors we might want to see.
     * 
     * Ideally we would also eliminate the crtdbg redzone and
     * replace w/ our size-controllable redzone as well: but we
     * would have to map the _dbg routines straight to Heap* or
     * something (there are no release versions of all of them;
     * could try to use _base for a few) and replace operator delete
     * (or switch to replacing all alloc routines instead of
     * instrumenting).  We could document that app is better off not
     * using debug crt.  Note that the crtdbg redzone should get
     * marked unaddr by us, since we'll use the size passed to the
     * _dbg routine (and should be in our hashtable later, so should
     * never call RtlSizeHeap and get the full size and get
     * confused).
     */
    crtdbg_flag_ptr = (int *) lookup_internal_symbol(mod, CRTDBG_FLAG_NAME);
    if (crtdbg_flag_ptr == NULL)
        crtdbg_flag_ptr = (int *) lookup_internal_symbol(mod, CRTDBG_FLAG_NAME_ALT);
    LOG(2, "%s @"PFX"\n", CRTDBG_FLAG_NAME, crtdbg_flag_ptr);
    if (crtdbg_flag_ptr != NULL &&
        dr_safe_write(crtdbg_flag_ptr, sizeof(*crtdbg_flag_ptr), 
                      &zero, NULL)) {
        LOG(1, "disabled crtdbg checks\n");
    } else {
        /* XXX: turn into something more serious and tell user
         * we either need symbols or compilation w/ no crtdbg
         * to operate properly?
         */
        LOG(1, "WARNING: unable to disable crtdbg checks\n");
    }
}

static size_t
find_debug_delete_interception(app_pc mod_start, app_pc mod_end, size_t modoffs)
{
    /* i#722, i#655: MSVS libraries call _DELETE_CRT(ptr) or _DELETE_CRT_VEC(ptr)
     * which for release build map to operatore delete or operator delete[], resp.
     * However, for _DEBUG, both map to the same routine std::_DebugHeapDelete, which
     * explicitly calls the destructor and then call free(), sometimes via tailcall.
     * This means we would report a mismatch for an allocation with new but
     * deallocation with free().  To suppress that, we could use a suppression, but
     * b/c of the tailcall std::_DebugHeapDelete is sometimes not on the callstack.
     * So, we intercept the routine and set a flag saying "ignore mismatches for the
     * next call to free()".  But, we have to set the flag after the destructor, as
     * it can go destroy sub-objects.  So we look for the interior call/jmp to free
     * here.  (An alternative to decoding would be to intercept on entry and store a
     * stack of pointers, but it might get quite deep.  We have the sources to
     * std::_DebugHeapDelete so we know it's short w/ no surprises (at least in
     * current MSVS versions).
     */
    void *drcontext = dr_get_current_drcontext();
    IF_DEBUG(cls_alloc_t *pt = (cls_alloc_t *)
             drmgr_get_cls_field(drcontext, cls_idx_alloc);)
    byte *pc, *npc = mod_start + modoffs;
    instr_t inst;
    bool found = false;
    uint num_calls = 0;
    instr_init(drcontext, &inst);
    LOG(3, "%s: decoding "PFX"\n", __FUNCTION__, npc);
    do {
        app_pc tgt = NULL;
        instr_reset(drcontext, &inst);
        pc = npc;
        DOLOG(3, {
            disassemble_with_info(drcontext, pc, LOGFILE_GET(drcontext),
                                  true/*pc*/, true/*bytes*/);
        });
        /* look for partial map (i#730) */
        if (pc + MAX_INSTR_SIZE > mod_end) {
            WARN("WARNING: decoding off end of module for _DebugHeapDelete\n");
            break;
        }
        npc = decode(drcontext, pc, &inst);
        if (npc == NULL) {
            ASSERT(false, "invalid instr in _DebugHeapDelete");
            break;
        }
        if (instr_is_call_direct(&inst) || instr_is_ubr(&inst)) {
            /* The first direct call or jmp should be the call or tailcall to
             * free.  There may be a cbr earlier, and an indirect call to a
             * destructor.  There may also be a direct call to a destructor.  We
             * assume we've already found free() and that we only care about a
             * call/jmp to this module's free().
             */
            opnd_t op = instr_get_target(&inst);
            ASSERT(opnd_is_pc(op), "must be pc");
            tgt = opnd_get_pc(op);
        } else if (instr_get_opcode(&inst) == OP_call_ind) {
            /* When std:_DebugHeapDelete is in another module w/o static libc,
             * the call to libc's free is indirect (part of i#607 part C).
             */
            opnd_t op = instr_get_target(&inst);
            if (opnd_is_abs_addr(op))
                safe_read((app_pc) opnd_get_addr(op), sizeof(tgt), &tgt);
        }
        if (tgt != NULL) {
            LOG(3, "%s: found cti to "PFX"\n", __FUNCTION__, tgt);
            ASSERT(dr_mutex_self_owns(alloc_routine_lock), "caller must hold lock");
            if (hashtable_lookup(&alloc_routine_table, (void *)tgt) != NULL ||
                /* i#1031: we may not have processed libc yet.  Doing so at
                 * process init won't solve potential issues w/ a later-loaded
                 * lib depending on a still-not-yet-processed libc (if app doesn't
                 * use libc), so we assume that any 2nd call (1st is to destructor)
                 * to libc is a call to free.  Even if there are 2 destructor call
                 * instances, no destructor should live in libc.
                 */
                (num_calls > 0 && pc_is_in_libc(tgt) && !pc_is_in_libc(mod_start))) {
                LOG(2, "%s: found cti to free? "PFX"\n", __FUNCTION__, tgt);
                modoffs = pc - mod_start;
                found = true;
                break;
            }
        }
        if (instr_is_call(&inst))
            num_calls++;
        /* Should find before ret and within one page (this is a short routine) */
    } while (!instr_is_return(&inst) && npc - (mod_start + modoffs) < PAGE_SIZE);
    instr_free(drcontext, &inst);
    if (!found) {
        /* Bail.  If we just intercept entry we can suppress mismatch
         * on the wrong free (either in destructor in this routine,
         * or later if arg to this routine is NULL: which we can't
         * easily check w/ later intercept b/c of tailcall vs call).
         */
        WARN("WARNING: unable to suppress std::_DebugHeapDelete mismatch errors\n");
        modoffs = 0;
    }
    return modoffs;
}
#endif

#ifdef USE_DRSYMS
/* Given a generic operator type and its location in a module, checks its
 * args and converts to a corresponding nothrow operator type or to
 * HEAP_ROUTINE_INVALID if a placement operator.  Returns true if successful.
 */
static bool
distinguish_operator_no_argtypes(routine_type_t generic_type,
                                 routine_type_t *specific_type OUT,
                                 const char *name, const module_data_t *mod,
                                 size_t modoffs)
{
    bool known = false;
    /* XXX DRi#860: we don't yet have any types for Linux or Mingw so we fall
     * back to looking at the mangled form.  We also come here if we don't
     * have arg types despite having the pdb (for system pdbs like msvcp*.dll)
     * or for non-standard arg types.  Regardless of how we got here, we first
     * try to look at the mangled name.
     */
    drsym_debug_kind_t kind;
    drsym_error_t res = drsym_get_module_debug_kind(mod->full_path, &kind);
    ASSERT(specific_type != NULL, "invalid param");
    *specific_type = generic_type;
    if (res == DRSYM_SUCCESS && !TEST(DRSYM_PDB, kind)) {
        /* We can't get mangled names for PDBs, and there's no existing interface
         * to walk exports, so we fall back to decoding below.  Else, we continue.
         */
        drsym_error_t symres;
        drsym_info_t *sym;
#       define MAX_MANGLED_OPERATOR 64
        char sbuf[sizeof(*sym) + MAX_MANGLED_OPERATOR];
        sym = (drsym_info_t *) sbuf;
        sym->struct_size = sizeof(*sym);
        sym->name_size = MAX_MANGLED_OPERATOR;
        ASSERT(strlen(MANGLED_NAME_DELETE_ARRAY_NOTHROW) < MAX_MANGLED_OPERATOR,
               "need more space for mangled name lookup");
        /* XXX: share this sequence with callstack.c */
        STATS_INC(symbol_address_lookups);
        symres = drsym_lookup_address(mod->full_path, modoffs, sym, DRSYM_LEAVE_MANGLED);
        LOG(3, "looking at mangled name for %s\n", name);
        if (symres == DRSYM_SUCCESS || symres == DRSYM_ERROR_LINE_NOT_AVAILABLE) {
            /* We don't care if sym->name_available_size >= sym->name_size b/c
             * that means the name is too long for anything we're looking for.
             */
            /* Could make an array to shorten this code but then have to assume
             * things about type enum sequence and not clear it's cleaner
             */
            if (generic_type == HEAP_ROUTINE_NEW) {
                if (strcmp(sym->name, MANGLED_NAME_NEW) == 0) {
                    *specific_type = generic_type;
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_NEW_NOTHROW) == 0) {
                    *specific_type = convert_operator_to_nothrow(generic_type);
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_NEW_PLACEMENT) == 0) {
                    *specific_type = HEAP_ROUTINE_INVALID;
                    known = true;
                }
            } else if (generic_type == HEAP_ROUTINE_NEW_ARRAY) {
                if (strcmp(sym->name, MANGLED_NAME_NEW_ARRAY) == 0) {
                    *specific_type = generic_type;
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_NEW_ARRAY_NOTHROW) == 0) {
                    *specific_type = convert_operator_to_nothrow(generic_type);
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_NEW_ARRAY_PLACEMENT) == 0) {
                    *specific_type = HEAP_ROUTINE_INVALID;
                    known = true;
                }
            } else if (generic_type == HEAP_ROUTINE_DELETE) {
                if (strcmp(sym->name, MANGLED_NAME_DELETE) == 0) {
                    *specific_type = generic_type;
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_DELETE_NOTHROW) == 0) {
                    *specific_type = convert_operator_to_nothrow(generic_type);
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_DELETE_PLACEMENT) == 0) {
                    *specific_type = HEAP_ROUTINE_INVALID;
                    known = true;
                }
            } else if (generic_type == HEAP_ROUTINE_DELETE_ARRAY) {
                if (strcmp(sym->name, MANGLED_NAME_DELETE_ARRAY) == 0) {
                    *specific_type = generic_type;
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_DELETE_ARRAY_NOTHROW) == 0) {
                    *specific_type = convert_operator_to_nothrow(generic_type);
                    known = true;
                } else if (strcmp(sym->name, MANGLED_NAME_DELETE_ARRAY_PLACEMENT) == 0) {
                    *specific_type = HEAP_ROUTINE_INVALID;
                    known = true;
                }
            }
            LOG(3, "\tmangled name is %s => known=%d type=%d\n",
                sym->name, known, *specific_type);
        }
    }
    if (!known) {
        /* Note that both g++ and MSVS inline both nothrow and placement operators.
         * Thus, if we don't have symbols, we're probably fine as we want to ignore
         * placement and we can ignore the outer layer of nothrow if it just wraps
         * exception handling around the regular operator.
         * But, if we do have symbols but no arg types, we need to distinguish.
         * The strategy is to decode and if we see no call then assume it's
         * placement (which should just return its arg).
         * We'll give up on nothrow: but it's low-risk b/c it only comes into
         * play if we run out of memory.  Plus -replace_malloc doesn't throw
         * an exception yet anyway.
         */
        void *drcontext = dr_get_current_drcontext();
        instr_t inst;
        app_pc pc = mod->start + modoffs;
        instr_init(drcontext, &inst);
        /* XXX: in general when decoding we want to wait for the module to be relocated
         * and thus we want DRi#884 or to delay our symbol searches via our
         * mod_pending_tree.  But in this case, we only care about the opcodes, which
         * shouldn't be affected by relocations.
         */
        LOG(3, "decoding %s @"PFX" looking for placement operator\n", name, pc);
        ASSERT(drcontext != NULL, "must have DC");
        do {
            instr_reset(drcontext, &inst);
            if (!safe_decode(drcontext, pc, &inst, &pc)) {
                LOG(3, "\tfailed to decode "PFX"\n", pc);
                break;
            }
            if (pc == NULL || !instr_valid(&inst))
                break;
            if (instr_is_return(&inst)) {
                /* We hit a return w/ no cti first: we assume it's placement */
                LOG(2, "%s is straight-line: looks like a placement operator\n", name);
                *specific_type = HEAP_ROUTINE_INVALID;
                known = true;
                break;
            }
            if (instr_is_cti(&inst)) {
                /* While we're not really sure whether nothrow, we're pretty sure
                 * it's not placement so we mark as known to avoid the warning msg.
                 */
                LOG(2, "%s is not straight-line so not a placement operator\n", name);
                known = true;
                break;
            }
        } while (true);
        instr_free(drcontext, &inst);
    }
    return known;
}

/* Given a generic operator type and its location in a module, checks its
 * args and converts to a corresponding nothrow operator type or to
 * HEAP_ROUTINE_INVALID if a placement operator.
 */
static routine_type_t
distinguish_operator_type(routine_type_t generic_type,  const char *name,
                          const module_data_t *mod, size_t modoffs)
{
    size_t bufsz = 256; /* even w/ class names, should be big enough for most operators */
    char *buf;
    drsym_func_type_t *func_type;
    drsym_error_t err;
    routine_type_t specific_type = generic_type;
    const char *modname = dr_module_preferred_name(mod);
    bool known = false;
    if (modname == NULL)
        modname = "<noname>";

    ASSERT((is_new_routine(generic_type) || is_delete_routine(generic_type)) &&
           !is_operator_nothrow_routine(generic_type),
           "incoming type must be non-nothrow operator");

    buf = (char *) global_alloc(bufsz, HEAPSTAT_MISC);
    do {
        err = drsym_get_func_type(mod->full_path, modoffs, buf, bufsz, &func_type);
        if (err != DRSYM_ERROR_NOMEM)
            break;
        global_free(buf, bufsz, HEAPSTAT_MISC);
        bufsz *= 2;
        buf = (char *) global_alloc(bufsz, HEAPSTAT_MISC);
    } while (true);

    LOG(2, "%s in %s @"PFX" generic type=%d => drsyms res=%d, %d args\n",
        name, modname, mod->start + modoffs, generic_type, err,
        err == DRSYM_SUCCESS ? func_type->num_args : -1);

    if (err != DRSYM_SUCCESS) {
        /* Fall through to no-arg-type handling below */
    } else if (func_type->num_args == 1) {
        /* standard type: operator new(unsigned int) or operator delete(void *) */
        specific_type = generic_type;
        known = true;
    } else if (func_type->num_args == 2) {
        if (func_type->arg_types[1]->kind == DRSYM_TYPE_PTR) {
            drsym_ptr_type_t *arg_type = (drsym_ptr_type_t *) func_type->arg_types[1];
            LOG(3, "operator %s 2nd arg is pointer to kind=%d size=%d\n",
                name, arg_type->elt_type->kind, arg_type->elt_type->size);
            if (arg_type->elt_type->kind == DRSYM_TYPE_COMPOUND &&
                strcmp(((drsym_compound_type_t *)arg_type->elt_type)->name,
                       "std::nothrow_t") == 0) {
                /* This is nothrow new or delete */
                ASSERT(((drsym_compound_type_t *)arg_type->elt_type)->num_fields == 0,
                       "std::nothrow_t should not have any fields");
                specific_type = convert_operator_to_nothrow(generic_type);
                known = true;
                LOG(2, "operator %s in %s @"PFX" assumed to be nothrow\n",
                    name, modname, mod->start + modoffs);
            } else if (arg_type->elt_type->kind == DRSYM_TYPE_VOID ||
                       arg_type->elt_type->kind == DRSYM_TYPE_INT) {
                /* We assume that this is placement new or delete.
                 * Usually it will be "void *" but we want to allow "char *"
                 * or other int pointers.
                 * If the app has some other version we won't support it.
                 * In the future we could support annotations to
                 * tell us about it.
                 */
                specific_type = HEAP_ROUTINE_INVALID;
                known = true;
                LOG(2, "%s in %s @"PFX" assumed to be placement\n",
                    name, modname, mod->start + modoffs);
            }
        }
        if (!known) {
            /* We do rule out (non-std::nothrow_t) struct or class pointers
             * as not being placement.
             */
            WARN("WARNING: unknown 2-arg overload of %s in %s @"PFX"\n",
                 name, modname, mod->start + modoffs);
            specific_type = generic_type;
        }
    } else {
        /* MSVC++ has 3-arg and 4-arg operators which we assume
         * are all non-placement non-nothrow:
         *   operator new(unsigned int, _HeapManager*, int)
         *   operator new(unsigned int,int,char const *,int)
         */
        specific_type = generic_type;
        known = true;
    }
    if (!known) {
        /* XXX DRi#860: drsyms does not yet provide arg types for Linux/Mingw.
         * We don't just check for DRSYM_ERROR_NOT_IMPLEMENTED b/c we may have
         * symbols but no types (e.g., for system pdbs like msvcp*.dll: xref
         * i#607 part D) or we may encounter a weird overload.
         */
        known = distinguish_operator_no_argtypes(generic_type, &specific_type,
                                                 name, mod, modoffs);
        if (known) {
            LOG(2, "%s in %s @"PFX" determined to be type=%d\n",
                name, modname, mod->start + modoffs, specific_type);
        } else {
            WARN("WARNING: unable to determine args for operator %s in %s @"PFX"\n",
                 name, modname, mod->start + modoffs);
            specific_type = generic_type;
        }
    }
    global_free(buf, bufsz, HEAPSTAT_MISC);
    return specific_type;
}
#endif

/* for passing data to sym callback, and simpler to use for
 * non-USE_DRSYMS as well
 */
typedef struct _set_enum_data_t {
    alloc_routine_set_t *set;
    heapset_type_t set_type;
    const possible_alloc_routine_t *possible;
    uint num_possible;
    bool *processed;
    bool use_redzone;
    bool check_mismatch;
    /* if a wildcard lookup */
    const char *wildcard_name;
    const module_data_t *mod;
    /* points at libc set, for pairing up dbgcrt and crt */
    alloc_routine_set_t *set_libc;
} set_enum_data_t;

static void
add_to_alloc_set(set_enum_data_t *edata, byte *pc, uint idx)
{
    const char *modname = dr_module_preferred_name(edata->mod);
    if (modname == NULL)
        modname = "<noname>";
    ASSERT(edata != NULL && pc != NULL, "invalid params");
    /* look for partial map (i#730) */
    if (pc >= edata->mod->end) {
        LOG(1, "NOT intercepting %s @"PFX" beyond end of mapping for module %s\n",
            edata->possible[idx].name, pc, modname);
        return;
    }
#ifdef USE_DRSYMS
    if (op_use_symcache)
        symcache_add(edata->mod, edata->possible[idx].name, pc - edata->mod->start);
#endif
    if (edata->set == NULL) {
        edata->set = (alloc_routine_set_t *)
            global_alloc(sizeof(*edata->set), HEAPSTAT_HASHTABLE);
        memset(edata->set, 0, sizeof(*edata->set));
        edata->set->use_redzone = (edata->use_redzone && alloc_ops.redzone_size > 0);
        edata->set->client = client_add_malloc_routine(pc);
        edata->set->user_data = malloc_interface.malloc_set_init
            (edata->set_type, pc,
             edata->set_libc == NULL ? NULL : edata->set_libc->user_data);
        edata->set->type = edata->set_type;
        edata->set->check_mismatch = edata->check_mismatch;
        edata->set->set_libc = edata->set_libc;
    }
    add_alloc_routine(pc, edata->possible[idx].type, edata->possible[idx].name,
                      edata->set, edata->mod->start, modname);
    if (edata->processed != NULL)
        edata->processed[idx] = true;
    LOG(1, "intercepting %s @"PFX" type %d in module %s\n",
        edata->possible[idx].name, pc, edata->possible[idx].type, modname);
#if defined(WINDOWS) && defined(USE_DRSYMS)
    if (alloc_ops.disable_crtdbg && edata->possible[idx].type == HEAP_ROUTINE_SET_DBG)
        disable_crtdbg(edata->mod);
#endif
}

#ifdef USE_DRSYMS
/* It's faster to search for multiple symbols at once via regex
 * and strcmp to identify precise targets (i#315).
 */
static bool
enumerate_set_syms_cb(const char *name, size_t modoffs, void *data)
{
    uint i, add_idx;
    set_enum_data_t *edata = (set_enum_data_t *) data;
    ASSERT(edata != NULL && edata->processed != NULL, "invalid param");
    LOG(2, "%s: %s "PIFX"\n", __FUNCTION__, name, modoffs);
    for (i = 0; i < edata->num_possible; i++) {
        /* We do not check !edata->processed[i] b/c we want all copies.
         * Extra copies will have interception entries in hashtable,
         * but only the last one will be in the set's array of funcs.
         */
        /* For wildcard routines we assume they ONLY match routines we're
         * interested in, and we furthermore store all matches in symcache as
         * dups using the non-wildcard name in the possible* array.
         */
        size_t len = strlen(edata->possible[i].name);
        if ((edata->wildcard_name != NULL &&
             strcmp(edata->possible[i].name, edata->wildcard_name) == 0) ||
            (edata->wildcard_name == NULL &&
             (strcmp(name, edata->possible[i].name) == 0 ||
              /* Deal with PECOFF/ELF having "()" at end.
               * XXX: would be much nicer for drsyms to provide consistent format!
               */
              (strstr(name, edata->possible[i].name) == name &&
               strlen(name) == len + 2 &&
               name[len] == '(' && name[len+1] == ')')))) {
            add_idx = i;
# ifdef WINDOWS
            if (edata->possible[i].type == HEAP_ROUTINE_DebugHeapDelete &&
                /* look for partial map (i#730) */
                modoffs < edata->mod->end - edata->mod->start) {
                modoffs = find_debug_delete_interception
                    (edata->mod->start, edata->mod->end, modoffs);
            }
# endif
            if (is_new_routine(edata->possible[i].type) ||
                is_delete_routine(edata->possible[i].type)) {
                /* Distinguish placement and nothrow new and delete. */
                routine_type_t op_type = distinguish_operator_type
                    (edata->possible[i].type, edata->possible[i].name, edata->mod,
                     modoffs);
                if (op_type == HEAP_ROUTINE_INVALID)
                    modoffs = 0; /* skip */
                else if (op_type != edata->possible[i].type) {
                    /* Convert to nothrow.  add_to_alloc_set() takes an index into
                     * the possible array so we point at the corresponding nothrow
                     * entry in that array.
                     */
                    ASSERT(i < OPERATOR_ENTRIES, "possible_cpp_routines was reordered!");
                    add_idx += OPERATOR_ENTRIES;
                    ASSERT(edata->possible[add_idx].type == op_type,
                           "possible_cpp_routines types don't match assumptions");
                }
            }
            if (modoffs != 0)
                add_to_alloc_set(edata, edata->mod->start + modoffs, add_idx);
            break;
        }
    }
    return true; /* keep iterating */
}

/* Only supports "\w*" && prefix=="\w", "*\w" && suffix=="\w", 
 * or a wildcard for which we want to wrap all matches
 */
static void
find_alloc_regex(set_enum_data_t *edata, const char *regex,
                 const char *prefix, const char *suffix)
{
    uint i;
    bool full = false;
# ifdef WINDOWS
    if (edata->mod->start == get_libc_base(NULL) ||
        edata->mod->start == get_libcpp_base()) {
        /* The _calloc_impl in msvcr*.dll is private (i#960) */
        /* The std::_DebugHeapDelete<> (i#722) in msvcp*.dll is private (i#607 part C) */
        LOG(2, "%s: doing full symbol lookup for libc/libc++\n", __FUNCTION__);
        full = true;
    }
# endif
    if (lookup_all_symbols(edata->mod, regex, full,
                           enumerate_set_syms_cb, (void *)edata)) {
        for (i = 0; i < edata->num_possible; i++) {
            const char *name = edata->possible[i].name;
            if (!edata->processed[i] &&
                ((prefix != NULL && strstr(name, prefix) == name) ||
                 (suffix != NULL && strlen(name) >= strlen(suffix) &&
                  strcmp(name + strlen(name) - strlen(suffix), suffix) == 0))) {
                app_pc pc = NULL;
                /* XXX: somehow drsym_search_symbols misses msvcrt!malloc
                 * (dbghelp 6.11+ w/ full search does find it but full takes too
                 * long) so we always try an export lookup
                 */
                /* can't look up wildcard in exports */
                if (edata->wildcard_name != NULL)
                    pc = (app_pc) dr_get_proc_address(edata->mod->handle, name);
                if (pc != NULL) {
                    LOG(2, "regex didn't match %s but it's an export\n", name);
                    add_to_alloc_set(edata, pc, i);
                } else {
                    LOG(2, "marking %s as processed since regex didn't match\n", name);
                    ASSERT(edata->wildcard_name == NULL, "shouldn't get here");
                    edata->processed[i] = true;
                    if (op_use_symcache)
                        symcache_add(edata->mod, edata->possible[i].name, 0);
                }
            }
        }
    } else
        LOG(2, "WARNING: failed to look up symbols: %s\n", regex);
}

# if defined(WINDOWS) && defined(X64)
static app_pc
find_RtlFreeStringRoutine(const module_data_t *mod)
{
    /* i#995-c#3, RtlFreeStringRoutine is not an exported routine
     * but a pointer pointing to NtdllpFreeStringRoutine,
     * which may free memory by directly calling RtlpFreeHeap.
     * We find it by decoding RtlFreeOemString:
     *
     * On Win7-x64
     * ntdll!RtlFreeOemString:
     * 7721db30 4883ec28        sub     rsp,28h
     * 7721db34 488b4908        mov     rcx,qword ptr [rcx+8]
     * 7721db38 4885c9          test    rcx,rcx
     * 7721db3b 7406            je      ntdll!RtlFreeOemString+0x13 (00000000`7721db43)
     * 7721db3d ff15d5990200    call    qword ptr [ntdll!RtlFreeStringRoutine (00000000`77247518)]
     * 7721db43 4883c428        add     rsp,28h
     * 7721db47 c3              ret
     *
     * On WinXP-x64
     * ntdll32!RtlFreeOemString:
     * 7d65e0ac 8bff             mov     edi,edi
     * 7d65e0ae 55               push    ebp
     * 7d65e0af 8bec             mov     ebp,esp
     * 7d65e0b1 8b4508           mov     eax,[ebp+0x8]
     * 7d65e0b4 8b4004           mov     eax,[eax+0x4]
     * 7d65e0b7 85c0             test    eax,eax
     * 7d65e0b9 7407             jz      ntdll32!RtlFreeOemString+0x16 (7d65e0c2)
     * 7d65e0bb 50               push    eax
     * 7d65e0bc ff15bcf9617d call dword ptr [ntdll32!RtlFreeStringRoutine (7d61f9bc)]
     * 7d65e0c2 5d               pop     ebp
     * 7d65e0c3 c20400           ret     0x4
     */
    void *drcontext = dr_get_current_drcontext();
    instr_t instr;
    opnd_t opnd;
    int i, opc;
    app_pc pc;
    pc = (app_pc)dr_get_proc_address(mod->handle, "RtlFreeOemString");
    if (pc == NULL) {
        ASSERT(false, "fail to find RtlFreeOemString");
        return NULL;
    }
    instr_init(drcontext, &instr);
    for (i = 0; i < 20 /* we decode no more than 20 instrs */; i++) {
        instr_reset(drcontext, &instr);
        pc = decode(drcontext, pc, &instr);
        opc = instr_get_opcode(&instr);
        if (pc == NULL || opc == OP_call_ind || opc == OP_ret)
            break;
    }

    if (opc != OP_call_ind) {
        WARN("WARNING: fail to find call to RtlFreeStringRoutine\n");
        instr_free(drcontext, &instr);
        return NULL;
    }

    opnd = instr_get_target(&instr);
    instr_free(drcontext, &instr);
    if ((opnd_is_abs_addr(opnd) || opnd_is_rel_addr(opnd)) &&
        safe_read((void *)opnd_get_addr(opnd), sizeof(pc), &pc)) {
        LOG(2, "find RtlFreeStringRoutine "PFX
            " and RtlpFreeStringRoutine "PFX"\n", opnd_get_addr(opnd), pc);
    } else {
        pc = NULL;
        WARN("WARNING: fail to find call to RtlFreeStringRoutine\n");
    }
    return pc;
}
# endif /* WINDOWS && X64 */
#endif /* USE_DRSYMS */

/* caller must hold alloc routine lock */
static alloc_routine_set_t *
find_alloc_routines(const module_data_t *mod, const possible_alloc_routine_t *possible,
                    uint num_possible, bool use_redzone, bool check_mismatch,
                    bool expect_all, heapset_type_t type, alloc_routine_set_t *set_libc)
{
    set_enum_data_t edata;
    uint i;
#ifdef DEBUG
    const char *modname = dr_module_preferred_name(mod);
#endif
    edata.set = NULL;
    edata.set_type = type;
    edata.possible = possible;
    edata.num_possible = num_possible;
    edata.use_redzone = use_redzone;
    edata.check_mismatch = check_mismatch;
    edata.mod = mod;
    edata.processed = NULL;
    edata.wildcard_name = NULL;
    edata.set_libc = set_libc;
#ifdef USE_DRSYMS
    /* Symbol lookup is expensive for large apps so we batch some
     * requests together using regex symbol lookup, which cuts the
     * total lookup time in half.  i#315.
     */
    if (possible == possible_libc_routines ||
        IF_WINDOWS(possible == possible_crtdbg_routines ||)
        possible == possible_cpp_routines) {
        bool all_processed = true;
        edata.processed = (bool *)
            global_alloc(sizeof(*edata.processed)*num_possible, HEAPSTAT_MISC);
        memset(edata.processed, 0, sizeof(*edata.processed)*num_possible);

        /* First we check the symbol cache */
        if (op_use_symcache && symcache_module_is_cached(mod)) {
            size_t modoffs;
            uint count;
            uint idx;
            for (i = 0; i < num_possible; i++) {
                /* For cpp operators we have to find all of them.  We assume the
                 * symcache contains all entries for a particular symbol, if it
                 * has any (already assuming that elsewhere).  If not in
                 * symcache we do wildcard search below.  We also assume all
                 * function overloads are present but w/o function parameter
                 * names.  We use special names for the overloads we want
                 * to distinguish (currently, nothrow).
                 */
                for (idx = 0, count = 1;
                     idx < count && symcache_lookup(mod, possible[i].name,
                                                    idx, &modoffs, &count); idx++) {
                    STATS_INC(symbol_search_cache_hits);
                    edata.processed[i] = true;
                    if (modoffs != 0)
                        add_to_alloc_set(&edata, mod->start + modoffs, i);
                }
                if (all_processed && !edata.processed[i])
                    all_processed = false;
            }
        } else
            all_processed = false;
        if (!all_processed) {
            bool has_fast_search = lookup_has_fast_search(mod);
            if (possible == possible_libc_routines) {
                if (has_fast_search) { /* else faster to do indiv lookups */
                    find_alloc_regex(&edata, "mall*", "mall", NULL);
                    find_alloc_regex(&edata, "*alloc", NULL, "alloc");
                    find_alloc_regex(&edata, "*_impl", NULL, "_impl");
                }
# ifdef WINDOWS
            } else if (possible == possible_crtdbg_routines) {
                if (has_fast_search) { /* else faster to do indiv lookups */
                    find_alloc_regex(&edata, "*_dbg", NULL, "_dbg");
                    find_alloc_regex(&edata, "*_dbg_impl", NULL, "_dbg_impl");
                    find_alloc_regex(&edata, "_CrtDbg*", "_CrtDbg", NULL);
                    /* Exports not covered by the above will be found by
                     * individual query.
                     */
                }
# endif
            } else if (possible == possible_cpp_routines) {
                /* regardless of fast search we want to find all overloads */
                find_alloc_regex(&edata, "operator new*", "operator new", NULL);
                find_alloc_regex(&edata, "operator delete*", "operator delete", NULL);
# ifdef WINDOWS
                /* wrapper in place of real delete or delete[] operators
                 * (i#722,i#655)
                 */
                edata.wildcard_name = "std::_DebugHeapDelete<>";
                find_alloc_regex(&edata, "std::_DebugHeapDelete<*>",
                                 /* no export lookups so pass NULL */
                                 NULL, NULL);
                edata.wildcard_name = NULL;
# endif
            }
        }
    }
#endif
    for (i = 0; i < num_possible; i++) {
        app_pc pc;
        const char *name = possible[i].name;
        if (edata.processed != NULL && edata.processed[i])
            continue;
#ifdef WINDOWS
        /* i#997: only count _getptd as a heap layer if we have no _nh_malloc_dbg.
         * For /MTd we assume that if we can find _getptd we can find _nh_malloc_dbg.
         */
        if (!dbgcrt_nosyms && possible[i].type == HEAP_ROUTINE_GETPTD) {
            if (edata.processed != NULL)
                edata.processed[i] = true;
            continue;
        }
# ifdef X64
        if (possible == possible_rtl_routines &&
            possible[i].type == RTL_ROUTINE_FREE_STRING) {
            /* i#1032 add NtdllFreeStringRoutine as heap routine */
            pc = find_RtlFreeStringRoutine(edata.mod);
            if (pc != NULL)
                add_to_alloc_set(&edata, pc, i);
        }
# endif /* X64 */
#endif /* WINDOWS */
#ifdef USE_DRSYMS
        if (is_operator_nothrow_routine(possible[i].type)) {
            /* The name doesn't match the real symbol so we take the
             * name from the non-nothrow entry which we assume is prior.
             * However, since we're using the umangled name with no
             * parameters, we're only going to find the very first operator
             * with a singletone query like this, so this is sort of silly:
             * we rely on the symcache and find_alloc_regex() calls above.
             */
            ASSERT(i >= OPERATOR_ENTRIES, "possible_cpp_routines was reordered!");
            name = possible[i - OPERATOR_ENTRIES].name;
        }
#endif
        pc = lookup_symbol_or_export(mod, name);
        ASSERT(!expect_all || pc != NULL, "expect to find all alloc routines");
#ifdef LINUX
        /* PR 604274: sometimes undefined symbol has a value pointing at PLT:
         * we do not want to try and intercept that.
         * Ideally DR should exclude these weird symbols: that's i#341.
         * XXX: at least check whether in PLT: but would want i#76 (elf
         * section iterator) for that.
         * For now using a hack by decoding and looking for jmp*.
         * Someone hooking one of our targets would presumably use direct jmp.
         */
        if (pc != NULL) {
            char buf[16];
            if (pc < mod->end && safe_read(pc, sizeof(buf), buf)) {
                instr_t inst;
                void *drcontext = dr_get_current_drcontext();
                instr_init(drcontext, &inst);
                decode(drcontext, pc, &inst);
                if (!instr_valid(&inst) || instr_get_opcode(&inst) == OP_jmp_ind)
                    pc = NULL;
                instr_free(drcontext, &inst);
            } else
                pc = NULL;
            if (pc == NULL) {
                LOG(1, "NOT intercepting PLT or invalid %s in module %s\n",
                    possible[i].name, (modname == NULL) ? "<noname>" : modname);
            }
        }
#endif
        if (pc != NULL)
            add_to_alloc_set(&edata, pc, i);
#ifdef LINUX
        /* libc's malloc_usable_size() is used during initial heap walk */
        if (possible[i].type == HEAP_ROUTINE_SIZE_USABLE &&
            mod->start == get_libc_base(NULL)) {
            ASSERT(pc != NULL, "no malloc_usable_size in libc!");
            malloc_usable_size = (size_t(*)(void *)) pc;
        }
#endif
    }
    if (alloc_ops.replace_realloc && realloc_func_in_set(edata.set) != NULL)
        generate_realloc_replacement(edata.set);
#ifdef USE_DRSYMS
    if (edata.processed != NULL)
        global_free(edata.processed, sizeof(*edata.processed)*num_possible, HEAPSTAT_MISC);
#endif
    return edata.set;
}

static size_t
redzone_size(alloc_routine_entry_t *routine)
{
    return ((routine->set != NULL && routine->set->use_redzone) ? 
            alloc_ops.redzone_size : 0);
}

/* XXX i#882: make this static once malloc replacement replaces operators */
void
/* XXX: if we split the wrapping from the routine identification we'll
 * have to figure out how to separate alloc_routine_entry_t: currently an
 * opaque type in alloc_private.h
 */
malloc_wrap__intercept(app_pc pc, routine_type_t type, alloc_routine_entry_t *e)
{
#ifdef WINDOWS
    if (e->type == HEAP_ROUTINE_DBG_NOP_FALSE) {
        /* cdecl so no args to clean up.
         * we can't just insert a generated ret b/c our slowpath
         * assumes the raw bits are persistent.
         */
        if (!drwrap_replace(pc, (app_pc)replaced_nop_false_routine, false))
            ASSERT(false, "failed to replace dbg-nop");
    } else if (e->type == HEAP_ROUTINE_DBG_NOP_TRUE) {
        /* see above */
        if (!drwrap_replace(pc, (app_pc)replaced_nop_true_routine, false))
            ASSERT(false, "failed to replace dbg-nop");
    } else {
#endif
        if (!drwrap_wrap_ex(pc, alloc_hook,
                            e->intercept_post ? handle_alloc_post : NULL,
                            (void *)e, DRWRAP_UNWIND_ON_EXCEPTION))
            ASSERT(false, "failed to wrap alloc routine");
#ifdef WINDOWS
    }
#endif
}

/* XXX i#882: make this static once malloc replacement replaces operators */
void
malloc_wrap__unintercept(app_pc pc, routine_type_t type, alloc_routine_entry_t *e)
{
#ifdef WINDOWS
    if (e->type == HEAP_ROUTINE_DBG_NOP_FALSE ||
        e->type == HEAP_ROUTINE_DBG_NOP_TRUE) {
        if (!drwrap_replace(pc, NULL/*remove*/, true))
            ASSERT(false, "failed to unreplace dbg-nop");
    } else {
#endif
        if (!drwrap_unwrap(pc, alloc_hook,
                           e->intercept_post ? handle_alloc_post : NULL))
            ASSERT(false, "failed to unwrap alloc routine");
#ifdef WINDOWS
    }
#endif
}

/***************************************************************************
 * MALLOC SIZE
 */

#ifdef WINDOWS
typedef size_t (__stdcall *rtl_size_func_t)(IN reg_t /*really HANDLE*/ Heap,
                                            IN ULONG flags,
                                            IN PVOID ptr);
typedef size_t (*dbg_size_func_t)(IN byte *pc, int blocktype);
#else
/* points at libc's version, used in initial heap walk */
alloc_size_func_t malloc_usable_size;
#endif

/* malloc_usable_size exported, so declared in alloc.h */

static alloc_routine_entry_t *
get_size_func(alloc_routine_entry_t *routine)
{
    if (routine->set == NULL)
        return NULL;
    return size_func_in_set(routine->set);
}

static ssize_t
get_size_from_app_routine(IF_WINDOWS_(reg_t auxarg) app_pc real_base,
                          alloc_routine_entry_t *routine)
{
    ssize_t sz;
    alloc_routine_entry_t *size_func = get_size_func(routine);
    /* we avoid calling app size routine for two reasons: performance
     * (i#689 part 2) and correctness to avoid deadlocks (i#795, i#30).
     * for alloc_ops.get_padded_size, well, we risk it: removing the
     * drwrap lock (i#689 part 1 DRWRAP_NO_FRILLS) helps there b/c we
     * won't be holding a lock when we call here
     */
    ASSERT(alloc_ops.get_padded_size, "should not get here");
    ASSERT(!malloc_lock_held_by_self(), "should not hold lock here");
#ifdef WINDOWS
    if (is_rtl_routine(routine->type)) {
        /* auxarg is heap */
        reg_t heap = auxarg;
        ASSERT(heap != (reg_t)INVALID_HANDLE_VALUE && real_base != NULL, "invalid params");
        /* I used to use GET_NTDLL(RtlSizeHeap...)
         * but DR's private loader turned it into redirect_RtlSizeHeap
         * so going w/ what we stored from lookup
         */
        ASSERT(size_func != NULL, "invalid size func");
        /* 0 is an invalid value for a heap handle */
        if (heap == 0)
            return -1;
        else
            return (*(rtl_size_func_t)(size_func->pc))(heap, 0, real_base);
    }
#endif
    if (size_func != NULL) {
        /* WARNING: this is dangerous and a transparency violation since we're
         * calling an app library routine here, which can acquire an app lock.
         * We try to only do real handling in pre- and post- of outermost malloc
         * layers, where the app lock should NOT already be held, so we won't
         * have any conflicts with, say, malloc_lock.  We avoid asking for
         * malloc lock on inner layers by using
         * malloc_entry_exists_racy_nolock() for recursive handle_free_pre()
         * (I have seen a deadlock when acquiring malloc lock there for
         * RtlFreeHeap while holding app lock, and another thread has malloc
         * lock at _free_dbg() and wants app lock while calling _size_dbg()).
         */
#ifdef WINDOWS
        if (size_func->type == HEAP_ROUTINE_SIZE_REQUESTED_DBG) {
            /* auxarg is blocktype */
            sz = (*(dbg_size_func_t)(size_func->pc))(real_base, auxarg);
        } else
#endif
            sz = (*(alloc_size_func_t)(size_func->pc))(real_base);
        /* Note that malloc(0) has usable size > 0 */
        if (size_func->type == HEAP_ROUTINE_SIZE_USABLE && sz == 0)
            return -1;
        else
            return sz;
    }
    return -1;
}

/* Returns the size of an allocation as known to the underlying system
 * allocator (libc's malloc(), etc. for Linux, ntdll's Rtl*Heap for
 * Windows).  Unfortunately the interface exposed by the two return
 * different notions of size: for Linux we can only get the padded size,
 * while for Windows we can only get the requested size.
 * We'd prefer the requested size for all uses, and we use our hashtable
 * (which we now use for all mallocs by default) if possible.
 * Only if the hashtable lookup fails (e.g., during malloc prior to adding
 * to table) do we call the app size routine: and even then, users of this routine
 * simply need an upper bound on the requested size and a lower bound on the
 * padded size.
 * We can get the padded size for Windows via get_alloc_real_size().
 * Returns -1 on failure.
 */
static ssize_t
get_alloc_size(IF_WINDOWS_(reg_t auxarg) app_pc real_base, alloc_routine_entry_t *routine)
{
    ssize_t sz;
    /* i#30: if alloc_ops.record_allocs, prefer hashtable to avoid app lock
     * which can lead to deadlock
     */
    /* This will fail at post-malloc point before we've added to hashtable:
     * though currently it's debug msvcrt operator delete that's the only
     * problem, so we're ok w/ alloc calling app routine
     */
    sz = malloc_size(real_base);
    if (sz != -1)
        return sz + 2*redzone_size(routine);
    return get_size_from_app_routine(IF_WINDOWS_(auxarg) real_base, routine);
}

/* Returns the full usable footprint of the allocation at real_base.
 * Returns -1 on error.
 */
static ssize_t
get_padded_size(IF_WINDOWS_(reg_t auxarg) app_pc real_base, alloc_routine_entry_t *routine)
{
#ifdef LINUX
    /* malloc_usable_size() includes padding */
    ASSERT(routine->set != NULL &&
           routine->set->func[HEAP_ROUTINE_SIZE_USABLE] != NULL,
           "assuming linux has usable size avail");
    return get_size_from_app_routine(real_base, routine);
#else
    /* FIXME: this is all fragile: any better way, besides using our
     * own malloc() instead of intercepting system's?
     */
    alloc_routine_entry_t *size_func = get_size_func(routine);
# define HEAP_MAGIC_OFFS 0x50
    reg_t heap = auxarg;
    ssize_t result;
    ushort pad_size;
    ssize_t req_size;
    IF_DEBUG(byte delta);
# ifdef X64
    /* FIXME i#906: need to investigate the header format in 64-bit windows
     * to get actual padded size.
     */
    return ALIGN_FORWARD(get_alloc_size(auxarg, real_base, routine),
                         MALLOC_CHUNK_ALIGNMENT);
# endif
    if (!is_rtl_routine(routine->type) || auxarg == 0/*invalid heap for Rtl*/
# ifdef TOOL_DR_MEMORY
        /* FIXME i#789: win7sp1 has different header for padded_size.
         * the temporary solution is to return aligned size instead until
         * we know how to parse the header for correct padded size.
         */
        /* XXX i#789: need test on win7sp0 to check if we need skip sp0 too */
        || running_on_Win7SP1_or_later()
# endif
        ) {
        if (size_func == NULL ||
            is_size_requested_routine(size_func->type)) {
            /* FIXME PR 595800: should look at headers and try to
             * figure out padded size.  For now we just guess by aligning.
             */
            size_t sz = get_alloc_size(auxarg, real_base, routine);
            return ALIGN_FORWARD(sz, MALLOC_CHUNK_ALIGNMENT);
        } else {
            /* malloc_usable_size() includes padding */
            return get_size_from_app_routine(auxarg, real_base, routine);
        }
    }
    if (running_on_Vista_or_later()) {
        /* Some obfuscation is used to make exploits harder:
         * The first header dword must be xor-ed with a cookie
         * at heap+0x50.
         * The delta seems to be the 1st byte of 2nd header dword
         * instead of the 2nd byte: but we don't need it unless we add
         * a sanity check vs real_size.
         */
        size_t dw1, dw2;
        if (!safe_read((void *)(real_base-2*sizeof(size_t)), sizeof(dw1), &dw1) ||
            !safe_read((void *)(heap+HEAP_MAGIC_OFFS), sizeof(dw2), &dw2))
            ASSERT(false, "unable to access Rtl heap headers");
        pad_size = (ushort) (dw1 ^ dw2);
        IF_DEBUG(delta = (byte)(dw2 & 0xff));
    } else {
        /* Rtl heap headers: blocksize/8 is 1st 16 bits of header */
        IF_DEBUG(size_t dw2;)
        if (!safe_read((void *)(real_base-2*sizeof(size_t)), sizeof(pad_size), &pad_size))
            ASSERT(false, "unable to access Rtl heap headers");
        ASSERT(safe_read((void *)(real_base - sizeof(size_t)), sizeof(dw2), &dw2),
               "unable to access Rtl heap headers");
        /* i#892-c#9: the requested size difference is in the 3rd byte of the 2nd
         * header dword.
         */
        IF_DEBUG(delta = (byte)((dw2 >> 16) & 0xff));
    }
    if (!TEST(HEAP_ARENA, get_heap_region_flags(real_base)) &&
        /* There seem to be two extra heap header dwords, the first holding
         * the full size.  pad_size seems to hold the padding amount.
         */
        safe_read((void *)(real_base-4*sizeof(size_t)), sizeof(result), &result)) {
        /* Out-of-heap large alloc.  During execution we could
         * record the NtAllocateVirtualMemory but this routine
         * could be called at other times.
         */
# ifdef DEBUG
        req_size = get_alloc_size(heap, real_base, routine);
# endif
        ASSERT(result - pad_size == req_size, "Rtl large heap invalid assumption");
    } else {
        /* i#892: the size (pad_size << 3) get from header includes the size of
         * malloc header.
         */
        result = (pad_size << 3) - MALLOC_HEADER_SIZE;
# ifdef TOOL_DR_MEMORY
        /* FIXME i#363/i#789: some heap case we don't know how to read headers */
        /* sanity check for the size difference between real and request */
        ASSERT(delta >= MALLOC_HEADER_SIZE, "wrong size from header");
# endif
        req_size = get_alloc_size(heap, real_base, routine);
        if (result < req_size || result - req_size > 64*1024) {
            /* FIXME i#363: some heap case we don't know how to read headers for.
             * e.g., win7 LFH has a complex formula for some blocks.
             * for now we bail.
             */
            result = ALIGN_FORWARD(req_size, MALLOC_CHUNK_ALIGNMENT);
        }
    }
    return result;
#endif /* LINUX -> WINDOWS */
}

/***************************************************************************
 * MALLOC TRACKING
 *
 * We record the callstack and when allocated so we can report leaks.
 */

#ifdef USE_DRSYMS
# define POST_CALL_SYMCACHE_NAME "__DrMemory_post_call"
#endif

#ifdef USE_DRSYMS
/* We can't use post-call symcache entries at DR's module load event b/c
 * it's prior to rebasing.  Our solution is to wait for the first execution
 * from that module.  To find it we store the bounds in an interval tree.
 * Protected by post_call_lock.
 */
static rb_tree_t *mod_pending_tree;
static void *post_call_lock;
/* Used for quick check w/o need for lock.  Written while holding post_call_lock. */
static uint mod_pending_entries;
#endif

#ifdef USE_DRSYMS
static void
event_post_call_entry_added(app_pc postcall)
{
    module_data_t *data = dr_lookup_module(postcall);
    ASSERT(alloc_ops.cache_postcall, "shouldn't get here");
    if (data != NULL) {
        symcache_add(data, POST_CALL_SYMCACHE_NAME, postcall - data->start);
        dr_free_module_data(data);
    }
}
#endif

/* we need to know which heap allocations were there before we took
 * control (so we know whether size is stored in redzone) and for leak
 * reporting we also track the callstack for each alloc.
 * Synchronized externally to safeguard the externally-allocated payload.
 */
/* There are several cases where having an interval tree instead of a
 * hashtable would be useful:
 * - leak scanning wouldn't have to create its own interval tree by
 *   walking the entire hashtable on each nudge/exit
 * - locating nearby mallocs (PR 535568) would be more accurate and efficient
 * - locating live mallocs inside an unmapped heap arena would be
 *   more accurate and efficient (xref PR 520916)
 * - locating bounds of mallocs used as stack regions (PR 525807)
 * For PR 535568 I measured replacing the malloc hashtable with an
 * interval tree and the cost is noticeable on heap-intensive
 * benchmarks (and there aren't substantially more lookups than
 * insertions and deletions), so sticking with a hashtable!
 */
#define ALLOC_TABLE_HASH_BITS 12
static hashtable_t malloc_table;
/* we could switch to a full-fledged known-owner lock, or a recursive lock.
 * xref i#129.
 */
#define THREAD_ID_INVALID ((thread_id_t)0) /* invalid thread id on Linux+Windows */
static thread_id_t malloc_lock_owner = THREAD_ID_INVALID;

/* PR 525807: to handle malloc-based stacks we need an interval tree
 * for large mallocs.  Putting all mallocs in a tree instead of a table
 * is too expensive (PR 535568).
 */
static rb_tree_t *large_malloc_tree;
static void *large_malloc_lock;

enum {
    MALLOC_VALID  = MALLOC_RESERVED_1,
    MALLOC_PRE_US = MALLOC_RESERVED_2,
    /* MALLOC_ALLOCATOR_FLAGS use (MALLOC_RESERVED_3 | MALLOC_RESERVED_4) */
    MALLOC_ALLOCATOR_CHECKED   = MALLOC_RESERVED_5,
    /* We ignore Rtl*Heap-internal allocs */
    MALLOC_RTL_INTERNAL        = MALLOC_RESERVED_6,
    /* i#607 part A: try to handle msvc*d.dll w/o syms */
    MALLOC_LIBC_INTERNAL_ALLOC = MALLOC_RESERVED_7,
    /* The rest are reserved for future use */
};

/* We could save space by storing this in the redzone, if big enough,
 * though we'd have to squash app writes there (on that note we're not
 * preventing app from clobbering our size stored in redzone)
 */
typedef struct _malloc_entry_t {
    app_pc start;
    app_pc end;
    /* Dr. Heapstat needs to know the usable size.  To look it up needs the Heap
     * handle on Windows.  We could store that in the heap_region list but we'd
     * need some work to identify it for large mallocs that use
     * NtAllocateVirtualMemory.  For now we avoid that lookup by storing the
     * extra size as a diff to save space.
     * Update: we now have the Heap handle in the rbtree so we could use that.
     */
    ushort usable_extra;
    ushort flags; /* holds MALLOC_* flags */
    void *data;
} malloc_entry_t;

/* Returns true if the malloc entry is ignored by us,
 * e.g. entry for windows internal rtl allocation.
 */
static inline bool
malloc_entry_is_native(malloc_entry_t *e)
{
    if (e == NULL)
        return true;
#ifdef WINDOWS
    return TEST(MALLOC_RTL_INTERNAL, e->flags);
#else
    return false;
#endif
}

static void
malloc_entry_free(void *v)
{
    malloc_entry_t *e = (malloc_entry_t *) v;
    if (!malloc_entry_is_native(e))
        client_malloc_data_free(e->data);
    global_free(e, sizeof(*e), HEAPSTAT_HASHTABLE);
}

/* Mallocs are aligned to 8 so drop the bottom 3 bits */
static uint
malloc_hash(void *v)
{
    uint hash = (uint)(ptr_uint_t) v;
    ASSERT(MALLOC_CHUNK_ALIGNMENT == 8, "update hash func please");
    /* Many mallocs are larger than 8 and we get fewer collisions w/ >> 5 */
    return (hash >> 5);
}

/* If track_allocs is false, only callbacks and callback returns are tracked.
 * Else: if track_heap is false, only syscall allocs are tracked;
 *       else, syscall allocs and mallocs are tracked.
 */
void
alloc_init(alloc_options_t *ops, size_t ops_size)
{
    drmgr_priority_t pri_insert = {sizeof(pri_insert), "drmemory.alloc.insert",
                                   /* must go after CLS tracking */
                                   NULL, DRMGR_PRIORITY_NAME_CLS,
                                   DRMGR_PRIORITY_INSERT_ALLOC};
    if (!drmgr_register_bb_instrumentation_event(alloc_event_bb_analysis,
                                                 alloc_event_bb_insert, &pri_insert))
        ASSERT(false, "drmgr registration failed");

    ASSERT(ops_size <= sizeof(alloc_ops), "option struct too large");
    memcpy(&alloc_ops, ops, ops_size);
    ASSERT(alloc_ops.track_allocs || !alloc_ops.track_heap,
           "track_heap requires track_allocs");

    cls_idx_alloc = drmgr_register_cls_field(alloc_context_init, alloc_context_exit);
    ASSERT(cls_idx_alloc > -1, "unable to reserve CLS field");

    if (!alloc_ops.track_allocs)
        alloc_ops.track_heap = false;

    if (alloc_ops.track_allocs) {
        hashtable_init_ex(&alloc_routine_table, ALLOC_ROUTINE_TABLE_HASH_BITS,
                          HASH_INTPTR, false/*!str_dup*/, false/*!synch*/,
                          alloc_routine_entry_free, NULL, NULL);
        alloc_routine_lock = dr_mutex_create();
        /* We want leaner wrapping and we are ok w/ no dups and no dynamic
         * wrap changes
         */
        drwrap_set_global_flags(DRWRAP_NO_FRILLS | DRWRAP_FAST_CLEANCALLS);
    }

    if (alloc_ops.replace_realloc) {
        /* we need generated code for our realloc replacements */
        /* b/c we may need to add to this gencode during execution if
         * the app loads a library w/ a "realloc", we keep it read-only
         * to work around DRi#404.
         */
        gencode_start = (byte *)
            nonheap_alloc(GENCODE_SIZE,
                          DR_MEMPROT_READ|DR_MEMPROT_EXEC,
                          HEAPSTAT_GENCODE);
        gencode_cur = gencode_start;
        gencode_lock = dr_mutex_create();
        if (!drwrap_wrap((app_pc)replace_realloc_size_app, replace_realloc_size_pre,
                         replace_realloc_size_post))
            ASSERT(false, "failed to wrap realloc size");
    }

    if (alloc_ops.track_allocs) {
        hashtable_config_t hashconfig;
        hashtable_init_ex(&malloc_table, ALLOC_TABLE_HASH_BITS, HASH_INTPTR,
                          false/*!str_dup*/, false/*!synch*/, malloc_entry_free,
                          malloc_hash, NULL);
        /* hash lookup can be a bottleneck so it's worth taking some extra space
         * to reduce the collision chains
         */
        hashconfig.size = sizeof(hashconfig);
        hashconfig.resizable = true;
        hashconfig.resize_threshold = 50; /* default is 75 */
        hashtable_configure(&malloc_table, &hashconfig);

        large_malloc_tree = rb_tree_create(NULL);
        large_malloc_lock = dr_mutex_create();
#ifdef USE_DRSYMS
        if (alloc_ops.cache_postcall)
            mod_pending_tree = rb_tree_create(NULL);
#endif
    }

#ifdef USE_DRSYMS
    if (alloc_ops.track_allocs && alloc_ops.cache_postcall) {
        post_call_lock = dr_mutex_create();
        if (!drwrap_register_post_call_notify(event_post_call_entry_added))
            ASSERT(false, "drwrap event registration failed");
    }
#endif

    /* set up the per-malloc API */
    if (alloc_ops.replace_malloc)
        alloc_replace_init();
    else
        malloc_wrap_init();
}

void
alloc_exit(void)
{
    /* Check for leaks.
     * FIXME: provide a hashtable iterator instead of breaking abstraction
     * barrier here.
     */
    uint i;
    if (!alloc_ops.track_allocs)
        return;

    if (alloc_ops.replace_malloc)
        alloc_replace_exit();

    hashtable_delete_with_stats(&alloc_routine_table, "alloc routine table");
    dr_mutex_destroy(alloc_routine_lock);

    LOG(1, "final malloc table size: %u bits, %u entries\n",
        malloc_table.table_bits, malloc_table.entries);
    /* we can't hold malloc_table.lock b/c report_leak() acquires it
     * for malloc_get_caller()
     */
    for (i = 0; i < HASHTABLE_SIZE(malloc_table.table_bits); i++) {
        hash_entry_t *he;
        for (he = malloc_table.table[i]; he != NULL; he = he->next) {
            malloc_entry_t *e = (malloc_entry_t *) he->payload;
            if (TEST(MALLOC_VALID, e->flags) && !malloc_entry_is_native(e)) {
                client_exit_iter_chunk(e->start, e->end, TEST(MALLOC_PRE_US, e->flags),
                                       e->flags, e->data);
            }
        }
    }

    if (alloc_ops.track_allocs) {
        hashtable_delete(&malloc_table);
        rb_tree_destroy(large_malloc_tree);
        dr_mutex_destroy(large_malloc_lock);
#ifdef USE_DRSYMS
        if (alloc_ops.cache_postcall) {
            dr_mutex_destroy(post_call_lock);
            rb_tree_destroy(mod_pending_tree);
        }
#endif
    }

    if (alloc_ops.replace_realloc) {
        nonheap_free(gencode_start, GENCODE_SIZE, HEAPSTAT_GENCODE);
        dr_mutex_destroy(gencode_lock);
    }

    drmgr_unregister_cls_field(alloc_context_init, alloc_context_exit, cls_idx_alloc);
}

static uint
malloc_allocator_type(alloc_routine_entry_t *routine)
{
    if (routine->type == HEAP_ROUTINE_NEW ||
        routine->type == HEAP_ROUTINE_NEW_NOTHROW ||
        routine->type == HEAP_ROUTINE_DELETE ||
        routine->type == HEAP_ROUTINE_DELETE_NOTHROW)
        return MALLOC_ALLOCATOR_NEW;
    else if (routine->type == HEAP_ROUTINE_NEW_ARRAY ||
             routine->type == HEAP_ROUTINE_NEW_ARRAY_NOTHROW ||
             routine->type == HEAP_ROUTINE_DELETE_ARRAY ||
             routine->type == HEAP_ROUTINE_DELETE_ARRAY_NOTHROW)
        return MALLOC_ALLOCATOR_NEW_ARRAY;
    else
        return MALLOC_ALLOCATOR_MALLOC;
}

#ifdef WINDOWS
static void
alloc_find_syscalls(void *drcontext, const module_data_t *info)
{
    const char *modname = dr_module_preferred_name(info);
    if (modname == NULL)
        return;

    if (stri_eq(modname, "ntdll.dll")) {
        app_pc addr_KiCallback = (app_pc)
            dr_get_proc_address(info->handle, "KiUserCallbackDispatcher");
        ASSERT(addr_KiCallback != NULL, "can't find Ki routine");
        if (!drwrap_wrap_ex(addr_KiCallback, alloc_wrap_Ki, NULL, (void*)1, 0))
            ASSERT(false, "failed to wrap");
        if (alloc_ops.track_allocs) {
            app_pc addr_KiAPC, addr_KiLdrThunk, addr_KiException, addr_KiRaise;
            addr_KiAPC = (app_pc) dr_get_proc_address(info->handle,
                                                      "KiUserApcDispatcher");
            addr_KiLdrThunk = (app_pc) dr_get_proc_address(info->handle,
                                                           "LdrInitializeThunk");
            addr_KiException = (app_pc) dr_get_proc_address(info->handle,
                                                            "KiUserExceptionDispatcher");
            addr_KiRaise = (app_pc) dr_get_proc_address(info->handle,
                                                        "KiRaiseUserExceptionDispatcher");
            /* Assuming that KiUserCallbackExceptionHandler,
             * KiUserApcExceptionHandler, and the Ki*SystemCall* routines are not
             * entered from the kernel.
             */
            ASSERT(addr_KiAPC != NULL && addr_KiLdrThunk != NULL &&
                   addr_KiException != NULL && addr_KiRaise != NULL,
                   "can't find Ki routine");
            if (!drwrap_wrap_ex(addr_KiAPC, alloc_wrap_Ki, NULL, (void*)0, 0) ||
                !drwrap_wrap(addr_KiException, alloc_wrap_exception, NULL) ||
                !drwrap_wrap_ex(addr_KiRaise, alloc_wrap_Ki, NULL, (void*)0, 0))
                ASSERT(false, "failed to wrap");
            /* we should ignore LdrInitializeThunk on pre-Vista since we'll get
             * there via APC: only on Vista+ is it a "Ki" routine
             */
            if (running_on_Vista_or_later() &&
                !drwrap_wrap_ex(addr_KiLdrThunk, alloc_wrap_Ki, NULL, (void*)0, 0))
                ASSERT(false, "failed to wrap");
            
            sysnum_mmap = sysnum_from_name(drcontext, info, "NtMapViewOfSection");
            ASSERT(sysnum_mmap != -1, "error finding alloc syscall #");
            sysnum_munmap = sysnum_from_name(drcontext, info, "NtUnmapViewOfSection");
            ASSERT(sysnum_munmap != -1, "error finding alloc syscall #");
            sysnum_valloc = sysnum_from_name(drcontext, info, "NtAllocateVirtualMemory");
            ASSERT(sysnum_valloc != -1, "error finding alloc syscall #");
            sysnum_vfree = sysnum_from_name(drcontext, info, "NtFreeVirtualMemory");
            ASSERT(sysnum_vfree != -1, "error finding alloc syscall #");
            sysnum_continue = sysnum_from_name(drcontext, info, "NtContinue");
            ASSERT(sysnum_continue != -1, "error finding alloc syscall #");
            sysnum_cbret = sysnum_from_name(drcontext, info, "NtCallbackReturn");
            ASSERT(sysnum_cbret != -1, "error finding alloc syscall #");
            sysnum_setcontext = sysnum_from_name(drcontext, info, "NtSetContextThread");
            ASSERT(sysnum_setcontext != -1, "error finding alloc syscall #");
            sysnum_mapcmf = sysnum_from_name(drcontext, info, "NtMapCMFModule");
            ASSERT(sysnum_mapcmf != -1 || !running_on_Win7_or_later(),
                   "error finding alloc syscall #");
            sysnum_RaiseException = sysnum_from_name(drcontext, info, "NtRaiseException");
            ASSERT(sysnum_RaiseException != -1, "error finding alloc syscall #");
            
            if (alloc_ops.track_heap) {
                dr_mutex_lock(alloc_routine_lock);
                find_alloc_routines(info, possible_rtl_routines,
                                    POSSIBLE_RTL_ROUTINE_NUM, true/*redzone*/,
                                    true/*mismatch*/, false/*may not see all*/,
                                    HEAPSET_RTL, NULL);
                dr_mutex_unlock(alloc_routine_lock);
            }
        }
    } else if (stri_eq(modname, "user32.dll")) {
        sysnum_UserConnectToServer =
            sysnum_from_name(drcontext, info, "UserConnectToServer");
        /* UserConnectToServer is not exported and so requires symbols or i#388's
         * table.  It's not present prior to Vista or on 32-bit kernels.
         */
        ASSERT(sysnum_UserConnectToServer != -1 ||
               !is_wow64_process() || !running_on_Vista_or_later(),
               "error finding alloc syscall #");
    }
}
#endif

#ifdef USE_DRSYMS
/* caller must hold post_call_lock */
static void
alloc_load_symcache_postcall(const module_data_t *info)
{
    ASSERT(info != NULL, "invalid args");
    ASSERT(dr_mutex_self_owns(post_call_lock), "caller must hold lock");
    if (alloc_ops.track_allocs && alloc_ops.cache_postcall) {
        size_t modoffs;
        uint count;
        uint idx;
        ASSERT(symcache_module_is_cached(info), "must have symcache");
        for (idx = 0, count = 1;
             idx < count && symcache_lookup(info, POST_CALL_SYMCACHE_NAME,
                                            idx, &modoffs, &count); idx++) {
            /* XXX: drwrap_mark_as_post_call is going to go grab yet another
             * lock.  Should we expose drwrap's locks?
             */
            if (modoffs != 0)
                drwrap_mark_as_post_call(info->start + modoffs);
        }
    }
}
#endif

#ifdef WINDOWS
static bool
module_has_pdb(const module_data_t *info)
{
# ifdef USE_DRSYMS
    /* Our notion of whether we have symbols must match symcache
     * b/c us thinking we have symbols and symcache having negative
     * entries is a disaster (i#973).
     */
    if (op_use_symcache && symcache_module_is_cached(info))
        return symcache_module_has_debug_info(info);
    else
        return module_has_debug_info(info);
# else
    return false;
# endif
}
#endif

void
alloc_module_load(void *drcontext, const module_data_t *info, bool loaded)
{
    alloc_routine_set_t *set_libc = NULL;
    alloc_routine_set_t *set_cpp = NULL;
    bool use_redzone = true;
#ifdef WINDOWS
    /* i#607 part C: is msvcp*d.dll present, yet we do not have symbols? */
    bool dbgcpp = false, dbgcpp_nosyms = false;
#endif
    bool search_syms = true, search_libc_syms = true;
    const char *modname = dr_module_preferred_name(info);

#ifdef WINDOWS
    alloc_find_syscalls(drcontext, info);
#endif

    if (modname != NULL &&
        (strcmp(modname, "drmemorylib.dll") == 0 ||
         strcmp(modname, "dynamorio.dll") == 0))
        search_syms = false;

#ifdef WINDOWS
    if (alloc_ops.skip_msvc_importers && search_syms &&
        module_imports_from_msvc(info) &&
        (modname == NULL ||
         (!text_matches_pattern(modname, "msvcp*.dll", true/*ignore case*/) &&
          !text_matches_pattern(modname, "msvcr*.dll", true/*ignore case*/)))) {
        /* i#963: assume there are no static libc routines if the module
         * imports from dynamic libc (unless it's dynamic C++ lib).
         * Note that we'll still pay the cost of loading the module for
         * string routine replacement, but we'll save in time and dbghelp
         * data structure space by not looking up all the alloc syms.
         * Also note that malloc, etc. still show up in the syms b/c they're
         * stub routines for IAT import.
         * XXX: Should we look through the imports and see whether malloc
         * is there?
         */
        LOG(1, "module %s imports from msvc* so not searching inside it\n",
            modname == NULL ? "" : modname);
        search_libc_syms = false;
    }
#endif

    if (alloc_ops.track_heap && search_syms) {
#ifdef WINDOWS
        bool no_dbg_routines = false;
#endif

#ifdef WINDOWS
        /* match msvcrtd.dll and msvcrNNd.dll */
        if (modname != NULL &&
            text_matches_pattern(modname, "msvcr*d.dll", true/*ignore case*/)) {
            if (!module_has_pdb(info)) {
                if (alloc_ops.replace_malloc) {
                    /* FIXME i#607 part A for replacing: we don't yet have the
                     * nosyms fallback in place
                     */
                    NOTIFY_ERROR("FATAL ERROR: usage of Visual Studio Debug C DLL "
                                 "(without its symbols) is not supported. "
                                 "Please rebuild your application with either the Release "
                                 "build (/MD or /MT) or static Debug library (/MTd). "
                                 "With /MD, to avoid C++ Debug DLL, remove _DEBUG define."
                                 NL);
                    dr_abort();
                } else {
                    /* i#607 part A: we need pdb symbols for dbgcrt in order to intercept
                     * all allocations.  If we don't have them we have a fallback but it
                     * may have false negatives.
                     * XXX i#143: add automated symbol retrieval
                     */
                    dbgcrt_nosyms = true;
                }
            }
        } else if (modname != NULL &&
                   text_matches_pattern(modname, "msvcp*d.dll", true/*ignore case*/)) {
            dbgcpp = true;
            if (!module_has_pdb(info)) {
                /* i#607 part C: w/o symbols we have to disable
                 * mismatch detection within msvcp*d.dll b/c we won't be able to
                 * locate std::_DebugHeapDelete<*> for i#722.
                 */
                dbgcpp_nosyms = true;
                WARN("WARNING: no symbols for %s so disabling mismatch detection\n",
                     modname);
            }
        }
#endif

        dr_mutex_lock(alloc_routine_lock);
#ifdef WINDOWS
        if (search_libc_syms && lookup_symbol_or_export(info, "_malloc_dbg") != NULL) {
            if (modname == NULL ||
                !text_matches_pattern(modname, "msvcrt.dll", true/*ignore case*/)) {
                /* i#500: debug operator new calls either malloc, which calls
                 * _nh_malloc_dbg, or calls _nh_malloc_dbg directly; yet debug
                 * operator delete calls _free_dbg: so we're forced to disable all
                 * redzones since the layers don't line up.  In general when using
                 * debug version of msvcrt everything is debug, so we disable
                 * redzones for all C and C++ allocators.  However, msvcrt.dll
                 * contains _malloc_dbg (but not _nh_malloc_dbg), yet its operator
                 * new is not debug and calls (regular) malloc.  Note that
                 * _nh_malloc_dbg is not exported so we can't use that as a decider.
                 */
                use_redzone = false;
                LOG(1, "NOT using redzones for any allocators in %s "PFX"\n",
                    (modname == NULL) ? "<noname>" : modname, info->start);
            } else {
                LOG(1, "NOT using redzones for _dbg routines in %s "PFX"\n",
                    (modname == NULL) ? "<noname>" : modname, info->start);
            }
        } else {
            /* optimization: assume no other dbg routines if no _malloc_dbg */
            no_dbg_routines = true;
        }
#endif
        if (search_libc_syms) {
            set_libc = find_alloc_routines(info, possible_libc_routines,
                                           POSSIBLE_LIBC_ROUTINE_NUM, use_redzone,
                                           true/*mismatch*/, false/*!expect all*/,
                                           HEAPSET_LIBC, NULL);
            if (info->start == get_libc_base(NULL)) {
                if (set_dyn_libc != NULL)
                    WARN("WARNING: two libcs found");
                set_dyn_libc = set_libc;
            }
        }
#ifdef WINDOWS
        /* i#26: msvcrtdbg adds its own redzone that contains a debugging
         * data structure.  The problem is that operator delete() assumes
         * this data struct is placed immediately prior to the ptr
         * returned by malloc.  We aren't intercepting new or delete
         * as full layers so we simply skip our redzone for msvcrtdbg: after all there's
         * already a redzone there.
         */
        /* We watch debug operator delete b/c it reads malloc's headers (i#26)
         * but we now watch it in general so we don't need to special-case
         * it here.
         */
        if (search_libc_syms && !no_dbg_routines) {
            alloc_routine_set_t *set_dbgcrt =
                find_alloc_routines(info, possible_crtdbg_routines,
                                    POSSIBLE_CRTDBG_ROUTINE_NUM, false/*no redzone*/,
                                    true/*mismatch*/, false/*!expect all*/,
                                    HEAPSET_LIBC_DBG, set_libc);
            /* XXX i#967: not sure this is right: some malloc calls,
             * or operator new, might use shared libc?
             */
            if (set_libc == NULL)
                set_libc = set_dbgcrt;
        }
#endif
        if (alloc_ops.intercept_operators) {
            alloc_routine_set_t *corresponding_libc = set_libc;
#ifdef WINDOWS
            /* we assume we only hit i#26 where op delete reads heap
             * headers when _malloc_dbg is present in same lib, for
             * static debug msvcp.  for dynamic we check by name.
             */
            bool is_dbg = (!no_dbg_routines || dbgcpp);
#endif
            /* Assume that a C++ library w/o a local libc calls into the
             * shared libc.
             * XXX i#967: this may not be the right thing: to be sure we'd
             * have to disasm the library's operator new to find the malloc
             * set it calls into.
             */
            if (corresponding_libc == NULL)
                corresponding_libc = set_dyn_libc;
#ifdef WINDOWS
            /* Even if the module imports from msvcp*.dll it can still have
             * template instantiations in it of std::_DebugHeapDelete.
             * Plus, we need to intercept the local operator stubs in order
             * to properly do heap mismatch checks.
             */
#endif
            set_cpp = find_alloc_routines(info, possible_cpp_routines,
                                          POSSIBLE_CPP_ROUTINE_NUM, use_redzone,
                                          IF_WINDOWS_ELSE(!dbgcpp_nosyms, true),
                                          false/*!expect all*/,
                                          IF_WINDOWS(is_dbg ? HEAPSET_CPP_DBG :)
                                          HEAPSET_CPP, corresponding_libc);
        }
        if (set_cpp != NULL) {
            /* for static, use corresponding libc for size.
             * for dynamic, use dynamic libc.
             */
            alloc_routine_set_t *cpp_libc =
                (set_libc == NULL) ? set_dyn_libc : set_libc;
            if (cpp_libc != NULL) {
                set_cpp->func[HEAP_ROUTINE_SIZE_USABLE] =
                    cpp_libc->func[HEAP_ROUTINE_SIZE_USABLE];
                set_cpp->func[HEAP_ROUTINE_SIZE_REQUESTED] =
                    cpp_libc->func[HEAP_ROUTINE_SIZE_REQUESTED];
            } else
                WARN("WARNING: no libc found for cpp\n");
        }
        dr_mutex_unlock(alloc_routine_lock);
    }

#ifdef USE_DRSYMS
    if (alloc_ops.track_allocs && alloc_ops.cache_postcall &&
        symcache_module_is_cached(info)) {
        dr_mutex_lock(post_call_lock);
        if (loaded) {
            alloc_load_symcache_postcall(info);
        } else {
            /* i#690: we can't add post-call entries now b/c the module has
             * not been rebased and we need to store actual code so we
             * use a pending entry.
             */
            IF_DEBUG(rb_node_t *existing =)
                rb_insert(mod_pending_tree, info->start, info->end - info->start, NULL);
            mod_pending_entries++;
            ASSERT(existing == NULL, "new module overlaps w/ existing");
        }
        dr_mutex_unlock(post_call_lock);
    }
#endif
}

#ifdef USE_DRSYMS
static void
alloc_check_pending_module(app_pc pc)
{
    if (alloc_ops.track_allocs && alloc_ops.cache_postcall) {
        rb_node_t *node;

        /* avoid lookup on every single bb */
        static app_pc last_page;
        if (mod_pending_entries == 0 || (app_pc)PAGE_START(pc) == last_page)
            return;
        last_page = (app_pc)PAGE_START(pc);

        /* checks above make it ok to use write lock here */
        dr_mutex_lock(post_call_lock);
        node = rb_in_node(mod_pending_tree, pc);
        if (node != NULL) {
            module_data_t *info;
            rb_delete(mod_pending_tree, node);
            mod_pending_entries--;
            info = dr_lookup_module(pc);
            ASSERT(info != NULL, "module can't disappear");
            ASSERT(symcache_module_is_cached(info), "must have symcache");
            alloc_load_symcache_postcall(info);
            dr_free_module_data(info);
        }
        dr_mutex_unlock(post_call_lock);
    }
}
#endif

void
alloc_module_unload(void *drcontext, const module_data_t *info)
{
    if (alloc_ops.track_heap) {
        uint i;
        /* Rather than re-looking-up all the symbols, or storing
         * them all in some module-indexed table (we don't even
         * store them all in each set b/c of duplicates), we walk
         * the interception table.  It's not very big, and module
         * unload is rare.
         * chromium ui_tests run:
         *   final alloc routine table size: 7 bits, 44 entries
         */
        dr_mutex_lock(alloc_routine_lock);
        for (i = 0; i < HASHTABLE_SIZE(alloc_routine_table.table_bits); i++) {
            hash_entry_t *he, *nxt;
            for (he = alloc_routine_table.table[i]; he != NULL; he = nxt) {
                alloc_routine_entry_t *e = (alloc_routine_entry_t *) he->payload;
                /* we are removing while while iterating */
                nxt = he->next;
                if (e->set->modbase == info->start) {
                    IF_DEBUG(bool found;)
                    /* could wait for realloc but we remove on 1st hit */
                    if (e->set->realloc_replacement != NULL) {
                        /* put replacement routine on free list (i#545) */
                        byte **free_list;
                        dr_mutex_lock(gencode_lock);
                        if (!drwrap_replace(realloc_func_in_set(e->set)->pc,
                                            NULL/*remove*/, true))
                            ASSERT(false, "failed to un-replace realloc");
#ifdef WINDOWS
                        free_list = (e->set->type == HEAPSET_RTL) ? &gencode_free_Rtl :
                            ((e->set->type == HEAPSET_LIBC_DBG) ? &gencode_free_dbg :
                             &gencode_free);
#else
                        free_list = &gencode_free;
#endif
                        LOG(3, "writing "PFX" as next free list to "PFX"\n",
                            *free_list, e->set->realloc_replacement);
                        /* we keep read-only to work around DRi#404 */
                        if (!dr_memory_protect(gencode_start, GENCODE_SIZE,
                                               DR_MEMPROT_READ|DR_MEMPROT_WRITE|
                                               DR_MEMPROT_EXEC)) {
                            ASSERT(false, "failed to unprotect realloc gencode");
                        } else {
                            *((byte **)e->set->realloc_replacement) = *free_list;
                            if (!dr_memory_protect(gencode_start, GENCODE_SIZE,
                                                   DR_MEMPROT_READ|DR_MEMPROT_EXEC)) {
                                ASSERT(false, "failed to re-protect realloc gencode");
                            }
                        }
                        *free_list = e->set->realloc_replacement;
                        e->set->realloc_replacement = NULL;
                        dr_mutex_unlock(gencode_lock);
                    }

                    malloc_interface.malloc_unintercept(e->pc, e->type, e);

                    IF_DEBUG(found =)
                        hashtable_remove(&alloc_routine_table, (void *)e->pc);

                    LOG(3, "removing %s "PFX" from interception table: found=%d\n",
                        e->name, e->pc, found); 
                    DOLOG(1, {
                        if (!found) {
                            /* some dlls have malloc_usable_size and _msize
                             * pointing at the same place, which will trigger this
                             */
                            LOG(1, "WARNING: did not find %s @"PFX" for %s\n",
                                e->name, e->pc,
                                dr_module_preferred_name(info) == NULL ? "<null>" :
                                dr_module_preferred_name(info));
                        }
                    });
                }
            }
        }
        dr_mutex_unlock(alloc_routine_lock);
    }

    if (alloc_ops.track_allocs) {
#ifdef USE_DRSYMS
        if (alloc_ops.cache_postcall) {
            rb_node_t *node;
            dr_mutex_lock(post_call_lock);
            node = rb_in_node(mod_pending_tree, info->start);
            /* module unloaded w/o any code executed */
            if (node != NULL) {
                rb_delete(mod_pending_tree, node);
                mod_pending_entries--;
            }
            dr_mutex_unlock(post_call_lock);
        }
#endif
    }
}

void
alloc_fragment_delete(void *dc/*may be NULL*/, void *tag)
{
    /* switched to checking consistency at lookup time (i#673) */
    return;
}

/***************************************************************************
 * Per-malloc API routing
 */

void
malloc_lock(void)
{
    malloc_interface.malloc_lock();
}

void
malloc_unlock(void)
{
    malloc_interface.malloc_unlock();
}

app_pc
malloc_end(app_pc start)
{
    return malloc_interface.malloc_end(start);
}

void
malloc_add(app_pc start, app_pc end, app_pc real_end, bool pre_us,
           uint client_flags, dr_mcontext_t *mc, app_pc post_call)
{
    malloc_interface.malloc_add(start, end, real_end, pre_us,
                                    client_flags, mc, post_call);
}

bool
malloc_is_pre_us(app_pc start)
{
    return malloc_interface.malloc_is_pre_us(start);
}

bool
malloc_is_pre_us_ex(app_pc start, bool ok_if_invalid)
{
    return malloc_interface.malloc_is_pre_us_ex(start, ok_if_invalid);
}

ssize_t
malloc_size(app_pc start)
{
    return malloc_interface.malloc_size(start);
}

ssize_t
malloc_size_invalid_only(app_pc start)
{
    return malloc_interface.malloc_size_invalid_only(start);
}

void *
malloc_get_client_data(app_pc start)
{
    return malloc_interface.malloc_get_client_data(start);
}

uint
malloc_get_client_flags(app_pc start)
{
    return malloc_interface.malloc_get_client_flags(start);
}

bool
malloc_set_client_flag(app_pc start, uint client_flag)
{
    return malloc_interface.malloc_set_client_flag(start, client_flag);
}

bool
malloc_clear_client_flag(app_pc start, uint client_flag)
{
    return malloc_interface.malloc_clear_client_flag(start, client_flag);
}

void
malloc_iterate(malloc_iter_cb_t cb, void *iter_data)
{
    malloc_interface.malloc_iterate(cb, iter_data);
}

/***************************************************************************
 * Per-malloc API for wrapping
 */

/* We need to support our malloc routines being called either on their
 * own or from within malloc_iterate(), so we need self-recursion support
 * of one level.  We do not need general recursion support.
 */
static bool
malloc_lock_held_by_self(void)
{
    /* reading this variable should be atomic */
    void *drcontext = dr_get_current_drcontext();
    if (drcontext == NULL) {
        ASSERT(false, "should always have dcontext w/ PR 536058");
        return false;
    }
    return (dr_get_thread_id(drcontext) == malloc_lock_owner);
}

static void
malloc_lock_internal(void)
{
    void *drcontext = dr_get_current_drcontext();
    hashtable_lock(&malloc_table);
    if (drcontext != NULL) /* paranoid even w/ PR 536058 */
        malloc_lock_owner = dr_get_thread_id(drcontext);
}

static void
malloc_unlock_internal(void)
{
    malloc_lock_owner = THREAD_ID_INVALID;
    hashtable_unlock(&malloc_table);
}

static bool
malloc_lock_if_not_held_by_me(void)
{
    if (malloc_lock_held_by_self())
        return false;
    malloc_lock_internal();
    return true;
}

static void
malloc_unlock_if_locked_by_me(bool by_me)
{
    if (by_me)
        malloc_unlock_internal();
}

static void
malloc_wrap__lock(void)
{
    /* For external calls we can't store the result so we look up in unlock */
    malloc_lock_if_not_held_by_me();
}

static void
malloc_wrap__unlock(void)
{
    malloc_unlock_if_locked_by_me(malloc_lock_held_by_self());
}

/* If a client needs the real (usable) end, for pre_us mallocs the client can't
 * use get_alloc_real_size() via pt->auxarg as there is no pt and would need
 * the Heap handle passed in: instead we just pass in the real_end.  We don't
 * need real_base; we do pass real_base to client_handle_malloc() but we don't
 * need to store it, only real_end, driven by Dr. Heapstat's usage.
 */
static void
malloc_add_common(app_pc start, app_pc end, app_pc real_end,
                  uint flags, uint client_flags, dr_mcontext_t *mc, app_pc post_call,
                  uint alloc_type)
{
    malloc_entry_t *e = (malloc_entry_t *) global_alloc(sizeof(*e), HEAPSTAT_HASHTABLE);
    malloc_entry_t *old_e;
    bool locked_by_me;
    ASSERT((alloc_ops.redzone_size > 0 && TEST(MALLOC_PRE_US, flags)) ||
           alloc_ops.record_allocs, 
           "internal inconsistency on when doing detailed malloc tracking");
#ifdef USE_DRSYMS
    IF_WINDOWS(ASSERT(ALIGN_BACKWARD(start, 64*1024) != (ptr_uint_t) 
                      get_private_heap_handle(), "app using priv heap"));
#endif
    e->start = start;
    e->end = end;
    ASSERT(real_end != NULL && real_end - end <= USHRT_MAX, "real_end suspicously big");
    e->usable_extra = (real_end - end);
    e->flags = MALLOC_VALID | flags;
    e->flags |= alloc_type;
    LOG(3, "%s: type=%x\n", __FUNCTION__, alloc_type);
    e->flags |= (client_flags & MALLOC_POSSIBLE_CLIENT_FLAGS);
    /* grab lock around client call and hashtable operations */
    locked_by_me = malloc_lock_if_not_held_by_me();

    if (!malloc_entry_is_native(e)) { /* don't show internal allocs to client */
        e->data = client_add_malloc_pre(e->start, e->end, e->end + e->usable_extra,
                                        NULL, mc, post_call);
    } else
        e->data = NULL;

    ASSERT(is_entirely_in_heap_region(start, end), "heap data struct inconsistency");
    /* We invalidate rather than remove on a free and finalize the remove
     * when the free succeeds, so a race can hit a conflict.
     * Update: we no longer do this but leaving code for now
     */
    old_e = hashtable_add_replace(&malloc_table, (void *) start, (void *)e);

    if (!malloc_entry_is_native(e) && end - start >= LARGE_MALLOC_MIN_SIZE) {
        malloc_large_add(e->start, e->end - e->start);
    }

    if (!malloc_entry_is_native(e)) { /* don't show internal allocs to client */
        /* PR 567117: client event with entry in hashtable */
        client_add_malloc_post(e->start, e->end, e->end + e->usable_extra, e->data);
    }

#ifdef STATISTICS
    if (!malloc_entry_is_native(e))
        STATS_INC(num_mallocs);
    if (num_mallocs % 10000 == 0) {
        hashtable_cluster_stats(&malloc_table, "malloc table");
        LOG(1, "malloc table stats after %u malloc calls\n", num_mallocs);
    }
#endif

    malloc_unlock_if_locked_by_me(locked_by_me);
    if (old_e != NULL) {
        ASSERT(!TEST(MALLOC_VALID, old_e->flags), "internal error in malloc tracking");
        malloc_entry_free(old_e);
    }
    LOG(2, "MALLOC "PFX"-"PFX"\n", start, end);
    DOLOG(3, {
        print_callstack_to_file(dr_get_current_drcontext(), mc, post_call,
                                INVALID_FILE/*use pt->f*/);
    });
}

static void
malloc_wrap__add(app_pc start, app_pc end, app_pc real_end,
                 bool pre_us, uint client_flags, dr_mcontext_t *mc, app_pc post_call)
{
    malloc_add_common(start, end, real_end, pre_us ? MALLOC_PRE_US : 0,
                      client_flags, mc, post_call, 0);
}

/* up to caller to lock and unlock */
static malloc_entry_t *
malloc_lookup(app_pc start)
{
    return hashtable_lookup(&malloc_table, (void *) start);
}

/* Note that this also frees the entry.  Caller should be holding lock. */
static void
malloc_entry_remove(malloc_entry_t *e)
{
    app_pc start, end, real_end;
    bool native = malloc_entry_is_native(e);
    ASSERT(e != NULL, "invalid arg");
    if (!native) {
        /* cache values for post-event */
        start = e->start;
        end = e->end;
        real_end = e->end + e->usable_extra;
        client_remove_malloc_pre(e->start, e->end, e->end + e->usable_extra, e->data);
        if (e->end - e->start >= LARGE_MALLOC_MIN_SIZE) {
            malloc_large_remove(e->start);
        }
    }
    if (hashtable_remove(&malloc_table, e->start)) {
#ifdef STATISTICS
        if (!native)
            STATS_INC(num_frees);
#endif
    }
    if (!native) {
        /* PR 567117: client event with entry removed from hashtable */
        client_remove_malloc_post(start, end, real_end);
    }
}

#ifdef WINDOWS
static void
malloc_remove(app_pc start)
{
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = malloc_lookup(start);
    if (e != NULL)
        malloc_entry_remove(e);
    malloc_unlock_if_locked_by_me(locked_by_me);
}
#endif

static size_t
malloc_entry_size(malloc_entry_t *e)
{
    return (e == NULL ? (size_t)-1 : (e->end - e->start));
}

static ushort
malloc_entry_usable_extra(malloc_entry_t *e)
{
    return (e == NULL ? 0 : e->usable_extra);
}

/* caller must hold lock */
static void
malloc_entry_set_valid(malloc_entry_t *e, bool valid)
{
    if (e != NULL) {
        /* cache values for post-event */
        app_pc start = e->start, end = e->end, real_end = e->end + e->usable_extra;
        /* FIXME: should we tell client whether undoing false call failure prediction? */
        /* Call client BEFORE updating hashtable, to be consistent w/
         * other add/remove calls, so that any hashtable iteration will
         * NOT find the changes yet (PR 560824)
         */
        if (valid) {
            e->data = client_add_malloc_pre(e->start, e->end, e->end + e->usable_extra,
                                            e->data, NULL, NULL);
        } else {
            client_remove_malloc_pre(e->start, e->end, e->end + e->usable_extra, e->data);
        }
        ASSERT((TEST(MALLOC_VALID, e->flags) && !valid) ||
               (!TEST(MALLOC_VALID, e->flags) && valid),
               "internal error in malloc tracking");
        if (valid) {
            e->flags |= MALLOC_VALID;
            if (e->end - e->start >= LARGE_MALLOC_MIN_SIZE) {
                /* large malloc tree removes and re-adds rather than marking invalid
                 * b/c can recover data from hashtable on failure
                 */
                malloc_large_add(e->start, e->end - e->start);
            }
            /* PR 567117: client event with entry in hashtable */
            client_add_malloc_post(e->start, e->end, e->end + e->usable_extra, e->data);
        } else {
            e->flags &= ~MALLOC_VALID;
            if (e->end - e->start >= LARGE_MALLOC_MIN_SIZE) {
                /* large malloc tree removes and re-adds rather than marking invalid */
                malloc_large_remove(e->start);
            }
            /* PR 567117: client event with entry removed from hashtable */
            client_remove_malloc_post(start, end, real_end);
        }
    } /* ok to be NULL: a race where re-used in malloc and then freed already */
}

static void
malloc_set_valid(app_pc start, bool valid)
{
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        malloc_entry_set_valid(e, valid);
    malloc_unlock_if_locked_by_me(locked_by_me);
}

static bool
malloc_entry_is_pre_us(malloc_entry_t *e, bool ok_if_invalid)
{
    return (TEST(MALLOC_PRE_US, e->flags) &&
            (TEST(MALLOC_VALID, e->flags) || ok_if_invalid));
}

#ifdef WINDOWS
static bool
malloc_entry_is_libc_internal(malloc_entry_t *e)
{
    return TEST(MALLOC_LIBC_INTERNAL_ALLOC, e->flags);
}
#endif

/* if alloc has already been checked, returns 0.
 * if not, returns type, and then marks allocation as having been checked.
 * caller should hold lock
 */
static uint
malloc_alloc_entry_type(malloc_entry_t *e)
{
    uint res;
    if (TEST(MALLOC_ALLOCATOR_CHECKED, e->flags))
        res = 0;
    else {
        res = (e->flags & MALLOC_ALLOCATOR_FLAGS);
        e->flags |= MALLOC_ALLOCATOR_CHECKED;
    }
    return res;
}

/* if alloc has already been checked, returns 0.
 * if not, returns type, and then marks allocation as having been checked.
 */
static uint
malloc_alloc_type(byte *start)
{
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    uint res = 0;
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        res = malloc_alloc_entry_type(e);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return res;
}

static bool
malloc_wrap__is_pre_us_ex(app_pc start, bool ok_if_invalid)
{
    bool res = false;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        res = malloc_entry_is_pre_us(e, ok_if_invalid);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return res;
}

static bool
malloc_wrap__is_pre_us(app_pc start)
{
    return malloc_is_pre_us_ex(start, false/*only valid*/);
}

static void
malloc_set_pre_us(app_pc start)
{
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        e->flags |= MALLOC_PRE_US;
    malloc_unlock_if_locked_by_me(locked_by_me);
}

/* Returns true if the malloc is ignored by us */
static inline bool
malloc_entry_is_native_ex(malloc_entry_t *e, app_pc start, cls_alloc_t *pt,
                          bool consider_being_freed)
{
    if (malloc_entry_is_native(e))
        return true;
#ifdef WINDOWS
    /* the free routine might call other routines like size
     * after we removed from malloc table (i#432)
     */
    return (consider_being_freed && start == pt->alloc_being_freed &&
            start != NULL && (e == NULL || !TEST(MALLOC_VALID, e->flags)));
#else
    /* optimization: currently nothing in the table */
    return false;
#endif
}

static bool
malloc_is_native(app_pc start, cls_alloc_t *pt, bool consider_being_freed)
{
#ifdef WINDOWS
    bool res = false;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    res = malloc_entry_is_native_ex(e, start, pt, consider_being_freed);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return res;
#else
    /* optimization: currently nothing in the table */
    return false;
#endif
}

#ifdef DEBUG
/* WARNING: unsafe routine!  Could crash accessing memory that gets freed,
 * so only call when caller can assume entry should exist.
 */
static bool
malloc_entry_exists_racy_nolock(app_pc start)
{
    malloc_entry_t *e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    return (e != NULL && TEST(MALLOC_VALID, e->flags));
}
#endif

static app_pc
malloc_wrap__end(app_pc start)
{
    app_pc end = NULL;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL && TEST(MALLOC_VALID, e->flags))
        end = e->end;
    malloc_unlock_if_locked_by_me(locked_by_me);
    return end;
}

/* Returns -1 on failure */
static ssize_t
malloc_wrap__size(app_pc start)
{
    ssize_t sz = -1;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL && TEST(MALLOC_VALID, e->flags))
        sz = (e->end - start);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return sz;
}

/* Returns -1 on failure.  Only looks at invalid malloc regions. */
static ssize_t
malloc_wrap__size_invalid_only(app_pc start)
{
    ssize_t sz = -1;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL && !TEST(MALLOC_VALID, e->flags))
        sz = (e->end - start);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return sz;
}

static void *
malloc_wrap__get_client_data(app_pc start)
{
    void *res = NULL;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        res = e->data;
    malloc_unlock_if_locked_by_me(locked_by_me);
    return res;
}

static uint
malloc_wrap__get_client_flags(app_pc start)
{
    uint res = 0;
    malloc_entry_t *e;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL)
        res = (e->flags & MALLOC_POSSIBLE_CLIENT_FLAGS);
    malloc_unlock_if_locked_by_me(locked_by_me);
    return res;
}

static bool
malloc_wrap__set_client_flag(app_pc start, uint client_flag)
{
    malloc_entry_t *e;
    bool found = false;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL) {
        e->flags |= (client_flag & MALLOC_POSSIBLE_CLIENT_FLAGS);
        found = true;
    }
    malloc_unlock_if_locked_by_me(locked_by_me);
    return found;
}

static bool
malloc_wrap__clear_client_flag(app_pc start, uint client_flag)
{
    malloc_entry_t *e;
    bool found = false;
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    e = (malloc_entry_t *) hashtable_lookup(&malloc_table, (void *) start);
    if (e != NULL) {
        e->flags &= ~(client_flag & MALLOC_POSSIBLE_CLIENT_FLAGS);
        found = true;
    }
    malloc_unlock_if_locked_by_me(locked_by_me);
    return found;
}

static void
malloc_iterate_internal(bool include_native, malloc_iter_cb_t cb, void *iter_data)
{
    uint i;
    /* we do support being called while malloc lock is held but caller should
     * be careful that table is in a consistent state (staleness does this)
     */
    bool locked_by_me = malloc_lock_if_not_held_by_me();
    for (i = 0; i < HASHTABLE_SIZE(malloc_table.table_bits); i++) {
        hash_entry_t *he, *nxt;
        for (he = malloc_table.table[i]; he != NULL; he = nxt) {
            malloc_entry_t *e = (malloc_entry_t *) he->payload;
            /* support malloc_remove() while iterating */
            nxt = he->next;
            if (TEST(MALLOC_VALID, e->flags) &&
                (include_native || !malloc_entry_is_native(e))) {
                if (!cb(e->start, e->end, e->end + e->usable_extra,
                        TEST(MALLOC_PRE_US, e->flags),
                        include_native ? e->flags :
                        (e->flags & MALLOC_POSSIBLE_CLIENT_FLAGS),
                        e->data, iter_data)) {
                    goto malloc_iterate_done;
                }
            }
        }
    }
 malloc_iterate_done:
    malloc_unlock_if_locked_by_me(locked_by_me);
}

static void
malloc_wrap__iterate(malloc_iter_cb_t cb, void *iter_data)
{
    malloc_iterate_internal(false, cb, iter_data);
}

static void *
malloc_wrap__set_init(heapset_type_t type, app_pc pc, void *libc_data)
{
    return NULL;
}

static void
malloc_wrap__set_exit(heapset_type_t type, app_pc pc, void *user_data, void *libc_data)
{
    /* nothing */
}

#ifdef WINDOWS
bool
alloc_in_create(void *drcontext)
{
    /* XXX: should we pass in_create as a param to client_handle_malloc() to avoid
     * this extra overhead to get back our own CLS?
     */
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    return pt->in_create;
}
#endif

bool
alloc_in_heap_routine(void *drcontext)
{
    /* XXX: should we pass in_create as a param to client_handle_malloc() to avoid
     * this extra overhead to get back our own CLS?
     */
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    return pt->in_heap_routine > 0;
}

static void
malloc_wrap_init(void)
{
    malloc_interface.malloc_lock = malloc_wrap__lock;
    malloc_interface.malloc_unlock = malloc_wrap__unlock;
    malloc_interface.malloc_end = malloc_wrap__end;
    malloc_interface.malloc_add = malloc_wrap__add;
    malloc_interface.malloc_is_pre_us = malloc_wrap__is_pre_us;
    malloc_interface.malloc_is_pre_us_ex = malloc_wrap__is_pre_us_ex;
    malloc_interface.malloc_size = malloc_wrap__size;
    malloc_interface.malloc_size_invalid_only = malloc_wrap__size_invalid_only;
    malloc_interface.malloc_get_client_data = malloc_wrap__get_client_data;
    malloc_interface.malloc_get_client_flags = malloc_wrap__get_client_flags;
    malloc_interface.malloc_set_client_flag = malloc_wrap__set_client_flag;
    malloc_interface.malloc_clear_client_flag = malloc_wrap__clear_client_flag;
    malloc_interface.malloc_iterate = malloc_wrap__iterate;
    malloc_interface.malloc_intercept = malloc_wrap__intercept;
    malloc_interface.malloc_unintercept = malloc_wrap__unintercept;
    malloc_interface.malloc_set_init = malloc_wrap__set_init;
    malloc_interface.malloc_set_exit = malloc_wrap__set_exit;
}

/*
 ***************************************************************************/

#ifdef WINDOWS
static void
handle_cbret(bool syscall)
{
    void *drcontext = dr_get_current_drcontext();
    client_handle_cbret(drcontext);
}
#endif

bool
alloc_syscall_filter(void *drcontext, int sysnum)
{
    /* improve performance by not intercepting everything.  makes a difference
     * on linux in particular where ignorable syscalls are inlined.
     */
#ifdef WINDOWS
    if (sysnum == sysnum_mmap || sysnum == sysnum_munmap ||
        sysnum == sysnum_valloc || sysnum == sysnum_vfree ||
        sysnum == sysnum_cbret || sysnum == sysnum_continue ||
        sysnum == sysnum_RaiseException ||
        sysnum == sysnum_setcontext || sysnum == sysnum_mapcmf ||
        sysnum == sysnum_UserConnectToServer) {
        return true;
    } else
        return false;
#else
    switch (sysnum) {
    case SYS_mmap:
    case SYS_munmap:
    IF_X86_32(case SYS_mmap2:)
    case SYS_mremap:
    case SYS_brk:
    case SYS_clone:
        return true;
    default:
        return false;
    }
#endif
}

void
handle_pre_alloc_syscall(void *drcontext, int sysnum, dr_mcontext_t *mc, reg_t sysarg[],
                         uint arg_cap)
{
#if defined(WINDOWS) || defined(DEBUG)
    cls_alloc_t *pt = drmgr_get_cls_field(drcontext, cls_idx_alloc);
#endif
#ifdef WINDOWS
    if (sysnum == sysnum_mmap || sysnum == sysnum_munmap ||
        sysnum == sysnum_valloc || sysnum == sysnum_vfree ||
        sysnum == sysnum_cbret || sysnum == sysnum_continue ||
        sysnum == sysnum_setcontext || sysnum == sysnum_mapcmf) {
        HANDLE process;
        pt->expect_sys_to_fail = false;
        if (sysnum == sysnum_mmap || sysnum == sysnum_munmap ||
            sysnum == sysnum_valloc || sysnum == sysnum_vfree ||
            sysnum == sysnum_mapcmf) {
            process = (HANDLE)
                dr_syscall_get_param(drcontext,
                                     (sysnum == sysnum_mmap ||
                                      sysnum == sysnum_mapcmf) ? 1 : 0);
            if (sysnum == sysnum_mapcmf) {
                /* i#423, and xref DRi#415: the 2nd param is often -1
                 * (NT_CURRENT_PROCESS) but is sometimes NULL and also observed to be
                 * 0x3, so we assume it is NOT a process handle as originally believed,
                 * and that this syscall only operates on the current process.
                 */
                pt->syscall_this_process = true;
            } else
                pt->syscall_this_process = is_current_process(process);
            DOLOG(2, {
                if (!pt->syscall_this_process)
                    LOG(2, "sysnum %d on other process "PIFX"\n", sysnum, process);
            });
        }
        if (sysnum == sysnum_valloc) {
            uint type = (uint) dr_syscall_get_param(drcontext, 4);
            pt->valloc_type = type;
            pt->valloc_commit = false;
            if (alloc_ops.track_heap) {
                if (pt->syscall_this_process && TEST(MEM_COMMIT, type)) {
                    app_pc *base_ptr = (app_pc *) dr_syscall_get_param(drcontext, 1);
                    app_pc base;
                    MEMORY_BASIC_INFORMATION mbi;
                    /* We distinguish HeapAlloc from VirtualAlloc b/c the former
                     * reserves a big region and then commits pieces of it.
                     * We assume that anything w/ that behavior should be treated
                     * as a heap where its pieces are NOT addressable at commit time,
                     * but only at sub-page parcel-out time.
                     */
                    if (safe_read(base_ptr, sizeof(base), &base) &&
                        dr_virtual_query(base, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                        pt->valloc_commit = (base == NULL /* no prior reservation */ ||
                                             (TEST(MEM_RESERVE, type) &&
                                              mbi.State == MEM_FREE) ||
                                             /* We require in_heap_routine to allow
                                              * RtlAllocateHandle, which does reserve and
                                              * then commit pieces but is NOT heap
                                              */
                                             pt->in_heap_routine == 0);
                        if (is_in_heap_region(base)) {
                            /* trying to handle cygwin or other cases where we
                             * don't yet follow all their alloc routines
                             * (xref i#197, i#480)
                             */
                            pt->valloc_commit = false;
                        } else if (pt->in_heap_routine > 0 && !pt->valloc_commit) {
                            /* this is a heap reservation that we missed: perhaps
                             * from cygwin (xref i#197, i#480)
                             */
                            /* [mbi.AllocationBase, mbi.BaseAddress + mbi.RegionSize)
                             * would work for single large reservations: but best
                             * to be safe
                             */
                            byte *alloc_base;
                            size_t alloc_size = allocation_size(base, &alloc_base);
                            LOG(2, "Adding unknown heap region "PFX"-"PFX"\n",
                                alloc_base, alloc_base + alloc_size);
                            heap_region_add(alloc_base, alloc_base + alloc_size,
                                            HEAP_ARENA, mc);
                        }
                    } else {
                        WARN("WARNING: NtAllocateVirtualMemory: error reading param\n");
                    }
                }
            }
        } else if (sysnum == sysnum_vfree) {
            pt->valloc_type = (uint) dr_syscall_get_param(drcontext, 3);
            if (pt->syscall_this_process) {
                app_pc *base_ptr = (app_pc *) dr_syscall_get_param(drcontext, 1);
                size_t *size_ptr = (size_t *) dr_syscall_get_param(drcontext, 2);
                app_pc arg_base, base;
                size_t sz = 0;
                if (safe_read(size_ptr, sizeof(sz), &sz))
                    LOG(2, "NtFreeVirtualMemory pre-size="PFX"\n", sz);
                if (safe_read(base_ptr, sizeof(arg_base), &arg_base)) {
                    sz = allocation_size(arg_base, &base);
                    LOG(2, "NtFreeVirtualMemory alloc size of "PFX" is "PFX"\n",
                        arg_base, sz);
                }
                /* FIXME: check whether size exceeds malloc-recorded size */
                if (sz == 0 || base == NULL) {
                    pt->expect_sys_to_fail = true;
                    /* If our failure predictions aren't accurate enough we
                     * may have to implement the complex scheme in my notes
                     * for handling failed frees
                     */
                    client_invalid_heap_arg((app_pc)sysnum/*use sysnum as pc*/,
                                            base, mc, "HeapFree", true);
                } else {
                    pt->expect_sys_to_fail = false;
                }
            }
        } else if (sysnum == sysnum_munmap) {
            pt->munmap_base = (app_pc) dr_syscall_get_param(drcontext, 1);
            /* we have to walk now (post-syscall nothing to walk): we'll restore
             * if the syscall fails */
            if (pt->syscall_this_process) {
                /* syscall still works when not passed alloc base, but mmap_walk
                 * looks up the base so we don't need to call allocation_size() */
                LOG(2, "NtUnmapViewOfSection: "PFX"\n", pt->munmap_base);
                client_handle_munmap(pt->munmap_base,
                                     allocation_size(pt->munmap_base, NULL),
                                     false/*file-backed*/);
            }
        } else if (sysnum == sysnum_cbret) {
            handle_cbret(true/*syscall*/);
        } else if (sysnum == sysnum_continue) {
            client_handle_continue(drcontext, mc);
            if (pt->in_seh)
                pt->in_seh = false;
            /* else, an APC */
        }
    }
#else /* WINDOWS */
    if (sysnum == SYS_munmap) {
        app_pc base = (app_pc) dr_syscall_get_param(drcontext, 0);
        size_t size = (size_t) dr_syscall_get_param(drcontext, 1);
        /* If it fails, we restore post-syscall.
         * FIXME: need to store shadow values here so can restore.
         * Also need to handle races: xref race handling for malloc.
         */
        LOG(2, "SYS_munmap "PFX"-"PFX"\n", base, base+size);
        client_handle_munmap(base, size, false/*up to caller to determine*/);
        /* if part of heap remove it from list */
        if (alloc_ops.track_heap)
            heap_region_remove(base, base+size, mc);
    }
# ifdef DEBUG
    else if (sysnum == SYS_brk) {
        pt->sbrk = (app_pc) dr_syscall_get_param(drcontext, 0);
    }
# endif
#endif /* WINDOWS */
    client_pre_syscall(drcontext, sysnum, sysarg);
}

#ifdef WINDOWS
bool
is_in_seh(void *drcontext)
{
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    return pt->in_seh;
}

/* Here for sharing among users of the callstack module.
 * i#701: on unwind, xbp is set to original fault value and can
 * result in an incorrect callstack.  May help on earlier steps in SEH
 * as well so not trying to detect unwind: instead we just track between
 * KiUserExceptionDispatcher and NtContinue and ignore xbp throughout.
 */
bool
is_in_seh_unwind(void *drcontext, dr_mcontext_t *mc)
{
    return is_in_seh(drcontext);
}


static void
handle_post_valloc(void *drcontext, dr_mcontext_t *mc, cls_alloc_t *pt, reg_t sysarg[])
{
    bool success = NT_SUCCESS(dr_syscall_get_result(drcontext));
    if (success && pt->syscall_this_process) {
        app_pc *base_ptr = (app_pc *) sysarg[1];
        size_t *size_ptr = (size_t *) sysarg[3];
        app_pc base;
        size_t size;
        if (!safe_read(base_ptr, sizeof(*base_ptr), &base) ||
            !safe_read(size_ptr, sizeof(*size_ptr), &size)) {
            LOG(1, "WARNING: NtAllocateVirtualMemory: error reading param\n");
            return;
        }
        LOG(2, "NtAllocateVirtualMemory: "PFX"-"PFX" %s%s%s%s\n",
            base, base+size, pt->valloc_commit ? "vcommit " : "",
            TEST(MEM_RESERVE, pt->valloc_type) ? "reserve " : "",
            TEST(MEM_COMMIT, pt->valloc_type) ? "commit " : "",
            pt->in_heap_routine > 0 ? "in-heap " : "");
        if (alloc_ops.track_heap) {
            /* if !valloc_commit, we assume it's part of a heap */
            if (pt->valloc_commit) {
                /* FIXME: really want to test overlap of two regions! */
                ASSERT(!is_in_heap_region(base),
                       "HeapAlloc vs VirtualAlloc: error distinguishing");
                if (pt->in_heap_routine == 0) {
                    LOG(2, "NtAllocateVirtualMemory non-heap alloc "PFX"-"PFX"\n",
                        base, base+size);
                    client_handle_mmap(drcontext, base, size, true/*anon*/);
                } else {
                    byte *heap_base, *heap_end;
                    if (heap_region_bounds(base - 1, &heap_base, &heap_end, NULL) &&
                        /* do not extend adjacent if this is really for a different
                         * Heap (i#520)
                         */
                        (pt->heap_handle == 0 ||
                         (HANDLE) pt->heap_handle == heap_region_get_heap(base -1))) {
                        /* Some allocators (tcmalloc, e.g.) extend
                         * their heap even w/o an up-front reservation
                         */
                        ASSERT(heap_end == base, "query error");
                        heap_region_adjust(heap_base, base+size);
                    } else {
                        /* We assume this is a very large malloc, which is allocated
                         * straight from the OS instead of the heap pool.
                         * FIXME: our red zone here will end up wasting an entire 64KB
                         * if the request size + headers would have been 64KB-aligned.
                         */
                        LOG(2, "NtAllocateVirtualMemory big heap alloc "PFX"-"PFX"\n",
                            base, base+size);
                        /* there are headers on this one */
                        heap_region_add(base, base+size, 0, mc);
                        /* set Heap if from RtlAllocateHeap */
                        if (pt->in_heap_adjusted > 0 && pt->heap_handle != 0)
                            heap_region_set_heap(base, (HANDLE) pt->heap_handle);
                    }
                }
            } else if (TEST(MEM_RESERVE, pt->valloc_type) &&
                       !TEST(MEM_COMMIT, pt->valloc_type) &&
                       pt->in_heap_routine > 0) {
                /* we assume this is a new Heap reservation */
                heap_region_add(base, base+size, HEAP_ARENA, mc);
                /* set Heap if from RtlAllocateHeap */
                if (pt->in_heap_adjusted > 0 && pt->heap_handle != 0)
                    heap_region_set_heap(base, (HANDLE) pt->heap_handle);
            }
        } else {
            if (TEST(MEM_COMMIT, pt->valloc_type)) {
                LOG(2, "NtAllocateVirtualMemory commit "PFX"-"PFX"\n",
                    base, base+size);
                client_handle_mmap(drcontext, base, size, true/*anon*/);
            }
        }
    } else {
        DOLOG(2, {
            app_pc *base_ptr = (app_pc *) sysarg[1];
            size_t *size_ptr = (size_t *) sysarg[3];
            app_pc base;
            size_t size;
            if (safe_read(base_ptr, sizeof(*base_ptr), &base) &&
                safe_read(size_ptr, sizeof(*size_ptr), &size)) {
                LOG(2, "NtAllocateVirtualMemory res="PFX" %s: "PFX"-"PFX"\n",
                    dr_syscall_get_result(drcontext),
                    pt->syscall_this_process ? "" : "other-process", base, base+size);
            } else {
                LOG(1, "WARNING: NtAllocateVirtualMemory: error reading param\n");
                return;
            }
        });
    }
}

static void
handle_post_vfree(void *drcontext, dr_mcontext_t *mc, cls_alloc_t *pt, reg_t sysarg[])
{
    bool success = NT_SUCCESS(dr_syscall_get_result(drcontext));
    app_pc *base_ptr = (app_pc *) sysarg[1];
    size_t *size_ptr = (size_t *) sysarg[2];
    app_pc base;
    size_t size;
    if (!pt->syscall_this_process)
        return;
    if (success &&
        safe_read(base_ptr, sizeof(*base_ptr), &base) &&
        safe_read(size_ptr, sizeof(*size_ptr), &size)) {
        LOG(2, "NtFreeVirtualMemory: "PFX"-"PFX", %s%s%s\n", base, base+size,
            TEST(MEM_DECOMMIT, pt->valloc_type) ? "decommit " : "",
            TEST(MEM_RELEASE, pt->valloc_type) ? "release " : "",
            pt->in_heap_routine > 0 ? "in-heap " : "");
        ASSERT(!pt->expect_sys_to_fail, "expected NtFreeVirtualMemory to succeed");
        if (alloc_ops.track_heap) {
            /* Are we freeing an entire region? */
            if (((pt->valloc_type == MEM_DECOMMIT && size == 0) ||
                 pt->valloc_type == MEM_RELEASE) &&
                pt->in_heap_routine > 0 && is_in_heap_region(base)) {
                /* all these separate lookups are racy */
                app_pc heap_end = NULL;
                bool found;
                heap_region_bounds(base, NULL, &heap_end, NULL);
                if (size == 0)
                    size = allocation_size(base, NULL);
                found = heap_region_remove(base, base+size, mc);
                ASSERT(found, "heap region tracking bug");
                /* FIXME: this is racy, doing this post-syscall; should
                 * switch to interval tree to look up base pre-syscall
                 */
                /* PR 469229: in some cases a large heap reservation
                 * is made and then all but the last page is freed; that
                 * page is then used as the heap.  Makes no sense.
                 */
                if (heap_end > base+size) {
                    LOG(2, "left "PFX"-"PFX" at end of heap region\n",
                        base+size, heap_end);
                    /* heap_region_remove leaves the remaining piece there */
                }
                client_handle_munmap(base, size, true/*anon*/);
            } else {
                /* we ignore decommits from inside heap regions.
                 * we shouldn't see any releases from inside heap regions
                 * that bypass our check above, though.
                 */
                ASSERT(pt->valloc_type == MEM_DECOMMIT ||
                       !is_in_heap_region(base), "heap region tracking bug");
                if (alloc_ops.record_allocs) {
                    malloc_remove(base);
                }
            }
        } else {
            client_handle_munmap(base, size, true/*anon*/);
        }
    } else {
        DOLOG(1, {
            if (success) {
                LOG(1, "WARNING: NtFreeVirtualMemory: error reading param\n");
            } else {
                /* not serious: we didn't do anything pre-syscall */
                if (!pt->expect_sys_to_fail)
                    LOG(1, "WARNING: NtFreeVirtualMemory failed unexpectedly");
            }
        });
    }
}

static void
handle_post_UserConnectToServer(void *drcontext, dr_mcontext_t *mc, cls_alloc_t *pt,
                                reg_t sysarg[])
{
    if (NT_SUCCESS(dr_syscall_get_result(drcontext))) {
        /* A data file is mmapped by csrss and its base is stored at offset 0x10 */
# define UserConnectToServer_BASE_OFFS 0x10
        app_pc base;
        if (safe_read((byte *)sysarg[1] + UserConnectToServer_BASE_OFFS,
                      sizeof(base), &base)) {
            app_pc check_base;
            size_t sz = allocation_size(base, &check_base);
            if (base == check_base)
                client_handle_mmap(drcontext, base, sz, false/*file-backed*/);
            else
                WARN("WARNING: UserConnectToServer has invalid base");
        }
    }
}
#endif /* WINDOWS */

void
handle_post_alloc_syscall(void *drcontext, int sysnum, dr_mcontext_t *mc, reg_t sysarg[],
                          uint arg_cap)
{
    cls_alloc_t *pt = drmgr_get_cls_field(drcontext, cls_idx_alloc);
#ifdef WINDOWS
    /* we access up to param#4 */
    ASSERT(arg_cap >= 6, "need to up #sysargs stored");
    if (sysnum == sysnum_mmap) {
        /* FIXME: provide a memory tracking interface? */
        bool success = NT_SUCCESS(dr_syscall_get_result(drcontext));
        if (success && pt->syscall_this_process) {
            app_pc *base_ptr = (app_pc *) sysarg[2];
            app_pc base;
            if (!safe_read(base_ptr, sizeof(*base_ptr), &base)) {
                LOG(1, "WARNING: NtMapViewOfSection: error reading param\n");
            } else {
                LOG(2, "NtMapViewOfSection: "PFX"\n", base);
                client_handle_mmap(drcontext, base, allocation_size(base, NULL),
                                   false/*file-backed*/);
            }
        }
    } else if (sysnum == sysnum_munmap) {
        bool success = NT_SUCCESS(dr_syscall_get_result(drcontext));
        if (!success && pt->syscall_this_process) {
            /* restore */
            LOG(2, "NtUnmapViewOfSection failed: restoring "PFX"\n", pt->munmap_base);
            client_handle_munmap_fail(pt->munmap_base,
                                      allocation_size(pt->munmap_base, NULL),
                                      false/*file-backed*/);
        }
    } else if (sysnum == sysnum_valloc) {
        handle_post_valloc(drcontext, mc, pt, sysarg);
    } else if (sysnum == sysnum_vfree) {
        if (pt->syscall_this_process)
            handle_post_vfree(drcontext, mc, pt, sysarg);
    } else if (sysnum == sysnum_mapcmf) {
        bool success = NT_SUCCESS(dr_syscall_get_result(drcontext));
        if (success && pt->syscall_this_process) {
            app_pc *base_ptr = (app_pc *) sysarg[5];
            app_pc base;
            if (!safe_read(base_ptr, sizeof(*base_ptr), &base)) {
                LOG(1, "WARNING: NtMapCMFModule: error reading param\n");
            } else {
                LOG(2, "NtMapCMFModule: "PFX"-"PFX"\n",
                    base, base + allocation_size(base, NULL));
                client_handle_mmap(drcontext, base, allocation_size(base, NULL),
                                   /* I believe it's file-backed: xref DRi#415 */
                                   false/*file-backed*/);
            }
        }
    } else if (sysnum == sysnum_UserConnectToServer) {
        handle_post_UserConnectToServer(drcontext, mc, pt, sysarg);
    }
#else /* WINDOWS */
    ptr_int_t result = dr_syscall_get_result(drcontext);
    bool success = (result >= 0);
    ASSERT(arg_cap >= 4, "need to up #sysargs stored");
    if (sysnum == SYS_mmap IF_X86_32(|| sysnum == SYS_mmap2)) {
        unsigned long flags = 0;
        size_t size = 0;
        /* libc interprests up to -PAGE_SIZE as an error */
        bool mmap_success = (result > 0 || result < -PAGE_SIZE);
        if (mmap_success) {
            app_pc base = (app_pc) result;
            if (sysnum == IF_X64_ELSE(SYS_mmap, SYS_mmap2)) {
                /* long sys_mmap2(unsigned long addr, unsigned long len,
                 *                unsigned long prot, unsigned long flags,
                 *                unsigned long fd, unsigned long pgoff)
                 */
                flags = (unsigned long) sysarg[3];
                size = (size_t) sysarg[1];
            }
# ifdef X86_32
            if (sysnum == SYS_mmap) {
                mmap_arg_struct_t arg;
                if (!safe_read((void *)sysarg[0], sizeof(arg), &arg)) {
                    ASSERT(false, "failed to read successful mmap arg struct");
                    /* fallback is to walk as though an image */
                    memset(&arg, 0, sizeof(arg));
                }
                flags = arg.flags;
                size = arg.len;
            }
# endif
            LOG(2, "SYS_mmap: "PFX"-"PFX" %d\n", base, base+size, flags);
            client_handle_mmap(drcontext, base, size, TEST(MAP_ANONYMOUS, flags));
            if (TEST(MAP_ANONYMOUS, flags)) {
                if (pt->in_heap_routine > 0) {
                    /* We don't know whether a new arena or a one-off large
                     * malloc: doesn't matter too much since we don't
                     * really distinguish inside our heap list anyway.
                     */
                    if (alloc_ops.track_heap)
                        heap_region_add(base, base+size, HEAP_ARENA/*FIXME:guessing*/, mc);
                }
            }
        } else {
            LOG(2, "SYS_mmap failed "PIFX"\n", result);
        }
        /* FIXME: races: could be unmapped already */
    }
    else if (sysnum == SYS_munmap) {
        if (!success) {
            /* we already marked unaddressable: restore */
            app_pc base = (app_pc) sysarg[0];
            size_t size = (size_t) sysarg[1];
            dr_mem_info_t info;
            LOG(2, "SYS_munmap "PFX"-"PFX" failed\n", base, base+size);
            if (!dr_query_memory_ex(base, &info))
                ASSERT(false, "mem query failed");
            client_handle_munmap_fail(base, size, info.type != DR_MEMTYPE_IMAGE);
            if (alloc_ops.track_heap && pt->in_heap_routine > 0)
                heap_region_add(base, base+size, HEAP_ARENA/*FIXME:guessing*/, mc);
        }
    } 
    else if (sysnum == SYS_mremap) {
        app_pc old_base = (app_pc) sysarg[0];
        size_t old_size = (size_t) sysarg[1];
        app_pc new_base = (app_pc) result;
        size_t new_size = (size_t) sysarg[2];
        /* libc interprets up to -PAGE_SIZE as an error */
        bool mmap_success = (result > 0 || result < -PAGE_SIZE);
        if (mmap_success) {
            /* FIXME: we're waiting to invalidate, as opposed to munmap where we
             * invalidate in pre-syscall: one reason is that mremap is much more
             * likely to fail.  However, waiting means races where a new alloc
             * comes in are much more likely.  Need to put in place a master
             * race handler.  For now we just want common cases working.
             */
            dr_mem_info_t info;
            LOG(2, "SYS_mremap from "PFX"-"PFX" to "PFX"-"PFX"\n",
                old_base, old_base+old_size, new_base, new_base+new_size);
            if (!dr_query_memory_ex(new_base, &info))
                ASSERT(false, "mem query failed");
            client_handle_mremap(old_base, old_size, new_base, new_size,
                                 info.type == DR_MEMTYPE_IMAGE);
            /* Large realloc may call mremap (PR 488643) */
            if (alloc_ops.track_heap && pt->in_heap_routine > 0 &&
                is_in_heap_region(old_base)) {
                ASSERT(is_entirely_in_heap_region(old_base, old_base + old_size),
                       "error in large malloc tracking");
                heap_region_remove(old_base, old_base + old_size, mc);
                heap_region_add(new_base, new_base + new_size,
                                HEAP_ARENA/*FIXME:guessing*/, mc);
            }
        }
    }
    else if (sysnum == SYS_brk) {
        /* We can mostly ignore SYS_brk since we treat heap as unaddressable
         * until sub-allocated, though we do want the bounds for suppressing
         * header accesses by malloc code.
         */
        byte *heap_start = get_heap_start();
        LOG(2, "SYS_brk "PFX" => "PFX"\n", pt->sbrk, result);
        if (!is_in_heap_region(heap_start) && (byte *)result > heap_start) {
            /* no heap prior to this point */
            heap_region_add(heap_start, (byte *) result, HEAP_ARENA, 0);
        } else
            heap_region_adjust(heap_start, (byte *) result);
    }
#endif /* WINDOWS */
    client_post_syscall(drcontext, sysnum, sysarg);
}

static inline app_pc
get_retaddr_at_entry(dr_mcontext_t *mc)
{
    app_pc retaddr = NULL;
    if (alloc_ops.conservative) {
        if (!safe_read((void *)mc->xsp, sizeof(retaddr), &retaddr))
            ASSERT(false, "error reading retaddr at func entry");
    } else
        retaddr = *(app_pc*)(mc->xsp);
    return retaddr;
}

static inline void
record_mc_for_client(cls_alloc_t *pt, void *wrapcxt)
{
    /* Present the outer layer as the top of the allocation call stack,
     * regardless of how many inner layers we went through (i#913).
     * XXX: before we rarely needed mc on pre-hooks: now that we need it
     * on many perhaps should move to main hook and pass through?
     * XXX: this can't go in set_handling_heap_layer() b/c we currently
     * pass operators through and don't handle until malloc/free!
     */
    dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR);
    pt->outer_xsp = mc->xsp;
    pt->outer_xbp = mc->xbp;
    pt->outer_retaddr = drwrap_get_retaddr(wrapcxt);
    LOG(3, "\t@ level=%d recorded xsp="PFX" xbp="PFX" ra="PFX"\n",
        pt->in_heap_routine, pt->outer_xsp, pt->outer_xbp, pt->outer_retaddr);
}

/* Returns the top frame pc to pass to the client and temporarily sets
 * the mc fields so that callstacks appear to end at the outer heap
 * layer (i#913).
 * Call restore_mc_for_client() afterward to restore the mc.
 */
static inline app_pc
set_mc_for_client(cls_alloc_t *pt, void *wrapcxt, dr_mcontext_t *mc, app_pc post_call)
{
    if (pt->allocator != 0) {
        pt->xsp_tmp = mc->xsp;
        pt->xbp_tmp = mc->xbp;
        mc->xsp = pt->outer_xsp;
        mc->xbp = pt->outer_xbp;
        /* XXX i#639: we'd like to have the outer heap routine itself
         * on the callstack.  However, doing so here can result in missing the
         * caller frame (i#913).  What we want is to be able to pass multiple
         * pre-set pcs to the callstack walker, not just the top.
         */
        return pt->outer_retaddr;
    } else if (post_call != NULL)
        return post_call;
    else
        return drwrap_get_retaddr(wrapcxt);
}

/* Call after calling set_mc_for_client() and then invoking a client routine */
static inline void
restore_mc_for_client(cls_alloc_t *pt, void *wrapcxt, dr_mcontext_t *mc)
{
    if (pt->allocator != 0) {
        mc->xsp = pt->xsp_tmp;
        mc->xbp = pt->xbp_tmp;
    }
}

/* RtlAllocateHeap(HANDLE heap, ULONG flags, ULONG size) */
/* void *malloc(size_t size) */
#define ARGNUM_MALLOC_SIZE(type) (IF_WINDOWS((type == RTL_ROUTINE_MALLOC) ? 2 :) 0)
/* RtlReAllocateHeap(HANDLE heap, ULONG flags, PVOID ptr, SIZE_T size) */
/* void *realloc(void *ptr, size_t size) */
#define ARGNUM_REALLOC_PTR(type) (IF_WINDOWS((type == RTL_ROUTINE_REALLOC) ? 2 :) 0)
#define ARGNUM_REALLOC_SIZE(type) (IF_WINDOWS((type == RTL_ROUTINE_REALLOC) ? 3 :) 1)
/* RtlFreeHeap(HANDLE heap, ULONG flags, PVOID ptr) */
/* void free(void *ptr) */
#define ARGNUM_FREE_PTR(type) (IF_WINDOWS((type == RTL_ROUTINE_FREE) ? 2 :) 0)
/* ULONG NTAPI RtlSizeHeap(HANDLE Heap, ULONG Flags, PVOID Block) */
/* void malloc_usable_size(void *ptr) */
#define ARGNUM_SIZE_PTR(type) (IF_WINDOWS((type == RTL_ROUTINE_SIZE) ? 2 :) 0)

/* As part of PR 578892 we must report invalid heap block args to all routines,
 * since we ignore unaddr inside the routines.
 * Caller should check for NULL separately if it's not an invalid arg.
 * Pass invalid in if known; else block will be looked up in malloc table.
 */
static bool
check_valid_heap_block(bool known_invalid, byte *block, cls_alloc_t *pt, void *wrapcxt,
                       const char *routine, bool is_free)
{
    if ((known_invalid || malloc_end(block) == NULL) &&
        /* do not report errors when on a heap tangent: there can be LFH blocks
         * or other meta-objects for which we never saw the alloc (i#432)
         */
        IF_WINDOWS_ELSE(!pt->heap_tangent, true)) {
        /* call_site for call;jmp will be jmp, so retaddr better even if post-call */
        client_invalid_heap_arg(drwrap_get_retaddr(wrapcxt),
                                /* client_data not needed so not bothering */
                                block, drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR),
                                translate_routine_name(routine), is_free);
        return false;
    }
    return true;
}

static void
enter_heap_routine(cls_alloc_t *pt, app_pc pc, alloc_routine_entry_t *routine)
{
    if (pt->in_heap_routine == 0) {
        /* if tangent, check_recursive_same_sequence() will call this */
        client_entering_heap_routine();
    }
    pt->in_heap_routine++;
    /* Exceed array depth => just don't record: only needed on jmp-to-post-call-bb
     * and on DGC.
     */
    if (pt->in_heap_routine < MAX_HEAP_NESTING) {
        pt->last_alloc_routine[pt->in_heap_routine] = pc;
        if (!alloc_ops.conservative)
            pt->last_alloc_info[pt->in_heap_routine] = routine;
    } else {
        LOG(0, "WARNING: %s exceeded heap nesting %d >= %d\n",
            get_alloc_routine_name(pc), pt->in_heap_routine, MAX_HEAP_NESTING);
    }
}

/* Returns true if this call is a recursive helper to aid in the same
 * sequences of calls.  If it's a new tangential sequence, pushes a new
 * data structure on the per-thread stack and returns false.
 * In the latter case, caller should re-set cls_alloc_t.
 *
 * The args_match checks use either pt->alloc_base or pt->alloc_size.
 * Thus, anyone setting in_heap_adjusted should set both alloc_{base,size}
 * via set_handling_heap_layer() so we don't compare to garbage.
 */
static bool
check_recursive_same_sequence(void *drcontext, cls_alloc_t **pt_caller,
                              alloc_routine_entry_t *routine,
                              ptr_int_t arg1, ptr_int_t arg2)
{
    cls_alloc_t *pt = *pt_caller;
    /* We assume that a typical nested call to an alloc routine (malloc, realloc,
     * calloc) is working on the same allocation and not a separate one.
     * We do our adjustments in the outer pre and the outer post.
     */
    if (pt->in_heap_routine > 1 && pt->in_heap_adjusted > 0) {
#ifdef WINDOWS
        /* However, there are some cases where a heap routine will go
         * off on a tangent.  E.g., "heap maintenance".  For that
         * we need to push our context on the data struct stack
         * and go process the tangent.  Xref i#301.
         */
        /* Arg mismatch is ok across layers: e.g., crtdbg asking for extra
         * space.  Since we've only seen tangents in Rtl, instead of a general
         * is-this-same-layer, we just check whether this is Rtl->Rtl transition.
         * If we do end up with tangents across layers we'll have to be
         * more careful w/ the args and store which layer we used in
         * delay free queue, etc. so the redzone adjustment matches.
         */
        if (is_rtl_routine(routine->type)) {
            bool tangent = false;
            alloc_routine_entry_t *last_routine = NULL;
            alloc_routine_entry_t last_routine_local;
            ASSERT(pt->in_heap_routine > 0, "invalid heap counter");
            if (pt->in_heap_routine < MAX_HEAP_NESTING) {
                tangent = (pt->last_alloc_routine[pt->in_heap_routine-1] != NULL);
                /* need to get last_routine */
                if (alloc_ops.conservative) {
                    /* i#708: get a copy from table while holding lock, rather than
                     * using pointer into struct that can be deleted if module
                     * is racily unloaded
                     */
                    app_pc pc = pt->last_alloc_routine[pt->in_heap_routine-1];
                    if (pc != NULL && get_alloc_entry(pc, &last_routine_local))
                        last_routine = &last_routine_local;
                    else {
                        LOG(1, "WARNING: may misclassify recursion: bad last routine\n");
                    }
                } else {
                    last_routine = (alloc_routine_entry_t *)
                        pt->last_alloc_info[pt->in_heap_routine-1];
                }
                if (!alloc_ops.replace_realloc) {
                    if (last_routine != NULL &&
                        last_routine->type == RTL_ROUTINE_REALLOC &&
                        (routine->type == RTL_ROUTINE_MALLOC ||
                         routine->type == RTL_ROUTINE_FREE)) {
                        /* no new context for just realloc calling malloc+free (i#441) */
                        tangent = false;
                    }
                }
            } else
                LOG(1, "WARNING: may misclassify recursion: exceeding nest max\n");
            LOG(2, "check_recursive %s: "PFX" vs "PFX"\n", routine->name, arg1, arg2);
            if (tangent && last_routine != NULL &&
                is_rtl_routine(last_routine->type) && arg1 != arg2) {
                cls_alloc_t *new_pt;
                IF_DEBUG(bool ok;)
                LOG(2, "%s recursive call: tangential => new context\n", routine->name);
                pt->in_heap_routine--; /* undo the inc */
                IF_DEBUG(ok =)
                    drmgr_push_cls(drcontext);
                ASSERT(ok, "drmgr cls stack push failed: tangent tracking error!");
                new_pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
                new_pt->heap_tangent = true;
                enter_heap_routine(new_pt, routine->pc, routine);
                ASSERT(new_pt->in_heap_routine == 1, "inheap not cleared in new cxt");
                *pt_caller = new_pt;
                return false;
            }
        }
#endif
        LOG(2, "%s recursive call: helper, so no adjustments\n", routine->name);
        return true;
    }
    /* non-recursive */
    return false;
}

static void
set_handling_heap_layer(cls_alloc_t *pt, byte *alloc_base, size_t alloc_size)
{
    pt->in_heap_adjusted = pt->in_heap_routine;
    /* We want to set both of these so our args_match checks for
     * check_recursive_same_sequence() are always accurate even when
     * one routine type calls another unrelated type.
     */
    pt->alloc_base = alloc_base;
    pt->alloc_size = alloc_size;
}

#ifdef WINDOWS
static void
set_auxarg(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
           alloc_routine_entry_t *routine)
{
    routine_type_t type = routine->type;
    if (is_free_routine(type)) {
        if (type == RTL_ROUTINE_FREE) {
            /* Note that these do not reflect what's really being freed if
             * -delay_frees > 0 
             */
            pt->alloc_flags = (uint) drwrap_get_arg(wrapcxt, 1);
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 0);
        } else if (type == HEAP_ROUTINE_FREE_DBG) {
            pt->alloc_flags = 0;
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 1);
        } else {
            pt->alloc_flags = 0;
            pt->auxarg = 0;
        }
    } else if (is_size_routine(type)) {
        if (type == RTL_ROUTINE_SIZE) {
            pt->alloc_flags = (uint) drwrap_get_arg(wrapcxt, 1);
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 0);
        } else if (type == HEAP_ROUTINE_SIZE_REQUESTED_DBG) {
            pt->alloc_flags = 0;
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 1);
        } else {
            pt->alloc_flags = 0;
            pt->auxarg = 0;
        }
    } else if (is_malloc_routine(type) ||
               is_realloc_routine(type) ||
               is_calloc_routine(type)) {
        if (is_rtl_routine(type)) {
            pt->alloc_flags = (uint) drwrap_get_arg(wrapcxt, 1);
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 0);
        } else if (type == HEAP_ROUTINE_MALLOC_DBG) {
            pt->alloc_flags = 0;
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 1);
        } else if (type == HEAP_ROUTINE_REALLOC_DBG || type == HEAP_ROUTINE_CALLOC_DBG) {
            pt->alloc_flags = 0;
            pt->auxarg = (ptr_int_t) drwrap_get_arg(wrapcxt, 2);
        } else {
            pt->alloc_flags = 0;
            pt->auxarg = 0;
        }
    }
}
#endif

/**************************************************
 * FREE
 */

/* Records allocator as well as the outer layer for reporting any error (i#913) */
static void
record_allocator(void *drcontext, cls_alloc_t *pt, alloc_routine_entry_t *routine,
                 void *wrapcxt)
{
    /* just record outer layer: leave adjusting to malloc (i#123).
     * XXX: we assume none of the "fake" outer layers like LdrShutdownProcess
     * call operators in a way we care about
     */
    /* new[] will call new w/ no change in in_heap_routine (i#674) so
     * we have to only record when 0.  this means we assume we always
     * reach malloc-post where allocator field will get cleared.
     */
    if (pt->in_heap_routine == 0/*always for new, and called pre-enter for malloc*/ &&
        pt->allocator == 0) {
        if (routine->set->check_mismatch)
            pt->allocator = malloc_allocator_type(routine);
        else {
            LOG(3, "unable to detect mismatches so not recording alloc type\n");
            pt->allocator = MALLOC_ALLOCATOR_UNKNOWN;
        }
#ifdef WINDOWS
        pt->ignore_next_mismatch = false; /* just in case */
#endif
        LOG(3, "alloc type: %x\n", pt->allocator);

        record_mc_for_client(pt, wrapcxt);
    }
}

/* i#123: report mismatch in free/delete/delete[]
 * Caller must hold malloc lock
 * Also records the outer layer for reporting any error (i#913)
 */
static bool
handle_free_check_mismatch(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                           alloc_routine_entry_t *routine, malloc_entry_t *entry)
{
    /* XXX: safe_read */
#ifdef WINDOWS
    routine_type_t type = routine->type;
#endif
    app_pc base = (app_pc) drwrap_get_arg(wrapcxt, ARGNUM_FREE_PTR(type));
    /* We pass in entry to avoid an extra hashtable lookup */
    uint alloc_type = (entry == NULL) ? malloc_alloc_type(base) :
        malloc_alloc_entry_type(entry);
    uint free_type = malloc_allocator_type(routine);
    LOG(3, "alloc/free match test: alloc %x vs free %x %s\n",
        alloc_type, free_type, routine->name);

    /* A convenient place to record outermost layer (even if not handled: we want
     * operator delete) on free
     */
    record_mc_for_client(pt, wrapcxt);

#if defined(WINDOWS) && defined(X64)
    /* no mismatch check for RtlFreeStringRoutine */
    if (type == RTL_ROUTINE_FREE_STRING)
        return true;
#endif
    if (entry == NULL && alloc_type == MALLOC_ALLOCATOR_UNKNOWN) {
        /* try 4 bytes back, in case this is an array w/ size passed to delete */
        alloc_type = malloc_alloc_type(base - sizeof(int));
        if (alloc_type != MALLOC_ALLOCATOR_UNKNOWN)
            base -= sizeof(int);
        else {
            /* try 4 bytes in, in case this is a non-array passed to delete[] */
            alloc_type = malloc_alloc_type(base + sizeof(int));
            if (alloc_type != MALLOC_ALLOCATOR_UNKNOWN)
                base += sizeof(int);
        }
    }
    /* If have no info, can't say whether a mismatch */
    if (alloc_type != MALLOC_ALLOCATOR_UNKNOWN &&
        alloc_type != free_type) {
#ifdef WINDOWS
        /* Modules using msvcr*.dll still have their own operator wrappers that
         * make tailcalls to msvcr*.dll; yet this is done asymmetrically such
         * that if there are no symbols for the module, all drmem sees is the
         * call to msvcr*.dll, and the asymmetry causes a mismatch.  Given that
         * VS{2005,2008,2010} under /MD and /MDd all have private wrappers, the
         * solution is to simply not report mismatches when the outer layer is
         * in msvcr*.dll and we're dealing with [] vs non-[].
         */
        if (routine->set->modbase == get_libc_base(NULL) &&
            alloc_type != MALLOC_ALLOCATOR_MALLOC &&
            free_type != MALLOC_ALLOCATOR_MALLOC) {
            LOG(2, "ignoring operator mismatch b/c msvcr* is outer layer\n");
            return true;
        }
#endif
        /* i#643: operator collapse makes distinguishing impossible */
        if (!routine->set->check_mismatch) {
            LOG(2, "ignoring operator mismatch b/c delete==delete[]\n");
            return true;
        }
        client_mismatched_heap(drwrap_get_retaddr(wrapcxt),
                               base, drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR),
                               malloc_alloc_type_name(alloc_type),
                               translate_routine_name(routine->name),
                               malloc_get_client_data(base));
        return false;
    }
    return true;
}

static void
handle_free_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                alloc_routine_entry_t *routine)
{
#if defined(WINDOWS) || defined(DEBUG)
    routine_type_t type = routine->type;
#endif
#if defined(WINDOWS) && defined(X64)
    IF_DEBUG(bool valid = true;)
#endif
    void *arg = drwrap_get_arg(wrapcxt, ARGNUM_FREE_PTR(type));
    app_pc base, real_base;
#ifdef WINDOWS
    HANDLE heap = (type == RTL_ROUTINE_FREE) ? ((HANDLE) drwrap_get_arg(wrapcxt, 0)) : NULL;
#endif
    bool size_in_zone = (redzone_size(routine) > 0 && alloc_ops.size_in_redzone);
    size_t size = 0;
    malloc_entry_t *entry;

    base = (app_pc)arg;
    real_base = base;
    pt->alloc_being_freed = base;

    if (check_recursive_same_sequence(drcontext, &pt, routine, (ptr_int_t) base,
                                      (ptr_int_t) pt->alloc_base -
                                      redzone_size(routine))) {
        /* we assume we're called from RtlReAllocateheap, who will handle
         * all adjustments and shadow updates */
        LOG(2, "free of "PFX" recursive: not adjusting\n", base);
        /* try to catch errors like PR 406714 */
        ASSERT(/* don't crash calling size routine so first see whether
                * entry exists: but don't use lock, since we can deadlock
                * due to our use of app lock to get size combined with
                * using malloc lock on both outer and inner malloc layers
                */
               base == NULL ||
               !malloc_entry_exists_racy_nolock(base) ||
               get_alloc_size(IF_WINDOWS_((reg_t)heap) base, routine) != -1,
               "free recursion count incorrect");
        return;
    }
    /* We avoid worrying about races between our pre & post free instru
     * by assuming we can always predict when free will fail.  This
     * requires always tracking mallocs even when not counting leaks.
     * We can get off after an invalid free due to a corner case race, but
     * we require user to fix invalid frees before trusting all later errors.
     */
    /* We must have synchronized access to avoid races and ensure we report
     * an error on the 2nd free to the same base
     */
    malloc_lock();
    entry = malloc_lookup(base);
    if (entry != NULL &&
        (malloc_entry_is_native_ex(entry, base, pt, false)
#ifdef WINDOWS
         /* i#607 part A: if we missed the libc-layer alloc, we have to ignore the
          * libc-layer free b/c it's too late to add our redzone.
          * XXX: for now we do not check in all other query routines like size.
          */
         || (malloc_entry_is_libc_internal(entry) && !is_rtl_routine(routine->type))
#endif
         )) {
        malloc_entry_remove(entry);
        malloc_unlock();
        return;
    }
    if (pt->in_heap_routine == 1/*alread incremented, so outer*/) {
        /* N.B.: should be called even if not reporting mismatches as it also
         * records the outer layer (i#913)
         */
#ifdef WINDOWS
        if (pt->ignore_next_mismatch)
            pt->ignore_next_mismatch = false;
        else
#endif
            handle_free_check_mismatch(drcontext, pt, wrapcxt, routine, entry);
    }
#ifdef WINDOWS
    else if (pt->ignore_next_mismatch)
        pt->ignore_next_mismatch = false;
#endif

    if (entry != NULL && redzone_size(routine) > 0 &&
        !malloc_entry_is_pre_us(entry, false))
        real_base = base - redzone_size(routine);
    if (entry == NULL
        /* call will fail if heap handle does not match.
         * it will not fail if flags are invalid.
         * instead of tracking the heap handle we could call RtlValidateHeap here?
         */
        IF_WINDOWS(|| (type == RTL_ROUTINE_FREE && heap_region_get_heap(base) != heap))) {
        if (pt->in_realloc) {
            /* when realloc calls free we've already invalidated the heap */
            ASSERT(pt->in_heap_routine > 1, "realloc calling free inconsistent");
        } else if (!check_valid_heap_block(true/*invalid*/, base, pt, wrapcxt,
                                           translate_routine_name(routine->name),
                                           true/*is free()*/)) {
            pt->expect_lib_to_fail = true;
        } /* else, probably LFH free which we should ignore */
    } else {
        app_pc change_base;
        size_t real_size;
#ifdef WINDOWS
        ptr_int_t auxarg;
        int auxargnum;
#endif
        app_pc top_pc;
        dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR);

        pt->expect_lib_to_fail = false;
        if (redzone_size(routine) > 0) {
            ASSERT(redzone_size(routine) >= sizeof(size_t),
                   "redzone < 4 not supported");
            if (malloc_entry_is_pre_us(entry, false)) {
                /* was allocated before we took control, so no redzone */
                size_in_zone = false;
                LOG(2, "free of pre-control "PFX"-"PFX"\n", base, base+size);
            } else {
                drwrap_set_arg(wrapcxt, ARGNUM_FREE_PTR(type), (void *)real_base);
            }
        }
        /* We don't know how to read the Rtl headers, so we can
         * use our redzone or hashtable to store the size, or call RtlSizeHeap.
         * either way, we treat extra space beyond the requested as unaddressable,
         * which seems the right way to go;
         * on linux w/o a stored size in redzone or hashtable, we do not have 
         * the requested size as malloc_usable_size() returns the padded size
         * (as opposed to RtlSizeHeap which returns the requested size),
         * so now we assume we have the hashtable and get the size from redzone
         * or hashtable.
         */
        if (size_in_zone)
            size = *((size_t *)(base - redzone_size(routine)));
        else {
            /* since we have hashtable, we can use it to retrieve the app size */
            size = malloc_entry_size(entry);
            ASSERT((ssize_t)size != -1, "error determining heap block size");
        }
        /* i#858, we obtain the real_size from entry instead of size + redzone */
        if (base != real_base) {
            ASSERT(base - real_base == alloc_ops.redzone_size, "redzone mismatch");
            real_size = (base - real_base) + size + malloc_entry_usable_extra(entry);
        } else {
            /* A pre-us alloc or msvcrtdbg alloc (i#26) w/ no redzone */
            real_size = size;
        }
        LOG(2, "free-pre" IF_WINDOWS(" heap="PFX)" ptr="PFX
            " size="PIFX" => "PFX" real size = "PIFX"\n",
            IF_WINDOWS_(heap) base, size, real_base, real_size);

        ASSERT(routine->set != NULL, "free must be part of set");
#ifdef WINDOWS
        auxargnum = (type == RTL_ROUTINE_FREE ? 0 :
                     (type == HEAP_ROUTINE_FREE_DBG) ? 1 : -1);
        auxarg = (ptr_int_t) (auxargnum == -1 ? NULL : drwrap_get_arg(wrapcxt, auxargnum));
#endif

        top_pc = set_mc_for_client(pt, wrapcxt, mc, NULL);
        change_base = client_handle_free
            /* if we pass routine->pc, we can miss a frame b/c call_site may
             * be at top of stack with ebp pointing to its parent frame.
             * developer doesn't need to see explicit free() frame, right?
             */
            (base, size, real_base, real_size, mc, top_pc,
             routine->set->client _IF_WINDOWS(&auxarg));
        restore_mc_for_client(pt, wrapcxt, mc);
#ifdef WINDOWS
        if (auxargnum != -1)
            drwrap_set_arg(wrapcxt, auxargnum, (void *)auxarg);
#endif
        if (change_base != real_base) {
            LOG(2, "free-pre client %d changing base from "PFX" to "PFX"\n",
                type, real_base, change_base);
            drwrap_set_arg(wrapcxt, ARGNUM_FREE_PTR(type), (void *)change_base);
            /* for set_handling_heap_layer for recursion check.
             * we assume has redzone: doesn't matter, just has to match the
             * check_recursive_same_sequence call at top of this routine.
             * XXX: redzone size can vary across layers, so actually
             * it does matter: but ok for now b/c across layers we
             * always assume non-tangent.
             */
            base = change_base + redzone_size(routine);
            LOG(2, "\tchanged base => pt->alloc_base="PFX"\n", base);
            size = 0;
        }

        malloc_entry_remove(entry);
    }
    malloc_unlock();

    set_handling_heap_layer(pt, base, size);
#ifdef WINDOWS
    set_auxarg(drcontext, pt, wrapcxt, routine);
#endif
}

static void
handle_free_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                 dr_mcontext_t *mc, alloc_routine_entry_t *routine)
{
    pt->alloc_being_freed = NULL;
#ifdef WINDOWS
    if (routine->type == RTL_ROUTINE_FREE) {
        if (mc->xax == 0/*FALSE==failure*/) {
            /* If our prediction is wrong, we can't undo the shadow memory
             * changes since we've lost which were defined vs undefined,
             * along with whether this malloc was pre-us or not.  We
             * also can't undo any report of an invalid free.  See notes for a
             * solution other than holding a lock in the code cache: it requires
             * being able to restore shadow memory values and malloc
             * struct, using a nonce to know which malloc it went with.
             */
            ASSERT(pt->expect_lib_to_fail, "free() failure unexpected");
        } else {
            ASSERT(!pt->expect_lib_to_fail || pt->alloc_base == NULL,
                   "free() success unexpected");
        }
    }
#endif
    /* free() has no return value */
}

/**************************************************
 * SIZE
 */

static void
handle_size_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                alloc_routine_entry_t *routine)
{
    routine_type_t type = routine->type;
    app_pc base = (app_pc) drwrap_get_arg(wrapcxt, ARGNUM_SIZE_PTR(type));
    if (malloc_is_native(base, pt, true))
        return;
    /* non-recursive: else we assume base already adjusted */
    if (check_recursive_same_sequence(drcontext, &pt, routine, (ptr_int_t) base,
                                      (ptr_int_t) pt->alloc_base -
                                      redzone_size(routine))) {
        return;
    }
    /* store the block being asked about, in case routine changes the param */
    set_handling_heap_layer(pt, base, 0);
#ifdef WINDOWS
    set_auxarg(drcontext, pt, wrapcxt, routine);
#endif
    if (redzone_size(routine) > 0) {
        /* ensure wasn't allocated before we took control (so no redzone) */
        if (check_valid_heap_block(false, pt->alloc_base, pt, wrapcxt,
                                   /* FIXME: should have caller invoke and use
                                    * alloc_routine_name?  kernel32 names better
                                    * than Rtl though
                                    */
                                   routine->name, is_free_routine(type)) &&
            pt->alloc_base != NULL &&
            !malloc_is_pre_us(pt->alloc_base)) {
            LOG(2, "size query: changing "PFX" to "PFX"\n",
                pt->alloc_base, pt->alloc_base - redzone_size(routine));
            drwrap_set_arg(wrapcxt, ARGNUM_SIZE_PTR(type), (void *)
                           ((ptr_uint_t)drwrap_get_arg(wrapcxt, ARGNUM_SIZE_PTR(type)) -
                            redzone_size(routine)));
        }
    }
}

static void
handle_size_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                 dr_mcontext_t *mc, alloc_routine_entry_t *routine)
{
    uint failure = IF_WINDOWS_ELSE((routine->type == RTL_ROUTINE_SIZE) ? ~0UL : 0, 0);
    if (mc->xax != failure) {
        if (malloc_is_native(pt->alloc_base, pt, true))
            return;
        /* we want to return the size without the redzone */
        if (redzone_size(routine) > 0 &&
            !malloc_is_pre_us(pt->alloc_base) &&
            /* non-recursive: else we assume it's another Rtl routine calling
             * and we should use the real size anyway (e.g., RtlReAllocateHeap
             * calls RtlSizeHeap: xref i#259
             */
            pt->in_heap_adjusted == 0/*already decremented*/ &&
            /* similarly, use real size for unknown block in heap tangent */
            IF_WINDOWS_ELSE((!pt->heap_tangent || malloc_end(pt->alloc_base) != NULL),
                            true)) {
            if (pt->alloc_base != NULL) {
                LOG(2, "size query: changing "PFX" to "PFX"\n",
                    mc->xax, mc->xax - redzone_size(routine)*2);
                mc->xax -= redzone_size(routine)*2;
                drwrap_set_mcontext(wrapcxt);
#ifdef WINDOWS
                /* RtlSizeHeap returns exactly what was asked for, while
                 * malloc_usable_size includes padding which is hard to predict
                 */
                ASSERT(routine->type == HEAP_ROUTINE_SIZE_USABLE ||
                       !alloc_ops.size_in_redzone ||
                       mc->xax == *((size_t *)(pt->alloc_base - redzone_size(routine))),
                       "size mismatch");
#endif
            } else {
                ASSERT(false, "unexpected NULL succeeding for size query");
            }
        }
    }
}

/**************************************************
 * MALLOC
 */

static size_t
size_plus_redzone_overflow(alloc_routine_entry_t *routine, size_t asked_for)
{
    /* avoid overflow (we expect to fail anyway): PR 531262 */
    return (asked_for + redzone_size(routine)*2 < asked_for);
}

/* If realloc is true, this is realloc(NULL, size) */
static void
handle_malloc_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                  alloc_routine_entry_t *routine)
{
    routine_type_t type = routine->type;
    bool realloc = is_realloc_routine(type);
    uint argnum = realloc ? ARGNUM_REALLOC_SIZE(type) : ARGNUM_MALLOC_SIZE(type);
    size_t size = (size_t) drwrap_get_arg(wrapcxt, argnum);
#ifdef WINDOWS
    /* Tell NtAllocateVirtualMemory which Heap to use for any new segment (i#296) */
    if (is_rtl_routine(type))
        pt->heap_handle = (HANDLE) drwrap_get_arg(wrapcxt, 0);
    /* Low-Fragmentation Heap uses RtlAllocateHeap for blocks that it parcels
     * out via the same RtlAllocateHeap.  The flag 0x800000 indicates the
     * alloc for the block through RtlpAllocateUserBlock.
     */
    /* Lookaside lists also use RtlAllocateHeap for handing out allocs
     * w/o needing global synch on the main Heap.
     * Seems to always pass 0xa for the flags, and nobody else does that.
     * Not sure why it wouldn't lazily zero.
     */
# define RTL_LFH_BLOCK_FLAG 0x800000
# define RTL_LOOKASIDE_BLOCK_FLAGS (HEAP_ZERO_MEMORY | HEAP_GROWABLE)
    if (is_rtl_routine(type)) {
        uint flags = (uint) drwrap_get_arg(wrapcxt, 1);
        if (TEST(RTL_LFH_BLOCK_FLAG, flags)) {
            LOG(2, "%s is LFH block size="PIFX" alloc: ignoring\n", routine->name, size);
            pt->ignored_alloc = true;
            return;
        }
        if (TESTALL(RTL_LOOKASIDE_BLOCK_FLAGS, flags)) {
            LOG(2, "%s is lookaside block size="PIFX" alloc: ignoring\n",
                routine->name, size);
            pt->ignored_alloc = true;
            return;
        }
    }
#endif
    if (check_recursive_same_sequence(drcontext, &pt, routine, pt->alloc_size,
                                      size - redzone_size(routine)*2)) {
#ifdef WINDOWS
        DOLOG(2, {
            /* Heap passed in is useful to know */
            if (is_rtl_routine(type))
                LOG(2, "Rtl Heap="PFX", flags="PIFX"\n",
                    drwrap_get_arg(wrapcxt, 0), pt->alloc_flags);
        });
#endif
        return;
    }
    set_handling_heap_layer(pt, NULL, size);
#ifdef WINDOWS
    set_auxarg(drcontext, pt, wrapcxt, routine);
#endif
    if (redzone_size(routine) > 0) {
        /* FIXME: if app asks for 0 bytes should we not add our redzone in
         * case the app never frees the memory?  We'd need a way to record
         * which allocations have redzones: which we need anyway to tell
         * which ones were allocated before we took control.
         * Note that glibc malloc allocates a chunk w/ header even
         * for malloc(0).
         */
        if (size_plus_redzone_overflow(routine, pt->alloc_size)) {
            /* We assume malloc() will fail on this so we don't handle this
             * scenario in free(), etc. (PR 531262)
             */
            LOG(1, "WARNING: asked-for size "PIFX" too big to fit redzone\n",
                pt->alloc_size);
        } else {
            drwrap_set_arg(wrapcxt, argnum, (void *)(ptr_uint_t)
                           (pt->alloc_size + redzone_size(routine)*2));
        }
    }
    /* FIXME PR 406742: handle HEAP_GENERATE_EXCEPTIONS windows flag */
    LOG(2, "malloc-pre" IF_WINDOWS(" heap="PFX)
        " size="PIFX IF_WINDOWS(" flags="PIFX) "%s\n",
        IF_WINDOWS_(drwrap_get_arg(wrapcxt, 0))
        pt->alloc_size _IF_WINDOWS(pt->alloc_flags),
        realloc ? "(realloc(NULL,sz))" : "");
}

/* Returns the actual allocated size.  This can be either the
 * requested size that Dr. Memory passed to the system allocator
 * (including any redzones added) or that requested size padded to
 * some alignment.  For the exact padded size, use padded_size_out.
 * Returns -1 on error.
 * In order to export this we'd need to have either the malloc
 * hashtable or the heap regions store info to get the
 * alloc_routine_entry_t.
 */
static size_t
get_alloc_real_size(IF_WINDOWS_(reg_t auxarg) app_pc real_base, size_t app_size,
                    size_t *padded_size_out, alloc_routine_entry_t *routine)
{
    size_t real_size;
    alloc_routine_entry_t *size_func = get_size_func(routine);
    if (size_func != NULL) {
        real_size = get_alloc_size(IF_WINDOWS_(auxarg) real_base, routine);
        if (alloc_ops.get_padded_size && padded_size_out != NULL) {
            *padded_size_out = get_padded_size(IF_WINDOWS_(auxarg)
                                               real_base, routine);
            if (*padded_size_out == -1) {
                /* i#787: the size returned from malloc_usable_size() in Linux
                 * is not 8-byte aligned but 4-byte aligned.
                 */
                *padded_size_out = ALIGN_FORWARD(real_size,
                                                 IF_WINDOWS_ELSE(8, 4));
            }
        } else if (padded_size_out != NULL) {
            /* i#787: the size returned from malloc_usable_size() in Linux
             * is not 8-byte aligned but 4-byte aligned.
             */
            *padded_size_out = ALIGN_FORWARD(real_size,
                                             IF_WINDOWS_ELSE(8, 4));
        }
    } else {
        /* FIXME: if no malloc_usable_size() (and can't call malloc_size()
         * as this malloc is not yet in the hashtable), then for now we
         * ignore any extra padding.  We may have to figure out which malloc
         * it is and know the header layout and/or min alloc sizes for
         * common mallocs.
         */
        ASSERT(!size_plus_redzone_overflow(routine, app_size),
               "overflow should have failed");
        real_size = app_size + 2*redzone_size(routine);
        /* Unless re-using a larger free chunk, aligning to 8 should do it */
        if (padded_size_out != NULL) {
            /* i#787: the size returned from malloc_usable_size() in Linux
             * is not 8-byte aligned but 4-byte aligned.
             */
            *padded_size_out = ALIGN_FORWARD(real_size,
                                             IF_WINDOWS_ELSE(8, 4));
        }
    }
    return real_size;
}

static app_pc
adjust_alloc_result(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    dr_mcontext_t *mc, size_t *padded_size_out,
                    size_t *real_size_out,
                    bool used_redzone, alloc_routine_entry_t *routine)
{
    if (mc->xax != 0) {
        app_pc app_base = (app_pc) mc->xax;
        size_t real_size;
        bool query_for_size = alloc_ops.get_padded_size;
        if (query_for_size) {
            real_size = get_alloc_real_size(IF_WINDOWS_(pt->auxarg) app_base,
                                            pt->alloc_size, padded_size_out, routine);
        } else {
            /* avoid calling app size routine for two reasons: performance
             * (i#689 part 2) and correctness to avoid deadlocks (i#795, i#30)
             */
            real_size = pt->alloc_size + redzone_size(routine) * 2;
            if (padded_size_out != NULL)
                *padded_size_out = ALIGN_FORWARD
                    /* using 4 for linux b/c of i#787 */
                    (real_size, IF_WINDOWS_ELSE(MALLOC_CHUNK_ALIGNMENT, 4));
        }
        ASSERT(real_size != -1, "error getting real size");
        /* If recursive we assume called by RtlReAllocateHeap where we
         * already adjusted the size */
        if (used_redzone && redzone_size(routine) > 0)
            app_base += redzone_size(routine);
        LOG(2, "%s-post "PFX"-"PFX" = "PIFX" (really "PFX"-"PFX" "PIFX")\n",
            routine->name, app_base, app_base+pt->alloc_size, pt->alloc_size,
            app_base - (used_redzone ? redzone_size(routine) : 0),
            app_base - (used_redzone ? redzone_size(routine) : 0) + real_size, real_size);
        if (used_redzone && redzone_size(routine) > 0) {
            if (alloc_ops.size_in_redzone) {
                ASSERT(redzone_size(routine) >= sizeof(size_t), "redzone size too small");
                /* store the size for our own use */
                *((size_t *)mc->xax) = pt->alloc_size;
            }
            /* FIXME: could there be alignment guarantees provided
             * by RtlAllocateHeap that we're messing up?
             * Should we preserve any obvious alignment we see?
             */
            LOG(2, "%s-post changing from "PFX" to "PFX"\n",
                routine->name, mc->xax, app_base);
            mc->xax = (reg_t) app_base;
            drwrap_set_mcontext(wrapcxt);
        }
#ifdef WINDOWS
        /* it's simplest to do Heap tracking here instead of correlating
         * syscalls w/ RtlCreateHeap vs large heap chunks
         */
        if (is_rtl_routine(routine->type) && pt->auxarg != 0)
            heap_region_set_heap(app_base, (HANDLE)pt->auxarg);
#endif
        if (real_size_out != NULL)
            *real_size_out = real_size;
        return app_base;
    } else {
        return NULL;
    }
}

#ifdef WINDOWS
static void
check_for_inner_libc_alloc(cls_alloc_t *pt, void *wrapcxt, dr_mcontext_t *mc,
                           alloc_routine_entry_t *routine, app_pc top_pc,
                           byte *app_base, size_t app_size)
{
    /* i#607 part A: try to identify missing internal calls to
     * allocators when we have no symbols.  We'll reach the Rtl layer
     * w/o seeing any libc layer, yet our retaddr will be in libc.
     * Our solution is to add an entry for the requested alloc inside the
     * dbgcrt redzone so we can recognize it when we see the libc free.
     */
    LOG(3, "%s: dbgcrt_nosyms=%d, rtl=%d, ra="PFX", in libc=%d, level=%d adj=%d\n",
        __FUNCTION__, dbgcrt_nosyms, is_rtl_routine(routine->type),
        drwrap_get_retaddr(wrapcxt), pc_is_in_libc(drwrap_get_retaddr(wrapcxt)),
        pt->in_heap_routine, pt->in_heap_adjusted);
    if (dbgcrt_nosyms && is_rtl_routine(routine->type) &&
        pc_is_in_libc(drwrap_get_retaddr(wrapcxt)) &&
        /* We've already decremented and if Rtl was the outer adjusted should be 0
         * although in_heap_routine may not be (if inside _getptd for i#997, e.g.).
         */
        pt->in_heap_adjusted == 0) {
        LOG(2, "missed libc layer so marking as such\n");
        /* XXX: we're assuming this is a dbgcrt block but there's no way
         * to verify like we do during heap iteration (i#607 part B)
         * b/c the fields are all uninit at this point
         */
        if (app_size >= DBGCRT_PRE_REDZONE_SIZE + DBGCRT_POST_REDZONE_SIZE) {
            /* Skip the dbgcrt header */
            byte *inner_start = app_base + DBGCRT_PRE_REDZONE_SIZE;
            byte *inner_end = inner_start + app_size -
                (DBGCRT_PRE_REDZONE_SIZE + DBGCRT_POST_REDZONE_SIZE);
            LOG(2, "adding entry for dbgcrt inner alloc "PFX"-"PFX"\n",
                inner_start, inner_end);
            malloc_add_common(inner_start, inner_end, inner_end,
                              MALLOC_LIBC_INTERNAL_ALLOC, 0, mc, top_pc, pt->allocator);
        } else {
            WARN("WARNING: dbgcrt missed libc layer detected, but alloc too small");
        }
    }
}
#endif

static void
handle_alloc_failure(size_t sz, bool zeroed, bool realloc,
                     app_pc pc, dr_mcontext_t *mc)
{
    client_handle_alloc_failure(sz, zeroed, realloc, pc, mc);
}

static void
handle_malloc_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                   dr_mcontext_t *mc, bool realloc, app_pc post_call,
                   alloc_routine_entry_t *routine)
{
    app_pc real_base = (app_pc) mc->xax;
    size_t pad_size, real_size = 0;
    app_pc app_base = adjust_alloc_result(drcontext, pt, wrapcxt, mc, &pad_size,
                                          &real_size, true, routine);
    bool zeroed = IF_WINDOWS_ELSE(is_rtl_routine(routine->type) ?
                                  TEST(HEAP_ZERO_MEMORY, pt->alloc_flags) :
                                  pt->in_calloc, pt->in_calloc);
    if (pt->in_calloc) {
        /* calloc called malloc, so instruct post-calloc to NOT do anything */
        pt->malloc_from_calloc = true;
    }
#ifdef WINDOWS
    if (pt->in_realloc) {
        /* realloc called malloc, so instruct post-realloc to NOT do anything */
        pt->malloc_from_realloc = true;
    }
#endif
    if (app_base == NULL) {
        handle_alloc_failure(pt->alloc_size, zeroed, realloc, post_call, mc);
    } else {
        if (alloc_ops.record_allocs) {
            app_pc top_pc = set_mc_for_client(pt, wrapcxt, mc, post_call);
#ifdef WINDOWS
            check_for_inner_libc_alloc(pt, wrapcxt, mc, routine, top_pc,
                                       app_base, pt->alloc_size);
#endif
            malloc_add_common(app_base, app_base + pt->alloc_size, real_base+pad_size,
                              0, 0, mc, top_pc, pt->allocator);
            restore_mc_for_client(pt, wrapcxt, mc);
        }
        client_handle_malloc(drcontext, app_base, pt->alloc_size, real_base,
                             /* if no padded size, use aligned real size */
                             alloc_ops.get_padded_size ?
                             pad_size : ALIGN_FORWARD(real_size, sizeof(uint)),
                             zeroed, realloc, mc);
    }
}

/**************************************************
 * REALLOC
 */

static void
handle_realloc_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                   alloc_routine_entry_t *routine)
{
    routine_type_t type = routine->type;
    app_pc real_base;
    bool size_in_zone = redzone_size(routine) > 0 && alloc_ops.size_in_redzone;
    bool invalidated = false;
    size_t size = (size_t) drwrap_get_arg(wrapcxt, ARGNUM_REALLOC_SIZE(type));
    app_pc base = (app_pc) drwrap_get_arg(wrapcxt, ARGNUM_REALLOC_PTR(type));
    malloc_entry_t *entry;
    if (base == NULL) {
        /* realloc(NULL, size) == malloc(size) (PR 416535) */
        /* call_site for call;jmp will be jmp, so retaddr better even if post-call */
        client_handle_realloc_null(drwrap_get_retaddr(wrapcxt),
                                   drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR));
        if (!alloc_ops.replace_realloc) {
            handle_malloc_pre(drcontext, pt, wrapcxt, routine);
            return;
        }
    }
    if (alloc_ops.replace_realloc) {
        /* subsequent malloc will clobber alloc_size */
        pt->realloc_replace_size = size;
        LOG(2, "realloc-pre "PFX" new size %d\n", base, pt->realloc_replace_size);
        return;
    }
    malloc_lock();
    entry = malloc_lookup(base);
    if (entry != NULL && malloc_entry_is_native_ex(entry, base, pt, true)) {
        malloc_entry_remove(entry);
        malloc_unlock();
        return;
    }
#ifdef WINDOWS
    /* Tell NtAllocateVirtualMemory which Heap to use for any new segment (i#296) */
    if (is_rtl_routine(type))
        pt->heap_handle = (HANDLE) drwrap_get_arg(wrapcxt, 0);
#endif
    if (check_recursive_same_sequence(drcontext, &pt, routine, pt->alloc_size,
                                      size - redzone_size(routine)*2)) {
        malloc_unlock();
        return;
    }
    set_handling_heap_layer(pt, base, size);
#ifdef WINDOWS
    set_auxarg(drcontext, pt, wrapcxt, routine);
#endif
    pt->in_realloc = true;
    real_base = pt->alloc_base;
    if (!check_valid_heap_block(entry == NULL, pt->alloc_base, pt, wrapcxt,
                                routine->name, is_free_routine(type))) {
        pt->expect_lib_to_fail = true;
        malloc_unlock();
        return;
    }
    if (redzone_size(routine) > 0) {
        ASSERT(redzone_size(routine) >= 4, "redzone < 4 not supported");
        if (malloc_entry_is_pre_us(entry, false)) {
            /* was allocated before we took control, so no redzone */
            /* since we have hashtable, we can use it to retrieve the app size
             * (see comments in handle_free_pre())
             */
            pt->realloc_old_size = malloc_entry_size(entry);
            ASSERT((ssize_t)pt->realloc_old_size != -1,
                   "error getting pre-us size");
            /* if we wait until post-free to check failure, we'll have
             * races, so we invalidate here: see comments for free */
            malloc_entry_set_valid(entry, false);
            invalidated = true;
            size_in_zone = false;
            LOG(2, "realloc of pre-control "PFX"-"PFX"\n",
                pt->alloc_base, pt->alloc_base + pt->realloc_old_size);
        } else {
            real_base -= redzone_size(routine);
            drwrap_set_arg(wrapcxt, ARGNUM_REALLOC_PTR(type), (void *)real_base);
        }
        /* realloc(non-NULL, 0) == free(non-NULL) (PR 493870, PR 493880) 
         * However, on some malloc impls it does re-alloc a 0-sized chunk:
         * - for dlmalloc in glibc, depending on REALLOC_ZERO_BYTES_FREES
         *   define, which is set on linux and visor, but not cygwin
         * - msvcrt does free, but HeapReAlloc does not
         *
         * Unfortunately we have to decide ahead of time whether to add
         * our redzone to the size (if we add the redzone and the app
         * assumes it's going to free, we can introduce a memory leak).
         * Rather than hardcoding which realloc does what we give up our
         * redzone.
         */
        if (pt->alloc_size > 0) {
            if (size_plus_redzone_overflow(routine, pt->alloc_size)) {
                /* We assume realloc() will fail on this so we don't handle this
                 * scenario in free(), etc. (PR 531262)
                 */
                LOG(1, "WARNING: asked-for size "PIFX" too big to fit redzone\n",
                    pt->alloc_size);
            } else {
                drwrap_set_arg(wrapcxt, ARGNUM_REALLOC_SIZE(type), (void *)
                               (ptr_uint_t)(pt->alloc_size + redzone_size(routine)*2));
            }
        }
    }
    /* We don't know how to read the Rtl headers, so we must
     * either use our redzone to store the size or call RtlSizeHeap.
     */
    if (size_in_zone)
        pt->realloc_old_size = *((size_t *)(pt->alloc_base - redzone_size(routine)));
    else if (!invalidated) {
        /* since we have hashtable, we can use it to retrieve the app size
         * (see comments in handle_free_pre())
         */
        pt->realloc_old_size = malloc_entry_size(entry);
        ASSERT((ssize_t)pt->realloc_old_size != -1, "error determining heap block size");
        pt->realloc_old_size -= redzone_size(routine)*2;
    }
    ASSERT((ssize_t)pt->realloc_old_size != -1,
           "error determining heap block size");
    LOG(2, "realloc-pre "IF_WINDOWS("heap="PFX)
        " base="PFX" oldsz="PIFX" newsz="PIFX"\n",
        IF_WINDOWS_(drwrap_get_arg(wrapcxt, 0))
        pt->alloc_base, pt->realloc_old_size, pt->alloc_size);
    if (alloc_ops.record_allocs && !invalidated)
        malloc_entry_set_valid(entry, false);
    malloc_unlock();
}

static void
handle_realloc_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    dr_mcontext_t *mc, app_pc post_call,
                    alloc_routine_entry_t *routine)
{
    if (alloc_ops.replace_realloc) {
        /* for sz==0 normal to return NULL */
        if (mc->xax == 0 && pt->realloc_replace_size != 0) {
            LOG(2, "realloc-post failure %d %d\n",
                pt->alloc_size, pt->realloc_replace_size);
            handle_alloc_failure(pt->alloc_size, false, true, post_call, mc);
        }
        return;
    }
    if (pt->alloc_base == NULL) {
        /* realloc(NULL, size) == malloc(size) (PR 416535) */
        handle_malloc_post(drcontext, pt, wrapcxt, mc, true/*realloc*/, post_call,
                           routine);
        return;
    }
    pt->in_realloc = false;
#ifdef WINDOWS
    if (pt->malloc_from_realloc) {
        /* post-malloc handled everything */
        pt->malloc_from_realloc = false;
        return;
    }
#endif
    if (mc->xax != 0) {
        app_pc real_base = (app_pc) mc->xax;
        size_t pad_size, real_size;
        app_pc app_base = adjust_alloc_result(drcontext, pt, wrapcxt, mc, &pad_size,
                                              &real_size,
                                              /* no redzone for sz==0 */
                                              pt->alloc_size != 0, routine);
        app_pc old_end = pt->alloc_base + pt->realloc_old_size;
        /* realloc sometimes calls free, but shouldn't be any conflicts */
        if (alloc_ops.record_allocs) {
            /* we can't remove the old one since it could have been
             * re-used already: so we leave it as invalid */
            app_pc top_pc = set_mc_for_client(pt, wrapcxt, mc, post_call);
#ifdef WINDOWS
            check_for_inner_libc_alloc(pt, wrapcxt, mc, routine, top_pc,
                                       app_base, pt->alloc_size);
#endif
            malloc_add_common(app_base, app_base + pt->alloc_size,
                              real_base + 
                              (alloc_ops.get_padded_size ? pad_size : real_size),
                              0, 0, mc, top_pc, pt->allocator);
            restore_mc_for_client(pt, wrapcxt, mc);
            if (pt->alloc_size == 0) {
                /* PR 493870: if realloc(non-NULL, 0) did allocate a chunk, mark
                 * as pre-us since we did not put a redzone on it
                 */
                ASSERT(real_base == app_base, "no redzone on realloc(,0)");
                malloc_set_pre_us(app_base);
                LOG(2, "realloc-post "PFX" sz=0 no redzone padsz="PIFX"\n",
                    app_base, pad_size);
            }
        }
        client_handle_realloc(drcontext, pt->alloc_base, old_end - pt->alloc_base,
                              app_base, pt->alloc_size, real_base, mc);
    } else if (pt->alloc_size != 0) /* for sz==0 normal to return NULL */ {
        /* if someone else already replaced that's fine */
        if (malloc_is_pre_us_ex(pt->alloc_base, true/*check invalid too*/) ||
            alloc_ops.record_allocs) {
            /* still there, and still pre-us if it was before */
            malloc_set_valid(pt->alloc_base, true);
            LOG(2, "re-instating failed realloc as pre-control "PFX"-"PFX"\n",
                pt->alloc_base, pt->alloc_base + pt->realloc_old_size);
        }
        handle_alloc_failure(pt->alloc_size, false, true, post_call, mc);
    }
}

/**************************************************
 * CALLOC
 */

static void
handle_calloc_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                  alloc_routine_entry_t *routine)
{
    /* void *calloc(size_t nmemb, size_t size) */
    size_t count = (size_t) drwrap_get_arg(wrapcxt, 0);
    size_t each = (size_t) drwrap_get_arg(wrapcxt, 1);
    size_t size = (size_t) (count * each);
#ifdef WINDOWS
    /* Tell NtAllocateVirtualMemory which Heap to use for any new segment (i#296) */
    if (is_rtl_routine(routine->type))
        pt->heap_handle = (HANDLE) drwrap_get_arg(wrapcxt, 0);
#endif
    if (check_recursive_same_sequence(drcontext, &pt, routine, pt->alloc_size,
                                      size - redzone_size(routine)*2)) {
        return;
    }
    set_handling_heap_layer(pt, NULL, size);
#ifdef WINDOWS
    set_auxarg(drcontext, pt, wrapcxt, routine);
#endif
    /* we need to handle calloc allocating by itself, or calling malloc */
    ASSERT(!pt->in_calloc, "recursive calloc not handled");
    pt->in_calloc = true;
    ASSERT((count == 0 || each == 0) ||
           (count * each >= count && count * each >= each), "calloc overflow");
    if (redzone_size(routine) > 0) {
        /* we may end up with more extra than we need, but it should be
         * fine: we'll only get off if we can't obtain the actual
         * malloc size post-malloc/calloc.
         * we'll keep exactly redzone_size prior to app's base,
         * so any stored size will be locatable, w/ the extra
         * all after the app's requested size.
         */
        if (count == 0 || each == 0) {
            drwrap_set_arg(wrapcxt, 0, (void *)(ptr_uint_t)1);
            drwrap_set_arg(wrapcxt, 1, (void *)(ptr_uint_t)(redzone_size(routine)*2));
        } else if (count < each) {
            /* More efficient to increase size of each (PR 474762) since
             * any extra due to no fractions will be multiplied by a
             * smaller number
             */
            size_t extra_each = (redzone_size(routine)*2 + count -1) / count;
            if (each + extra_each < each) { /* overflow */
                /* We assume calloc() will fail on this so we don't handle this
                 * scenario in free(), etc. (PR 531262)
                 * count*each could overflow: we assert above but don't handle.
                 */
                LOG(1, "WARNING: asked-for "PIFX"x"PIFX" too big to fit redzone\n",
                    count, each);
            } else
                drwrap_set_arg(wrapcxt, 1, (void *)(ptr_uint_t)(each + extra_each));
        } else {
            /* More efficient to increase the count */
            size_t extra_count = (redzone_size(routine)*2 + each - 1) / each;
            if (count + extra_count < count) { /* overflow */
                /* We assume calloc() will fail on this so we don't handle this
                 * scenario in free(), etc. (PR 531262).
                 * count*each could overflow: we assert above but don't handle.
                 */
                LOG(1, "WARNING: asked-for "PIFX"x"PIFX" too big to fit redzone\n",
                    count, each);
            } else
                drwrap_set_arg(wrapcxt, 0, (void *)(ptr_uint_t)(count + extra_count));
        }
    }
    LOG(2, "calloc-pre "PIFX" x "PIFX"\n", count, each);
}

static void
handle_calloc_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                   dr_mcontext_t *mc, app_pc post_call,
                   alloc_routine_entry_t *routine)
{
    app_pc real_base = (app_pc) mc->xax;
    size_t pad_size, real_size;
    app_pc app_base;
    ASSERT(pt->in_calloc, "calloc tracking messed up");
    pt->in_calloc = false;
    if (pt->malloc_from_calloc) {
        /* post-malloc handled everything */
        pt->malloc_from_calloc = false;
        return;
    }
    app_base = adjust_alloc_result(drcontext, pt, wrapcxt, mc, &pad_size,
                                   &real_size, true, routine);
    if (app_base == NULL) {
        handle_alloc_failure(pt->alloc_size, true, false, post_call, mc);
    } else {
        if (alloc_ops.record_allocs) {
#ifdef WINDOWS
            check_for_inner_libc_alloc(pt, wrapcxt, mc, routine, post_call,
                                       app_base, pt->alloc_size);
#endif
            malloc_add_common(app_base, app_base + pt->alloc_size, real_base+pad_size,
                              0, 0, mc, post_call, pt->allocator);
        }
        client_handle_malloc(drcontext, app_base, pt->alloc_size, real_base,
                             /* if no padded size, use aligned real size */
                             alloc_ops.get_padded_size ?
                             pad_size : ALIGN_FORWARD(real_size, sizeof(uint)),
                             true/*zeroed*/, false/*!realloc*/, mc);
    }
}

#ifdef WINDOWS
/**************************************************
 * CREATE
 */

static void
handle_create_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt)
{
    /* RtlCreateHeap(ULONG Flags,
     *               PVOID HeapBase OPTIONAL,
     *               SIZE_T ReserveSize OPTIONAL,
     *               SIZE_T CommitSize OPTIONAL,
     *               PVOID Lock OPTIONAL,
     *               PRTL_HEAP_PARAMETERS Parameters OPTIONAL);
     */
    LOG(2, "RtlCreateHeap flags="PFX", base="PFX", res="PFX", commit="PFX"\n",
        drwrap_get_arg(wrapcxt, 0), drwrap_get_arg(wrapcxt, 1),
        drwrap_get_arg(wrapcxt, 2), drwrap_get_arg(wrapcxt, 3));
    pt->in_create = true;
    /* don't use stale values for setting Heap (i#296) */
    pt->heap_handle = (HANDLE) drwrap_get_arg(wrapcxt, 0);
}

static void
handle_create_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                   dr_mcontext_t *mc)
{
    /* RtlCreateHeap(ULONG Flags,
     *               PVOID HeapBase OPTIONAL,
     *               SIZE_T ReserveSize OPTIONAL,
     *               SIZE_T CommitSize OPTIONAL,
     *               PVOID Lock OPTIONAL,
     *               PRTL_HEAP_PARAMETERS Parameters OPTIONAL);
     */
    if (mc->xax != 0) {
        HANDLE heap = (HANDLE) mc->xax;
        heap_region_set_heap((byte *)heap, heap);
    }
    pt->in_create = false;
}

/**************************************************
 * DESTROY
 */

typedef struct _heap_destroy_info_t {
    HANDLE heap;
    byte *start;
    byte *end;
} heap_destroy_info_t;

static bool
heap_destroy_iter_cb(app_pc start, app_pc end, app_pc real_end,
                     bool pre_us, uint client_flags,
                     void *client_data, void *iter_data)
{
    heap_destroy_info_t *info = (heap_destroy_info_t *) iter_data;
    if (start < info->end && end >= info->start) {
        ASSERT(start >= info->start && end <= info->end,
               "alloc should be entirely inside Heap");
        /* we already called client_handle_heap_destroy() for whole-heap handling.
         * we also call a special cb for individual handling.
         * additionally, client_remove_malloc_*() will be called by malloc_remove().
         */
        if (!TEST(MALLOC_RTL_INTERNAL, client_flags))
            client_remove_malloc_on_destroy(info->heap, start, end);
        /* yes the iteration can handle this.  this involves another lookup but
         * that's ok: RtlDestroyHeap is rare.
         */
        LOG(2, "removing chunk "PFX"-"PFX" in removed arena "PFX"-"PFX"\n",
            start, end, info->start, info->end);
        malloc_remove(start);
    }
    return true;
}

static bool
heap_destroy_segment_iter_cb(byte *start, byte *end, uint flags
                             _IF_WINDOWS(HANDLE heap), void *data)
{
    HANDLE my_heap = (HANDLE) data;
    heap_destroy_info_t info;
    if (heap != my_heap)
        return true;
    info.heap = my_heap;
    info.start = start;
    info.end = end;
    LOG(2, "RtlDestroyHeap handle="PFX" segment="PFX"-"PFX"\n", heap, start, end);
    /* FIXME: a heap interval tree would be much more efficient but
     * it slows down the common case too much (xref PR 535568) and we
     * assume RtlDestroyHeap is pretty rare.
     * If there are many mallocs and the heap is small we could instead
     * walk the heap like we used to using either shadow info (though
     * xref PR 539402 on accuracy issues) or just every 8 bytes (like
     * -leaks_only used to do).
     */
    malloc_iterate_internal(true/*include native and LFH*/,
                            heap_destroy_iter_cb, (void *) &info);
    return true;
}

static void
handle_destroy_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                   alloc_routine_entry_t *routine)
{
    /* RtlDestroyHeap(PVOID HeapHandle)
     *
     * The app does not have to free individual allocs first.
     * For removing the heap region, we should already handle that at
     * the syscall level.  We assume that such removal, other than
     * when preceded by this Windows-only routine, always frees
     * individual allocs first.
     */
    HANDLE heap = (HANDLE) drwrap_get_arg(wrapcxt, 0);
    LOG(2, "RtlDestroyHeap handle="PFX"\n", heap);
    /* There can be multiple segments so we must iterate.  This relies
     * on having labeled each heap region/segment with its Heap ahead
     * of time.  To do that we set pt->heap_handle so we have the
     * value regardless of whether coming from an outer layer (i#296).
     * pt->auxarg is also used by _dbg and is only set when Rtl is the
     * outer routine, so we use a dedicated field.
     *
     * An alternative would be to wait for NtFreeVirtualMemory called
     * from RtlDestroyHeap to find all heap segments: but what if
     * RtlDestroyHeap re-uses the memory instead of freeing?
     */
    heap_region_iterate(heap_destroy_segment_iter_cb, (void *) heap);
    /* i#264: client needs to clean up any data related to allocs inside this heap */
    ASSERT(routine->set != NULL, "destroy must be part of set");
    client_handle_heap_destroy(drcontext, heap, routine->set->client);
}

static void
handle_destroy_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt)
{
    /* RtlDestroyHeap(ULONG Flags,
     *               PVOID HeapBase OPTIONAL,
     *               SIZE_T ReserveSize OPTIONAL,
     *               SIZE_T CommitSize OPTIONAL,
     *               PVOID Lock OPTIONAL,
     *               PRTL_HEAP_PARAMETERS Parameters OPTIONAL);
     */
}

/**************************************************
 * GETINFO
 */

static void
handle_userinfo_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    alloc_routine_entry_t *routine)
{
    /* 3 related routines here:
     *   BOOLEAN NTAPI
     *   RtlGetUserInfoHeap(
     *       IN PVOID HeapHandle,
     *       IN ULONG Flags,
     *       IN PVOID BaseAddress,
     *       OUT PVOID *UserValue,
     *       OUT PULONG UserFlags);
     *   BOOLEAN NTAPI
     *   RtlSetUserValueHeap(
     *       IN PVOID HeapHandle,
     *       IN ULONG Flags,
     *       IN PVOID BaseAddress,
     *       IN PVOID UserValue);
     *   BOOLEAN NTAPI
     *   RtlSetUserFlagsHeap(
     *       IN PVOID HeapHandle,
     *       IN ULONG Flags,
     *       IN PVOID BaseAddress,
     *       IN ULONG UserFlags);
     */
    app_pc base = (app_pc) drwrap_get_arg(wrapcxt, 2);
    if (malloc_is_native(base, pt, true))
        return;
    if (check_recursive_same_sequence(drcontext, &pt, routine, (ptr_int_t) base,
                                      (ptr_int_t) pt->alloc_base -
                                      redzone_size(routine))) {
        return;
    }
    set_handling_heap_layer(pt, base, 0);
    LOG(2, "Rtl*User*Heap "PFX", "PFX", "PFX"\n",
        drwrap_get_arg(wrapcxt, 0), drwrap_get_arg(wrapcxt, 1),
        drwrap_get_arg(wrapcxt, 2));
    if (check_valid_heap_block(false, pt->alloc_base, pt, wrapcxt, routine->name, false) &&
        redzone_size(routine) > 0) {
        /* ensure wasn't allocated before we took control (so no redzone) */
        if (pt->alloc_base != NULL &&
            !malloc_is_pre_us(pt->alloc_base) &&
            /* non-recursive: else we assume base already adjusted */
            pt->in_heap_routine == 1) {
            LOG(2, "Rtl*User*Heap: changing "PFX" to "PFX"\n",
                pt->alloc_base, pt->alloc_base - redzone_size(routine)*2);
            drwrap_set_arg(wrapcxt, 2, (void *)((ptr_uint_t)drwrap_get_arg(wrapcxt, 2) -
                                                redzone_size(routine)));
        }
    }
}

static void
handle_userinfo_post(void *drcontext, cls_alloc_t *pt, void *wrapcxt)
{
    /* FIXME: do we need to adjust the uservalue result? */
}

/**************************************************
 * VALIDATE
 */

static void
handle_validate_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    alloc_routine_entry_t *routine)
{
    /* we need to adjust the pointer to take into account our redzone
     * (otherwise the validate code calls ntdll!DbgPrint, DR complains
     * about int3, and the process exits)
     */
    app_pc base = (app_pc) drwrap_get_arg(wrapcxt, 2);
    if (malloc_is_native(base, pt, true))
        return;
    if (check_recursive_same_sequence(drcontext, &pt, routine, (ptr_int_t) base,
                                      (ptr_int_t) pt->alloc_base -
                                      redzone_size(routine))) {
        return;
    }
    set_handling_heap_layer(pt, base, 0);
    if (redzone_size(routine) > 0) {
        /* BOOLEAN NTAPI RtlValidateHeap(HANDLE Heap, ULONG Flags, PVOID Block)
         * Block is optional
         */
        app_pc block = (app_pc) drwrap_get_arg(wrapcxt, 2);
        pt->alloc_base = block; /* in case self-recurses */
        if (block == NULL) {
            ASSERT(false, "RtlValidateHeap on entire heap not supported");
        } else if (check_valid_heap_block(false, block, pt, wrapcxt, "HeapValidate",
                                          false)) {
            if (!malloc_is_pre_us(block)) {
                LOG(2, "RtlValidateHeap: changing "PFX" to "PFX"\n",
                    block, block - redzone_size(routine));
                drwrap_set_arg(wrapcxt, 2, (void *)(block - redzone_size(routine)));
            }
        }
    } 
}


/**************************************************
 * RtlCreateActivationContext
 */

static void
handle_create_actcxt_pre(void *drcontext, cls_alloc_t *pt, void *wrapcxt)
{
    /* i#352: kernel32!CreateActCtxW invokes csrss via
     * kernel32!NtWow64CsrBaseCheckRunApp to map in a data file.  The
     * data structure offsets there vary by platform, and
     * NtWow64CsrBaseCheckRunApp is not exported, but the mapped file
     * is not touched until RtlCreateActivationContext is called: so
     * we wait for that.  The base of the mapped file is passed as the
     * 2nd param to RtlCreateActivationContext.
     *
     * Note that the dealloc is not done via csrss but via regular
     * NtUnmapViewOfSection by ntdll!RtlpFreeActivationContext
     * calling kernel32!BasepSxsActivationContextNotification
     */
    byte *base = (byte *) drwrap_get_arg(wrapcxt, 1);
    size_t size = allocation_size(base, NULL);
    if (size != 0) {
        LOG(2, "RtlCreateActivationContext (via csrss): "PFX"-"PFX"\n",
            base, base + size);
        client_handle_mmap(drcontext, base, size, false/*file-backed*/);
    } else {
        LOG(1, "WARNING: RtlCreateActivationContext invalid base "PFX"\n", base);
    }
}

#endif /* WINDOWS */

/**************************************************
 * SHARED HOOK CODE
 */

/* only used if alloc_ops.track_heap */
static void
handle_alloc_pre_ex(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    app_pc call_site, app_pc expect, 
                    alloc_routine_entry_t *routine);

static void
alloc_hook(void *wrapcxt, INOUT void **user_data)
{
    app_pc pc = drwrap_get_func(wrapcxt);
    /* XXX: for -conservative we should do a lookup and not trust *user_data
     * b/c we could have racy unload of a module
     */
    alloc_routine_entry_t *routine = (alloc_routine_entry_t *) *user_data;
    void *drcontext = drwrap_get_drcontext(wrapcxt);
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    app_pc retaddr;

    /* pass to handle_alloc_post()
     * N.B.: note that I tried passing pt, but the cost of handling a
     * heap tangent pushing a new CLS context outweighs the gain from
     * not having to call drmgr_get_cls_field() in the post-hook
     * (I tried A. storing user_data in pt and having heap tangent update,
     * B. having the pre-helpers return pt, and C. storing pt->in_heap_routine
     * level pre and post handle_alloc_pre_ex() and re-calling
     * drmgr_get_cls_field() if the level didn't change: none beat out
     * the current code).  (I had a backpointer in pt to get drcontext.)
     */
    *user_data = drcontext;

    ASSERT(pc != NULL, "alloc_hook: pc is NULL!");
    ASSERT(alloc_ops.track_heap, "unknown reason in alloc hook");

    /* if the entry was a jmp* and we didn't see the call prior to it,
     * we did not know the retaddr, so add it now 
     */
    retaddr = drwrap_get_retaddr(wrapcxt);
    /* We will come here again after the flush-redirect.
     * FIXME: should we try to flush the call instr itself: don't
     * know size though but can be pretty sure.
     */
    LOG(3, "alloc_hook retaddr="PFX"\n", retaddr);

    /* If we did not yet do the pre-call instrumentation (i.e., we
     * came here via indirect call/jmp) then do it now.  Note that
     * we can't use "in_heap_routine==0 || !has_entry" as the test
     * here since we want to repeat the pre-instru for a recursive
     * invocation of a call* for which we did identify the
     * retaddr.  An example of a recursive call is glibc's
     * double-free check calling strdup and calloc.
     * Update: we now longer do pre-call instru so we always call the
     * pre-hook here.
     */
    handle_alloc_pre_ex(drcontext, pt, wrapcxt, retaddr, pc, routine);
}

#ifdef WINDOWS
static void
alloc_wrap_exception(void *wrapcxt, void OUT **user_data)
{
    void *drcontext = dr_get_current_drcontext();
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR); /* don't need xmm */
    /* XXX PR 408545: preserve pre-fault values and watch NtContinue and
     * longjmp (unless longjmp from top handler still invokes
     * NtContinue) and determine whether returning to heap routine.  For
     * now assuming heap routines do not handle faults.
     *
     * Update: drwrap now uses heuristics to try and handle SEH unwind.
     * we don't need to clear pt->in_heap{routine,adjusted} b/c
     * drwrap will now call our post-call w/ wrapcxt==NULL
     */
    LOG(2, "Exception in app\n");
    pt->in_seh = true;
    client_handle_exception(drcontext, mc);
    client_handle_Ki(drcontext, drwrap_get_func(wrapcxt), mc, false/*!cb*/);
}

static void
alloc_wrap_Ki(void *wrapcxt, void OUT **user_data)
{
    app_pc pc = drwrap_get_func(wrapcxt);
    void *drcontext = dr_get_current_drcontext();
    dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR); /* don't need xmm */
    bool is_cb = (bool) *user_data;
    ASSERT(pc != NULL, "alloc_hook: pc is NULL!");
    /* our per-thread data is private per callback so we're already handling
     * cbs (though we don't expect callbacks to interrupt heap routines).
     * we handle exceptions interrupting heap routines here.
     */
    client_handle_Ki(drcontext, pc, mc, is_cb);
    if (is_cb)
        client_handle_callback(drcontext);
}
#endif /* WINDOWS */

/* only used if alloc_ops.track_heap */
static void
handle_alloc_pre_ex(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                    app_pc call_site, app_pc expect, alloc_routine_entry_t *routine)
{
    routine_type_t type;
    alloc_routine_entry_t routine_local;
    if (alloc_ops.conservative) {
        /* i#708: get a copy from table while holding lock, rather than using
         * pointer into struct that can be deleted if module is racily unloaded
         */
        if (!get_alloc_entry(expect, &routine_local)) {
            ASSERT(false, "fatal: can't find alloc entry");
            return; /* maybe release build will limp along */
        }
        routine = &routine_local;
    }
    ASSERT(routine != NULL, "invalid param");
    type = routine->type;

    ASSERT(expect != NULL, "handle_alloc_pre: expect is NULL!");
    LOG(2, "entering alloc routine "PFX" %s type=%d rec=%d adj=%d%s\n",
        expect, get_alloc_routine_name(expect), type,
        pt->in_heap_routine, pt->in_heap_adjusted,
        pt->in_heap_routine > 0 ? " (recursive)" : "");
    DOLOG(3, {
        /* check for things like i#816 */
        ASSERT(strcmp(get_alloc_routine_name(expect), routine->name) == 0,
               "error in user_data passed to pre-alloc hook");
    });
    DOLOG(4, {
        print_callstack_to_file(drcontext, drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR),
                                call_site, INVALID_FILE/*use pt->f*/);
    });
#if defined(WINDOWS) && defined (USE_DRSYMS)
    DODEBUG({
        if (is_rtl_routine(type) &&
            (is_free_routine(type) || is_size_routine(type) ||
             is_malloc_routine(type) || is_realloc_routine(type) ||
             is_calloc_routine(type))) {
            HANDLE heap = (HANDLE) drwrap_get_arg(wrapcxt, 0);
            ASSERT(heap != get_private_heap_handle(), "app is using priv heap");
        }
    });
#endif

    /* i#123: check for mismatches.  Because of placement new and other
     * complications, new and delete are non-adjusting layers: we just
     * do mismatch checks.
     * N.B.: record_allocator() should be called even if not reporting
     * mismatches as it also records the outer layer (i#913)
     */
    if (is_new_routine(type) ||
        is_malloc_routine(type) ||
        is_realloc_routine(type) ||
        is_calloc_routine(type)) {
        record_allocator(drcontext, pt, routine, wrapcxt);
    } else if (is_delete_routine(type) ||
               is_free_routine(type)) {
        if (pt->in_heap_routine == 0) {
            if (is_delete_routine(type)) {
                /* free() checked in handle_free_pre */
                /* N.B.: should be called even if not reporting mismatches as it also
                 * records the outer layer (i#913)
                 */
                handle_free_check_mismatch(drcontext, pt, wrapcxt, routine, NULL);
#ifdef WINDOWS
                pt->ignore_next_mismatch = false; /* just in case */
#endif
            }
            pt->allocator = 0; /* in case missed alloc post */
        }
#ifdef WINDOWS
    } else if (type == HEAP_ROUTINE_DebugHeapDelete) {
        /* std::_DebugHeapDelete<*> is used for both delete and delete[] and it
         * directly calls free so we can't find mismatches when it's used.  This
         * is i#722 and i#655.  We're at the call/jmp to free inside
         * std::_DebugHeapDelete, so unfortunately we can't easily get the arg
         * and store it as a better check than a bool flag w/o having more info
         * on whether jmp or call.
         */
        pt->ignore_next_mismatch = true;
#endif
    }
    if (!routine->intercept_post)
        return; /* no post wrap so don't "enter" */
             
    enter_heap_routine(pt, expect, routine);

    if (is_free_routine(type)) {
        handle_free_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (is_size_routine(type)) {
        handle_size_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (is_malloc_routine(type)) {
        handle_malloc_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (is_realloc_routine(type)) {
        handle_realloc_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (is_calloc_routine(type)) {
        handle_calloc_pre(drcontext, pt, wrapcxt, routine);
    }
#ifdef WINDOWS
    else if (type == RTL_ROUTINE_CREATE) {
        handle_create_pre(drcontext, pt, wrapcxt);
    }
    else if (type == RTL_ROUTINE_DESTROY) {
        handle_destroy_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (type == RTL_ROUTINE_VALIDATE) {
        handle_validate_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (type == RTL_ROUTINE_GETINFO ||
             type == RTL_ROUTINE_SETINFO ||
             type == RTL_ROUTINE_SETFLAGS) {
        handle_userinfo_pre(drcontext, pt, wrapcxt, routine);
    }
    else if (type == RTL_ROUTINE_HEAPINFO) {
        /* i#280: turn both HeapEnableTerminationOnCorruption and
         * HeapCompatibilityInformation (xref i#63)
         * into no-ops.  We have the routine fail: seems better to
         * have app know than to pretend it worked?
         * I fail via invalid param rather than replacing routine
         * and making up some errno.
         */
        /* RtlSetHeapInformation(HANDLE heap, HEAP_INFORMATION_CLASS class, ...) */
        LOG(1, "disabling %s "PFX" %d\n", routine->name,
            drwrap_get_arg(wrapcxt, 0), drwrap_get_arg(wrapcxt, 1));
        drwrap_set_arg(wrapcxt, 1, (void *)(ptr_int_t)-1);
    }
    else if (type == RTL_ROUTINE_CREATE_ACTCXT) {
        handle_create_actcxt_pre(drcontext, pt, wrapcxt);
    }
    else if (alloc_ops.disable_crtdbg && type == HEAP_ROUTINE_SET_DBG) {
        /* i#51: disable crt dbg checks: don't let app request _CrtCheckMemory */
        LOG(1, "disabling %s %d\n", routine->name, drwrap_get_arg(wrapcxt, 0));
        drwrap_set_arg(wrapcxt, 0, (void *)(ptr_uint_t)0);
    }
#endif
    else if (type == HEAP_ROUTINE_NOT_HANDLED) {
        /* XXX: once we have the aligned-malloc routines turn this
         * into a NOTIFY_ERROR and dr_abort
         */
        LOG(1, "WARNING: unhandled heap routine %s\n", routine->name);
    }
    else if (type == HEAP_ROUTINE_NOT_HANDLED_NOTIFY) {
        /* XXX: once we have the aligned-malloc routines turn this
         * into a NOTIFY_ERROR and dr_abort
         */
        NOTIFY_ERROR("unhandled heap routine %s"NL, routine->name);
        dr_abort();
    }
}

/* only used if alloc_ops.track_heap */
static void
handle_alloc_post_func(void *drcontext, cls_alloc_t *pt, void *wrapcxt,
                       dr_mcontext_t *mc, app_pc func, app_pc post_call,
                       alloc_routine_entry_t *routine)
{
    routine_type_t type;
    bool adjusted = false;
    alloc_routine_entry_t routine_local;

    if (alloc_ops.conservative) {
        /* i#708: get a copy from table while holding lock, rather than using
         * pointer into struct that can be deleted if module is racily unloaded
         */
        if (!get_alloc_entry(func, &routine_local)) {
            ASSERT(false, "fatal: can't find alloc entry");
            return; /* maybe release build will limp along */
        }
        routine = &routine_local;
    }

    type = routine->type;
    ASSERT(func != NULL, "handle_alloc_post: func is NULL!");
    ASSERT(pt->in_heap_routine > 0, "caller should have checked");

    /* We speculatively place our post-alloc instru, and if once in_heap_routine
     * is > 0 there are call sites that do not always call alloc routines,
     * we can decrement when we should wait -- but no such scenario should
     * exist in regular alloc code.
     */
    LOG(2, "leaving alloc routine "PFX" %s rec=%d adj=%d\n",
        func, get_alloc_routine_name(func),
        pt->in_heap_routine, pt->in_heap_adjusted);
    DOLOG(4, {
        print_callstack_to_file(drcontext, mc, post_call, INVALID_FILE/*use pt->f*/);
    });
    if (pt->in_heap_routine == pt->in_heap_adjusted) {
        pt->in_heap_adjusted = 0;
        adjusted = true;
    }
    pt->in_heap_routine--;
    if (wrapcxt == NULL) {
        /* an exception occurred.  we've already decremented so we're done */
        return;
    }
    if (pt->in_heap_adjusted > 0 ||
        (!adjusted && pt->in_heap_adjusted < pt->in_heap_routine)) {
        if (pt->ignored_alloc) {
            LOG(2, "ignored post-alloc routine "PFX" %s => "PFX"\n",
                func, get_alloc_routine_name(func), mc->xax);
            /* remember the alloc so we can ignore on size or free */
            ASSERT(is_malloc_routine(type) ||
                   is_realloc_routine(type) ||
                   is_calloc_routine(type), "ignored_alloc incorrectly set");
            malloc_add_common((app_pc)mc->xax,
                              /* don't need size */
                              (app_pc)mc->xax, (app_pc)mc->xax,
                              MALLOC_RTL_INTERNAL, 0, mc, post_call, type);
            pt->ignored_alloc = false;
        } else {
            /* some outer level did the adjustment, so nop for us */
            LOG(2, "recursive post-alloc routine "PFX" %s: no adjustments; eax="PFX"\n",
                func, get_alloc_routine_name(func), mc->xax);
        }
        return;
    }
    if (pt->in_heap_routine == 0)
        client_exiting_heap_routine();

    if (is_new_routine(type)) {
        /* clear to handle placement new */
        pt->allocator = 0;
    }
    else if (is_free_routine(type)) {
        handle_free_post(drcontext, pt, wrapcxt, mc, routine);
    }
    else if (is_size_routine(type)) {
        handle_size_post(drcontext, pt, wrapcxt, mc, routine);
    }
    else if (is_malloc_routine(type)) {
        handle_malloc_post(drcontext, pt, wrapcxt, mc, false/*!realloc*/,
                           post_call, routine);
    } else if (is_realloc_routine(type)) {
        handle_realloc_post(drcontext, pt, wrapcxt, mc, post_call, routine);
    } else if (is_calloc_routine(type)) {
        handle_calloc_post(drcontext, pt, wrapcxt, mc, post_call, routine);
#ifdef WINDOWS
    } else if (type == RTL_ROUTINE_GETINFO ||
               type == RTL_ROUTINE_SETINFO ||
               type == RTL_ROUTINE_SETFLAGS) {
        handle_userinfo_post(drcontext, pt, wrapcxt);
    } else if (type == RTL_ROUTINE_CREATE) {
        handle_create_post(drcontext, pt, wrapcxt, mc);
    } else if (type == RTL_ROUTINE_DESTROY) {
        handle_destroy_post(drcontext, pt, wrapcxt);
#endif
    }
    /* b/c operators no longer have exits (i#674) we must clear here */
    pt->allocator = 0;
}

/* only used if alloc_ops.track_heap */
static void
handle_alloc_post(void *wrapcxt, void *user_data)
{
    app_pc post_call = drwrap_get_retaddr(wrapcxt);
    void *drcontext = (void *) user_data;
    cls_alloc_t *pt = (cls_alloc_t *) drmgr_get_cls_field(drcontext, cls_idx_alloc);
    dr_mcontext_t *mc = drwrap_get_mcontext_ex(wrapcxt, DR_MC_GPR);

    ASSERT(alloc_ops.track_heap, "requires track_heap");
    ASSERT(pt->in_heap_routine > 0, "shouldn't be called");

    handle_alloc_post_func(drcontext, pt, wrapcxt, mc,
                           pt->last_alloc_routine[pt->in_heap_routine], post_call,
                           (alloc_routine_entry_t *)
                           pt->last_alloc_info[pt->in_heap_routine]);

#ifdef WINDOWS
    if (pt->in_heap_routine == 0 && pt->heap_tangent) {
        IF_DEBUG(bool ok;)
        ASSERT(pt->in_heap_adjusted == 0, "inheap vs adjust mismatch");
        LOG(2, "leaving heap tangent: popping cls_alloc_t stack\n");
        IF_DEBUG(ok = )
            drmgr_pop_cls(drcontext);
        ASSERT(ok, "drmgr cls stack pop failed: tangent tracking error!");
        /* Re-enter prior context (xref i#981) */
        client_entering_heap_routine();
    }
#endif
}

bool
alloc_entering_alloc_routine(app_pc pc)
{
    return (drwrap_is_wrapped(pc, alloc_hook, handle_alloc_post) ||
            drwrap_is_wrapped(pc, alloc_hook, NULL));
}

bool
alloc_exiting_alloc_routine(app_pc pc)
{
    /* XXX: this is a post-call point for ANY drwrap wrap!
     * But it's difficult to do any better.
     * Fortunately, currently we're the only wrapper for DrMem.
     * Consequences of false positive here are extra slowpath
     * hits, not correctness, and will only happen if other
     * wrap postcalls are hit while executing new code in allocators.
     */
    return drwrap_is_post_wrap(pc);
}

static dr_emit_flags_t
alloc_event_bb_analysis(void *drcontext, void *tag, instrlist_t *bb,
                        bool for_trace, bool translating, OUT void **user_data)
{
#ifdef USE_DRSYMS
    if (alloc_ops.track_heap) {
        /* We must call this before drwrap checks for post-call instru so we
         * do it during analysis as we only need to check 1st instr anyway
         * b/c elision is turned off (if we move to insert, add "drwrap" as
         * the priority.before field)
         */
        alloc_check_pending_module(dr_fragment_app_pc(tag));
    }
#endif
    return DR_EMIT_DEFAULT;
}

static dr_emit_flags_t
alloc_event_bb_insert(void *drcontext, void *tag, instrlist_t *bb, instr_t *inst,
                      bool for_trace, bool translating, void *user_data)
{
    app_pc pc = instr_get_app_pc(inst);
    if (pc == NULL)
        return DR_EMIT_DEFAULT;
#ifdef WINDOWS
    if (instr_get_opcode(inst) == OP_int &&
        opnd_get_immed_int(instr_get_src(inst, 0)) == CBRET_INTERRUPT_NUM) {
        dr_insert_clean_call(drcontext, bb, inst, (void *)handle_cbret, false,
                             1, OPND_CREATE_INT32(0/*not syscall*/));
    }
#endif
    return DR_EMIT_DEFAULT;
}

/***************************************************************************
 * Large malloc tree
 */

/* PR 525807: to handle malloc-based stacks we need an interval tree
 * for large mallocs.  Putting all mallocs in a tree instead of a table
 * is too expensive (PR 535568).
 */

typedef struct _large_iter_data_t {
    bool (*cb)(byte *start, size_t size, void *data);
    void *data;
} large_iter_data_t;

void
malloc_large_add(byte *start, size_t size)
{
    IF_DEBUG(rb_node_t *node;)
    ASSERT(size >= LARGE_MALLOC_MIN_SIZE, "not large enough");
    dr_mutex_lock(large_malloc_lock);
    LOG(2, "large malloc add "PFX"-"PFX"\n", start, start+ size);
    IF_DEBUG(node =)
        rb_insert(large_malloc_tree, start, size, NULL);
    dr_mutex_unlock(large_malloc_lock);
    ASSERT(node == NULL, "error in large malloc tree");
    STATS_INC(num_large_mallocs);
}

void
malloc_large_remove(byte *start)
{
    rb_node_t *node;
    dr_mutex_lock(large_malloc_lock);
    LOG(2, "large malloc remove "PFX"\n", start);
    node = rb_find(large_malloc_tree, start);
    ASSERT(node != NULL, "error in large malloc tree");
    if (node != NULL)
        rb_delete(large_malloc_tree, node);
    dr_mutex_unlock(large_malloc_lock);
}

bool
malloc_large_lookup(byte *addr, byte **start OUT, size_t *size OUT)
{
    bool res = false;
    rb_node_t *node;
    dr_mutex_lock(large_malloc_lock);
    node = rb_in_node(large_malloc_tree, addr);
    if (node != NULL) {
        rb_node_fields(node, start, size, NULL);
        res = true;
    }
    dr_mutex_unlock(large_malloc_lock);
    return res;
}

static bool
malloc_large_iter_cb(rb_node_t *node, void *iter_data)
{
    large_iter_data_t *data = (large_iter_data_t *) iter_data;
    byte *start;
    size_t size;
    rb_node_fields(node, &start, &size, NULL);
    return data->cb(start, size, data->data);
}

void
malloc_large_iterate(bool (*iter_cb)(byte *start, size_t size, void *data),
                     void *iter_data)
{
    large_iter_data_t data = {iter_cb, iter_data};
    dr_mutex_lock(large_malloc_lock);
    rb_iterate(large_malloc_tree, malloc_large_iter_cb, &data);
    dr_mutex_unlock(large_malloc_lock);
}

