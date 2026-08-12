#include "rained.h"

typedef struct rained_clang_state rained_clang_state;

typedef struct
{
    arena               *frame_arena;
    arena               *forever_arena;
    rained_buffer       *buffers;
    rained_tile         *tile_left;
    rained_tile         *tile_right;
    rained_tile         *focused_tile;
    rained_tile         *mouse_drag_tile;
    rained_clang_state  *clang_state;
    u32                 font_size;
    font_atlas          font;
    b32                 reparse_pending;

} rained_state;

rained_state global_state;

#include "rained_clang.c"

internal u32 caret_get_column(rained_view *view, caret *caret)
{
    u32 start = find_line_start(view->buffer, caret->position);
    chopped_line_list list = chop_lines(view->buffer, view->width_cells, start, -1, 1, global_state.frame_arena);
    chopped_line l = list.lines[chopped_line_list_find_position(list, caret->position)];
    return caret->position - l.pos_in_text;
}

// todo: we probably don't want overlapping selection ranges, so merge by ranges too?
internal void merge_overlapping_carets_in_a_slow_way(rained_view *view)
{
    u32 n = 0;
    for(u32 i = 0; i < view->num_carets; i++)
    {
        b32 overlapped = 0;
        for(i32 j = n; j >= 0; j--)
        {
            if(i != j && view->carets[i].position == view->carets[j].position)
            {
                overlapped = 1;
                break;
            }
        }
        if(!overlapped)
        {
            view->carets[n] = view->carets[i];
            n++;
        }
    }  
    view->num_carets = n;
}

internal void caret_move_right(rained_view *view, caret *caret)
{
    u32 p = caret->position;
    while(caret->position != view->buffer->text_size)
    {
        char c = view->buffer->text[caret->position];
        caret->position++;
        if(c == '\n')
        {
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret->wish_column = caret_get_column(view, caret);
}

internal void caret_move_left(rained_view *view, caret *caret)
{
    while(caret->position)
    {
        caret->position--;
        char c = view->buffer->text[caret->position];
        if(c == '\r')
        {
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret->wish_column = caret_get_column(view, caret);
}

typedef enum
{
    character_jump_point,
    character_whitespace,
    character_newline,
    character_other

} character_kind;

internal character_kind get_character_kind(c8 c)
{
    switch(c)
    {
        case '`':
        case '~':
        case '!':
        case '@':
        case '#':
        case '$':
        case '%':
        case '^':
        case '&':
        case '*':
        case '(':            
        case ')':
        case '-':
        case '=':
        case '+':
        case '[':
        case '{':
        case ']':
        case '}':
        case '\\':
        case '|':
        case ';':
        case ':':
        case '\'':
        case '"':
        case ',':
        case '.':
        case '<':
        case '>':
        case '/':
        case '?':
        {
            return character_jump_point;
        }
        case ' ':
        {
            return character_whitespace;
        }
        case '\r':
        case '\n':
        {
            return character_newline;
        }
        default:
        {
            return character_other;
        }
    }
}

internal u32 caret_next_token(rained_view *view, caret *caret)
{
    u32 pos = caret->position;
    character_kind first = get_character_kind(view->buffer->text[pos]);
    character_kind prev = first;
    character_kind cur;
    while(pos != view->buffer->text_size)
    {
        pos++;
        cur = get_character_kind(view->buffer->text[pos]);
        if(prev != cur)
        {
            if(first == character_whitespace)
            {
                break;
            }
            else
            {
                if(cur != character_whitespace || cur == character_newline)
                {
                    break;
                }
            }
        }
        prev = cur;
    }
    return pos;
}

internal u32 caret_prev_token(rained_view *view, caret *caret)
{
    u32 pos = caret->position;
    if(pos)
    {
        pos--;
        character_kind first = get_character_kind(view->buffer->text[pos]);
        character_kind cur = first;
        character_kind next;
        while(pos)
        {        
            next = get_character_kind(view->buffer->text[pos - 1]);
    
            if(next != cur)
            {
                if(first == character_whitespace)
                {
                    if(cur != character_whitespace)
                    {
                        break;
                    }
                }
                else
                {
                    break;
                }
            }
            cur = next;
            pos--;
        }
    }
    return pos;
}

internal void caret_jump_right(rained_view *view, caret *caret)
{    
    caret->position = caret_next_token(view, caret);
    caret->wish_column = caret_get_column(view, caret);
}

internal void caret_jump_left(rained_view *view, caret *caret)
{
    caret->position = caret_prev_token(view, caret);
    caret->wish_column = caret_get_column(view, caret);
}

internal void caret_move_to_prev_line(rained_buffer *buffer, caret *caret)
{
    while(caret->position)
    {
        if(buffer->text[caret->position] == '\n')
        {
            caret->position--;
            break;
        }
        caret->position--;
    }
}

internal void caret_move_to_line_start(rained_buffer *buffer, caret *caret)
{
    while(1)
    {
        if(caret->position == 0 || buffer->text[caret->position - 1] == '\n')
        {
            break;
        }
        caret->position--;
    }
}

internal void caret_go_to_column(rained_view *view, caret *caret, u32 col)
{
    caret_move_to_line_start(view->buffer, caret);
    u32 c = 0;
    while(c != col && caret->position != view->buffer->text_size)
    {
        if(view->buffer->text[caret->position] == '\r')
        {
            break;
        }
        caret->position++;
        c++;
    }
}

internal void caret_move_to_next_line(rained_buffer *buffer, caret *caret)
{
    while(caret->position != buffer->text_size)
    {
        if(buffer->text[caret->position] == '\n')
        {
            caret->position++;
            break;
        }
        caret->position++;
    }
}

internal void caret_move_up(rained_view *view, caret *caret)
{
    caret_move_to_prev_line(view->buffer, caret);
    caret_go_to_column(view, caret, caret->wish_column);
}

internal void caret_move_down(rained_view *view, caret *caret)
{
    caret_move_to_next_line(view->buffer, caret);
    caret_go_to_column(view, caret, caret->wish_column);
}

internal void carets_bubble_sort_top_to_bottom(rained_view *view)
{
    for(;;)
    {
        u32 num_swaps = 0;
        for(u32 i = 0; i < view->num_carets - 1; i++)
        {
            caret a = view->carets[i];
            caret b = view->carets[i + 1];
            if(a.position > b.position)
            {
                view->carets[i] = b;
                view->carets[i + 1] = a;
                num_swaps++; 
            }
        }
        if(num_swaps == 0)
        {
            break;
        }
    }
}

internal void buffer_apply_edit(rained_buffer *buffer, text_edit *edit)
{
    if(edit->additive)
    {
        buffer->text_arena->used += edit->string.length;
        arena_commit_to_fit_used(buffer->text_arena);
        for(u32 i = buffer->text_size + edit->string.length - 1; i >= edit->position + edit->string.length; i--)
        {
            buffer->text[i] = buffer->text[i - edit->string.length];
        }
        for(u32 i = 0; i < edit->string.length; i++)
        {
            buffer->text[edit->position + i] = edit->string.p[i];
        }
        buffer->text_size += edit->string.length;
    }
    else
    {
        for(u32 i = edit->position; i < buffer->text_size - edit->string.length; i++)
        {
            buffer->text[i] = buffer->text[i + edit->string.length];
        }
        buffer->text_size -= edit->string.length;
        buffer->text_arena->used -= edit->string.length;   
    }
}

internal void buffer_push_edit(rained_buffer *buffer, text_edit *entry)
{
    dll_push(buffer->edits, entry);
    if(!buffer->patch_edits)
    {
        buffer->patch_edits = entry;
    }
    if(!buffer->schedule_edits)
    {
        buffer->schedule_edits = entry;
    }
}

internal text_edit *buffer_push_insert_edit(rained_buffer *buffer, u32 position, string insert)
{
    text_edit *e = arena_push_struct(buffer->arena1, text_edit);
    *e = (text_edit)
    {
        .additive = 1,
        .position = position
    };
    e->string.length = insert.length;
    e->string.p = arena_copy(buffer->arena1, insert.p, insert.length);
    buffer_push_edit(buffer, e);
    return e;
}

internal text_edit *buffer_push_delete_edit(rained_buffer *buffer, u32 position, u32 length)
{
    text_edit *e = arena_push_struct(buffer->arena1, text_edit);
    *e = (text_edit)
    {
        .additive = 0,
        .position = position
    };
    e->string.length = length;
    e->string.p = arena_copy(buffer->arena1, buffer->text + position, length);
    buffer_push_edit(buffer, e);
    return e;
}

internal void carets_insert_string(rained_view *view, string insert)
{
    carets_bubble_sort_top_to_bottom(view);
    u32 offset = 0;
    for(u32 c = 0; c < view->num_carets; c++)
    {
        caret *caret = &view->carets[c];
        if(insert.length)
        {
            caret->position += offset;
            offset += insert.length;
            text_edit *e = buffer_push_insert_edit(view->buffer, caret->position, insert);
            buffer_apply_edit(view->buffer, e);
            caret->position += insert.length;
            caret->wish_column = caret_get_column(view, caret);
        }
    }
    view->fit_caret = 1;
    view->buffer->is_dirty = 1;
}

internal void carets_delete_length(rained_view *view, u32 *lengths)
{
    carets_bubble_sort_top_to_bottom(view);
    u32 offset = 0;
    for(u32 c = 0; c < view->num_carets; c++)
    {
        caret *caret = &view->carets[c];
        caret->position -= offset;
        u32 num_to_remove = min(lengths[c], caret->position);
        
        // dont leave a stray /r fuhhhh.
        if(caret->position >= num_to_remove + 1)
        {
            if(view->buffer->text[caret->position - num_to_remove] == '\n' && 
                view->buffer->text[caret->position - num_to_remove - 1] == '\r')
            {
                num_to_remove++;
            }
        }

        offset += num_to_remove;
        caret->position -= num_to_remove;
        text_edit *e = buffer_push_delete_edit(view->buffer, caret->position, num_to_remove);
        buffer_apply_edit(view->buffer, e);
        caret->wish_column = caret_get_column(view, caret);
    }
    view->fit_caret = 1;
    view->buffer->is_dirty = 1;
}

internal void caret_spawn_new_below(rained_view *view)
{
    caret bottom_caret = view->carets[0];
    for(u32 i = 0; i < view->num_carets; i++)
    {
        if(view->carets[i].position > bottom_caret.position)
        {
            bottom_caret = view->carets[i];
        }
    }
    view->carets[view->num_carets] = bottom_caret;
    caret_move_down(view, &view->carets[view->num_carets]);
    view->num_carets++;
}

internal void caret_spawn_new_above(rained_view *view)
{
    caret top_caret = view->carets[0];
    for(u32 i = 0; i < view->num_carets; i++)
    {
        if(view->carets[i].position < top_caret.position)
        {
            top_caret = view->carets[i];
        }
    }
    view->carets[view->num_carets] = top_caret;
    caret_move_up(view, &view->carets[view->num_carets]);
    view->num_carets++;
}

internal void undo(rained_view *view)
{
    /*
    copy carets before
    for each entry...
    entry.additive = !entry.additive
    apply_edit
    */
}

internal void redo(rained_view *view)
{
    /*
    copy carets after
    for each entry...
    apply_edit
    */
}

internal rained_buffer *open_empty_buffer(rained_state *state)
{
    arena *text_arena = arena_alloc(gb(1), 0);
    rained_buffer *buffer = arena_push_struct_zero(state->forever_arena, rained_buffer);
    *buffer = (rained_buffer)
    {
        .text = arena_head(text_arena),
        .text_arena = text_arena,
        .arena1 = arena_alloc(gb(1), mb(1)),
    };
    sll_push(state->buffers, buffer);
    return buffer;
}

internal rained_buffer *open_buffer_from_file(rained_state *state, string path)
{
    arena *text_arena = arena_alloc(gb(1), 0);
    u32 file_size = 0;
    char *text = os_read_file(path.p, &file_size, text_arena);
    rained_buffer *buffer = arena_push_struct_zero(state->forever_arena, rained_buffer);
    *buffer = (rained_buffer)
    {
        .path = string_copy(path, state->forever_arena),
        .text = text,
        .text_arena = text_arena,
        .text_size = file_size,
        .arena1 = arena_alloc(gb(1), mb(1)),
    };
    sll_push(state->buffers, buffer);
    return buffer;
}

internal rained_view *tile_push_view(rained_tile *tile, rained_buffer *buffer)
{
    rained_view *view = arena_push_struct_zero(global_state.forever_arena, rained_view);
    *view = (rained_view)
    {
        .num_carets = 1,
        .buffer = buffer,
    };
    sll_push(tile->view, view);
    return view;
}

internal void tile_switch_view(rained_tile *tile, rained_view *view)
{
    tile->view = view;
}

internal rained_view *tile_find_view_by_buffer_file_name(rained_tile *tile, string buffer_file_name)
{
    rained_view *v = tile->view;
    while(v)
    {
        if(path_match(v->buffer->path, buffer_file_name))
        {
            return v;
        }
        v = v->next;
    }
    return v;
}

internal void tile_pop_view(rained_tile *tile)
{
    // leak
    sll_pop(tile->view);
}

internal chopped_line_list chop_lines(rained_buffer *buffer, u32 width_cells, u32 start, u32 num_to_chop, u32 num_lines, arena *arena)
{
    chopped_line_list res = 
    {
        .lines = arena_push(arena, 0, 64),
        .count = 0
    };
    u32 pos = start;
    u32 line_start = start;
    u32 line = 0;
    while(res.count < num_to_chop && line < num_lines)
    {
        if(pos == buffer->text_size)
        {
            chopped_line *l = arena_push_struct_noalign(arena, chopped_line);
            *l = (chopped_line)
            {
                .line_index = line,
                .pos_in_text = line_start,
                .length = pos - line_start,
            };
            res.count++;
            break;            
        }

        char c = buffer->text[pos];
        if(c == '\n')
        {
            chopped_line *l = arena_push_struct_noalign(arena, chopped_line);
            *l = (chopped_line)
            {
                .line_index = line,
                .pos_in_text = line_start,
                .length = pos - line_start,
            };
            line_start = pos + 1;
            res.count++;
            line++;
        }
        else if(pos - line_start >= width_cells - 1 && c > 31)
        {
            chopped_line *l = arena_push_struct_noalign(arena, chopped_line);
            *l = (chopped_line)
            {
                .line_index = line,
                .pos_in_text = line_start,
                .length = pos - line_start,
            };
            line_start = pos;
            res.count++;
        }
        pos++;
    }
    return res;
}

internal u32 position_to_line_in_chopped_line(rained_buffer *buffer, u32 width_cells, u32 line_start, u32 pos, arena *arena)
{
    chopped_line_list p = chop_lines(buffer, width_cells, line_start, -1, 1, arena);
    for(u32 i = 0; i < p.count; i++)
    {
        if(pos >= p.lines[i].pos_in_text && pos < p.lines[i].pos_in_text + p.lines[i].length)
        {
            return i;
        }
    }
    return 0;
}

typedef struct
{
    renderer_command    *commands_first;
    renderer_command    *commands;
    u32                 screen_w, screen_h;
    rect                rect;

} draw_context;

internal void push_renderer_command(draw_context *ctx, renderer_command cmd)
{
    renderer_command *p = arena_push_struct(global_state.frame_arena, renderer_command);
    *p = cmd;
    sll_push_queue(ctx->commands_first, ctx->commands, p);
}

internal u32 find_line(rained_buffer *buffer, u32 line, u32 *out_line)
{
    *out_line = 0;
    u32 line_start = 0;
    u32 i = 0;
    while(*out_line != line && i < buffer->text_size)
    {
        if(buffer->text[i] == '\n')
        {
            (*out_line)++;
            line_start = i + 1;
        }
        i++;
    }
    return line_start;
}

internal b32 find_line_next(rained_buffer *buffer, u32 *p)
{
    while(*p < buffer->text_size)
    {
        if(buffer->text[*p] == '\n')
        {
            (*p)++;       
            return 1;
        }
        (*p)++;
    }
    return 0;
}

// todo: research why this seems to work
internal u32 find_line_prev(rained_buffer *buffer, u32 p)
{
    if(p == 0)
    {
        return p;
    }
    p--; 
    while(p > 0)
    {
        if(p == 0 || buffer->text[p] == '\r')
        {
            break;
        }
        p--;
    }
    while(p > 0)
    {
        if(buffer->text[p - 1] == '\n')
        {
            return p;
        }
        p--;
    }
    return p;
}

internal u32 find_line_start(rained_buffer *buffer, u32 p)
{
    while(1)
    {
        if(p == 0 || buffer->text[p - 1] == '\n')
        {
            break;
        }
        p--;
    }
    return p;
}

internal u32 find_line_end(rained_buffer *buffer, u32 line_start)
{
    u32 p = line_start;
    while(1)
    {
        if(p == buffer->text_size - 1 || buffer->text[p] == '\r' || buffer->text[p] == '\n')
        {
            break;
        }
        p++;
    }
    return p;
}

internal u32 find_line_index(rained_buffer *buffer, u32 p)
{
    u32 line_index = 0;
    for(u32 i = 0; i < p; i++)
    {
        if(buffer->text[i] == '\n')
        {
            line_index++;
        }
    }
    return line_index;
}

internal u32 chopped_line_list_find_position(chopped_line_list list, u32 p)
{
    for(u32 i = 0; i < list.count; i++)
    {
        if(p >= list.lines[i].pos_in_text && p < list.lines[i].pos_in_text + list.lines[i].length)
        {
            return i;
        }
    }
    return list.count - 1;
}

internal void caret_step_step_line_columns(rained_buffer *buffer, caret *caret, u32 count)
{
    u32 c = 0;
    while(c < count && caret->position != buffer->text_size)
    {
        if(buffer->text[caret->position] == '\r')
        {
            break;
        }
        caret->position++;
        c++;
    }
}

internal void caret_move_down_chopped(rained_view *view, caret *caret)
{
    u32 start = find_line_start(view->buffer, caret->position);
    chopped_line_list list = chop_lines(view->buffer, view->width_cells, start, -1, 1, global_state.frame_arena);
    u32 line_ind = chopped_line_list_find_position(list, caret->position) + 1;
    if(line_ind < list.count)
    {
        caret->position = list.lines[line_ind].pos_in_text;
        caret_step_step_line_columns(view->buffer, caret, min(caret->wish_column, list.lines[line_ind].length));
        return;
    }
    caret_move_to_next_line(view->buffer, caret);
    caret_go_to_column(view, caret, caret->wish_column);
}

internal void caret_move_up_chopped(rained_view *view, caret *caret)
{
    u32 start = find_line_start(view->buffer, caret->position);
    chopped_line_list list = chop_lines(view->buffer, view->width_cells, start, -1, 1, global_state.frame_arena);
    u32 line_ind = chopped_line_list_find_position(list, caret->position);
    if(line_ind > 0)
    {
        line_ind--;
        caret->position = list.lines[line_ind].pos_in_text;
        caret_step_step_line_columns(view->buffer, caret, min(caret->wish_column, list.lines[line_ind].length));
        return;
    }
    caret_move_to_prev_line(view->buffer, caret);
    caret_move_to_line_start(view->buffer, caret);
    list = chop_lines(view->buffer, view->width_cells, caret->position, -1, 1, global_state.frame_arena);
    caret->position = list.lines[list.count - 1].pos_in_text;
    caret_step_step_line_columns(view->buffer, caret, min(caret->wish_column, list.lines[list.count - 1].length));
}

#define d_color_text                0x00ebdbb2
#define d_color_bg                  0x00000000
#define d_color_caret_line          0x001F1F1F
#define d_color_selection           0x00545e52
#define d_color_token_function      0x008ec07c
#define d_color_token_macro         0x00fabd2f
#define d_color_token_type          0x00fabd2f
#define d_color_token_keyword       0x00fe8019
#define d_color_token_comment       0x00928374

internal void draw_view(draw_context *ctx, rained_view *view, f32 scroll_amount, b32 is_focused)
{
    PROFILE_BEGIN("draw_view");

    u32 cell_width = global_state.font.glyph_width;
    u32 cell_height = global_state.font.glyph_height;
    view->width_cells = (ctx->rect.max_x - ctx->rect.min_x + cell_width - 1) / cell_width;
    view->height_cells = (ctx->rect.max_y - ctx->rect.min_y + cell_height - 1) / cell_height + 1;
    
    {
        u32 cell_count = view->width_cells; 
        cell *cells = arena_push(global_state.frame_arena, cell_count * sizeof(cell), 64);
    
        for(u32 i = 0; i < cell_count; i++)
        {
            cells[i] = (cell) 
            {
                .bg_color = 0x00545e52 / 2
            };
        }

        for(u32 j = 0; j < view->buffer->path.length; j++)
        {
            cells[j] = (cell) 
            { 
                .atlas_index = view->buffer->path.p[j] - 32,
                .text_color = d_color_text,
                .bg_color = 0x00545e52 / 2,
            };
        }

        u32 title_height = cell_height;

        rect rect = 
        {
            .max_x = ctx->rect.max_x,
            .max_y = title_height,
            .min_x = ctx->rect.min_x,
            .min_y = ctx->rect.min_y,
        };
        
        push_renderer_command(ctx, (renderer_command)
        {
            .kind = RENDERER_COMMAND_CODE_VIEW,
            .rect = rect,
            .code_view = 
            {
                .font = global_state.font,
                .cells = cells,
                .num_cells_x = view->width_cells,
                .num_cells_y = 1,
                .view_offset_pixels = 0.0f,
                .vsync_line_position = os_time_us() / 1000 % ctx->screen_w,
            }
        });

        ctx->rect.min_y = title_height;
    }

    if(view->fit_caret)
    {
        // fit up...
        carets_bubble_sort_top_to_bottom(view);
        u32 fit_pos = view->carets[0].position;
        u32 caret_line = find_line_index(view->buffer, fit_pos);
        if(caret_line <= view->line_index)
        {
            u32 line_start = find_line_start(view->buffer, fit_pos);
            u32 offset = position_to_line_in_chopped_line(view->buffer, view->width_cells, line_start, fit_pos, global_state.frame_arena) * cell_height;
            if(caret_line == view->line_index)
            {
                view->y_offset_pixels = min(view->y_offset_pixels, offset);
            }
            else
            {
                view->y_offset_pixels = offset;   
                view->line_index = caret_line;
            }
        }

        {
            fit_pos = view->carets[view->num_carets - 1].position;
            // fit down. offset the caret line height_cells + offset chopped lines up for a new view line_index and offset...
            // todo: there's a bug with very long lines, when caret chopped line index > screen height it just stops tracking 
            u32 line_start = find_line_start(view->buffer, fit_pos);
            u32 offset = position_to_line_in_chopped_line(view->buffer, view->width_cells, line_start, fit_pos, global_state.frame_arena);
            u32 height = view->height_cells - min(view->height_cells, 4);
            u32 pos = fit_pos;
            u32 line_ind = find_line_index(view->buffer, pos);
            while(line_ind)
            {
                pos = find_line_prev(view->buffer, pos);
                line_ind--;
                chopped_line_list l = chop_lines(view->buffer, view->width_cells, pos, -1, 1, global_state.frame_arena);
                offset += l.count;
                if(offset > height)
                {
                    u32 offset_pixels = (offset - height);
                    offset_pixels *= cell_height;

                    if(line_ind > view->line_index)
                    {
                        view->line_index = line_ind;
                        view->y_offset_pixels = offset_pixels;
                    }
                    if(line_ind == view->line_index)
                    {
                        view->y_offset_pixels = max(view->y_offset_pixels, offset_pixels);
                    }

                    break;
                }
            }
        }
        
        view->fit_caret = 0;
    }

    u32 p = find_line(view->buffer, view->line_index, &view->line_index);

    view->y_offset_pixels -= scroll_amount;
    
    while(view->y_offset_pixels < 0 && view->line_index)
    {
        p = find_line_prev(view->buffer, p);
        view->line_index--;
        chopped_line_list l = chop_lines(view->buffer, view->width_cells, p, -1, 1, global_state.frame_arena);
        view->y_offset_pixels = cell_height * l.count + view->y_offset_pixels;
    }

    while(1)
    {
        chopped_line_list l = chop_lines(view->buffer, view->width_cells, p, -1, 1, global_state.frame_arena);
        
        if(view->y_offset_pixels / cell_height > l.count && l.count)
        {   
            u32 next_p = p;
            if(find_line_next(view->buffer, &next_p))
            {
                view->line_index++;
                view->y_offset_pixels -= cell_height * l.count;
                p = next_p;
            }
            else
            {
                break;
            }
        }
        else
        {
            break;
        }
    }

    u32 cell_count = view->width_cells * view->height_cells; 
    cell *cells = arena_push(global_state.frame_arena, cell_count * sizeof(cell), 64);

    for(u32 i = 0; i < cell_count; i++)
    {
        cells[i] = (cell) 
        {
            .bg_color = d_color_bg
        };
    }

    i32 offset_lines = max(0, view->y_offset_pixels / cell_height);

    chopped_line_list chopped = chop_lines(view->buffer, view->width_cells, p, view->height_cells + offset_lines, -1, global_state.frame_arena);

    if(chopped.count == 0)
    {
        chopped.count = 1;
        chopped.lines = arena_push_struct_zero(global_state.frame_arena, chopped_line);
    }

    u32 *caret_lines = arena_push(global_state.frame_arena, sizeof(u32) * view->num_carets, 8);
    for(u32 i = 0 ; i < view->num_carets; i++)
    {
        caret_lines[i] = find_line_index(view->buffer, view->carets[i].position);
    }

    i32 count = min((i32)view->height_cells, (i32)(chopped.count) - (i32)(offset_lines));
    for(i32 y = 0; y < count; y++)
    {
        chopped_line *l = &chopped.lines[offset_lines + y];

        for(u32 j = 0; j < min(l->length, view->width_cells); j++)
        {
            char c = view->buffer->text[l->pos_in_text + j];
            u32 cell_index = j + y * view->width_cells;

            cells[cell_index] = (cell) 
            { 
                .atlas_index = c - 32,
                .text_color = d_color_text,
                .bg_color = d_color_bg,
            };
        }

        for(u32 k = 0; k < view->num_carets; k++)
        {
            caret *caret = &view->carets[k];
            
            if(view->line_index + l->line_index == caret_lines[k])
            {
                for(u32 j = 0; j < view->width_cells; j++)
                {
                    cells[y * view->width_cells + j].bg_color = d_color_caret_line;
                }
            }
            if(caret->selection_active)
            {
                u32 start = max(l->pos_in_text, min(caret->position, caret->selection_pos));
                u32 end = min((l->pos_in_text + l->length), max(caret->position, caret->selection_pos));
                for(u32 j = start; j < end; j++)
                {
                    cells[y * view->width_cells + j - l->pos_in_text].bg_color = d_color_selection;
                }
            }
        }
        
        if(is_focused)
        {
            for(u32 k = 0; k < view->num_carets; k++)
            {
                caret *caret = &view->carets[k];
                if(caret->position >= l->pos_in_text && caret->position < l->pos_in_text + l->length)
                {      
                    cell *c = &cells[caret->position - l->pos_in_text + y * view->width_cells];
                    c->bg_color = ~c->bg_color;
                    c->text_color = ~c->text_color;
                }
            }
        }
    }
    
    if(!view->is_a_command_view && view->buffer->path.p)
    {
        PROFILE_BEGIN("tokens");
        highlight_token_array arr = rained_clang_query_tokens_for_file(global_state.clang_state, view->buffer);

        for(u32 i = 0; i < arr.num_tokens; i++)
        {
            highlight_token t = arr.tokens[i];
            u32 color = 0;

            switch(t.kind)
            {
                case highlight_token_function: color = d_color_token_function; break;
                case highlight_token_macro: color = d_color_token_macro; break;
                case highlight_token_type: color = d_color_token_type; break;
                case highlight_token_keyword: color = d_color_token_keyword; break;
                case highlight_token_comment: color = d_color_token_comment; break;
    
                default:
                {
                    continue;
                }
            }

            // apply edits between the version of the buffer that was used for the latest token cache update and the current one...
            text_edit *e = view->buffer->patch_edits;
            while(e)
            {
                if(e->additive)
                {
                    if(e->position < t.offset)
                    {
                        t.offset += e->string.length;
                    }
                    else if(e->position <= t.offset + t.length)
                    {
                        t.length += e->string.length;
                    }
                }
                else
                {
                    if(e->position + e->string.length <= t.offset)
                    {
                        t.offset -= e->string.length;
                    }
                    else if(e->position < t.offset + t.length)
                    {
                        t.length -= min(t.offset + t.length, e->position + e->string.length) - max(t.offset, e->position);
                        if(e->position < t.offset)
                        {
                            t.offset = e->position;
                        }
                    }
                }
                e = e->prev;
            }

            if(t.offset + t.length < p)
            {
                continue;
            }

            if(t.offset > chopped.lines[chopped.count - 1].pos_in_text)
            {
                break;
            }

            for(i32 y = 0; y < count; y++)
            {
                chopped_line *l = &chopped.lines[offset_lines + y];
    
                for(i32 j = max(t.offset, l->pos_in_text); j < min(l->pos_in_text + l->length, t.offset + t.length); j++)
                {
                    i32 x = j - l->pos_in_text;
                    u32 cell_index = x + y * view->width_cells;
                    cell *c = &cells[cell_index];
                    c->text_color = color;
                }
            }
        }
        PROFILE_END();
    }

    push_renderer_command(ctx, (renderer_command)
    {
        .kind = RENDERER_COMMAND_CODE_VIEW,
        .rect = ctx->rect,
        .code_view = 
        {
            .font = global_state.font,
            .cells = cells,
            .num_cells_x = view->width_cells,
            .num_cells_y = view->height_cells,
            .view_offset_pixels = view->y_offset_pixels < 0.0f ? view->y_offset_pixels : (i32)view->y_offset_pixels % cell_height,
            .vsync_line_position = os_time_us() / 1000 % ctx->screen_w,
        }
    });
    PROFILE_END();
}

// todo: there is a bug with big lines.
internal u32 tile_screen_pos_to_view_buffer_text_pos(rained_tile *tile, i32 screen_x, i32 screen_y)
{
    screen_y -= global_state.font.glyph_height;
    screen_y += tile->view->y_offset_pixels;
    i32 pos_column = max(0, (screen_x - tile->rect.min_x) / (i32)global_state.font.glyph_width);
    i32 pos_line = max(0, (screen_y - tile->rect.min_y) / (i32)global_state.font.glyph_height);
    u32 view_line_p = find_line(tile->view->buffer, tile->view->line_index, &tile->view->line_index);
    chopped_line_list l = chop_lines(tile->view->buffer, tile->view->width_cells, view_line_p, tile->view->height_cells, -1, global_state.frame_arena);
    chopped_line line = l.lines[max(min(pos_line, l.count - 1), 0)];
    return line.pos_in_text + min(line.length - 1, pos_column);
}

internal void draw_tile(draw_context *ctx, rained_tile *tile, b32 is_focused, f32 scroll)
{
    if(tile->view)
    {
        ctx->rect = tile->rect;
        draw_view(ctx, tile->view, scroll, is_focused);
    }
}

internal void carets_delete(rained_view *view, b32 delete_to_the_right, b32 delete_until_next_token)
{
    u32 *lengths = arena_push(global_state.frame_arena, sizeof(u32) * view->num_carets, 8);

    for(u32 j = 0; j < view->num_carets; j++)
    {
        caret *caret = &view->carets[j];
        u32 length = 1;

        if(delete_until_next_token)
        {
            if(delete_to_the_right)
            {
                length = caret_next_token(view, caret) - caret->position;
                caret->position += length;
            }
            else
            {
                length = caret->position - caret_prev_token(view, caret);
            }
        }
        else
        {
            if(caret->selection_active)
            {
                if(caret->position > caret->selection_pos)
                {
                    length = caret->position - caret->selection_pos;
                }
                else
                {
                    length = caret->selection_pos - caret->position;
                    caret->position = caret->selection_pos;
                }
            }
            else if(delete_to_the_right)
            {
                caret->position += length;
            }
        }

        lengths[j] = length;
        caret->selection_active = 0;
    }
    carets_delete_length(view, lengths);
}

internal void carets_insert_or_replace_selection_with_string(rained_view *view, string str)
{
    u32 *delete_lengths = arena_push(global_state.frame_arena, sizeof(u32) * view->num_carets, 8);
    b32 do_delete = 0;
    for(u32 j = 0; j < view->num_carets; j++)
    {
        caret *caret = &view->carets[j];
        u32 length = 0;
        if(caret->selection_active)
        {
            if(caret->position > caret->selection_pos)
            {
                length = caret->position - caret->selection_pos;
            }
            else
            {
                length = caret->selection_pos - caret->position;
                caret->position = caret->selection_pos;
            }
        }
        delete_lengths[j] = length;
        do_delete |= length;
        caret->selection_active = 0;
    }
    if(do_delete)
    {
        carets_delete_length(view, delete_lengths);
    }
    carets_insert_string(view, str);
}


typedef struct
{
    u32 start, length;
} copy_cut_range;

internal copy_cut_range *carets_get_copy_or_cut_ranges(rained_view *view, arena *arena)
{
    copy_cut_range *res = arena_push(arena, sizeof(copy_cut_range) * view->num_carets, 8);
    for(u32 i = 0; i < view->num_carets; i++)
    {
        caret caret = view->carets[i];
        if(caret.selection_active)
        {
            res[i].start = min(caret.position, caret.selection_pos);
            res[i].length = max(caret.position, caret.selection_pos) - res[i].start;
        }
        else
        {
            res[i].start = find_line_start(view->buffer, caret.position);
            res[i].length = find_line_end(view->buffer, res[i].start) - res[i].start;
        }
    }
    return res;
}

internal renderer_command *draw(rained_input *input)
{
    static b32 did_init;
    if(!did_init)
    {
        did_init = 1;   
        global_state.forever_arena = arena_alloc(gb(1), mb(1));
        global_state.frame_arena = arena_alloc(gb(1), mb(1));
        global_state.tile_left = arena_push_struct_zero(global_state.forever_arena, rained_tile);
        global_state.tile_right = arena_push_struct_zero(global_state.forever_arena, rained_tile);
        global_state.tile_left->next = global_state.tile_right;
        global_state.focused_tile = global_state.tile_left;
        rained_buffer *b0 = open_buffer_from_file(&global_state, arena_push_cstring(global_state.frame_arena, ".\\rained.c"));
        rained_buffer *b1 = open_buffer_from_file(&global_state, arena_push_cstring(global_state.frame_arena, ".\\rained_win32.c"));
        rained_view *v0 = tile_push_view(global_state.tile_left, b0);
        rained_view *v1 = tile_push_view(global_state.tile_right, b1);
        global_state.font_size = 18;
        
        global_state.clang_state = arena_push_struct_zero(global_state.forever_arena, rained_clang_state);
        global_state.clang_state->arena = arena_alloc(tb(1), mb(1));
        arena *clang_arena = arena_alloc(gb(1), mb(1));
        rained_clang_thread_context *ctx = arena_push_struct(clang_arena, rained_clang_thread_context);
        *ctx = (rained_clang_thread_context)
        {
            .state = global_state.clang_state,
        };
        os_create_thread(&rained_clang_thread_entry_point, ctx, L"rained_clang_thread");
        rained_clang_schedule_reparse(global_state.clang_state, global_state.buffers);
    }
    arena_reset(global_state.frame_arena);

    PROFILE_BEGIN("make font");
    if(global_state.font.size != global_state.font_size)
    {
        os_release_font_atlas(global_state.font);
        global_state.font = os_make_font_atlas(global_state.font_size, global_state.frame_arena);
    }
    PROFILE_END();
    
    rained_view *view = global_state.focused_tile->view;

    for(u32 i = 0; i < input->input_queue_count; i++)
    {
        input_event e = input->input_queue[i];

        if(e.is_down && (u8)e.code == KEY_SHIFT)
        {
            for(u32 i = 0; i < view->num_carets; i++)
            {
                if(!view->carets[i].selection_active)
                {
                    view->carets[i].selection_pos = view->carets[i].position;
                    view->carets[i].selection_active = 1;
                }
            }
        }

        if(e.is_down)
        {
            if(e.type == INPUT_EVENT_KEY)
            {
                if(e.code == KEY_F11)
                {
                    os_toggle_fullscreen();
                }
                else if(e.code == KEY_THE_ONE_RIGHT_BELOW_ESCAPE)
                {
                    view->carets[0] = view->carets[view->num_carets - 1];
                    view->num_carets = 1;
                }
                else if(e.code == (KEY_DOWN | MODIFIER_ALT | MODIFIER_CTRL))
                {
                    caret_spawn_new_below(view);
                }
                else if(e.code == (KEY_UP | MODIFIER_ALT | MODIFIER_CTRL))
                {
                    caret_spawn_new_above(view);
                }
                else if(e.code == ('Z' | MODIFIER_CTRL))
                {
                    undo(view);
                }
                else if(e.code == ('Y' | MODIFIER_CTRL))
                {
                    redo(view);
                }
                else if(e.code == (KEY_THE_ONE_TO_THE_LEFT_OF_A_RIGHT_SHIFT | MODIFIER_CTRL))
                {
                    global_state.focused_tile = global_state.focused_tile == global_state.tile_left ? global_state.tile_right : global_state.tile_left; 
                }
                else if(e.code == ('S' | MODIFIER_CTRL))
                {
                    os_write_file(view->buffer->path.p, view->buffer->text, view->buffer->text_size);
                }
                else if(e.code == ('N' | MODIFIER_CTRL))
                {
                    rained_buffer *b = open_empty_buffer(&global_state);
                    tile_push_view(global_state.focused_tile, b);
                }
                else if(e.code == ('P' | MODIFIER_CTRL))
                {
                    // note: listen, i don't know what am do i want to do ui-wise right now. we just open a buffer, and we type some text there, and we do something on enter, okay? and then we don't even free the buffer memory, we just leak the whole thing, i dont give a flying fuck, this is my editor
                    rained_buffer *b = open_empty_buffer(&global_state);
                    tile_push_view(global_state.focused_tile, b)->is_a_command_view = 1;
                }
                else if(e.code == KEY_ENTER)
                {
                    if(global_state.focused_tile->view->is_a_command_view)
                    {
                        rained_view *v = tile_find_view_by_buffer_file_name(global_state.focused_tile, global_state.focused_tile->view->buffer->text_string);
                        if(v)
                        {
                            tile_pop_view(global_state.focused_tile);
                            tile_switch_view(global_state.focused_tile, v);
                        }
                        else
                        {
                            // todo: a typo? have this (int*)0 = 0 fuck you
                            rained_buffer *b = open_buffer_from_file(&global_state, global_state.focused_tile->view->buffer->text_string);
                            tile_pop_view(global_state.focused_tile);
                            tile_push_view(global_state.focused_tile, b);
                        }
                    }
                }
                else if(e.code == ('D' | MODIFIER_CTRL))
                {
                    string file_name;
                    u32 position;
                    if(rained_clang_find_definition(global_state.clang_state, global_state.focused_tile->view->buffer, global_state.focused_tile->view->carets[0], &position, &file_name, global_state.frame_arena))
                    {
                        rained_view *view = tile_find_view_by_buffer_file_name(global_state.focused_tile, file_name);
                        if(view)
                        {
                            tile_switch_view(global_state.focused_tile, view);
                        }
                        else
                        {
                            rained_buffer *b = open_buffer_from_file(&global_state, file_name);
                            tile_push_view(global_state.focused_tile, b);
                        }
                        global_state.focused_tile->view->carets[0].position = position;
                        global_state.focused_tile->view->fit_caret = 1;
                    }
                }
                else if(e.code == ('C' | MODIFIER_CTRL))
                {
                    copy_cut_range *ranges = carets_get_copy_or_cut_ranges(view, global_state.frame_arena);
                    u32 size = 0;
                    char *text = arena_head(global_state.frame_arena);
                    for(u32 i = 0; i < view->num_carets; i++)
                    {
                        copy_cut_range range = ranges[i];
                        memcpy(arena_push_noalign(global_state.frame_arena, range.length), view->buffer->text + range.start, range.length);
                        size += range.length;
                    }
                    os_clipboard_set((string) 
                    {
                        .p = text,
                        .length = size,
                    });
                }
                else if(e.code == ('X' | MODIFIER_CTRL))
                {
                    copy_cut_range *ranges = carets_get_copy_or_cut_ranges(view, global_state.frame_arena);
                    u32 size = 0;
                    char *text = arena_head(global_state.frame_arena);
                    for(u32 i = 0; i < view->num_carets; i++)
                    {
                        copy_cut_range range = ranges[i];
                        memcpy(arena_push_noalign(global_state.frame_arena, range.length), view->buffer->text + range.start, range.length);
                        size += range.length;
                    }
                    os_clipboard_set((string) 
                    {
                        .p = text,
                        .length = size,
                    });

                    u32 *delete_lengths = arena_push(global_state.frame_arena, sizeof(u32) * view->num_carets, 8);      
                    for(u32 i = 0; i < view->num_carets; i++)
                    {
                        delete_lengths[i] = ranges[i].length;
                        view->carets[i].position = ranges[i].start + ranges[i].length;
                        view->carets[i].selection_active = 0;
                    }
                    carets_delete_length(view, delete_lengths);
                }
                else if(e.code == ('V' | MODIFIER_CTRL))
                {
                    string to_paste = os_clipboard_get(global_state.frame_arena);
                    carets_insert_or_replace_selection_with_string(view, to_paste);
                }
                else if(e.code == (KEY_PLUS | MODIFIER_CTRL))
                {
                    global_state.font_size++;
                }
                else if(e.code == (KEY_MINUS | MODIFIER_CTRL))
                {
                    if(global_state.font_size > 1)
                    {
                        global_state.font_size--;
                    }
                }
                else if(e.code == ('W' | MODIFIER_CTRL))
                {
                    if(global_state.focused_tile->view->next)
                    {
                        tile_pop_view(global_state.focused_tile);
                    }
                }
                else if(e.code == KEY_BACKSPACE || e.code == (KEY_BACKSPACE | MODIFIER_CTRL))
                {
                    carets_delete(view, 0, e.code & MODIFIER_CTRL);
                }
                else if(e.code == KEY_DELETE || e.code == (KEY_DELETE | MODIFIER_CTRL))
                {
                    carets_delete(view, 1, e.code & MODIFIER_CTRL);
                }
            }
        }

        if(e.type == INPUT_EVENT_TEXT)
        {
            if(e.character > 31 && e.character < 127 && e.character != '`') // i've lost my enter key
            {
                string insert = (string)
                {
                    .p = &e.character,
                    .length = 1
                };
                carets_insert_or_replace_selection_with_string(view, insert);
            }
            else if(e.character == '\r')
            {
                char line_end[2] = { '\r', '\n' };
                string insert = (string)
                {
                    .p = line_end,
                    .length = 2
                };
                carets_insert_or_replace_selection_with_string(view, insert);
            }
        }

        if(e.type == INPUT_EVENT_MOUSE_BUTTON)
        {
            if(e.lmb.is_down && !e.lmb.was_down)
            {
                rained_tile *t = global_state.tile_left;
                while(t)
                {
                    if(rect_contains_point(t->rect, e.x, e.y))
                    {
                        global_state.focused_tile = t;
                        global_state.mouse_drag_tile = t;
                        global_state.mouse_drag_tile->view->num_carets = 1;
                        global_state.mouse_drag_tile->view->carets[0].selection_pos = tile_screen_pos_to_view_buffer_text_pos(t, e.x, e.y);
                        global_state.mouse_drag_tile->view->carets[0].selection_active = 1;
                        break;
                    }
                    t = t->next;
                }
            }
        }

        for(u32 i = 0; i < view->num_carets; i++)
        {
            caret *caret = &view->carets[i];
            if(e.is_down)
            {
                if(e.type == INPUT_EVENT_KEY)
                {
                    if(e.code == KEY_RIGHT || e.code == (KEY_RIGHT | MODIFIER_SHIFT))
                    {
                        caret_move_right(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == KEY_LEFT || e.code == (KEY_LEFT | MODIFIER_SHIFT))
                    {
                        caret_move_left(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == KEY_DOWN || e.code == (KEY_DOWN | MODIFIER_SHIFT))
                    {
                        caret_move_down_chopped(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == KEY_UP || e.code == (KEY_UP | MODIFIER_SHIFT))
                    {
                        caret_move_up_chopped(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == (KEY_DOWN | MODIFIER_CTRL) || e.code == (KEY_DOWN | MODIFIER_SHIFT | MODIFIER_CTRL))
                    {
                        caret_move_down_chopped(view, caret);
                        caret_move_down_chopped(view, caret);
                        caret_move_down_chopped(view, caret);
                        caret_move_down_chopped(view, caret);
                        caret_move_down_chopped(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == (KEY_UP | MODIFIER_CTRL) || e.code == (KEY_UP | MODIFIER_SHIFT | MODIFIER_CTRL))
                    {
                        caret_move_up_chopped(view, caret);
                        caret_move_up_chopped(view, caret);
                        caret_move_up_chopped(view, caret);
                        caret_move_up_chopped(view, caret);
                        caret_move_up_chopped(view, caret);
                        view->fit_caret = 1;
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                    }
                    else if(e.code == (KEY_LEFT | MODIFIER_CTRL | MODIFIER_ALT) || e.code == (KEY_LEFT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT))
                    {
                        caret->position = find_line_start(view->buffer, caret->position);
                        caret->wish_column = caret_get_column(view, caret);
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                        view->fit_caret = 1;
                    }
                    else if(e.code == (KEY_RIGHT | MODIFIER_CTRL | MODIFIER_ALT) || e.code == (KEY_RIGHT | MODIFIER_CTRL | MODIFIER_ALT | MODIFIER_SHIFT))
                    {
                        caret->position = find_line_end(view->buffer, caret->position);
                        caret->wish_column = caret_get_column(view, caret);
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                        view->fit_caret = 1;
                    }
                    else if(e.code == (KEY_RIGHT | MODIFIER_CTRL) || e.code == (KEY_RIGHT | MODIFIER_CTRL | MODIFIER_SHIFT))
                    {
                        caret_jump_right(view, caret);
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                        view->fit_caret = 1;
                    }
                    else if(e.code == (KEY_LEFT | MODIFIER_CTRL) || e.code == (KEY_LEFT | MODIFIER_CTRL | MODIFIER_SHIFT))
                    {
                        caret_jump_left(view, caret);
                        caret->selection_active = (e.code & MODIFIER_SHIFT);
                        view->fit_caret = 1;
                    }
                    else if(e.code == KEY_HOME)
                    {
                        caret->position = 0;
                        view->fit_caret = 1;
                    }
                    else if(e.code == KEY_END)
                    {
                        caret->position = view->buffer->text_size;
                        view->fit_caret = 1;
                    }
                    else if(e.code == KEY_PAGEDOWN)
                    {
                    #if 0 
                        chopped_line_list l = chop_lines(view->buffer, view->width_cells, caret->position, view->height_cells - 2, -1, global_state.frame_arena);
                        caret->position = l.lines[l.count - 1].pos_in_text;
                        view->fit_caret = 1;
                    #endif
                    }
                    else if(e.code == KEY_PAGEUP)
                    {
                        
                    }
                }
            }
        }

        merge_overlapping_carets_in_a_slow_way(view);
    }

    b32 any_buffer_is_dirty = 0;
    rained_buffer *buffer = global_state.buffers;
    while(buffer)
    {
        any_buffer_is_dirty |= buffer->is_dirty;
        buffer->is_dirty = 0;
        buffer = buffer->next;
    }

    global_state.reparse_pending |= any_buffer_is_dirty;

    if(global_state.reparse_pending)
    {
        b32 scheduled = rained_clang_schedule_reparse(global_state.clang_state, global_state.buffers);
        global_state.reparse_pending = !scheduled;
    }
    
    // todo: doesn't work the way i want it to when mouse leaves the client area. im not sure what to do about this right now
    if(global_state.mouse_drag_tile)
    {
        if(input->lmb)
        {
            global_state.mouse_drag_tile->view->num_carets = 1;
            caret *caret = &global_state.mouse_drag_tile->view->carets[0];
            caret->position = tile_screen_pos_to_view_buffer_text_pos(global_state.mouse_drag_tile, input->mouse_x, input->mouse_y);
            caret->wish_column = caret_get_column(global_state.mouse_drag_tile->view, caret);
        }
        else 
        {
            global_state.mouse_drag_tile = 0;
        }
    }

    global_state.tile_left->rect = (rect)
    {
        .max_x = input->screen_w / 2,
        .max_y = input->screen_h
    }; 

    global_state.tile_right->rect = (rect)
    {
        .min_x = input->screen_w / 2 + 2,
        .max_x = input->screen_w,
        .max_y = input->screen_h    
    };

    draw_context ctx = 
    {
        .rect = (rect)
        {
            .max_x = input->screen_w,
            .max_y = input->screen_h,
        },
        .screen_w = input->screen_w,
        .screen_h = input->screen_h
    };

    rained_tile *t = global_state.tile_left;
    while(t)
    {
        draw_tile(&ctx, t, global_state.focused_tile == t, rect_contains_point(t->rect, input->mouse_x, input->mouse_y) ? input->mouse_wheel_delta : 0.0f);
        t = t->next;
    }

    // tile separator type sh
    push_renderer_command(&ctx, (renderer_command)
    {
        .kind = RENDERER_COMMAND_RECT,
        .rect = (rect)
        {
            .min_x = input->screen_w / 2,
            .min_y = 0,
            .max_x = input->screen_w / 2 + 2,
            .max_y = input->screen_h,
        },
        .quad.color = 0x001F1F1F
    });

    return ctx.commands_first;
}