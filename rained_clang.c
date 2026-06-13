#include "rained.h"
#include "clang-c/Index.h"

typedef struct
{
    const char          *str;
    enum CXCursorKind   kind; 

} ast_node;

arena       *node_arena;
ast_node    *nodes;
u32         num_nodes;

internal enum CXChildVisitResult visit_node(CXCursor current_cursor, CXCursor parent, CXClientData client_data)
{
    CXString current_display_name = clang_getCursorDisplayName(current_cursor);
    ast_node *n = arena_push_struct_noalign(node_arena, ast_node);
    *n = (ast_node)
    {
        .str = clang_getCString(current_display_name),
        .kind = clang_getCursorKind(current_cursor),
    };
    num_nodes++;
    return CXChildVisit_Recurse;
}

internal void clang_test()
{
    CXIndex index = clang_createIndex(0, 0);
    CXTranslationUnit unit = clang_parseTranslationUnit(index, "rained_win32.c", 0, 0,0,0,CXTranslationUnit_None);
    assert(unit);
    CXCursor cursor = clang_getTranslationUnitCursor(unit);
    node_arena = arena_alloc(gb(1), mb(1));
    nodes = arena_head(node_arena);
    clang_visitChildren(cursor, &visit_node, 0);
}