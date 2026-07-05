#include "rained.h"
#include "clang-c/Index.h"

struct rained_clang_state
{
    CXIndex             index;
    CXTranslationUnit   translation_unit;

};

internal void rained_clang_parse_the_whole_thing(rained_clang_state *state)
{
    if(state->translation_unit)
    {
        clang_disposeTranslationUnit(state->translation_unit);
    }
    u32 options = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing;
    enum CXErrorCode err = clang_parseTranslationUnit2(state->index, "rained_win32.c", 0, 0,0,0, options, &state->translation_unit);
    assert(err == CXError_Success);
}

internal void rained_clang_init(rained_clang_state *state)
{
    state->index = clang_createIndex(0, 0);
    rained_clang_parse_the_whole_thing(state);
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
    *file_path = arena_push_cstring(arena, (char*)clang_getCString(cxstr));
    clang_disposeString(cxstr);
    return 1;
}

typedef struct
{
    char *str;
    enum CXCursorKind   kind;
    u32                 offset, line, column, length;

} ast_node;

typedef struct
{
    u32         start;
    u32         length;
    arena       *arena;
    ast_node    *nodes;
    u32         num_nodes;
    CXFile      file;

} gather_nodes_context;

internal enum CXChildVisitResult gather_node(CXCursor current_cursor, CXCursor parent, CXClientData client_data)
{
    gather_nodes_context *ctx = (gather_nodes_context *)client_data;
    
    CXSourceRange range = clang_getCursorExtent(current_cursor);
    CXSourceLocation range_start = clang_getRangeStart(range);
    u32 line, column, offset;
    CXFile file;
    clang_getFileLocation(range_start, &file, &line, &column, &offset);
    
    if(clang_File_isEqual(file, ctx->file))
    {
        CXSourceLocation range_end = clang_getRangeEnd(range);
        u32 end_offset = 0;

        CXFile end_file;
        clang_getFileLocation(range_end, &end_file, 0, 0, &end_offset);

        u32 length = end_offset - offset;
        enum CXCursorKind kind = clang_getCursorKind(current_cursor);
        CXString current_display_name = clang_getCursorDisplayName(current_cursor);
        char *str = clang_getCString(current_display_name);

        if(file == end_file)
        {
            ast_node *n = arena_push_struct_noalign(ctx->arena, ast_node);
            *n = (ast_node)
            {
                .str = str,
                .kind = kind,
                .offset = offset, 
                .line = line, 
                .column = column, 
                .length = length
            };        
            ctx->num_nodes++;

        }
    }

    return CXChildVisit_Recurse;
}

typedef struct
{
    ast_node    *nodes;
    u32         num_nodes;

} ast_node_array;

internal ast_node_array rained_clang_buffer_ast_nodes_from_range(rained_clang_state *state, rained_buffer *buffer, u32 position, u32 length, arena *arena)
{
    CXFile file = clang_getFile(state->translation_unit, buffer->path.p);
    /*
    CXSourceLocation loc = clang_getLocationForOffset(state->translation_unit, file, position);
    CXCursor cursor = clang_getCursor(state->translation_unit, loc);
    */

    CXCursor cursor = clang_getTranslationUnitCursor(state->translation_unit);

    gather_nodes_context *ctx = arena_push_struct(arena, gather_nodes_context);
    *ctx = (gather_nodes_context)
    {
        .start = position,
        .length = length,
        .arena = arena,
        .nodes = arena_head(arena),
        .file = file
    };

    clang_visitChildren(cursor, &gather_node, ctx);

    return (ast_node_array)
    {
        .nodes = ctx->nodes,
        .num_nodes = ctx->num_nodes
    };
}