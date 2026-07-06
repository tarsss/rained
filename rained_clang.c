#include "rained.h"
#include "clang-c/Index.h"

struct rained_clang_state
{
    CXIndex             index;
    CXTranslationUnit   translation_unit;

};

void rained_clang_test_visit_inclusion(CXFile included_file, CXSourceLocation *inclusion_stack, unsigned include_len, CXClientData client_data)
{
    char *str = clang_getCString(clang_getFileName(included_file));
}

internal void rained_clang_parse_the_whole_thing(rained_clang_state *state)
{
    if(state->translation_unit)
    {
        clang_disposeTranslationUnit(state->translation_unit);
    }
    const char *argv[] = 
    {
        "-I", "C:\\llvm\\include\\",
        "-I", "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Tools\\MSVC\\14.29.30133\\ATLMFC\\include",
        "-I", "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Tools\\MSVC\\14.29.30133\\include",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.8\\include\\um",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\ucrt",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\shared",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\um",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\winrt",
        "-I", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\cppwinr",
    };
    i32 argc = lengthof(argv);

    u32 options = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing;
    enum CXErrorCode err = clang_parseTranslationUnit2(state->index, "rained_win32.c", argv, argc,0,0, options, &state->translation_unit);

    u32 n = clang_getNumDiagnostics(state->translation_unit);
    for(u32 i = 0; i < n; i++)
    {
        CXDiagnostic diagnostic = clang_getDiagnostic(state->translation_unit, 0);
        char *spelling = clang_getCString(clang_getDiagnosticSpelling(diagnostic));
        enum CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
        char *severity_str;
        CXSourceLocation loc = clang_getDiagnosticLocation(diagnostic);
        CXFile file;
        u32 line, column;
        clang_getFileLocation(loc, &file, &line, &column, 0);
        char *file_name = clang_getCString(clang_getFileName(file));
        switch(severity)
        {
            case CXDiagnostic_Ignored: severity_str = "CXDiagnostic_Ignored"; break;
            case CXDiagnostic_Note: severity_str = "CXDiagnostic_Note"; break;
            case CXDiagnostic_Warning: severity_str = "CXDiagnostic_Warning"; break;
            case CXDiagnostic_Error: severity_str = "CXDiagnostic_Error"; break;
            case CXDiagnostic_Fatal: severity_str = "CXDiagnostic_Fatal"; break;
        }
        char buf[1024];
        stbsp_sprintf(buf, "%s \"%s\" %s(%u,%u)\n", severity_str, spelling, file_name, line, column);
        os_debug_output_string(buf);
    }

    clang_getInclusions(state->translation_unit, &rained_clang_test_visit_inclusion, 0);

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