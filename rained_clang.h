#include "clang-c/Index.h"

typedef enum highlight_token_kind
{
    highlight_token_none,
    highlight_token_type,
    highlight_token_function,
    highlight_token_macro,
    highlight_token_comment,
    highlight_token_keyword

} highlight_token_kind;

typedef struct
{
    highlight_token_kind    kind;
    u32                     offset;
    u32                     length;

} highlight_token;

typedef struct
{
    highlight_token *tokens;
    u32             num_tokens;

} highlight_token_array;

typedef struct
{
    arena                           *arena;
    highlight_token_array           arr;
    u32                             tu_version;

} file_token_cache_entry_tokens;

typedef struct file_token_cache_entry file_token_cache_entry;
struct file_token_cache_entry
{
    // todo: store a file id or a path instead of referencing the buffer...
    rained_buffer                   *buffer;
    file_token_cache_entry_tokens   front;
    file_token_cache_entry_tokens   back;
    file_token_cache_entry          *next;
    b32                             update;
    b32                             flag;
};

typedef struct
{
    string data;
    string path;

} buffer_snapshot;

struct rained_clang_state
{
    volatile u32                        lock;

    arena                               *arena;

    b32                                 reparse;
    buffer_snapshot                     *reparse_buffers;
    u32                                 num_reparse_buffers;

    CXIndex                             index;
    CXTranslationUnit                   translation_unit;
    arena                               *token_cache_entries_arena;
    file_token_cache_entry              *token_cache;
    u32                                 tu_version;
};