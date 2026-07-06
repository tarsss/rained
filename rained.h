
#ifndef RAINED_H
#define RAINED_H

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"
#include <stdint.h>

#ifdef ASAN_ENABLED
    void __asan_poison_memory_region(void const volatile *addr, size_t size);
    void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
    #define ASAN_POISON_REGION(addr, size) __asan_poison_memory_region(addr, size);
    #define ASAN_UNPOISON_REGION(addr, size) __asan_unpoison_memory_region(addr, size);
#else
    #define ASAN_POISON_REGION(addr, size)
    #define ASAN_UNPOISON_REGION(addr, size)
#endif

#define assert(a) if(!(a)) { *(volatile int*)0 = 0; }
#define assert_hr(a) assert(SUCCEEDED(a))
#define alignof(n) (_Alignof(n))
#define lengthof(arr) (sizeof(arr) / sizeof(arr[0]))
#define internal static
#define global static

typedef uint8_t     u8;
typedef int8_t      i8;
typedef uint16_t    u16;
typedef int16_t     i16;
typedef uint32_t    u32;
typedef int32_t     i32;
typedef uint64_t    u64;
typedef int64_t     i64;
typedef float       f32;
typedef double      f64;
typedef int8_t      b8;
typedef int32_t     b32;

#define kb(n) ((u64)n << 10)
#define mb(n) ((u64)n << 20)
#define gb(n) ((u64)n << 30)

#define min(a,b) (a < b ? a : b)
#define max(a,b) (a > b ? a : b)

#define tb(n) ((u64)n << 40)

#define PAGE_SIZE 4096

u32 ceil_pow2_u32(u32 n)
{
    if (n <= 1) 
    {
        return 1;
    }
    u32 p = 1 << (32 - __lzcnt(n - 1));
    return p;
}

#ifdef SPALL_ENABLED
#include "spall.h"
static SpallProfile spall_profile;
static SpallBuffer spall_buffer;
static HANDLE spall_file;
internal void spall_begin(char *name)
{
    // cant you get the size of the string at compile time?
    spall_buffer_begin(&spall_profile, &spall_buffer, name, cstring_length(name), os_time_us());
}
internal void spall_end()
{
    spall_buffer_end(&spall_profile, &spall_buffer, os_time_us());
}
SPALL_NOINSTRUMENT bool spall_callback_write(SpallProfile *sp, const void *data, size_t length)
{
    return WriteFile(spall_file, data, length, 0, 0);
}
SPALL_NOINSTRUMENT bool spall_callback_flush(SpallProfile *sp)
{
    return 1;
}
SPALL_NOINSTRUMENT void spall_callback_close(SpallProfile *sp)
{
    assert(CloseHandle(spall_file));
}
internal void spall_begin_profiling()
{
    spall_file = CreateFileA("profile.spall", 
        GENERIC_WRITE | FILE_APPEND_DATA, 
        FILE_SHARE_READ,
        0,
        CREATE_ALWAYS, 
        FILE_ATTRIBUTE_NORMAL, 
        0);

    assert(spall_file);

    spall_init_callbacks(1000, &spall_callback_write, &spall_callback_flush, &spall_callback_close, 0, &spall_profile);

    u64 buffer_size = mb(1);
    void *mem;
    os_mem_reserve(buffer_size, &mem);
    os_mem_commit(mem, buffer_size);
    memset(mem, 'd', buffer_size);
    spall_buffer = (SpallBuffer)
    {
        .length = buffer_size,
        .data = mem
    };
    assert(spall_buffer_init(&spall_profile, &spall_buffer));
}
internal void spall_end_profiling()
{
    spall_buffer_quit(&spall_profile, &spall_buffer);
    os_mem_free(spall_buffer.data);
    spall_quit(&spall_profile);
}
#define PROFILE_BEGIN(name) spall_begin(name)
#define PROFILE_END() spall_end();
#else
#define PROFILE_BEGIN(name)
#define PROFILE_END()
#endif

typedef struct
{
    void    *base;
    u64     reserved;
    u64     commited;
    u64     used;

} arena, arena_t;

#pragma pack(push, 1)
typedef struct
{
    u16     FileType;        /* File type, always 4D42h ("BM") */
	u32     FileSize;        /* Size of the file in bytes */
	u16     Reserved1;       /* Always 0 */
	u16     Reserved2;       /* Always 0 */
	u32     BitmapOffset;    /* Starting position of image data in bytes */
	u32     Size;            /* Size of this header in bytes */
	u32     Width;           /* Image width in pixels */
	u32     Height;          /* Image height in pixels */
	u16     Planes;          /* Number of color planes */
	u16     BitsPerPixel;    /* Number of bits per pixel */
	u32     Compression;     /* Compression methods used */
	u32     SizeOfBitmap;    /* Size of bitmap in bytes */
} bitmap_header;
#pragma pack(pop)

typedef struct
{
    bitmap_header   *header;
    void            *data;

} loaded_bitmap;

typedef enum INPUT_EVENT
{
    INPUT_EVENT_ERROR = 0,
    INPUT_EVENT_KEY,
    INPUT_EVENT_TEXT,
    INPUT_EVENT_MOUSE_BUTTON,

} INPUT_EVENT;

typedef enum KEY_MODIFIER
{
    MODIFIER_SHIFT = 256,
    MODIFIER_CTRL  = 512,
    MODIFIER_ALT   = 1024,

} KEY_MODIFIER;

typedef enum KEY_CODE
{
    KEY_SHIFT = 0x10,
    KEY_CTRL = 0x11,
    KEY_ALT = 0x12,
    KEY_ENTER = 0x0D,
    KEY_PAGEUP = 0x21,
    KEY_PAGEDOWN = 0x22,
    KEY_END = 0x23,
    KEY_HOME = 0x24,
    KEY_LEFT = 0x25,
    KEY_UP = 0x26,
    KEY_RIGHT = 0x27,
    KEY_DOWN = 0x28,
    KEY_F1 = 0x70,
    KEY_F2 = 0x71,
    KEY_F3 = 0x72,
    KEY_F4 = 0x73,
    KEY_F5 = 0x74,
    KEY_F6 = 0x75,
    KEY_F7 = 0x76,
    KEY_F8 = 0x77,
    KEY_F9 = 0x78,
    KEY_F10 = 0x79,
    KEY_F11 = 0x7A,
    KEY_F12 = 0x7B,
    KEY_THE_ONE_TO_THE_LEFT_OF_A_RIGHT_SHIFT = 0xBF,
    KEY_PLUS = 0xBB,
    KEY_COMMA = 0xBC,
    KEY_MINUS = 0xBD,
    KEY_PERIOD = 0xBE,
    KEY_THE_ONE_RIGHT_BELOW_ESCAPE = 0xC0,
    
    
} KEY_CODE;

typedef struct
{
    b8 is_down;
    b8 was_down;

} mouse_button_event;

typedef struct
{
    INPUT_EVENT     type;
    union
    {
        struct
        {
            u16     code;
            b8      is_repeat;
            b8      is_down;
        };
        char        character;
        struct
        {
            mouse_button_event lmb;
            mouse_button_event rmb;
            mouse_button_event mmb;
            u32 x, y;
        };
    };

} input_event;

typedef struct
{
    u32 atlas_index;
    u32 text_color;
    u32 bg_color;

} cell;

typedef struct
{
    u32 line_index;
    u32 pos_in_text;
    u32 length;

} chopped_line;

typedef struct
{
    chopped_line    *lines;
    u32             count;
} chopped_line_list;

typedef struct
{
    u32 position;
    u32 wish_column;
    u32 selection_pos;
    b32 selection_active;
    
} caret;

typedef enum
{
    TEXT_EDIT_INSERT,
    TEXT_EDIT_DELETE,

} text_edit_kind;

typedef struct
{
    u32     *lengths;

} text_edit_delete;

typedef struct
{
    char    *p;
    u32     length;

} string;

typedef struct
{
    string  *strings;

} text_edit_insert;

typedef struct
{
    input_event *input_queue;
    u32         input_queue_count;
    i32         mouse_x, mouse_y;
    b8          lmb, rmb, mmb;
    f32         mouse_wheel_delta;
    u32         screen_w, screen_h;
    u64         frame_start;

} rained_input;

typedef struct
{
    i32 min_x, min_y, max_x, max_y;
} rect;

internal b32 rect_contains_point(rect rect, i32 x, i32 y)
{
    return x >= rect.min_x && x <= rect.max_x && y >= rect.min_y && y <= rect.max_y;
}

typedef struct
{
    u64 stuff[2];
    
} os_font_atlas_handle;

typedef struct
{
    u32 size;
    u32 atlas_width_glyphs;
    u32 glyph_width;
    u32 glyph_height;
    os_font_atlas_handle os_handle;

} font_atlas;

typedef enum renderer_command_kind
{
    RENDERER_COMMAND_CODE_VIEW,
    RENDERER_COMMAND_RECT,

} renderer_command_kind;

typedef struct renderer_command renderer_command;
struct renderer_command
{
    renderer_command        *next;
    renderer_command_kind   kind;
    
    rect rect;
    union
    {
        struct
        {
            font_atlas font;
            cell *cells;
            u32 num_cells_x;
            u32 num_cells_y;
            i32 view_offset_pixels;
            u32 vsync_line_position;
            i32 pointer_x;
            i32 pointer_y;
        } code_view;
        struct
        {
            u32 color;
        } quad;
    };
};

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
    string              path;
    arena               *text_arena;
    union
    {
        struct
        {
            char        *text;
            u32         text_size;
        };
        string          text_string;
    };
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
    f32                 y_offset_pixels;
    caret               carets[1024];
    u32                 num_carets;
    rained_view         *next;
    b32                 is_a_command_view; // note: mfgghhhhhhhh? idk.
    b32                 fit_caret;
    u32                 width_cells;
    u32                 height_cells;
};

typedef struct rained_tile rained_tile;
struct rained_tile
{
    rect                rect;
    rained_view         *view;
    rained_tile         *next;
};

internal void os_mem_reserve(u64 size, void **address);
internal void os_mem_commit(void *reserved, u64 size);
internal void os_mem_free(void *base);
internal u64 os_time_us();
internal void *os_read_file(char *path, u32 *out_size, arena_t *arena);
internal void os_write_file(char *path, void *stuff, u64 size_bytes);
internal void os_toggle_fullscreen();
internal string os_clipboard_get(arena *arena);
internal void os_clipboard_set(string text);
internal font_atlas os_make_font_atlas(u32 size, arena *scratch);
internal void os_release_font_atlas(font_atlas atlas);
internal void os_debug_output_string(char *str);

internal u32 chopped_line_list_find_position(chopped_line_list list, u32 p);
internal chopped_line_list chop_lines(rained_buffer *buffer, u32 width_cells, u32 start, u32 num_to_chop, u32 num_lines, arena *arena);
internal u32 find_line_start(rained_buffer *buffer, u32 p);

#endif