#include "rained.h"
#include "rained_clang.h"

internal void rained_clang_lock(rained_clang_state *state)
{
    while(__sync_val_compare_and_swap(&state->lock, 1, 0));
}

internal void rained_clang_unlock(rained_clang_state *state)
{
    state->lock = 0;
}

void rained_clang_test_visit_inclusion(CXFile included_file, CXSourceLocation *inclusion_stack, unsigned include_len, CXClientData client_data)
{
    char *str = clang_getCString(clang_getFileName(included_file));
}

typedef struct
{
    arena       *arena;
    ast_node    *nodes;
    u32         num_nodes;

} gather_nodes_context;

internal enum CXChildVisitResult gather_node(CXCursor current_cursor, CXCursor parent, CXClientData client_data)
{
    gather_nodes_context *ctx = (gather_nodes_context *)client_data;
    
    CXSourceRange range = clang_Cursor_getSpellingNameRange(current_cursor,0,0);
    //CXSourceRange range = clang_getCursorExtent(current_cursor);
    CXSourceLocation range_start = clang_getRangeStart(range);
    u32 line, column, offset;
    CXFile file;
    clang_getFileLocation(range_start, &file, &line, &column, &offset);
    
    CXSourceLocation range_end = clang_getRangeEnd(range);
    u32 end_offset = 0;

    CXFile end_file;
    clang_getFileLocation(range_end, &end_file, 0, 0, &end_offset);

    u32 length = end_offset - offset;
    enum CXCursorKind kind = clang_getCursorKind(current_cursor);
    CXString display_name = clang_getCursorDisplayName(current_cursor);
    char *display_name_str = clang_getCString(display_name);
    CXString spelling = clang_getCursorSpelling(current_cursor);
    char *spelling_str = clang_getCString(spelling);

    if(file == end_file)
    {
        ast_node *n = arena_push_struct_noalign(ctx->arena, ast_node);
        *n = (ast_node)
        {
            .file = file,
            .display_name = display_name_str,
            .spelling = spelling_str,
            .kind = kind,
            .offset = offset, 
            .line = line, 
            .column = column, 
            .length = length
        };        
        ctx->num_nodes++;
    }

    return CXChildVisit_Recurse;
}

internal void rained_clang_schedule_reparse(rained_clang_state *state, rained_buffer *buffers)
{
    rained_clang_lock(state);
    state->reparse = 1;
    state->reparse_buffers = buffers;
    rained_clang_unlock(state);
}

internal void rained_clang_parse_the_whole_thing(rained_clang_state *state, rained_buffer *buffers)
{
    if(state->translation_unit)
    {
        clang_disposeTranslationUnit(state->translation_unit);
        arena_reset(state->nodes_arena);
        state->nodes = 0;
        state->num_nodes = 0;
        arena_reset(state->token_cache_arena);
        state->token_cache = 0;
        state->translation_unit = 0;
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

    arena *scratch = arena_alloc(gb(1), mb(1));
    
    u32 num_unsaved_files = 0;
    struct CXUnsavedFile *unsaved_files = arena_head(scratch);
    rained_buffer *buffer = buffers;
    while(buffer)
    {
        if(buffer->path.p)
        {
            struct CXUnsavedFile *f = arena_push_struct_noalign(scratch, struct CXUnsavedFile);
            *f = (struct CXUnsavedFile)
            {
                .Filename = buffer->path.p,
                .Contents = buffer->text,
                .Length = buffer->text_size,
            };
            num_unsaved_files++;   
        }
        buffer = buffer->next;
    }

    u32 options = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing;
    enum CXErrorCode err = clang_parseTranslationUnit2(state->index, "rained_win32.c", argv, argc, unsaved_files, num_unsaved_files, options, &state->translation_unit);
    assert(err == CXError_Success);

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

    PROFILE_BEGIN("clang gather nodes");
    CXCursor cursor = clang_getTranslationUnitCursor(state->translation_unit);
    gather_nodes_context *ctx = arena_push_struct(state->nodes_arena, gather_nodes_context);
    state->nodes = arena_head(state->nodes_arena);
    *ctx = (gather_nodes_context)
    {
        .arena = state->nodes_arena,
        .nodes = state->nodes,
    };
    clang_visitChildren(cursor, &gather_node, ctx);
    state->num_nodes = ctx->num_nodes;

    arena_release(scratch);

    PROFILE_END();
}

internal b32 rained_clang_find_definition(rained_clang_state *state, rained_buffer *buffer, caret caret, u32 *position, string *file_path, arena *arena)
{
    rained_clang_lock(state);
    CXFile file = clang_getFile(state->translation_unit, buffer->path.p);
    if(file)
    {
        CXSourceLocation loc = clang_getLocationForOffset(state->translation_unit, file, caret.position);
        CXCursor cursor = clang_getCursor(state->translation_unit, loc);
        CXCursor definition = clang_getCursorReferenced(cursor);
        if(!clang_Cursor_isNull(definition))
        {
            CXSourceLocation def_loc = clang_getCursorLocation(definition);
            CXFile def_file;
            u32 def_pos;
            clang_getFileLocation(def_loc, &def_file, 0, 0, position);
            CXString cxstr = clang_getFileName(def_file);
            *file_path = arena_push_cstring(arena, (char*)clang_getCString(cxstr));
            clang_disposeString(cxstr);
            rained_clang_unlock(state);
            return 1;
        }
    }
    rained_clang_unlock(state);
    return 0;
}

internal highlight_token_array rained_clang_tokens_from_range(rained_clang_state *state, rained_buffer *buffer, u32 position, u32 length, arena *arena)
{
    PROFILE_BEGIN("nodes_from_range");
    CXFile file = clang_getFile(state->translation_unit, buffer->path.p);

    highlight_token *tokens = arena_head(arena);
    u32 num_tokens = 0;

    for(u32 i = 0; i < state->num_nodes; i++)
    {
        ast_node node = state->nodes[i];
        if(clang_File_isEqual(node.file, file))
        {
            highlight_token t;
            switch(node.kind)
            {
                case CXCursor_CallExpr:
                case CXCursor_FunctionDecl:
                {
                    t.kind = highlight_token_function;
                    break;
                }
                case CXCursor_MacroDefinition:
                case CXCursor_MacroExpansion:
                case CXCursor_EnumConstantDecl:
                {
                    t.kind = highlight_token_macro;
                    break;
                }
                case CXCursor_TypeRef:
                case CXCursor_StructDecl:
                case CXCursor_TypedefDecl:
                case CXCursor_EnumDecl:
                {
                    t.kind = highlight_token_type;
                    break;
                }
                default: 
                {
                    continue;
                }
            }
            t.offset = node.offset;
            t.length = node.length;
            *((highlight_token*)arena_push_struct_noalign(arena, highlight_token)) = t;
            num_tokens++;
        }
    }
    PROFILE_END();

    PROFILE_BEGIN("clang_tokenize");
    CXSourceLocation begin = clang_getLocationForOffset(state->translation_unit, file, position);
    CXSourceLocation end = clang_getLocationForOffset(state->translation_unit, file, position + length);

    CXSourceRange range = clang_getRange(begin, end);
    CXToken *cxtokens = 0;
    u32 num_cxtokens = 0;
    clang_tokenize(state->translation_unit, range, &cxtokens, &num_cxtokens);

    for(u32 i = 0; i < num_cxtokens; i++)
    {
        CXToken cxtoken = cxtokens[i];
        CXTokenKind kind = clang_getTokenKind(cxtoken);
        highlight_token t;
        switch(kind)
        {
            case CXToken_Keyword:
            {
                t.kind = highlight_token_keyword;
                break;
            }
            case CXToken_Comment:
            {
                t.kind = highlight_token_comment;
                break;
            }
            default:
            {
                continue;
            }
        }
        CXSourceRange token_range = clang_getTokenExtent(state->translation_unit, cxtoken);
        CXSourceLocation token_start = clang_getRangeStart(token_range);
        CXSourceLocation token_end = clang_getRangeEnd(token_range);
        u32 token_start_offset, token_end_offset;
        clang_getFileLocation(token_start, 0, 0, 0, &token_start_offset);
        clang_getFileLocation(token_end, 0, 0, 0, &token_end_offset);

        t.offset = token_start_offset;
        t.length = token_end_offset - token_start_offset;
        *((highlight_token*)arena_push_struct_noalign(arena, highlight_token)) = t;
        num_tokens++;
    }
    clang_disposeTokens(state->translation_unit, cxtokens, num_cxtokens);
    PROFILE_END();

    return (highlight_token_array)
    {
        .tokens = tokens,
        .num_tokens = num_tokens
    };
}

internal highlight_token_array rained_clang_query_tokens_for_file(rained_clang_state *state, rained_buffer *buffer)
{
    rained_clang_lock(state);

    file_and_highlight_token_array *entry = state->token_cache;
    while(entry)
    {
        if(entry->buffer == buffer)
        {
            return entry->arr;
        }
        entry = entry->next;
    }

    state->cache_tokens = 1;
    state->cache_tokens_buffer = buffer;
    rained_clang_unlock(state);
    
    return (highlight_token_array)
    {
        .num_tokens = 0
    };
}

typedef struct
{
    rained_clang_state *state;

} rained_clang_thread_context;

internal void rained_clang_thread_entry_point(void *data)
{
    rained_clang_thread_context *ctx = (rained_clang_thread_context *)data;
    ctx->state->index = clang_createIndex(0, 0);
    ctx->state->nodes_arena = arena_alloc(gb(1), mb(1));
    ctx->state->token_cache_arena = arena_alloc(gb(1), mb(1));

    while(1)
    {
        rained_clang_lock(ctx->state);
        if(ctx->state->reparse)
        {
            ctx->state->reparse = 0;
            rained_clang_unlock(ctx->state);
            rained_clang_parse_the_whole_thing(ctx->state, ctx->state->reparse_buffers);
        }

        rained_clang_lock(ctx->state);
        if(ctx->state->cache_tokens)
        {
            ctx->state->cache_tokens = 0;
            file_and_highlight_token_array *new_entry = arena_push_struct(ctx->state->token_cache_arena, file_and_highlight_token_array);
            *new_entry = (file_and_highlight_token_array)
            {
                .buffer = ctx->state->cache_tokens_buffer,
                .arr = rained_clang_tokens_from_range(ctx->state, ctx->state->cache_tokens_buffer, 0, ctx->state->cache_tokens_buffer->text_size, ctx->state->token_cache_arena)
            };
            sll_push(ctx->state->token_cache, new_entry);
        }
        rained_clang_unlock(ctx->state);

    }
}
