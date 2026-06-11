#include "rained.h"

typedef struct undo_buffer_entry undo_buffer_entry;
struct undo_buffer_entry
{
    text_edit_kind      kind;
    caret               *carets;
    u32                 num_carets; 
    string              *strings;
    undo_buffer_entry   *next;
    undo_buffer_entry   *prev;
};

typedef struct rained_buffer rained_buffer;
struct rained_buffer
{
    arena               *text_arena;
    char                *text;
    u32                 text_size;
    arena               *undo_buffer_arena;
    undo_buffer_entry   *undo_buffer_tail;
    undo_buffer_entry   *undo_buffer_position;
    rained_buffer       *next;
};

typedef struct rained_view rained_view;
struct rained_view
{
    rained_buffer       *buffer;
    u32                 line_index;
    f32                 view_offset_pixels;
    caret               carets[1024];
    u32                 num_carets;
    u64                 caret_last_move_time;
    u32                 width_cells, height_cells;
    rained_view         *next;
};

typedef struct
{
    arena               *frame_arena;
    arena               *forever_arena;
    renderer_command    *commands;
    rained_view         *views;
    rained_buffer       *buffers;

} rained_state;

rained_state global_state;

int _fltused;

#pragma function(memset)
void *memset(void *dest, int c, size_t count)
{
    char *bytes = (char *)dest;
    while (count--)
    {
        *bytes++ = (char)c;
    }
    return dest;
}

#pragma function(memcpy)
void *memcpy(void *dest, const void *src, size_t count)
{
    char *dest8 = (char *)dest;
    const char *src8 = (const char *)src;
    while (count--)
    {
        *dest8++ = *src8++;
    }
    return dest;
}

internal u32 cstring_length(char *cstr)
{
    u32 i = 0;
    while(cstr[i]) 
    { 
        i++; 
    }
    return i;
}

internal arena *arena_alloc(u64 reserve, u64 commit)
{
    // we reserve and commit at least one page

    u64 reserve_size = (reserve + PAGE_SIZE) / PAGE_SIZE * PAGE_SIZE;
    u64 commit_size = (commit + PAGE_SIZE) / PAGE_SIZE * PAGE_SIZE;

    void *base;

    os_mem_reserve(reserve_size, &base);
    os_mem_commit(base, commit_size);
    
    arena *header = base;

    header->reserved = reserve_size;
    header->commited = commit_size;
    header->used = sizeof(arena_t);
    header->base = base;

    ASAN_POISON_REGION((u8*)header->base + sizeof(arena_t), commit_size)

    return header;
}

internal void *arena_push_noalign(arena *arena, u64 size)
{    
    void *ptr = (void*)((u8*)arena->base + arena->used);

    arena->used += size;

    assert(arena->reserved >= arena->used);

    if(arena->commited < arena->used)
    {
        PROFILE_BEGIN("arena commit");
        // we need to commit more memory
        // for now, let's just say that we allocate enough pages to fit whatever we push
        // it will be fine if we have 2mb pages. of if we just dont shrink the arena 
        // todo: (perfomance) use large pages (to keep tlb happy and decrease the granulatiry)

        u64 commitSize = (arena->used + PAGE_SIZE) / PAGE_SIZE * PAGE_SIZE;

        arena->commited = commitSize;

        os_mem_commit(arena -> base, commitSize);
        PROFILE_END();
    }

    ASAN_UNPOISON_REGION(ptr, size);

    return ptr; 
}

internal void *arena_push(arena *arena, u64 size, u64 align)
{
    u32 rem = (64 + arena->used) % align;
    u32 padding = rem == 0 ? 0 : align - rem;
    u8 *mem = arena_push_noalign(arena, padding + size);
    return mem + padding;
}

internal void arena_reset(arena *arena)
{
    arena->used = sizeof(arena_t);

    ASAN_POISON_REGION((u8*)arena->base + sizeof(arena_t), arena->commited)
}

internal void arena_release(arena *arena)
{
    os_mem_free(arena->base);
}

#define arena_push_zero(a,s,al) (memset(arena_push(a, s, al), 0, s)) 
#define arena_push_struct(a, s) (arena_push(a, sizeof(s), 8))
#define arena_push_struct_noalign(a, s) (arena_push_noalign(a, sizeof(s)))
#define arena_push_struct_zero(a, s) (memset(arena_push(a, sizeof(s), 8), 0, sizeof(s)))
#define arena_copy(a, p, s) (memcpy(arena_push(a, s, 8), p, s))

#define sll_push(sll, e) if(sll) { e->next = sll; } sll = e;

internal loaded_bitmap load_bitmap(char *path, arena_t *arena)
{
    void *file = os_read_file(path, 0, arena);
    
    loaded_bitmap bitmap = { 0 };
    bitmap.header = (bitmap_header*)file;
    bitmap.data = ((u8*)file) + bitmap.header->BitmapOffset;

    // we want a raw uncompressed bitmap (bitfield encoding)
    assert(bitmap.header->Compression == 3);

    // bgra -> rgba
    u32 num_pixels = bitmap.header->Width * bitmap.header->Height;
    for(u32 i = 0; i < num_pixels; i++)
    {
        u8* ptr = ((u8*)bitmap.data) + i * 4;

        u8 b = *(ptr + 0);
        u8 g = *(ptr + 1);
        u8 r = *(ptr + 2);
        u8 a = *(ptr + 3);
        
        *(ptr + 0) = r;
        *(ptr + 1) = g;
        *(ptr + 2) = b;
        *(ptr + 3) = a;
    }

    return bitmap;
}

internal u32 caret_get_column(rained_buffer *buffer, caret *caret)
{
    for(u32 i = caret->position; i >= 0; i--)
    {
        if(i == 0)
        {
            return caret->position - i;
        }
        if(buffer->text[i] == '\n')
        {
            return caret->position - i - 1;
        }
    }
    return 0;
}

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

internal void caret_move_right(rained_buffer *buffer, caret *caret)
{
    u32 p = caret->position;
    while(caret->position != buffer->text_size)
    {
        char c = buffer->text[caret->position];
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
    caret->wish_column = caret_get_column(buffer, caret);
    
}

internal void caret_move_left(rained_buffer *buffer, caret *caret)
{
    while(caret->position)
    {
        caret->position--;
        char c = buffer->text[caret->position];
        if(c == '\r')
        {
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret->wish_column = caret_get_column(buffer, caret);
    
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

internal void caret_go_to_column(rained_buffer *buffer, caret *caret, u32 col)
{
    caret_move_to_line_start(buffer, caret);
    u32 c = 0;
    while(c != col && caret->position != buffer->text_size)
    {
        if(buffer->text[caret->position] == '\r')
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

internal void caret_move_up(rained_buffer *buffer, caret *caret)
{
    caret_move_to_prev_line(buffer, caret);
    caret_go_to_column(buffer, caret, caret->wish_column);
    
}

internal void caret_move_down(rained_buffer *buffer, caret *caret)
{
    caret_move_to_next_line(buffer, caret);
    caret_go_to_column(buffer, caret, caret->wish_column);
    
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

internal void undo_buffer_push(rained_buffer *buffer, undo_buffer_entry *entry)
{
    if(buffer->undo_buffer_position)
    {
        buffer->undo_buffer_position->next = entry;
        entry->prev = buffer->undo_buffer_position;
        buffer->undo_buffer_position = entry;
    }
    else
    {
        buffer->undo_buffer_position = entry;
        buffer->undo_buffer_tail = entry;
    }
}

internal void carets_insert_characters(rained_view *view, text_edit_insert insert, b32 write_undo)
{
    carets_bubble_sort_top_to_bottom(view);
    u32 offset = 0;
    for(u32 c = 0; c < view->num_carets; c++)
    {
        string s = insert.strings[c];
        caret *caret = &view->carets[c];
        if(s.length)
        {
            caret->position += offset;
            offset += s.length;
            u32 p = caret->position;
            for(u32 i = view->buffer->text_size + s.length - 1; i >= p + s.length; i--)
            {
                view->buffer->text[i] = view->buffer->text[i - s.length];
            }
            for(u32 i = 0; i < s.length; i++)
            {
                view->buffer->text[p + i] = s.p[i];
            }
            view->buffer->text_size += s.length;
            caret->position += s.length;
        }
        caret->wish_column = caret_get_column(view->buffer, caret);
    }
    if(write_undo)
    {
        undo_buffer_entry *e = arena_push_struct(view->buffer->undo_buffer_arena, undo_buffer_entry);
        *e = (undo_buffer_entry)
        {
            .kind = TEXT_EDIT_INSERT,
            .carets = arena_copy(view->buffer->undo_buffer_arena, view->carets, sizeof(caret) * view->num_carets),
            .num_carets = view->num_carets
        };
        e->strings = arena_push(view->buffer->undo_buffer_arena, view->num_carets * sizeof(string), 8);
        for(u32 i = 0; i < view->num_carets; i++)
        {
            e->strings[i].length = insert.strings[i].length;
            e->strings[i].p = arena_copy(view->buffer->undo_buffer_arena, insert.strings[i].p, insert.strings[i].length);
        }
        undo_buffer_push(view->buffer, e);
    }
}

// todo: don't push undo when each num_to_remove = 0
internal void carets_remove_characters(rained_view *view, text_edit_delete delete, b32 write_undo)
{
    undo_buffer_entry *e = arena_push_struct(view->buffer->undo_buffer_arena, undo_buffer_entry);
    if(write_undo)
    {
        *e = (undo_buffer_entry)
        {
            .kind = TEXT_EDIT_DELETE,
            .strings = arena_push_zero(view->buffer->undo_buffer_arena, sizeof(string) * view->num_carets, 8),
            .num_carets = view->num_carets,
        };
    }
    carets_bubble_sort_top_to_bottom(view);
    u32 offset = 0;
    for(u32 c = 0; c < view->num_carets; c++)
    {
        caret *caret = &view->carets[c];
        caret->position -= offset;
        u32 num_to_remove = min(delete.lengths[c], caret->position);
        // dont leave a stray /r fuhhhh.
        if(caret->position >= num_to_remove + 1)
        {
            if(view->buffer->text[caret->position - num_to_remove] == '\n' && 
                view->buffer->text[caret->position - num_to_remove - 1] == '\r')
            {
                num_to_remove++;
            }
        }
        if(write_undo)
        {
            e->strings[c].p = arena_copy(view->buffer->undo_buffer_arena, view->buffer->text + caret->position - num_to_remove, num_to_remove);
            e->strings[c].length = num_to_remove;
        }
        offset += num_to_remove;
        for(u32 i = caret->position - num_to_remove; i < view->buffer->text_size; i++)
        {
            view->buffer->text[i] = view->buffer->text[i + num_to_remove];
        }
        view->buffer->text_size -= num_to_remove;
        caret->position -= num_to_remove;
        caret->wish_column = caret_get_column(view->buffer, caret);
    }
    if(write_undo)
    {
        e->carets = arena_copy(view->buffer->undo_buffer_arena, view->carets, sizeof(caret) * view->num_carets),
        undo_buffer_push(view->buffer, e);
    }
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
    caret_move_down(view->buffer, &view->carets[view->num_carets]);
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
    caret_move_up(view->buffer, &view->carets[view->num_carets]);
    view->num_carets++;
}

internal void undo(rained_view *view)
{
    undo_buffer_entry *entry = view->buffer->undo_buffer_position;
    if(entry)
    {
        switch(entry->kind)
        {
            case TEXT_EDIT_DELETE:
            {
                memcpy(view->carets, entry->carets, sizeof(caret) * entry->num_carets);
                view->num_carets = entry->num_carets;
                text_edit_insert insert = 
                {
                    .strings = entry->strings,
                };
                carets_insert_characters(view, insert, 0);
                break;
            }
            case TEXT_EDIT_INSERT:
            {
                memcpy(view->carets, entry->carets, sizeof(caret) * entry->num_carets);
                view->num_carets = entry->num_carets;
                u32 *lengths = arena_push(global_state.frame_arena, sizeof(u32) * entry->num_carets, 8);
                for(u32 i = 0; i < entry->num_carets; i++)
                {
                    lengths[i] = entry->strings[i].length;
                }
                text_edit_delete delete = 
                {
                    .lengths = lengths,
                };
                carets_remove_characters(view, delete, 0);
                break;
            }
        }
        view->buffer->undo_buffer_position = entry->prev;
    }
}

internal void redo(rained_view *view)
{
    if(view->buffer->undo_buffer_position)
    {
        if(view->buffer->undo_buffer_position->next)
        {
            view->buffer->undo_buffer_position = view->buffer->undo_buffer_position->next;
        }
        else
        {
            return;
        }
    }
    else
    {
        view->buffer->undo_buffer_position = view->buffer->undo_buffer_tail;
    }
    undo_buffer_entry *entry = view->buffer->undo_buffer_position;
    if(entry)
    {
        switch(entry->kind)
        {
            case TEXT_EDIT_DELETE:
            {
                memcpy(view->carets, entry->carets, sizeof(caret) * entry->num_carets);
                view->num_carets = entry->num_carets;
                u32 *lengths = arena_push(global_state.frame_arena, sizeof(u32) * entry->num_carets, 8);
                for(u32 i = 0; i < entry->num_carets; i++)
                {
                    lengths[i] = entry->strings[i].length;
                }
                text_edit_delete delete = 
                {
                    .lengths = lengths,
                };
                u32 offset = 0;
                for(u32 i = 0; i < view->num_carets; i++)
                {
                    offset += entry->strings[i].length;
                    view->carets[i].position += offset;
                }
                carets_remove_characters(view, delete, 0);
                break;
            }
            case TEXT_EDIT_INSERT:
            {
                memcpy(view->carets, entry->carets, sizeof(caret) * entry->num_carets);
                view->num_carets = entry->num_carets;
                u32 offset = 0;
                for(u32 i = 0; i < view->num_carets; i++)
                {
                    offset += entry->strings[i].length;
                    view->carets[i].position -= offset;
                }
                text_edit_insert insert = 
                {
                    .strings = entry->strings,
                };
                carets_insert_characters(view, insert, 0);
                break;
            }
        }
    }
}

internal rained_buffer *open_buffer_from_file(rained_state *state, char *filename)
{
    arena *text_arena = arena_alloc(gb(1), mb(1));
    u32 file_size = 0;
    char *text = os_read_file(filename, &file_size, text_arena);
    rained_buffer *buffer = arena_push_struct_zero(state->forever_arena, rained_buffer);
    *buffer = (rained_buffer)
    {
        .text = text,
        .text_arena = text_arena,
        .text_size = file_size,
        .undo_buffer_arena = arena_alloc(gb(1), mb(1)),
    };
    sll_push(state->buffers, buffer);
    return buffer;
}

internal rained_view *create_view(rained_state *state, rained_buffer *buffer)
{
    rained_view *view = arena_push_struct_zero(state->forever_arena, rained_view);
    *view = (rained_view)
    {
        .num_carets = 1,
        .buffer = buffer,
    };
    sll_push(state->views, view);
    return view;
}

typedef struct
{
    chopped_line    *lines;
    u32             count;
} chopped_line_list;

internal chopped_line_list chop_lines(rained_view *view, u32 start, u32 num_to_chop, u32 num_lines, arena *arena)
{
    chopped_line_list res = 
    {
        .lines = arena_push(arena, 0, 64),
        .count = 0
    };
    u32 pos = start;
    u32 line_start = start;
    u32 line = 0;
    while(pos < view->buffer->text_size && res.count < num_to_chop && line < num_lines)
    {
        char c = view->buffer->text[pos];
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
        else if(pos - line_start >= view->width_cells - 1 && c > 31)
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

typedef struct
{
    renderer_command    *commands;
    u32                 screen_w, screen_h;
    rect                rect;

} draw_context;

internal void push_renderer_command(draw_context *ctx, renderer_command cmd)
{
    renderer_command *p = arena_push_struct(global_state.frame_arena, renderer_command);
    *p = cmd;
    sll_push(ctx->commands, p);
}

internal u32 find_line(rained_buffer *buffer, u32 line)
{
    u32 cur_line = 0;
    u32 line_start = 0;
    u32 i = 0;
    while(cur_line < line)
    {
        char c = buffer->text[i];
        if(c == '\n')
        {
            if(i + 1 >= buffer->text_size)
            {
                break;
            }
            cur_line++;
            line_start = i + 1;
        }
        i++;
        if(i == buffer->text_size)
        {
            break;
        }
    }
    return line_start;
}

internal void draw_view(draw_context *ctx, rained_view *view, f32 scroll_amount)
{
    u32 cell_width = 8;
    u32 cell_height = 18;

    view->width_cells = (ctx->rect.max_x - ctx->rect.min_x + cell_width - 1) / cell_width;
    view->height_cells = (ctx->rect.max_y - ctx->rect.min_y + cell_height - 1) / cell_height + 1;

    view->view_offset_pixels -= scroll_amount;
    
    while(view->view_offset_pixels < 0 && view->line_index)
    {
        view->line_index--;
        chopped_line_list l = chop_lines(view, find_line(view->buffer, view->line_index), -1, 1, global_state.frame_arena);
        view->view_offset_pixels = cell_height * l.count + view->view_offset_pixels;
    }

    while(1)
    {
        chopped_line_list l = chop_lines(view, find_line(view->buffer, view->line_index), -1, 1, global_state.frame_arena);
        
        if(view->view_offset_pixels / cell_height > l.count && l.count)
        {   
            view->line_index++;
            view->view_offset_pixels -= cell_height * l.count;
        }
        else
        {
            break;
        }
    }

    u32 text_color = 0x009F9F9F;
    u32 bg_color = 0x00000000;
    u32 caret_line_color = 0x001F1F1F;

    u32 cell_count = view->width_cells * view->height_cells; 
    cell *cells = arena_push(global_state.frame_arena, cell_count * sizeof(cell), 64);

    for(u32 i = 0; i < cell_count; i++)
    {
        cells[i] = (cell) 
        {
            .bg_color = bg_color
        };
    }

    i32 offset_lines = max(0, view->view_offset_pixels / cell_height);

    chopped_line_list chopped = chop_lines(view, find_line(view->buffer, view->line_index), view->height_cells + offset_lines, -1, global_state.frame_arena);

    for(u32 y = 0; y < min(view->height_cells, chopped.count); y++)
    {
        chopped_line *l = &chopped.lines[offset_lines + y];

        for(u32 j = 0; j < min(l->length, view->width_cells); j++)
        {
            char c = view->buffer->text[l->pos_in_text + j];
            u32 cell_index = j + y * view->width_cells;

            cells[cell_index] = (cell) 
            { 
                .atlas_index = c - 33,
                .text_color = text_color,
                .bg_color = bg_color,
            };
        }

        for(u32 k = 0; k < view->num_carets; k++)
        {
            caret *caret = &view->carets[k];
            if(caret->position >= l->pos_in_text && caret->position < l->pos_in_text + l->length)
            {
                for(u32 j = 0; j < view->width_cells; j++)
                {
                    cells[y * view->width_cells + j].bg_color = caret_line_color;
                }
            }
        }

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

    push_renderer_command(ctx, (renderer_command)
    {
        .code_view = 
        {
            .rect = ctx->rect,
            .cells = cells,
            .cell_width = cell_width,
            .cell_height = cell_height,
            .num_cells_x = view->width_cells,
            .num_cells_y = view->height_cells,
            .atlas_width_characters_x = 14,
            .atlas_height_characters_y = 7,
            .view_offset_pixels = view->view_offset_pixels < 0.0f ? view->view_offset_pixels : (i32)view->view_offset_pixels % cell_height,
            .vsync_line_position = os_time_us() / 1000 % ctx->screen_w,
        }
    });
}

internal renderer_command *draw(rained_input *input)
{
    static b32 did_init;
    if(!did_init)
    {
        global_state.forever_arena = arena_alloc(gb(1), mb(1));
        global_state.frame_arena = arena_alloc(gb(1), mb(1));
        did_init = 1;   

        rained_buffer *b0 = open_buffer_from_file(&global_state, "test.txt");
        rained_buffer *b1 = open_buffer_from_file(&global_state, "rained.c");
        rained_view *v0 = create_view(&global_state, b0);
        rained_view *v1 = create_view(&global_state, b1);
    }
    arena_reset(global_state.frame_arena);
    
    rained_view *view = global_state.views;

    for(u32 i = 0; i < input->input_queue_count; i++)
    {
        input_event e = input->input_queue[i];

        if(e.is_down)
        {
            if(e.type == INPUT_EVENT_KEY)
            {
                //if(!e.is_repeat)
                {
                    if(e.code == KEY_F11)
                    {
                        os_toggle_fullscreen();
                    }
                    if(e.code == KEY_THE_ONE_RIGHT_BELOW_ESCAPE)
                    {
                        view->carets[0] = view->carets[view->num_carets - 1];
                        view->num_carets = 1;
                    }
                }

                if(e.code == (KEY_DOWN | MODIFIER_ALT | MODIFIER_CTRL))
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
            }
        }

        if(e.type == INPUT_EVENT_TEXT)
        {
            if(e.character > 31 && e.character != '`') // i've lost my enter key
            {
                text_edit_insert insert = 
                {
                    .strings = arena_push(global_state.frame_arena, sizeof(string) * view->num_carets, 8)
                };
                for(u32 j = 0; j < view->num_carets; j++)
                {
                    insert.strings[j] = (string)
                    {
                        .p = &e.character,
                        .length = 1
                    };
                }
                carets_insert_characters(view, insert, 1);
            }
            else if(e.character == '\r')
            {
                char line_end[2] = { '\r', '\n' };
                text_edit_insert insert = 
                {
                    .strings = arena_push(global_state.frame_arena, sizeof(string) * view->num_carets, 8)
                };
                for(u32 j = 0; j < view->num_carets; j++)
                {
                    insert.strings[j] = (string)
                    {
                        .p = line_end,
                        .length = 2
                    };
                }
                carets_insert_characters(view, insert, 1);
            }
            else if(e.character == '\b')
            {
                text_edit_delete delete = 
                {
                    .lengths = arena_push(global_state.frame_arena, sizeof(u32) * view->num_carets, 8)
                };
                for(u32 j = 0; j < view->num_carets; j++)
                {
                    delete.lengths[j] = 1;
                }
                carets_remove_characters(view, delete, 1);
            }
        }

        for(u32 i = 0; i < view->num_carets; i++)
        {
            caret *caret = &view->carets[i];
            if(e.is_down)
            {
                if(e.type == INPUT_EVENT_KEY)
                {
                    if(e.code == KEY_RIGHT)
                    {
                        caret_move_right(view->buffer, caret);
                    }
                    else if(e.code == KEY_LEFT)
                    {
                        caret_move_left(view->buffer, caret);
                    }
                    else if(e.code == KEY_DOWN)
                    {
                        caret_move_down(view->buffer, caret);
                    }
                    else if(e.code == KEY_UP)
                    {
                        caret_move_up(view->buffer, caret);
                    }
                }
            }
        }

        merge_overlapping_carets_in_a_slow_way(view);
    }

    draw_context ctx = 
    {
        .screen_w = input->screen_w,  
        .screen_h = input->screen_h
    };
    ctx.rect = (rect)
    {
        .max_x = input->screen_w / 2,
        .max_y = input->screen_h
    };
    draw_view(&ctx, view, input->mouse_wheel_delta);
    ctx.rect = (rect)
    {
        .min_x = input->screen_w / 2,
        .max_x = input->screen_w,
        .max_y = input->screen_h
    };
    draw_view(&ctx, view->next, input->mouse_wheel_delta);

    return ctx.commands;
}