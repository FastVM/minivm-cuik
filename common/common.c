// Common is just a bunch of crap i want accessible to all projects in the Cuik repo
#include "arena.h"
#include "futex.h"
#include <stdatomic.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define NL_MAP_IMPL
#include <hash_map.h>

#define NL_HASH_SET_IMPL
#include <new_hash_map.h>

#define NBHS_IMPL
#include <nbhs.h>

#define NL_CHUNKED_ARRAY_IMPL
#include <chunked_array.h>

#define NL_BUFFER_IMPL
#include <buffer.h>

#define LOG_USE_COLOR
#include "log.c"

#include "perf.h"

uint64_t cuik__page_size = 0;
uint64_t cuik__page_mask = 0;

void cuik_init_terminal(void) {
    #if _WIN32
    // Raw input mode
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), ENABLE_PROCESSED_INPUT);

    // Enable ANSI/VT sequences on windows
    HANDLE output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output_handle != INVALID_HANDLE_VALUE) {
        DWORD old_mode;
        if (GetConsoleMode(output_handle, &old_mode)) {
            SetConsoleMode(output_handle, old_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }
    #endif
}

void* cuik__valloc(size_t size) {
    #ifdef _WIN32
    if (cuik__page_size == 0) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);

        cuik__page_size = si.dwPageSize;
        cuik__page_mask = si.dwPageSize - 1;
    }

    // round size to page size
    size = (size + cuik__page_mask) & ~cuik__page_mask;

    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    #else
    cuik__page_size = 4096;
    cuik__page_mask = 4095;

    return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    #endif
}

void cuik__vfree(void* ptr, size_t size) {
    #ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE);
    #else
    munmap(ptr, size);
    #endif
}

////////////////////////////////
// TB_Arenas
////////////////////////////////
TB_Arena* tb_arena_create(size_t chunk_size) {
    if (chunk_size == 0) {
        chunk_size = TB_ARENA_LARGE_CHUNK_SIZE;
    }

    // allocate initial chunk
    TB_Arena* arena = cuik__valloc(chunk_size);
    arena->avail = arena->data;
    arena->limit = &arena->data[chunk_size - sizeof(TB_Arena)];
    #ifndef NDEBUG
    arena->highest = arena->avail;
    #endif
    arena->top = arena;
    return arena;
}

void tb_arena_destroy(TB_Arena* restrict arena) {
    TB_Arena* c = arena;
    while (c != NULL) {
        TB_Arena* next = c->next;
        cuik__vfree(c, tb_arena_chunk_size(c));
        c = next;
    }
}

void* tb_arena_unaligned_alloc(TB_Arena* restrict arena, size_t size) {
    TB_Arena* top = arena->top;

    char* p = top->avail;
    if (LIKELY((top->avail + size) <= top->limit)) {
        top->avail += size;

        #ifndef NDEBUG
        if (p > top->highest) {
            top->highest = p;
        }
        #endif
        return p;
    } else {
        // slow path, we need to allocate more pages
        size_t chunk_size = tb_arena_chunk_size(arena);

        TB_Arena* c = top->next;
        if (c != NULL) {
            assert(tb_arena_chunk_size(arena));
            c->avail   = c->data + size;
            #ifndef NDEBUG
            c->highest = c->avail;
            #endif
        } else {
            assert(size < chunk_size - sizeof(TB_Arena));
            c = cuik__valloc(chunk_size);
            c->next    = NULL;
            c->avail   = c->data + size;
            c->limit   = &c->data[chunk_size - sizeof(TB_Arena)];
            #ifndef NDEBUG
            c->highest = c->avail;
            #endif

            // append to top
            arena->top->next = c;
        }

        arena->top = c;
        return c->data;
    }
}

TB_API void* tb_arena_realloc(TB_Arena* restrict arena, void* old, size_t old_size, size_t size) {
    old_size = (old_size + TB_ARENA_ALIGNMENT - 1) & ~(TB_ARENA_ALIGNMENT - 1);
    size = (size + TB_ARENA_ALIGNMENT - 1) & ~(TB_ARENA_ALIGNMENT - 1);

    char* p = old;
    if (p + old_size == arena->avail) {
        // try to resize
        arena->avail = old;
    }

    char* dst = tb_arena_unaligned_alloc(arena, size);
    if (dst != p && old) {
        memcpy(dst, old, size);
    }
    return dst;
}

void tb_arena_pop(TB_Arena* restrict arena, void* ptr, size_t size) {
    char* p = ptr;
    assert(p + size == arena->avail); // cannot pop from arena if it's not at the top

    arena->avail = p;
}

bool tb_arena_free(TB_Arena* restrict arena, void* ptr, size_t size) {
    size = (size + TB_ARENA_ALIGNMENT - 1) & ~(TB_ARENA_ALIGNMENT - 1);

    char* p = ptr;
    if (p + size == arena->avail) {
        arena->avail = p;
        return true;
    } else {
        return false;
    }
}

void tb_arena_realign(TB_Arena* restrict arena) {
    ptrdiff_t pos = arena->avail - arena->top->data;
    pos = (pos + TB_ARENA_ALIGNMENT - 1) & ~(TB_ARENA_ALIGNMENT - 1);

    arena->avail = &arena->top->data[pos];
}

TB_ArenaSavepoint tb_arena_save(TB_Arena* arena) {
    return (TB_ArenaSavepoint){ arena->top, arena->avail };
}

void tb_arena_restore(TB_Arena* arena, TB_ArenaSavepoint sp) {
    arena->top = sp.top;
    arena->avail = sp.avail;
}

void* tb_arena_alloc(TB_Arena* restrict arena, size_t size) {
    uintptr_t wm = (uintptr_t) arena->avail;
    assert((wm & ~0xFull) == wm);

    size = (size + TB_ARENA_ALIGNMENT - 1) & ~(TB_ARENA_ALIGNMENT - 1);
    return tb_arena_unaligned_alloc(arena, size);
}

void tb_arena_clear(TB_Arena* arena) {
    if (arena != NULL) {
        arena->avail = &arena->data[0];
        arena->top = arena;
    }
}

bool tb_arena_is_empty(TB_Arena* arena) {
    return arena->top != arena || (arena->avail - arena->data) > 0;
}

#ifndef NDEBUG
void tb_arena_reset_peak(TB_Arena* restrict arena) {
    TB_Arena* top_next = arena->top->next;
    for (TB_Arena* c = arena; c != top_next; c = c->next) {
        c->highest = c->avail;
    }
}

size_t tb_arena_peak_size(TB_Arena* arena) {
    size_t total = 0;
    TB_Arena* top_next = arena->top->next;
    for (TB_Arena* c = arena; c != top_next; c = c->next) {
        total += c->highest - (char*) c;
    }
    return total;
}
#endif

size_t tb_arena_chunk_size(TB_Arena* arena) {
    return arena->limit - (char*) arena;
}

size_t tb_arena_current_size(TB_Arena* arena) {
    size_t total = 0;
    TB_Arena* top_next = arena->top->next;
    for (TB_Arena* c = arena; c != top_next; c = c->next) {
        total += c->avail - (char*) c;
    }
    return total;
}

////////////////////////////////
// Futex functions
////////////////////////////////
#if !defined(__FreeBSD__)
void futex_dec(Futex* f) {
    if (atomic_fetch_sub(f, 1) == 1) {
        futex_signal(f);
    }
}
#endif

#ifdef __linux__
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

// [https://man7.org/linux/man-pages/man2/futex.2.html]
//   glibc provides no wrapper for futex()
#define futex(...) syscall(SYS_futex, __VA_ARGS__)

void futex_signal(Futex* addr) {
    int ret = futex(addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, NULL, 0);
    if (ret == -1) {
        __builtin_trap();
    }
}

void futex_broadcast(Futex* addr) {
    int ret = futex(addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT32_MAX, NULL, NULL, 0);
    if (ret == -1) {
        __builtin_trap();
    }
}

void futex_wait(Futex* addr, Futex val) {
    for (;;) {
        int ret = futex(addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, val, NULL, NULL, 0);

        if (ret == -1) {
            if (errno != EAGAIN) {
                __builtin_trap();
            }
        } else if (ret == 0) {
            if (*addr != val) {
                return;
            }
        }
    }
}

#undef futex
#elif defined(__APPLE__)

#include <errno.h>

enum {
    UL_COMPARE_AND_WAIT = 0x00000001,
    ULF_WAKE_ALL        = 0x00000100,
    ULF_NO_ERRNO        = 0x01000000
};

/* timeout is specified in microseconds */
int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout);
int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

void futex_signal(Futex* addr) {
    for (;;) {
        int ret = __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, addr, 0);
        if (ret >= 0) {
            return;
        }
        ret = -ret;
        if (ret == EINTR || ret == EFAULT) {
            continue;
        }
        if (ret == ENOENT) {
            return;
        }
        printf("futex wake fail?\n");
        __builtin_trap();
    }
}

void _tpool_broadcast(Futex* addr) {
    for (;;) {
        int ret = __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | ULF_WAKE_ALL, addr, 0);
        if (ret >= 0) {
            return;
        }
        ret = -ret;
        if (ret == EINTR || ret == EFAULT) {
            continue;
        }
        if (ret == ENOENT) {
            return;
        }
        printf("futex wake fail?\n");
        __builtin_trap();
    }
}

void futex_wait(Futex* addr, Futex val) {
    for (;;) {
        int ret = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, addr, val, 0);
        if (ret >= 0) {
            if (*addr != val) {
                return;
            }
            continue;
        }
        ret = -ret;
        if (ret == EINTR || ret == EFAULT) {
            continue;
        }
        if (ret == ENOENT) {
            return;
        }

        printf("futex wait fail?\n");
    }
}

#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef __GNUC__
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")
#endif

void futex_signal(Futex* addr) {
    WakeByAddressSingle((void*) addr);
}

void futex_broadcast(Futex* addr) {
    WakeByAddressAll((void*) addr);
}

void futex_wait(Futex* addr, Futex val) {
    for (;;) {
        WaitOnAddress(addr, (void *)&val, sizeof(val), INFINITE);
        if (*addr != val) break;
    }
}
#endif

void futex_wait_eq(Futex* addr, Futex val) {
    while (*addr != val) {
        futex_wait(addr, *addr);
    }
}
