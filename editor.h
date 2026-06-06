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

} INPUT_EVENT;

typedef enum KEY_MODIFIER
{
    MODIFIER_SHIFT = 256,
    MODIFIER_CTRL  = 512,
    MODIFIER_ALT   = 1024,

} KEY_MODIFIER;

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
    };

} input_event;

typedef struct
{
    u32 cell_width;
    u32 cell_height;
    u32 num_cells_x;
    u32 num_cells_y;
    u32 atlas_width_characters_x;
    u32 atlas_height_characters_y;
    i32 view_offset_pixels;
    u32 vsync_line_position;
    i32 pointer_x;
    i32 pointer_y;

} cbuffer_t;

typedef struct
{
    u32 atlas_index;
    u32 text_color;
    u32 bg_color;

} cell;

typedef struct
{
    u32 line_in_text;
    u32 pos_in_text;
    u32 length;

} chopped_line;


typedef struct
{
    u32 position;
    u32 wish_column;
    
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