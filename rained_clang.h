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

typedef struct file_and_highlight_token_array file_and_highlight_token_array;
struct file_and_highlight_token_array
{
    CXFile                          file;
    highlight_token_array           arr;
    file_and_highlight_token_array  *next;
};

typedef struct
{
    CXFile              file;
    enum CXCursorKind   kind;
    char                *display_name;
    char                *spelling;
    u32                 offset, line, column, length;

} ast_node;

struct rained_clang_state
{
    CXIndex                             index;
    CXTranslationUnit                   translation_unit;
    arena                               *nodes_arena;
    ast_node                            *nodes;
    u32                                 num_nodes;
    arena                               *token_cache_arena;
    file_and_highlight_token_array      *token_cache;
};
