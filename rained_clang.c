#include "rained.h"
#include "clang-c/Index.h"

struct rained_clang_state
{
    CXIndex             index;
    CXTranslationUnit   translation_unit;

};

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

internal void rained_clang_parse_the_whole_thing(rained_clang_state *state)
{
    if(state->translation_unit)
    {
        clang_disposeTranslationUnit(state->translation_unit);
    }
    enum CXErrorCode err = clang_parseTranslationUnit2(state->index, "rained_win32.c", 0, 0,0,0,CXTranslationUnit_None, &state->translation_unit);
    assert(err == CXError_Success);
}

internal void rained_clang_init(rained_clang_state *state)
{
    state->index = clang_createIndex(0, 0);
    rained_clang_parse_the_whole_thing(state);
    CXCursor cursor = clang_getTranslationUnitCursor(state->translation_unit);
    node_arena = arena_alloc(gb(1), mb(1));
    nodes = arena_head(node_arena);
    clang_visitChildren(cursor, &visit_node, 0);
}

internal b32 rained_clang_find_definition(rained_clang_state *state, rained_buffer *buffer, caret caret, u32 *position, string *file_path, arena *arena)
{
    rained_clang_parse_the_whole_thing(state);
    CXFile file = clang_getFile(state->translation_unit, buffer->path.p);
    CXSourceLocation loc = clang_getLocationForOffset(state->translation_unit, file, caret.position);
    CXCursor cursor = clang_getCursor(state->translation_unit, loc);
    CXCursor definition = clang_getCursorDefinition(cursor);
    CXSourceLocation def_loc = clang_getCursorLocation(definition);
    CXFile def_file;
    u32 def_pos;
    clang_getFileLocation(def_loc, &def_file, 0, 0, position);
    CXString cxstr = clang_getFileName(def_file);
    *file_path = arena_push_cstring(arena, clang_getCString(cxstr));
    clang_disposeString(cxstr);
    return 1;
}