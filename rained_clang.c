#include "rained.h"
#include "rained_clang.h"

volatile u32 clang_state_lock;

internal void rained_clang_lock()
{
    while(__sync_val_compare_and_swap(&clang_state_lock, 0, 1));
}

internal void rained_clang_unlock()
{
    clang_state_lock = 0;
}

void rained_clang_test_visit_inclusion(CXFile included_file, CXSourceLocation *inclusion_stack, unsigned include_len, CXClientData client_data)
{
    char *str = clang_getCString(clang_getFileName(included_file));
}

internal buffer_snapshot push_buffer_snapshot(rained_buffer *buffer, arena *arena)
{
    buffer_snapshot res = { 0 };
    res.data = arena_push_string_terminate(arena, buffer->text_string);
    if(buffer->path.p)
    {
        res.path = arena_push_string_terminate(arena, buffer->path);
    }
    return res;
}

internal b32 rained_clang_schedule_reparse(rained_clang_state *state, rained_buffer *buffers)
{
    rained_clang_lock();
    if(state->reparse)
    {
        rained_clang_unlock();
        return 0;
    }
    state->reparse = 1;
    
    state->num_reparse_buffers = 0;
    rained_buffer *b = buffers;
    while(b)
    {
        state->num_reparse_buffers++;
        b->schedule_edits = 0;
        b = b->next;
    }

    state->reparse_buffers = arena_push(state->arena, sizeof(buffer_snapshot) * state->num_reparse_buffers, 64);

    b = buffers;
    u32 i = 0;
    while(b)
    {
        state->reparse_buffers[i] = push_buffer_snapshot(b, state->arena);
        b = b->next;
        i++;
    }
    
    rained_clang_unlock();
    return 1;
}

internal void rained_clang_parse_the_whole_thing(rained_clang_state *state, buffer_snapshot *buffers, u32 num_buffers)
{
    PROFILE_BEGIN("clang parse");

    const char *argv[] = 
    {
        "-isystem", "C:\\llvm\\include\\",
        "-isystem", "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Tools\\MSVC\\14.29.30133\\ATLMFC\\include",
        "-isystem", "C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Tools\\MSVC\\14.29.30133\\include",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\NETFXSDK\\4.8\\include\\um",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\ucrt",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\shared",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\um",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\winrt",
        "-isystem", "C:\\Program Files (x86)\\Windows Kits\\10\\include\\10.0.22621.0\\cppwinr"
    };
    i32 argc = lengthof(argv);

    arena *scratch = arena_alloc(gb(1), mb(1));
    
    u32 num_unsaved_files = 0;
    struct CXUnsavedFile *unsaved_files = arena_head(scratch);
#if 1

    for(u32 i = 0; i < num_buffers; i++)
    {
        buffer_snapshot buffer = buffers[i];
        if(buffer.path.p)
        {
            struct CXUnsavedFile *f = arena_push_struct_noalign(scratch, struct CXUnsavedFile);
            *f = (struct CXUnsavedFile)
            {
                .Filename = buffer.path.p,
                .Contents = buffer.data.p,
                .Length = buffer.data.length,
            };
            num_unsaved_files++;
        }
    }
#endif

    u32 options = 0;
    options |= CXTranslationUnit_DetailedPreprocessingRecord;
    options |= CXTranslationUnit_KeepGoing;
    options |= CXTranslationUnit_CreatePreambleOnFirstParse;
    options |= CXTranslationUnit_RetainExcludedConditionalBlocks;

    /* NOTE

    reparsing w/o windows headers: ~50ms
    reparsing with windows headers: ~2.5 seconds
    reparsing with windows headers and CXTranslationUnit_PrecompiledPreamble: ~120ms
    
    preamble thing helps a lot, obviously... as long as the preamble isn't changing. and the way includes are structured right now, i.e first include the rained.c and then all of the windows stuff, whenever rained.c gets changed, it does a full parse from scratch, and we're down to 2.5 seconds again...

    one option is to cope with being mindfull about the order of includes and whether or not you're editing something upstream of windows headers, and it will be fast. another option is to look into precompiling headers manually...

    also, there's a bug when e.g

    #include "windows.h"
    #include "test.c"

    triggers a windows header reparse even though it's upstream. but...

    #include "windows.h"
    anything e.g ;
    #include "test.c"

    it does the reparse correctly. fucking clang. fuck you. fucking piece of shit
    */
   
    options |= CXTranslationUnit_PrecompiledPreamble;

    if(state->translation_unit)
    {
        PROFILE_BEGIN("clang_reparseTranslationUnit");
        u32 err = clang_reparseTranslationUnit(state->translation_unit, num_unsaved_files, unsaved_files, clang_defaultReparseOptions(state->translation_unit));
        assert(err == 0);
        PROFILE_END();
    }
    else
    {
        PROFILE_BEGIN("clang_parseTranslationUnit2");
        enum CXErrorCode err = clang_parseTranslationUnit2(state->index, "rained_win32.c", argv, argc, unsaved_files, num_unsaved_files, options, &state->translation_unit);
        assert(err == CXError_Success);
        PROFILE_END();
    }

#ifdef RAINED_CLANG_DEBUG
    PROFILE_BEGIN("diagnostics");
    CXDiagnosticSet ds = clang_getDiagnosticSetFromTU(state->translation_unit);
    u32 n = clang_getNumDiagnosticsInSet(ds);
    for(u32 i = 0; i < n; i++)
    {
        CXDiagnostic diagnostic = clang_getDiagnosticInSet(ds, i);
        //CXDiagnostic diagnostic = clang_getDiagnostic(state->translation_unit, 0);
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
    PROFILE_END();

    PROFILE_BEGIN("inclusions");
    clang_getInclusions(state->translation_unit, &rained_clang_test_visit_inclusion, 0);
    PROFILE_END();

    CXString str = clang_getTranslationUnitSpelling(state->translation_unit);
    os_debug_output_string(clang_getCString(str));
#endif

    arena_release(scratch);

    PROFILE_END();
    PROFILE_END();
}

internal b32 rained_clang_find_definition(rained_clang_state *state, rained_buffer *buffer, caret caret, u32 *position, string *file_path, arena *arena)
{
    rained_clang_lock();
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
            rained_clang_unlock();
            return 1;
        }
    }
    rained_clang_unlock();
    return 0;
}

internal highlight_token_array rained_clang_tokens_from_file(rained_clang_state *state, string path, arena *arena)
{
    CXFile file = clang_getFile(state->translation_unit, path.p);

    u64 file_size = 0;
    clang_getFileContents(state->translation_unit, file, &file_size);

    PROFILE_BEGIN("clang_tokenize");
    CXSourceLocation begin = clang_getLocationForOffset(state->translation_unit, file, 0);
    CXSourceLocation end = clang_getLocationForOffset(state->translation_unit, file, file_size);

    CXSourceRange range = clang_getRange(begin, end);
    CXToken *cxtokens = 0;
    u32 num_cxtokens = 0;
    clang_tokenize(state->translation_unit, range, &cxtokens, &num_cxtokens);
    
    PROFILE_BEGIN("annotate tokens");
    arena_t *scratch = arena_alloc(gb(1), mb(1)); // todo
    CXCursor *cxcursors = arena_push(scratch, sizeof(CXCursor) * num_cxtokens, 64);
    clang_annotateTokens(state->translation_unit, cxtokens, num_cxtokens, cxcursors);
    PROFILE_END();

    highlight_token *tokens = arena_head(arena);
    u32 num_tokens = 0;

    for(u32 i = 0; i < num_cxtokens; i++)
    {
        CXToken cxtoken = cxtokens[i];
        CXTokenKind kind = clang_getTokenKind(cxtoken);
        highlight_token t;
        t.kind = 0;

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
            case CXToken_Identifier:
            {
                CXCursor cursor = cxcursors[i];
                enum CXCursorKind cursor_kind = clang_getCursorKind(cursor);
                switch(cursor_kind)
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
                        break;
                    }
                }
                break;
            }
            default:
            {
                break;
            }
        }

        if(t.kind)
        {
            CXSourceRange token_range = clang_getTokenExtent(state->translation_unit, cxtoken);
            CXSourceLocation token_start = clang_getRangeStart(token_range);
            CXSourceLocation token_end = clang_getRangeEnd(token_range);
            u32 token_start_offset, token_end_offset;
            clang_getFileLocation(token_start, 0, 0, 0, &token_start_offset);
            clang_getFileLocation(token_end, 0, 0, 0, &token_end_offset);
    
            t.offset = token_start_offset;
    
            if(num_tokens)
            {
                assert(tokens[num_tokens - 1].offset <= t.offset);
            }
    
            t.length = token_end_offset - token_start_offset;
            *((highlight_token*)arena_push_struct_noalign(arena, highlight_token)) = t;
            num_tokens++;
        }

    }

    clang_disposeTokens(state->translation_unit, cxtokens, num_cxtokens);
    PROFILE_END();
    arena_release(scratch);
    return (highlight_token_array)
    {
        .tokens = tokens,
        .num_tokens = num_tokens
    };
}

internal highlight_token_array rained_clang_query_tokens_for_file(rained_clang_state *state, rained_buffer *buffer)
{
    PROFILE_BEGIN("query tokens");
    PROFILE_BEGIN("lock");
    rained_clang_lock();
    PROFILE_END();
    file_token_cache_entry *entry = state->token_cache;
    while(entry)
    {
        if(entry->buffer == buffer)
        {
            if(entry->back.tu_version > entry->front.tu_version)
            {
                file_token_cache_entry_tokens temp = entry->front;
                entry->front = entry->back;
                entry->back = temp;
            }
            if(entry->front.tu_version != state->tu_version)
            {
                entry->update = 1;
            }
            if(entry->flag)
            {
                entry->flag = 0;
                buffer->patch_edits = buffer->schedule_edits;
            }
            rained_clang_unlock();
            PROFILE_END();
            return entry->front.arr;
        }
        entry = entry->next;
    }

    if(!state->token_cache_entries_arena)
    {
        state->token_cache_entries_arena = arena_alloc(gb(1), kb(4));
    }

    file_token_cache_entry *new_entry = arena_push_struct(state->token_cache_entries_arena, file_token_cache_entry);
    *new_entry = (file_token_cache_entry)
    {
        .front = 
        {
            .arena = arena_alloc(gb(1), mb(1)),
        },
        .back = 
        {
            .arena = arena_alloc(gb(1), mb(1)),
        },
        .buffer = buffer,
        .update = 1,
    };
    sll_push(state->token_cache, new_entry);

    rained_clang_unlock();
    
    PROFILE_END();
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

    rained_clang_lock();
    ctx->state->index = clang_createIndex(0, 0);
    rained_clang_unlock();

    while(1)
    {
        rained_clang_lock();
            
        if(ctx->state->reparse)
        {
            rained_clang_unlock();
            rained_clang_parse_the_whole_thing(ctx->state, ctx->state->reparse_buffers, ctx->state->num_reparse_buffers);
            rained_clang_lock();

            ctx->state->reparse = 0;
            ctx->state->tu_version++;
        }

        file_token_cache_entry *entry = ctx->state->token_cache;
        while(entry)
        {
            if(entry->update)
            {
                rained_clang_unlock();
                arena_reset(entry->back.arena);
                highlight_token_array tokens = rained_clang_tokens_from_file(ctx->state, entry->buffer->path, entry->back.arena);
                rained_clang_lock();
                entry->back.arr = tokens;
                entry->update = 0;
                entry->flag = 1;
                entry->back.tu_version = ctx->state->tu_version;
            }
            entry = entry->next;
        }

        arena_reset(ctx->state->arena);

        rained_clang_unlock();

#ifdef SPALL_ENABLED
        // note: on exit we're just pulling the rug on our threads, and i'm not sure what to do about this right now. thus we have to flush explicitly.
        spall_buffer_flush(&spall_profile, &rained_get_thread_context()->spall_buffer);
#endif
    }
}
