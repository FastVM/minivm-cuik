#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifndef TB_API
#  ifdef __cplusplus
#    define TB_EXTERN extern "C"
#  else
#    define TB_EXTERN
#  endif
#  ifdef TB_DLL
#    ifdef TB_IMPORT_DLL
#      define TB_API TB_EXTERN __declspec(dllimport)
#    else
#      define TB_API TB_EXTERN __declspec(dllexport)
#    endif
#  else
#    define TB_API TB_EXTERN
#  endif
#endif

enum {
    TB_ARENA_SMALL_CHUNK_SIZE  =         4 * 1024,
    TB_ARENA_MEDIUM_CHUNK_SIZE =        64 * 1024,
    TB_ARENA_LARGE_CHUNK_SIZE  =       256 * 1024,

    TB_ARENA_ALIGNMENT = 16,
};

typedef struct TB_Arena TB_Arena;
struct TB_Arena {
    TB_Arena* next;

    // we only care about this for the root arena chunk but whatever
    TB_Arena* top;

    char* avail;
    char* limit;

    #ifndef NDEBUG
    char* highest;
    char* _pad;
    #endif

    char data[];
};

typedef struct TB_ArenaSavepoint {
    TB_Arena* top;
    char* avail;
} TB_ArenaSavepoint;

#define TB_ARENA_FOR(it, arena) for (TB_Arena* it = (arena); it != NULL; it = it->next)

#define TB_ARENA_ALLOC(arena, T) tb_arena_alloc(arena, sizeof(T))
#define TB_ARENA_ARR_ALLOC(arena, count, T) tb_arena_alloc(arena, (count) * sizeof(T))

TB_API TB_Arena* tb_arena_create(size_t chunk_size);
TB_API void tb_arena_destroy(TB_Arena* restrict arena);

TB_API void* tb_arena_unaligned_alloc(TB_Arena* restrict arena, size_t size);
TB_API void* tb_arena_alloc(TB_Arena* restrict arena, size_t size);

TB_API void* tb_arena_realloc(TB_Arena* restrict arena, void* old, size_t old_size, size_t size);

// return false on failure
TB_API bool tb_arena_free(TB_Arena* restrict arena, void* ptr, size_t size);
TB_API void tb_arena_pop(TB_Arena* restrict arena, void* ptr, size_t size);

// in case you wanna mix unaligned and aligned arenas
TB_API void tb_arena_realign(TB_Arena* restrict arena);

#ifndef NDEBUG
TB_API void tb_arena_reset_peak(TB_Arena* restrict arena);
TB_API size_t tb_arena_peak_size(TB_Arena* arena);
#endif

TB_API size_t tb_arena_chunk_size(TB_Arena* arena);
TB_API size_t tb_arena_current_size(TB_Arena* arena);

// savepoints
TB_API TB_ArenaSavepoint tb_arena_save(TB_Arena* arena);
TB_API void tb_arena_restore(TB_Arena* arena, TB_ArenaSavepoint sp);

// resets to only having one chunk
TB_API void tb_arena_clear(TB_Arena* arena);
