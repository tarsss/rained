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
    uint cell_width;
    uint cell_height;
    uint num_cells_x;
    uint num_cells_y;
    uint atlas_width_characters_x;
    uint atlas_height_characters_y;
    int view_offset_pixels;
    uint vsync_line_position;
    int pointer_x, pointer_y;
    int caret_x, caret_y;
}

#define rgb(a,b,c) (float3(a,b,c) / 255.0f)

uint get_cell_index(int2 pos)
{
    uint2 cell_pos = pos / uint2(cell_width, cell_height);
    return cell_pos.x + cell_pos.y * num_cells_x;
}

float3 unpack_color(uint rgba)
{
    float r = (rgba & 0x000000FF) / 256.0f;
    float g = (rgba & 0x0000FF00 >> 8) / 256.0f;
    float b = (rgba & 0x00FF0000 >> 16) / 256.0f;
    return float3(r,g,b);
}

[numthreads(8,8,1)]
void shader_cs(uint2 pixel_pos : SV_DispatchThreadID)
{

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

    uint2 offset_pixel_pos = pixel_pos;
    offset_pixel_pos.y += view_offset_pixels;
    
    // todo: once we do proper atlas generation, there will be 1:1 match in pixel/texel sizes and you won't have to remap, simple integer math instead
    uint cell_index = get_cell_index(offset_pixel_pos);
    cell c = cells[cell_index];

    float3 text_color = unpack_color(c.text_color);
    float3 bg_color = unpack_color(c.bg_color);

    // highlight the hovered cell
#if 0
    if(cell_index == get_cell_index(int2(pointer_x,pointer_y + view_offset_pixels)))
    {
        bg_color = 1.0f - bg_color;
        text_color = 1.0f - text_color;
    }
#endif

    // draw the caret and the line
#if 1
    uint2 d = (int2)pixel_pos - int2(caret_x, caret_y);
    if(d.y < cell_height && d.x < cell_width)
    {
        bg_color = 1.0f - bg_color;
        text_color = 1.0f - text_color;
    }
#endif

    if(c.index == 0)
    {
        output[pixel_pos] = float4(bg_color,1);
        return;
    }

    float2 atlas_character_pos = float2(c.index % atlas_width_characters_x, c.index / atlas_width_characters_x);
    float2 atlas_character_uv = atlas_character_pos / float2(atlas_width_characters_x, atlas_height_characters_y);
    float2 character_uv = (offset_pixel_pos % uint2(cell_width, cell_height)) / float2(cell_width, cell_height);
    float2 uv = atlas_character_uv + character_uv / float2(atlas_width_characters_x, atlas_height_characters_y);

    // note: our texture is flipped! you better just flip it on load instead of messing up with spaces.
    uv.y = 1.0f - uv.y;

    float tex = atlas.SampleLevel(atlas_sampler, uv, 1).x;
    tex = tex > 0.3f ? 1.0f : 0.0f; // todo note removeme
    output[pixel_pos] = float4(lerp(bg_color, text_color, tex), 1);
    
    //output[pixel_pos] = atlas.SampleLevel(atlas_sampler, uv, 1) * float4(text_color, 1);
}