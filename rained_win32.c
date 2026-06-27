#include "rained.c"

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include "cdwrite.h"
#include "shader.h"

HWND                        window;
WINDOWPLACEMENT             windowedPlacement;
HDC                         hdc;
ID3D11Device                *device;
ID3D11DeviceContext         *device_context;
IDXGISwapChain1             *swapchain;
ID3D11RenderTargetView      *rt_view;
ID3D11UnorderedAccessView   *uav;
u32                         screen_w;
u32                         screen_h;
u32                         new_screen_w;
u32                         new_screen_h;
u32                         swapchain_flags;
u32                         frame_count;
u64                         perf_counter_freq;
b8                          quit;
i16                         mouse_wheel_delta_accum;
f32                         mouse_wheel_delta;
i32                         mouse_x, mouse_y;
char                        buf[64]; // for sprintf, dumb
u64                         frame_start;
u32                         cell_buffer_count;

internal void os_mem_reserve(u64 size, void **address)
{
    *address = VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
}

internal void os_mem_commit(void *reserved, u64 size)
{
    VirtualAlloc(reserved, size, MEM_COMMIT, PAGE_READWRITE);
}

internal void os_mem_free(void *base)
{
    VirtualFree(base, 0, MEM_RELEASE);
}

internal u64 os_time_us()
{
    u64 counter_ticks = 0;
    QueryPerformanceCounter(((LARGE_INTEGER*)&counter_ticks));
    return counter_ticks * 1000000 / perf_counter_freq;
}

internal void win32_assert()
{
    u32 error = GetLastError();

    if(error == 0)
    {

    }
    else
    {
        void* message = 0;

        FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL,
                      error,
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      (LPTSTR) &message, 0,0);

        assert(0);
    }
}

internal void d3d11_write_buffer(ID3D11Resource *buffer, void *data, u64 size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11DeviceContext_Map(device_context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    memcpy(mapped.pData, data, size);
    ID3D11DeviceContext_Unmap(device_context, buffer, 0);
}

internal void *os_read_file(char *path, u32 *out_size, arena_t *arena)
{
    // note: will fail if filesize is over 32bits/4 gigs
    HANDLE fileHandle = CreateFileA(path, 
                        GENERIC_READ, 
                        FILE_SHARE_READ,
                        0,
                        OPEN_EXISTING, 
                        FILE_ATTRIBUTE_NORMAL, 
                        0);
    win32_assert();
    u32 size = GetFileSize(fileHandle, 0 /*64 bit size pointer goes here*/);

    if(out_size)
    {
        *out_size = size;
    }
    
    assert(size != 0);
    void *buffer = arena_push(arena, size, 64);
    win32_assert();
                                
    ReadFile(fileHandle,
             buffer,
             size,
             0,
             0);
    win32_assert();
    
    CloseHandle(fileHandle);
    return buffer;
}

internal void os_write_file(char *path, void *stuff, u64 size_bytes)
{
    HANDLE file = CreateFileA(path, 
        GENERIC_WRITE, 
        FILE_SHARE_READ,
        0,
        CREATE_ALWAYS, 
        FILE_ATTRIBUTE_NORMAL, 
        0);
    WriteFile(file, stuff, size_bytes, 0, 0);
    CloseHandle(file);
}

internal void os_toggle_fullscreen()
{
    // https://devblogs.microsoft.com/oldnewthing/20100412-00/?p=14353
    
    u64 dwStyle = GetWindowLong(window, GWL_STYLE);

    if (dwStyle & WS_OVERLAPPEDWINDOW)
    {
        MONITORINFO mi = { sizeof(mi) };
        
        if (GetWindowPlacement(window, &windowedPlacement)
            && GetMonitorInfo(MonitorFromWindow(window, MONITOR_DEFAULTTOPRIMARY), &mi)) 
        {
            // go fullscreen
            SetWindowLong(window, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window, HWND_TOP,
                        mi.rcMonitor.left, mi.rcMonitor.top,
                        mi.rcMonitor.right - mi.rcMonitor.left,
                        mi.rcMonitor.bottom - mi.rcMonitor.top,
                        SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
    } 
    else 
    {
        // return the window
        SetWindowLong(window, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window, &windowedPlacement);
        SetWindowPos(window, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
}

internal string os_clipboard_get(arena *arena)
{
    string res = { 0 };
    if(OpenClipboard(window))
    {
        HANDLE handle = GetClipboardData(CF_TEXT);
        void *mem = GlobalLock(handle);
        res = arena_push_cstring(arena, mem);
        GlobalUnlock(mem);
        CloseClipboard();
    }
    return res;
}

internal void os_clipboard_set(string text)
{
    if(OpenClipboard(window))
    {
        EmptyClipboard();
        HANDLE *handle = GlobalAlloc(GMEM_MOVEABLE, text.length + 1);
        char *mem = GlobalLock(handle);
        memcpy(mem, text.p, text.length);
        mem[text.length] = '\0';
        GlobalUnlock(handle);
        SetClipboardData(CF_TEXT, handle);
        CloseClipboard();
    }
}

typedef struct
{
    IDWriteFactory          *factory;
    IDWriteFactory2         *factory2;
    IDWriteFontCollection   *font_collection;
    IDWriteGdiInterop       *gdi_interop;

    IDWriteRenderingParams  *params;

    IDWriteFontFamily       *font_family;
    IDWriteFont             *font;
    IDWriteFontFace         *font_face;

} dwrite_state_t;

dwrite_state_t dwrite_state;

internal void os_release_font_atlas(font_atlas atlas)
{
    if(atlas.os_handle.stuff[0])
    {
        ID3D11Texture2D_Release((ID3D11Texture2D *)atlas.os_handle.stuff[0]);
        ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)atlas.os_handle.stuff[1]);
    }
}

internal void dwrite_init()
{
    HRESULT hr = 0;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (void**)&dwrite_state.factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory2, (void**)&dwrite_state.factory2);
    IDWriteFactory_GetSystemFontCollection(dwrite_state.factory, &dwrite_state.font_collection, FALSE);
    u32 font_family_index;
    b32 sneed;
    IDWriteFontCollection_FindFamilyName(dwrite_state.font_collection, L"consolas", &font_family_index, &sneed);
    hr = IDWriteFontCollection_GetFontFamily(dwrite_state.font_collection, font_family_index, &dwrite_state.font_family);
    hr = IDWriteFontFamily_GetFirstMatchingFont(dwrite_state.font_family, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &dwrite_state.font);
    hr = IDWriteFont_CreateFontFace(dwrite_state.font, &dwrite_state.font_face);    
    IDWriteFactory_GetGdiInterop(dwrite_state.factory, &dwrite_state.gdi_interop);

    IDWriteRenderingParams *default_params;
    IDWriteFactory_CreateRenderingParams(dwrite_state.factory, &default_params);
    
    f32 gamma = IDWriteRenderingParams_GetGamma(default_params);
    f32 enhanced_contrast = IDWriteRenderingParams_GetEnhancedContrast(default_params);
    f32 cleartype_level = IDWriteRenderingParams_GetClearTypeLevel(default_params);
    
    IDWriteFactory2_CreateCustomRenderingParams2(dwrite_state.factory2, gamma, enhanced_contrast, enhanced_contrast, cleartype_level, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_RENDERING_MODE_GDI_NATURAL, DWRITE_GRID_FIT_MODE_ENABLED, (IDWriteRenderingParams2 **)&dwrite_state.params);
}

internal font_atlas os_make_font_atlas(u32 size, arena *scratch)
{
    HRESULT hr = 0;

    font_atlas result = { 0 };

    result.size = size;
    result.glyph_height = size;
    
    u32 glyph_count = 127 - 32;
    u16 *glyph_indices = arena_push(scratch, sizeof(u16) * glyph_count, 8);
    for(u32 i = 0; i < glyph_count; i++)
    {
        u32 cp = 32 + i;
        hr = IDWriteFontFace_GetGlyphIndices(dwrite_state.font_face, &cp, 1, glyph_indices + i);
    }
    
    DWRITE_FONT_METRICS font_face_metrics;
    IDWriteFontFace_GetMetrics(dwrite_state.font_face, &font_face_metrics);
    DWRITE_GLYPH_METRICS *glyph_metrics = arena_push(scratch, sizeof(DWRITE_GLYPH_METRICS) * glyph_count, 8);
    hr = IDWriteFontFace_GetDesignGlyphMetrics(dwrite_state.font_face, glyph_indices, glyph_count, glyph_metrics, 0);
    #define dips_from_pixels(p) (p * 96.0f / 72.0f)
    hr = IDWriteFontFace_GetGdiCompatibleGlyphMetrics(dwrite_state.font_face, dips_from_pixels(result.glyph_height), 1.0f, 0, FALSE, glyph_indices, glyph_count, glyph_metrics, 0);
    
    DWRITE_GLYPH_OFFSET *glyph_offsets = arena_push(scratch, sizeof(DWRITE_GLYPH_OFFSET) * glyph_count, 8);
    f32 *glyph_advances = arena_push(scratch, sizeof(f32) * glyph_count, 8);

    f32 height = (font_face_metrics.ascent + font_face_metrics.descent + font_face_metrics.lineGap) * result.glyph_height / font_face_metrics.designUnitsPerEm;
    f32 mul = result.glyph_height / height;
    
    result.glyph_width = (f32)glyph_metrics[0].advanceWidth / font_face_metrics.designUnitsPerEm * result.glyph_height * mul;

    u32 atlas_width_glyphs = 14;
    u32 atlas_heiht_glyphs = 7;

    result.atlas_width_glyphs = atlas_width_glyphs;

    for(u32 i = 0; i < glyph_count; i++)
    {
        glyph_advances[i] = 0.0f;

        f32 a = font_face_metrics.ascent * result.glyph_height / font_face_metrics.designUnitsPerEm * mul;
        f32 b = i / atlas_width_glyphs * result.glyph_height;
        glyph_offsets[i] = (DWRITE_GLYPH_OFFSET)
        {
            .advanceOffset = i % atlas_width_glyphs * result.glyph_width,
            .ascenderOffset = -b + -a
        };
    }

    u32 atlas_width_pixels = 14 * result.glyph_width;
    u32 atlas_height_pixels = 7 * result.glyph_height;
    atlas_width_pixels = ceil_pow2_u32(max(atlas_width_pixels, atlas_height_pixels));
    atlas_height_pixels = atlas_width_pixels;

    IDWriteBitmapRenderTarget *bitmap_render_target;
    hr = IDWriteGdiInterop_CreateBitmapRenderTarget(dwrite_state.gdi_interop, 0, atlas_width_pixels, atlas_height_pixels, &bitmap_render_target);

    DWRITE_GLYPH_RUN glyph_run = 
    {
        .fontFace = dwrite_state.font_face,
        .fontEmSize = result.glyph_height * mul - 1,
        .glyphCount = glyph_count,
        .glyphIndices = glyph_indices,
        .glyphAdvances = glyph_advances,
        .glyphOffsets = glyph_offsets,
        .isSideways = 0,
        .bidiLevel = 0,
    };
    
    hr = IDWriteBitmapRenderTarget_DrawGlyphRun(bitmap_render_target, 0, 0, DWRITE_MEASURING_MODE_GDI_NATURAL, &glyph_run, dwrite_state.params, 0xFFFFFFFF, 0);

    HDC dc = IDWriteBitmapRenderTarget_GetMemoryDC(bitmap_render_target);
    HBITMAP bitmap = GetCurrentObject(dc, OBJ_BITMAP);
    DIBSECTION dib;
    GetObject(bitmap, sizeof(dib), &dib);

    D3D11_TEXTURE2D_DESC texture_desc =
    {
        .Width = dib.dsBm.bmWidth,
        .Height = dib.dsBm.bmHeight,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };

    D3D11_SUBRESOURCE_DATA texture_data =
    {
        .pSysMem = dib.dsBm.bmBits,
        .SysMemPitch = dib.dsBm.bmHeight * sizeof(u32),
    };

    ID3D11Device_CreateTexture2D(device, &texture_desc, &texture_data, (ID3D11Texture2D **)&result.os_handle.stuff[0]);
    ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)result.os_handle.stuff[0], NULL, (ID3D11ShaderResourceView **)&result.os_handle.stuff[1]);

    IDWriteBitmapRenderTarget_Release(bitmap_render_target);

    return result;
}

static input_event input_queue[128];
static u32         input_queue_count;

LRESULT window_callback(HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    stbsp_sprintf(buf, "msg 0x%04X\n", message);
    PROFILE_BEGIN(buf);
    LRESULT result = 0;
    switch (message)
    {
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_KEYUP:
        case WM_KEYDOWN:
        {
            b32 was_down = (lParam & (1 << 30));
            b32 is_down  = !(lParam & (1 << 31));

            u16 code = (u8)wParam;
            
            if(GetKeyState(VK_SHIFT) & 0x8000)
            {
                code |= MODIFIER_SHIFT;
            }

            if(GetKeyState(VK_CONTROL) & 0x8000)
            {
                code |= MODIFIER_CTRL;
            }

            if(GetKeyState(VK_MENU) & 0x8000)
            {
                code |= MODIFIER_ALT;
            }
            
            input_queue[input_queue_count] = (input_event)
            {
                .type = INPUT_EVENT_KEY,
                .code = code,
                .is_repeat = was_down && is_down,
                .is_down = is_down
            };
            input_queue_count++;
            break;
        }
        case WM_CHAR:
        {
            input_queue[input_queue_count] = (input_event)
            {
                .type = INPUT_EVENT_TEXT,
                .character = wParam
            };
            input_queue_count++;
            break;
        }
        case WM_MOUSEWHEEL:
        {
            mouse_wheel_delta_accum += (i16)(wParam >> 16);

            break;
        }
        case WM_MOUSEMOVE:
        {
            mouse_x = LOWORD(lParam);
            mouse_y = HIWORD(lParam);
            break;
        }
        case WM_SIZE:
        {
            if(wParam == SIZE_MINIMIZED)
            {
                // it sends zeroes for the window size in this case. we don't want it.
                break;
            }
    
            new_screen_w = (i32) LOWORD(lParam);
            new_screen_h = (i32) HIWORD(lParam);
    
            assert(new_screen_w != 0);
            assert(new_screen_h != 0);    
            break;
        }
        case WM_CLOSE:
        {
            DestroyWindow(window);
            break;
        }
        case WM_DESTROY:
        {
            PostQuitMessage(0);
            break;
        }
        default:
        {
            result = DefWindowProc(window, message, wParam, lParam);
            break;
        }
    }
    PROFILE_END();
    return result;
}

internal void resize_swapchain()
{
    if(rt_view)
    {
        ID3D11RenderTargetView_Release(rt_view);
        ID3D11UnorderedAccessView_Release(uav);
        rt_view = 0;
    }

    // resize
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(swapchain, 0, screen_w, screen_h, DXGI_FORMAT_UNKNOWN, swapchain_flags);
    assert_hr(hr);

    // create RenderTarget view for new backbuffer texture
    ID3D11Texture2D* backbuffer;
    IDXGISwapChain1_GetBuffer(swapchain, 0, &IID_ID3D11Texture2D, (void**)&backbuffer);
    ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource*)backbuffer, NULL, &rt_view);
    ID3D11Texture2D_Release(backbuffer);
    ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource*)backbuffer, 0, &uav);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(device_context, 0, 1, &uav, 0);
}

internal void busy_wait(u64 time_us)
{
    u64 a = os_time_us() + time_us;
    while(os_time_us() < a) { }
}

void __stdcall WinMainCRTStartup()
{
    QueryPerformanceFrequency((LARGE_INTEGER*)&perf_counter_freq);
    
    #ifdef SPALL_ENABLED
    spall_begin_profiling();
    #endif

    PROFILE_BEGIN("startup");

    HMODULE hModule = GetModuleHandle(0);
    HRESULT hr;
    WNDCLASS windowClass = {0};

    windowClass.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = window_callback;
    windowClass.hInstance = hModule;
    windowClass.hCursor = LoadCursor(0, IDC_ARROW);
    windowClass.hbrBackground = 0;
    windowClass.lpszClassName = "balls";
    windowClass.hIcon = LoadIconA(hModule, MAKEINTRESOURCE(1));

    RegisterClass(&windowClass);

    new_screen_w = 800;
    new_screen_h = 600;

    window = CreateWindowEx(
        WS_EX_APPWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        windowClass.lpszClassName,
        "rained",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        new_screen_w,
        new_screen_h,
        0,
        0,
        hModule,
        0);
    assert(window);

    D3D_FEATURE_LEVEL levels[] = 
    {
        D3D_FEATURE_LEVEL_11_0
    };
    
    u32 flags = D3D11_CREATE_DEVICE_SINGLETHREADED;

    #ifdef DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif

    hr = D3D11CreateDevice(0, 
        D3D_DRIVER_TYPE_HARDWARE,
        0,
        flags,
        levels,
        lengthof(levels),
        D3D11_SDK_VERSION,
        &device,
        0, // todo: use this to assert that you got the feature level you've asked for
        &device_context);
    assert_hr(hr);
    
    #ifdef DEBUG
    // for debug builds enable VERY USEFUL debug break on API errors
    {
        ID3D11InfoQueue* info;
        ID3D11Device_QueryInterface(device, &IID_ID3D11InfoQueue, (void**)&info);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        ID3D11InfoQueue_SetBreakOnSeverity(info, D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        ID3D11InfoQueue_Release(info);
    }

    // enable debug break for DXGI too
    {
        IDXGIInfoQueue* dxgiInfo;
        HRESULT hr;
        hr = DXGIGetDebugInterface1(0, &IID_IDXGIInfoQueue, (void**)&dxgiInfo);
        assert(SUCCEEDED(hr));
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        IDXGIInfoQueue_SetBreakOnSeverity(dxgiInfo, DXGI_DEBUG_ALL, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, TRUE);
        IDXGIInfoQueue_Release(dxgiInfo);
    }
    #endif

    ///////////////////////////////////////////////////////////////////////////
    // create DXGI swap chain

    // get DXGI device from D3D11 device
    IDXGIDevice* dxgiDevice;
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice, (void**)&dxgiDevice);
    assert_hr(hr);
    // get DXGI adapter from DXGI device
    IDXGIAdapter* dxgiAdapter;
    hr = IDXGIDevice_GetAdapter(dxgiDevice, &dxgiAdapter);
    assert_hr(hr);
    // get DXGI factory from DXGI adapter
    IDXGIFactory2* factory;
    hr = IDXGIAdapter_GetParent(dxgiAdapter, &IID_IDXGIFactory2, (void**)&factory);
    assert_hr(hr);

    IDXGIDevice1 *dxgiDevice1;
    hr = ID3D11Device_QueryInterface(device, &IID_IDXGIDevice1, (void**)&dxgiDevice1);
    assert_hr(hr);

    //hr = IDXGIDevice1_SetMaximumFrameLatency(dxgiDevice1, 1);
    //assert_hr(hr);

    #define FLIP_MODEL
    #define WAITABLE_OBJECT
    
#ifdef FLIP_MODEL
    swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
#endif

#ifdef WAITABLE_OBJECT
    swapchain_flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
#endif

    DXGI_SWAP_CHAIN_DESC1 desc =
    {
        // default 0 value for width & height means to get it from HWND automatically
        //.Width = 0,
        //.Height = 0,
        // or use DXGI_FORMAT_R8G8B8A8_UNORM_SRGB for storing sRGB
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        // FLIP presentation model does not allow MSAA framebuffer
        // if you want MSAA then you'll need to render offscreen and manually
        // resolve to non-MSAA framebuffer
        .SampleDesc = { 1, 0 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_UNORDERED_ACCESS,
        .BufferCount = 2,
        // we don't want any automatic scaling of window content
        // this is supported only on FLIP presentation model
        #ifdef FLIP_MODEL
        .Scaling = DXGI_SCALING_NONE,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        #else
        .SwapEffect = DXGI_SWAP_EFFECT_DISCARD,
        #endif
        
        // use more efficient FLIP presentation model
        // Windows 10 allows to use DXGI_SWAP_EFFECT_FLIP_DISCARD
        // for Windows 8 compatibility use DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL
        // for Windows 7 compatibility use DXGI_SWAP_EFFECT_DISCARD

        .Flags = swapchain_flags
    };

    swapchain_flags = swapchain_flags;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown*)device, window, &desc, NULL, NULL, &swapchain);
    assert_hr(hr);

    
#ifdef WAITABLE_OBJECT
    IDXGISwapChain2 *swapchain2;
    hr = IDXGISwapChain1_QueryInterface(swapchain, &IID_IDXGISwapChain2, (void**)&swapchain2);
    assert_hr(hr);

    hr = IDXGISwapChain2_SetMaximumFrameLatency(swapchain2, 1);
    assert_hr(hr);
    HANDLE waitable_object = IDXGISwapChain2_GetFrameLatencyWaitableObject(swapchain2);
#else
    IDXGIDevice1 *device1;
    IDXGIDevice_QueryInterface(device, &IID_IDXGIDevice1, &device1);
    IDXGIDevice1_SetMaximumFrameLatency(device1, 1);
#endif

    // disable silly Alt+Enter changing monitor resolution to match window size
    IDXGIFactory_MakeWindowAssociation(factory, window, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(factory);
    IDXGIAdapter_Release(dxgiAdapter);
    IDXGIDevice_Release(dxgiDevice);

    ID3D11ComputeShader *shader;
    hr = ID3D11Device_CreateComputeShader(device, g_shader_cs, lengthof(g_shader_cs), 0, &shader);
    assert_hr(hr);

    ID3D11DeviceContext_CSSetShader(device_context, shader, 0, 0);

    arena_t *scratch = arena_alloc(gb(1), mb(1));

    arena_reset(scratch);

    dwrite_init();

    ID3D11SamplerState *sampler_state;
    D3D11_SAMPLER_DESC sdesc =
    {
        .Filter = D3D11_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .MipLODBias = 0,
        .MaxAnisotropy = 1,
    };
    ID3D11Device_CreateSamplerState(device, &sdesc, &sampler_state);
    ID3D11DeviceContext_CSSetSamplers(device_context, 0, 1, &sampler_state);

    typedef struct 
    {
        i32 rect_min_x;
        i32 rect_min_y;
        i32 rect_max_x;
        i32 rect_max_y;
        u32 cell_width;
        u32 cell_height;
        u32 num_cells_x;
        u32 num_cells_y;
        u32 atlas_width_glyphs;
        i32 view_offset_pixels;
        u32 vsync_line_position;
        i32 pointer_x;
        i32 pointer_y;
        u32 kind;
        u32 quad_color;

    } cbuffer_t;

    D3D11_BUFFER_DESC cbuffer_desc = 
    {
        .ByteWidth = (sizeof(cbuffer_t) + 15) / 16 * 16,
        .Usage = D3D11_USAGE_DYNAMIC,
        .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
    };
    ID3D11Buffer *d3d11_cbuffer;
    ID3D11Device_CreateBuffer(device, &cbuffer_desc, 0, &d3d11_cbuffer);

    ID3D11DeviceContext_CSSetConstantBuffers(device_context, 0, 1, &d3d11_cbuffer);

    ID3D11Buffer *cell_buffer = 0;
    ID3D11ShaderResourceView *cell_buffer_srv;

    PROFILE_END();


    u64 prev_frame;
    
    MSG message;
    while(!quit)
    {
        PROFILE_BEGIN("loop");

    #ifdef WAITABLE_OBJECT
        PROFILE_BEGIN("waitable object");
        WaitForSingleObjectEx(waitable_object, INFINITE, TRUE);
        PROFILE_END();
    #endif
        
        u64 time = os_time_us();
        prev_frame = time - frame_start;
        frame_start = time;

        mouse_wheel_delta_accum = 0;
        input_queue_count = 0;

        PROFILE_BEGIN("poll");
        while (PeekMessage(&message, 0, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT)
            {
                quit = 1;
            }
            
            TranslateMessage(&message); // this will get us wm_char working.
            DispatchMessage(&message);
        }
        PROFILE_END();

        mouse_wheel_delta = (f32) mouse_wheel_delta_accum / 120.0f * 32.0f;

        if(screen_w != new_screen_w || screen_h != new_screen_h)
        {
            screen_w = new_screen_w;
            screen_h = new_screen_h;
            resize_swapchain();
        }

        rained_input in = 
        {
            .frame_start = frame_start,
            .input_queue = input_queue,
            .input_queue_count = input_queue_count,
            .mouse_wheel_delta = mouse_wheel_delta,
            .mouse_x = mouse_x,
            .mouse_y = mouse_y,
            .screen_h = screen_h,
            .screen_w = screen_w,
        };
        
        renderer_command *cmd = draw(&in);
        PROFILE_BEGIN("draw");

        // todo removeme
        f32 clear_color[4] = { 1,0,1,1 }; 
        ID3D11DeviceContext_ClearRenderTargetView(device_context, rt_view, clear_color);

        while(cmd)
        {
            u32 cell_count = cmd->code_view.num_cells_x * cmd->code_view.num_cells_y;
    
            if(cell_buffer_count < cell_count)
            {
                if(cell_buffer)
                {
                    ID3D11Buffer_Release(cell_buffer);
                    ID3D11ShaderResourceView_Release(cell_buffer_srv);
                }
    
                D3D11_BUFFER_DESC cell_buffer_desc = 
                {
                    .ByteWidth = cell_count * sizeof(cell),
                    .Usage = D3D11_USAGE_DYNAMIC,
                    .BindFlags = D3D11_BIND_SHADER_RESOURCE,
                    .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
                    .MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
                    .StructureByteStride = sizeof(cell)
                };

                ID3D11Device_CreateBuffer(device, &cell_buffer_desc, 0, &cell_buffer);
                D3D11_SHADER_RESOURCE_VIEW_DESC d = 
                {
                    .Format = DXGI_FORMAT_UNKNOWN,
                    .ViewDimension = D3D11_SRV_DIMENSION_BUFFER,
                    .Buffer.NumElements = cell_count,
                };
                ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)cell_buffer, &d, &cell_buffer_srv);
                ID3D11DeviceContext_CSSetShaderResources(device_context, 1, 1, &cell_buffer_srv);
                cell_buffer_count = cell_count;
            }
    
            cbuffer_t cb = 
            {
                .rect_min_x = cmd->rect.min_x,
                .rect_min_y = cmd->rect.min_y,
                .rect_max_x = cmd->rect.max_x,
                .rect_max_y = cmd->rect.max_y,
                .cell_width = cmd->code_view.font.glyph_width,
                .cell_height = cmd->code_view.font.glyph_height,
                .num_cells_x = cmd->code_view.num_cells_x,
                .num_cells_y = cmd->code_view.num_cells_y,
                .atlas_width_glyphs = cmd->code_view.font.atlas_width_glyphs,
                .view_offset_pixels = cmd->code_view.view_offset_pixels,
                .vsync_line_position = cmd->code_view.vsync_line_position,
                .pointer_x = cmd->code_view.pointer_x,
                .pointer_y = cmd->code_view.pointer_y,
                .kind = cmd->kind,
                .quad_color = cmd->quad.color,
            };
    
            d3d11_write_buffer((ID3D11Resource*)cell_buffer, cmd->code_view.cells, cell_count * sizeof(cell));
            d3d11_write_buffer((ID3D11Resource*)d3d11_cbuffer, &cb, sizeof(cbuffer_t));

            ID3D11DeviceContext_CSSetShaderResources(device_context, 0, 1, (ID3D11ShaderResourceView**)&cmd->code_view.font.os_handle.stuff[1]);
    
            ID3D11DeviceContext_Dispatch(device_context, (cmd->rect.max_x - cmd->rect.min_x + 8 - 1) / 8, (cmd->rect.max_y - cmd->rect.min_y + 8 - 1) / 8, 1);
            cmd = cmd->next;
        }
        PROFILE_END();

        ////////////////////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////

        PROFILE_BEGIN("present");
        hr = IDXGISwapChain1_Present(swapchain, 0, DXGI_PRESENT_ALLOW_TEARING);
        assert_hr(hr);

        ID3D11DeviceContext_CSSetUnorderedAccessViews(device_context, 0, 1, &uav, 0);
        
        if(frame_count == 0)
        {
            ShowWindow(window, SW_SHOW);
        }
        PROFILE_END();

        frame_count++;

        PROFILE_END();
    }
    #ifdef SPALL_ENABLED
    spall_end_profiling();
    #endif
    ExitProcess(0);
}