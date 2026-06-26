struct cell
{
    uint index;
    uint text_color;
    uint bg_color;
};

RWTexture2D<float4>     output          : register(u0);
sampler                 atlas_sampler   : register(s0);
Texture2D<float4>       atlas           : register(t0);
StructuredBuffer<cell>  cells           : register(t1);

cbuffer cb : register(b0)
{
    int rect_min_x;
    int rect_min_y;
    int rect_max_x;
    int rect_max_y;
    uint cell_width;
    uint cell_height;
    uint num_cells_x;
    uint num_cells_y;
    uint atlas_width_glyphs;
    int view_offset_pixels;
    uint vsync_line_position;
    int pointer_x; 
    int pointer_y;
}

#define rgb(a,b,c) (float3(a,b,c) / 255.0f)

uint get_cell_index(int2 pos)
{
    uint2 cell_pos = pos / uint2(cell_width, cell_height);
    return cell_pos.x + cell_pos.y * num_cells_x;
}

float4 unpack_color(int rgba)
{
    float r = (rgba & 0x000000FF);
    float g = (rgba & 0x0000FF00) >> 8;
    float b = (rgba & 0x00FF0000) >> 16;
    float a = (rgba & 0xFF000000) >> 24;
    return float4(r,g,b,a) / 255.0f;
}

[numthreads(8,8,1)]
void shader_cs(int2 thread_id : SV_DispatchThreadID)
{
    int2 pixel_pos = thread_id + int2(rect_min_x, rect_min_y);
    if(pixel_pos.x < 0 || pixel_pos.y < 0 || pixel_pos.x >= rect_max_x || pixel_pos.y >= rect_max_y)
    {
        return;
    }
    
    // draw a debug vertical line thingy to see tears better.
#if 0
    if(pixel_pos.x > vsync_line_position && pixel_pos.x < vsync_line_position + 2)
    {
        output[pixel_pos] = float4(1,0,0,1);
        return;
    }
#endif

    // draw the pointer/crossline/whatever for latency test.
#if 0
    int2 dif = int2(pointer_x, pointer_y) - pixel_pos;
    //if(all(abs(dif) < 8))
    if(any(abs(dif) < 8))
    {
        text_color = 1.0f - text_color;
        bg_color = 1.0f - bg_color;
    }
#endif

    uint2 offset_pixel_pos = thread_id;
    offset_pixel_pos.y += view_offset_pixels;
    
    uint cell_index = get_cell_index(offset_pixel_pos);
    cell c = cells[cell_index];

    float4 text_color = unpack_color(c.text_color);
    float4 bg_color = unpack_color(c.bg_color);

    if(c.index == 0)
    {
        output[pixel_pos] = bg_color;
        return;
    }

    int2 atlas_character_pos = int2(c.index % atlas_width_glyphs * cell_width, c.index / atlas_width_glyphs * cell_height);
    int2 atlas_pos = atlas_character_pos + offset_pixel_pos % int2(cell_width, cell_height);
    float4 tex = atlas.Load(int3(atlas_pos, 0), int2(0,0));
    
    output[pixel_pos] = lerp(bg_color, text_color, tex);
}