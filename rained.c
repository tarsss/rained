#include "rained.h"

u64 caret_last_move_time;

arena   *forever_arena;
arena   *frame_arena;

// view
f32                         view_offset_pixels;
caret                       carets[1024];
u32                         num_carets = 1;

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

// buffer
arena   *text_arena;
char    *text;
u32     text_size;
arena   *undo_buffer_arena;
undo_buffer_entry *undo_buffer_tail;
undo_buffer_entry *undo_buffer_position;

typedef struct
{

} code_view;

int _fltused;

internal void *memset(void *dest, int c, size_t count)
{
    char *bytes = (char *)dest;
    while (count--)
    {
        *bytes++ = (char)c;
    }
    return dest;
}

internal void *memcpy(void *dest, const void *src, size_t count)
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
#define arena_push_struct_zero(a, s) (memset(arena_push(a, sizeof(s), 8), 0, sizeof(s)))
#define arena_copy(a, p, s) (memcpy(arena_push(a, s, 8), p, s))

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

internal u32 caret_get_column(caret *caret)
{
    for(u32 i = caret->position; i >= 0; i--)
    {
        if(i == 0)
        {
            return caret->position - i;
        }
        if(text[i] == '\n')
        {
            return caret->position - i - 1;
        }
    }
    return 0;
}

internal void merge_overlapping_carets_in_a_slow_way()
{
    u32 n = 0;
    for(u32 i = 0; i < num_carets; i++)
    {
        b32 overlapped = 0;
        for(i32 j = n; j >= 0; j--)
        {
            if(i != j && carets[i].position == carets[j].position)
            {
                overlapped = 1;
                break;
            }
        }
        if(!overlapped)
        {
            carets[n] = carets[i];
            n++;
        }
    }  
    num_carets = n;
}

internal void caret_move_right(caret *caret)
{
    u32 p = caret->position;
    while(caret->position != text_size)
    {
        char c = text[caret->position];
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
    caret->wish_column = caret_get_column(caret);
    
}

internal void caret_move_left(caret *caret)
{
    while(caret->position)
    {
        caret->position--;
        char c = text[caret->position];
        if(c == '\r')
        {
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret->wish_column = caret_get_column(caret);
    
}

internal void caret_move_to_prev_line(caret *caret)
{
    while(caret->position)
    {
        if(text[caret->position] == '\n')
        {
            caret->position--;
            break;
        }
        caret->position--;
    }
}

internal void caret_move_to_line_start(caret *caret)
{
    while(1)
    {
        if(caret->position == 0 || text[caret->position - 1] == '\n')
        {
            break;
        }
        caret->position--;
    }
}

internal void caret_go_to_column(caret *caret, u32 col)
{
    caret_move_to_line_start(caret);
    u32 c = 0;
    while(c != col && caret->position != text_size)
    {
        if(text[caret->position] == '\r')
        {
            break;
        }
        caret->position++;
        c++;
    }
}

internal void caret_move_to_next_line(caret *caret)
{
    while(caret->position != text_size)
    {
        if(text[caret->position] == '\n')
        {
            caret->position++;
            break;
        }
        caret->position++;
    }
}

internal void caret_move_up(caret *caret)
{
    caret_move_to_prev_line(caret);
    caret_go_to_column(caret, caret->wish_column);
    
}

internal void caret_move_down(caret *caret)
{
    caret_move_to_next_line(caret);
    caret_go_to_column(caret, caret->wish_column);
    
}

internal void carets_bubble_sort_top_to_bottom()
{
    for(;;)
    {
        u32 num_swaps = 0;
        for(u32 i = 0; i < num_carets - 1; i++)
        {
            caret a = carets[i];
            caret b = carets[i + 1];
            if(a.position > b.position)
            {
                carets[i] = b;
                carets[i + 1] = a;
                num_swaps++; 
            }
        }
        if(num_swaps == 0)
        {
            break;
        }
    }
}

internal void undo_buffer_push(undo_buffer_entry *entry)
{
    if(undo_buffer_position)
    {
        undo_buffer_position->next = entry;
        entry->prev = undo_buffer_position;
        undo_buffer_position = entry;
    }
    else
    {
        undo_buffer_position = entry;
        undo_buffer_tail = entry;
    }
}

internal void carets_insert_characters(text_edit_insert insert, b32 write_undo)
{
    carets_bubble_sort_top_to_bottom();
    u32 offset = 0;
    for(u32 c = 0; c < num_carets; c++)
    {
        string s = insert.strings[c];
        caret *caret = &carets[c];
        if(s.length)
        {
            caret->position += offset;
            offset += s.length;
            u32 p = caret->position;
            for(u32 i = text_size + s.length - 1; i >= p + s.length; i--)
            {
                text[i] = text[i - s.length];
            }
            for(u32 i = 0; i < s.length; i++)
            {
                text[p + i] = s.p[i];
            }
            text_size += s.length;
            caret->position += s.length;
        }
        caret->wish_column = caret_get_column(caret);
    }
    if(write_undo)
    {
        undo_buffer_entry *e = arena_push_struct(undo_buffer_arena, undo_buffer_entry);
        *e = (undo_buffer_entry)
        {
            .kind = TEXT_EDIT_INSERT,
            .carets = arena_copy(undo_buffer_arena, carets, sizeof(caret) * num_carets),
            .num_carets = num_carets
        };
        e->strings = arena_push(undo_buffer_arena, num_carets * sizeof(string), 8);
        for(u32 i = 0; i < num_carets; i++)
        {
            e->strings[i].length = insert.strings[i].length;
            e->strings[i].p = arena_copy(undo_buffer_arena, insert.strings[i].p, insert.strings[i].length);
        }
        undo_buffer_push(e);
    }
}

// todo: don't push undo when each num_to_remove = 0
internal void carets_remove_characters(text_edit_delete delete, b32 write_undo)
{
    undo_buffer_entry *e = arena_push_struct(undo_buffer_arena, undo_buffer_entry);
    if(write_undo)
    {
        *e = (undo_buffer_entry)
        {
            .kind = TEXT_EDIT_DELETE,
            .strings = arena_push_zero(undo_buffer_arena, sizeof(string) * num_carets, 8),
            .num_carets = num_carets,
        };
    }
    carets_bubble_sort_top_to_bottom();
    u32 offset = 0;
    for(u32 c = 0; c < num_carets; c++)
    {
        caret *caret = &carets[c];
        caret->position -= offset;
        u32 num_to_remove = min(delete.lengths[c], caret->position);
        // dont leave a stray /r fuhhhh.
        if(caret->position >= num_to_remove + 1)
        {
            if(text[caret->position - num_to_remove] == '\n' && 
                text[caret->position - num_to_remove - 1] == '\r')
            {
                num_to_remove++;
            }
        }
        if(write_undo)
        {
            e->strings[c].p = arena_copy(undo_buffer_arena, text + caret->position - num_to_remove, num_to_remove);
            e->strings[c].length = num_to_remove;
        }
        offset += num_to_remove;
        for(u32 i = caret->position - num_to_remove; i < text_size; i++)
        {
            text[i] = text[i + num_to_remove];
        }
        text_size -= num_to_remove;
        caret->position -= num_to_remove;
        caret->wish_column = caret_get_column(caret);
    }
    if(write_undo)
    {
        e->carets = arena_copy(undo_buffer_arena, carets, sizeof(caret) * num_carets),
        undo_buffer_push(e);
    }
}

internal void caret_spawn_new_below()
{
    caret bottom_caret = carets[0];
    for(u32 i = 0; i < num_carets; i++)
    {
        if(carets[i].position > bottom_caret.position)
        {
            bottom_caret = carets[i];
        }
    }
    carets[num_carets] = bottom_caret;
    caret_move_down(&carets[num_carets]);
    num_carets++;
}

internal void caret_spawn_new_above()
{
    caret top_caret = carets[0];
    for(u32 i = 0; i < num_carets; i++)
    {
        if(carets[i].position < top_caret.position)
        {
            top_caret = carets[i];
        }
    }
    carets[num_carets] = top_caret;
    caret_move_up(&carets[num_carets]);
    num_carets++;
}

internal void undo()
{
    undo_buffer_entry *entry = undo_buffer_position;
    if(entry)
    {
        switch(entry->kind)
        {
            case TEXT_EDIT_DELETE:
            {
                memcpy(carets, entry->carets, sizeof(caret) * entry->num_carets);
                num_carets = entry->num_carets;
                text_edit_insert insert = 
                {
                    .strings = entry->strings,
                };
                carets_insert_characters(insert, 0);
                break;
            }
            case TEXT_EDIT_INSERT:
            {
                memcpy(carets, entry->carets, sizeof(caret) * entry->num_carets);
                num_carets = entry->num_carets;
                u32 *lengths = arena_push(frame_arena, sizeof(u32) * entry->num_carets, 8);
                for(u32 i = 0; i < entry->num_carets; i++)
                {
                    lengths[i] = entry->strings[i].length;
                }
                text_edit_delete delete = 
                {
                    .lengths = lengths,
                };
                carets_remove_characters(delete, 0);
                break;
            }
        }
        undo_buffer_position = entry->prev;
    }
}

internal void redo()
{
    if(undo_buffer_position)
    {
        if(undo_buffer_position->next)
        {
            undo_buffer_position = undo_buffer_position->next;
        }
        else
        {
            return;
        }
    }
    else
    {
        undo_buffer_position = undo_buffer_tail;
    }
    undo_buffer_entry *entry = undo_buffer_position;
    if(entry)
    {
        switch(entry->kind)
        {
            case TEXT_EDIT_DELETE:
            {
                memcpy(carets, entry->carets, sizeof(caret) * entry->num_carets);
                num_carets = entry->num_carets;
                u32 *lengths = arena_push(frame_arena, sizeof(u32) * entry->num_carets, 8);
                for(u32 i = 0; i < entry->num_carets; i++)
                {
                    lengths[i] = entry->strings[i].length;
                }
                text_edit_delete delete = 
                {
                    .lengths = lengths,
                };
                u32 offset = 0;
                for(u32 i = 0; i < num_carets; i++)
                {
                    offset += entry->strings[i].length;
                    carets[i].position += offset;
                }
                carets_remove_characters(delete, 0);
                break;
            }
            case TEXT_EDIT_INSERT:
            {
                memcpy(carets, entry->carets, sizeof(caret) * entry->num_carets);
                num_carets = entry->num_carets;
                u32 offset = 0;
                for(u32 i = 0; i < num_carets; i++)
                {
                    offset += entry->strings[i].length;
                    carets[i].position -= offset;
                }
                text_edit_insert insert = 
                {
                    .strings = entry->strings,
                };
                carets_insert_characters(insert, 0);
                break;
            }
        }
    }
}

internal renderer_command *draw(rained_input *input)
{
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
                        carets[0] = carets[num_carets - 1];
                        num_carets = 1;
                    }
                }

                if(e.code == (KEY_DOWN | MODIFIER_ALT | MODIFIER_CTRL))
                {
                    caret_spawn_new_below();
                }
                else if(e.code == (KEY_UP | MODIFIER_ALT | MODIFIER_CTRL))
                {
                    caret_spawn_new_above();
                }
                else if(e.code == ('Z' | MODIFIER_CTRL))
                {
                    undo();
                }
                else if(e.code == ('Y' | MODIFIER_CTRL))
                {
                    redo();
                }
            }
        }

        if(e.type == INPUT_EVENT_TEXT)
        {
            if(e.character > 31 && e.character != '`') // i've lost my enter key
            {
                text_edit_insert insert = 
                {
                    .strings = arena_push(frame_arena, sizeof(string) * num_carets, 8)
                };
                for(u32 j = 0; j < num_carets; j++)
                {
                    insert.strings[j] = (string)
                    {
                        .p = &e.character,
                        .length = 1
                    };
                }
                carets_insert_characters(insert, 1);
            }
            else if(e.character == '\r')
            {
                char line_end[2] = { '\r', '\n' };
                text_edit_insert insert = 
                {
                    .strings = arena_push(frame_arena, sizeof(string) * num_carets, 8)
                };
                for(u32 j = 0; j < num_carets; j++)
                {
                    insert.strings[j] = (string)
                    {
                        .p = line_end,
                        .length = 2
                    };
                }
                carets_insert_characters(insert, 1);
            }
            else if(e.character == '\b')
            {
                text_edit_delete delete = 
                {
                    .lengths = arena_push(frame_arena, sizeof(u32) * num_carets, 8)
                };
                for(u32 j = 0; j < num_carets; j++)
                {
                    delete.lengths[j] = 1;
                }
                carets_remove_characters(delete, 1);
            }
        }

        for(u32 i = 0; i < num_carets; i++)
        {
            caret *caret = &carets[i];
            if(e.is_down)
            {
                if(e.type == INPUT_EVENT_KEY)
                {
                    if(e.code == KEY_RIGHT)
                    {
                        caret_move_right(caret);
                    }
                    else if(e.code == KEY_LEFT)
                    {
                        caret_move_left(caret);
                    }
                    else if(e.code == KEY_DOWN)
                    {
                        caret_move_down(caret);
                    }
                    else if(e.code == KEY_UP)
                    {
                        caret_move_up(caret);
                    }
                }
            }
        }

        merge_overlapping_carets_in_a_slow_way();
    }

    ////////////////////////////////////////////////////////////////////////////////
    ////////////////////////////////////////////////////////////////////////////////

    u32 cell_width = 8;
    u32 cell_height = 18;

    u32 width = (input->screen_w + cell_width - 1) / cell_width;
    u32 height = (input->screen_h + cell_height - 1) / cell_height + 1;
    u32 cell_count = width * height; 

    cell *cells = arena_push(frame_arena, cell_count * sizeof(cell), 64);
    memset(cells, 0, cell_count * sizeof(cell));

    u32 topmost_visible_line = max(0, view_offset_pixels / cell_height);
    u32 text_position = 0;
    
    // dumb search for the first line from the beginning of the buffer
    {
        u32 line = 0;
        u32 line_start = 0;
        u32 i = 0;
        while(line < topmost_visible_line)
        {
            char c = text[i];
            if(c == '\n')
            {
                if(i + 1 >= text_size)
                {
                    break;
                }
                line++;
                line_start = i + 1;
            }
            i++;
            if(i == text_size)
            {
                break;
            }
        }
        text_position = line_start;
    }

    chopped_line *lines = arena_push(frame_arena, sizeof(chopped_line) * height, 8);
    u32 num_lines = 0;
    
    u32 column = 0;
    u32 line = 0;
    u32 line_start = text_position;

    while(line < height && text_position < text_size && num_lines < height)
    {
        char c = text[text_position];

        if(c == '\n')
        {
            lines[num_lines] = (chopped_line)
            {
                .line_in_text = topmost_visible_line + line,
                .pos_in_text = line_start,
                .length = text_position - line_start,
            };
            line_start = text_position + 1;
            num_lines++;
            line++;
            column = 0;
        }
        else
        {
            if(text_position - line_start >= width - 1 && c > 31)
            {
                lines[num_lines] = (chopped_line)
                {
                    .line_in_text = topmost_visible_line + line,
                    .pos_in_text = line_start,
                    .length = text_position - line_start,
                };
                line_start = text_position;
                num_lines++;
            }

            if(c != '\r')
            {
                column++;
            }
        }

        text_position++;
    }

    b32 caret_blink = 0;
    u32 caret_period = 1000 * 1000;
    if((input->frame_start - caret_last_move_time) % caret_period > caret_period / 2)
    {
        caret_blink = 1;
    }

    for(u32 i = 0; i < num_lines; i++)
    {
        chopped_line *l = &lines[i];
        for(u32 j = 0; j < min(l->length, width); j++)
        {
            char c = text[l->pos_in_text + j];
            u32 cell_index = j + i * width;
            u32 text_color = 0x00FFFFFF;
            u32 bg_color = 0x00000000;

            cells[cell_index] = (cell) 
            { 
                .atlas_index = c - 33,
                .text_color = text_color,
                .bg_color = bg_color,
            };
        }

        for(u32 k = 0; k < num_carets; k++)
        {
            caret *caret = &carets[k];
            if(caret->position >= l->pos_in_text && caret->position < l->pos_in_text + l->length)
            {
                for(u32 j = 0; j < width; j++)
                {
                    cells[i * width + j].bg_color = 0x001F1F1F;
                }
            }
        }

        for(u32 k = 0; k < num_carets; k++)
        {
            caret *caret = &carets[k];
            if(caret->position >= l->pos_in_text && caret->position < l->pos_in_text + l->length)          
            {      
                if(!caret_blink)
                {
                    cell *c = &cells[caret->position - l->pos_in_text + i * width];
                    c->bg_color = ~c->bg_color;
                    c->text_color = ~c->text_color;
                }
            }
        }
    }

    u32 view_offset_cell = view_offset_pixels < 0.0f ? view_offset_pixels : (i32)view_offset_pixels % cell_height;

    renderer_command *cmd = arena_push_struct(frame_arena, renderer_command);
    *cmd = (renderer_command)
    {
        .code_view = 
        {
            .cell_width = cell_width,
            .cell_height = cell_height,
            .num_cells_x = width,
            .num_cells_y = height,
            .atlas_width_characters_x = 14,
            .atlas_height_characters_y = 7,
            .view_offset_pixels = view_offset_cell,
            .vsync_line_position = os_time_us() / 1000 % input->screen_w,
            .pointer_x = input->mouse_x,
            .pointer_y = input->mouse_y
        }
    };
    return cmd;
}