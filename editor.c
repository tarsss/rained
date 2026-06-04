//#define SPALL_ENABLED
#define COBJMACROS

int _fltused;

#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>

#define STB_SPRINTF_IMPLEMENTATION
#include "stb_sprintf.h"
#include <stdint.h>
#include "shader.h"

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

#define kilobytes(n) (((u64)(n)) * (u64)1024)
#define megabytes(n) (((u64)(n)) * (u64)1024 * (u64)1024)
#define gigabytes(n) (((u64)(n)) * (u64)1024 * (u64)1024 * (u64)1024)
#define terabytes(n) (((u64)(n)) * (u64)1024 * (u64)1024 * (u64)1024 * (u64))

#define PAGE_SIZE 4096

#include "editor.h"

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

// view
u32                         cell_buffer_count;
f32                         view_offset_pixels;
u32                         caret_position;
u32                         caret_column;
u32                         caret_line;
u32                         caret_wish_column;
u64                         caret_last_move_time;
f32                         caret_screen_x;
f32                         caret_screen_y;

// buffer
arena   *text_arena;
char    *text;
u32     text_size;

internal void mem_reserve(u64 size, void **address)
{
    *address = VirtualAlloc(0, size, MEM_RESERVE, PAGE_READWRITE);
}

internal void mem_commit(void *reserved, u64 size)
{
    VirtualAlloc(reserved, size, MEM_COMMIT, PAGE_READWRITE);
}

internal void mem_free(void *base)
{
    VirtualFree(base, 0, MEM_RELEASE);
}

void *memset(void *dest, int c, size_t count)
{
    char *bytes = (char *)dest;
    while (count--)
    {
        *bytes++ = (char)c;
    }
    return dest;
}

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

internal u64 get_time_us()
{
    u64 counter_ticks = 0;
    QueryPerformanceCounter(((LARGE_INTEGER*)&counter_ticks));
    return counter_ticks * 1000000 / perf_counter_freq;
}


#ifdef SPALL_ENABLED
#include "spall.h"
static SpallProfile spall_profile;
static SpallBuffer spall_buffer;
static HANDLE spall_file;
internal void spall_begin(char *name)
{
    // cant you get the size of the string at compile time?
    spall_buffer_begin(&spall_profile, &spall_buffer, name, cstring_length(name), get_time_us());
}
internal void spall_end()
{
    spall_buffer_end(&spall_profile, &spall_buffer, get_time_us());
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

    u64 buffer_size = megabytes(1);
    void *mem;
    mem_reserve(buffer_size, &mem);
    mem_commit(mem, buffer_size);
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
    mem_free(spall_buffer.data);
    spall_quit(&spall_profile);
}
#define PROFILE_BEGIN(name) spall_begin(name)
#define PROFILE_END() spall_end();
#else
#define PROFILE_BEGIN(name)
#define PROFILE_END()
#endif

internal arena *arena_alloc(u64 reserve, u64 commit)
{
    // we reserve and commit at least one page

    u64 reserve_size = (reserve + PAGE_SIZE) / PAGE_SIZE * PAGE_SIZE;
    u64 commit_size = (commit + PAGE_SIZE) / PAGE_SIZE * PAGE_SIZE;

    void *base;

    mem_reserve(reserve_size, &base);
    mem_commit(base, commit_size);
    
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

        mem_commit(arena -> base, commitSize);
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
    mem_free(arena->base);
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

internal void *win32_read_file(char *path, u32 *out_size, arena_t *arena)
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

internal loaded_bitmap load_bitmap(char *path, arena_t *arena)
{
    void *file = win32_read_file(path, 0, arena);
    
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


internal void toggle_fullscreen()
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
        case WM_KEYUP:
        case WM_KEYDOWN:
        {
            b32 was_down = (lParam & (1 << 30));
            b32 is_down  = !(lParam & (1 << 31));

            input_queue[input_queue_count] = (input_event)
            {
                .type = INPUT_EVENT_KEY,
                .is_repeat = was_down && is_down,
                .vk_code = wParam,
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

ID3D11ShaderResourceView *d3d11_upload_bitmap(loaded_bitmap bitmap)
{
    ID3D11ShaderResourceView* texture_view;
    D3D11_TEXTURE2D_DESC texture_desc =
    {
        .Width = bitmap.header->Width,
        .Height = bitmap.header->Width,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 },
        .Usage = D3D11_USAGE_IMMUTABLE,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE,
    };

    D3D11_SUBRESOURCE_DATA texture_data =
    {
        .pSysMem = bitmap.data,
        .SysMemPitch = bitmap.header->Height * sizeof(u32),
    };

    ID3D11Texture2D* texture;
    ID3D11Device_CreateTexture2D(device, &texture_desc, &texture_data, &texture);
    ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource*)texture, NULL, &texture_view);
    ID3D11Texture2D_Release(texture);
    return texture_view;
}

void resize_swapchain()
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

void busy_wait(u64 time_us)
{
    u64 a = get_time_us() + time_us;
    while(get_time_us() < a) { }
}

u32 caret_get_column()
{
    for(u32 i = caret_position; i >= 0; i--)
    {
        if(i == 0)
        {
            return caret_position - i;
        }
        if(text[i] == '\n')
        {
            return caret_position - i - 1;
        }
    }
    return 0;
}

void caret_move_right()
{
    u32 p = caret_position;
    while(caret_position != text_size)
    {
        char c = text[caret_position];
        caret_position++;
        if(c == '\n')
        {
            caret_line++;
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret_last_move_time = frame_start;
    caret_column = caret_get_column();
    caret_wish_column = caret_column;
}

void caret_move_left()
{
    while(caret_position)
    {
        caret_position--;
        char c = text[caret_position];
        if(c == '\r')
        {
            caret_line--;
            break;
        }
        if(c > 31)
        {
            break;
        }
    }
    caret_last_move_time = frame_start;
    caret_column = caret_get_column();
    caret_wish_column = caret_column;
}

void caret_move_to_prev_line()
{
    while(caret_position)
    {
        if(text[caret_position] == '\n')
        {
            caret_position--;
            caret_line--;
            break;
        }
        caret_position--;
    }
}

void caret_move_to_line_start()
{
    while(1)
    {
        if(caret_position == 0 || text[caret_position - 1] == '\n')
        {
            break;
        }
        caret_position--;
    }
}

void caret_go_to_column(u32 col)
{
    caret_move_to_line_start();
    u32 c = 0;
    while(c != col && caret_position != text_size)
    {
        if(text[caret_position] == '\r')
        {
            break;
        }
        caret_position++;
        c++;
    }
}

void caret_move_to_next_line()
{
    while(caret_position != text_size)
    {
        if(text[caret_position] == '\n')
        {
            caret_position++;
            caret_line++;
            break;
        }
        caret_position++;
    }
}

void caret_move_up()
{
    caret_move_to_prev_line();
    caret_go_to_column(caret_wish_column);
    caret_column = caret_get_column();
    caret_last_move_time = frame_start;
}

void caret_move_down()
{
    caret_move_to_next_line();
    caret_go_to_column(caret_wish_column);
    caret_column = caret_get_column();
    caret_last_move_time = frame_start;
}

void caret_insert_characters(char *c, u32 count)
{    
    u32 p = caret_position;
    for(u32 i = text_size + count - 1; i >= p + count; i--)
    {
        text[i] = text[i - count];
    }
    for(u32 i = 0; i < count; i++)
    {
        text[p + i] = c[i];
    }
    text_size += count;
}

void caret_remove_characters_to_the_right(u32 count)
{
    for(u32 i = caret_position; i < text_size; i++)
    {
        text[i] = text[i + count];
    }
    text_size -= count;
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
        "editor",
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

    arena_t *scratch = arena_alloc(gigabytes(1), megabytes(1));
    loaded_bitmap font_bitmap = load_bitmap("font.bmp", scratch);
    ID3D11ShaderResourceView *font_texture_srv = d3d11_upload_bitmap(font_bitmap);
    arena_reset(scratch);

    ID3D11DeviceContext_CSSetShaderResources(device_context, 0, 1, &font_texture_srv);

    ID3D11SamplerState *sampler_state;
    D3D11_SAMPLER_DESC sdesc =
    {
        .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
        .AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
        .AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
        .MipLODBias = 0,
        .MaxAnisotropy = 1,
    };
    ID3D11Device_CreateSamplerState(device, &sdesc, &sampler_state);
    ID3D11DeviceContext_CSSetSamplers(device_context, 0, 1, &sampler_state);

    u32 cell_width = 8;
    u32 cell_height = 18;

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

    ID3D11Buffer *cell_buffer;
    ID3D11ShaderResourceView *cell_buffer_srv;

    arena *forever_arena = arena_alloc(gigabytes(1), megabytes(1));
    arena *frame_arena = arena_alloc(gigabytes(1), megabytes(1));
    
    text_arena = arena_alloc(gigabytes(1), megabytes(1));
    text = win32_read_file("editor.c", &text_size, text_arena);

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
        
        u64 time = get_time_us();
        prev_frame = time - frame_start;
        frame_start = time;

        //PROFILE_BEGIN("sleep");
        //busy_wait(10ULL * 1000 * 1000);
        //PROFILE_END();

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
        view_offset_pixels -= mouse_wheel_delta;

        for(u32 i = 0; i < input_queue_count; i++)
        {
            input_event e = input_queue[i];

            if(e.is_down)
            {
                if(e.type == INPUT_EVENT_KEY)
                {
                    if(!e.is_repeat)
                    {
                        if(e.vk_code == VK_F11)
                        {
                            toggle_fullscreen();
                        }
                    }

                    if(e.vk_code == VK_RIGHT)
                    {
                        caret_move_right();
                    }
                    else if(e.vk_code == VK_LEFT)
                    {
                        caret_move_left();
                    }
                    else if(e.vk_code == VK_DOWN)
                    {
                        caret_move_down();
                    }
                    else if(e.vk_code == VK_UP)
                    {
                        caret_move_up();
                    }
                }
            }

            if(e.type == INPUT_EVENT_TEXT)
            {
                if(e.character > 31)
                {
                    caret_insert_characters(&e.character, 1);
                    caret_move_right();
                }
                else if(e.character == '\r')
                {
                    char line_end[2] = { '\r', '\n' };
                    caret_insert_characters(line_end, 2);
                    caret_move_right();
                }
                else if(e.character == '\b' && caret_position > 0)
                {
                    if(caret_position > 1 && text[caret_position - 1] == '\n')
                    {
                        caret_move_left();
                        caret_remove_characters_to_the_right(2);
                    }
                    else
                    {
                        caret_move_left();
                        caret_remove_characters_to_the_right(1);
                    }
                }
            }
        }

        ////////////////////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////

        if(screen_w != new_screen_w || screen_h != new_screen_h)
        {
            screen_w = new_screen_w;
            screen_h = new_screen_h;
            resize_swapchain();
        }

        ////////////////////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////

        u32 width = (screen_w + cell_width - 1) / cell_width;
        u32 height = (screen_h + cell_height - 1) / cell_height + 1;
        u32 cell_count = width * height; 

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
        u32 line_start_column = 0;

        while(line < height && text_position < text_size && num_lines < height)
        {
            char c = text[text_position];

            if(c == '\n')
            {
                lines[num_lines] = (chopped_line)
                {
                    .line_in_text = topmost_visible_line + line,
                    .chop_offset = line_start_column,
                    .pos_in_text = line_start,
                    .length = text_position - line_start,
                };
                line_start = text_position + 1;
                line_start_column = 0;
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
                        .chop_offset = line_start_column,
                        .pos_in_text = line_start,
                        .length = column,
                    };
                    line_start = text_position;
                    line_start_column = column;
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
        if((time - caret_last_move_time) % caret_period > caret_period / 2)
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

            if(l->line_in_text == caret_line)
            {
                for(u32 j = 0; j < width; j++)
                {
                    cells[i * width + j].bg_color = 0x001F1F1F;
                }
                
                if(!caret_blink)
                {
                    if(caret_column >= l->chop_offset)
                    {
                        cell *c = &cells[caret_column - l->chop_offset + i * width];
                        c->bg_color = ~c->bg_color;
                        c->text_color = ~c->text_color;
                    }
                }
            }
        }

        u32 view_offset_cell = view_offset_pixels < 0.0f ? view_offset_pixels : (i32)view_offset_pixels % cell_height;

        cbuffer_t cb = 
        {
            .cell_width = cell_width,
            .cell_height = cell_height,
            .num_cells_x = width,
            .num_cells_y = height,
            .atlas_width_characters_x = 14,
            .atlas_height_characters_y = 7,
            .view_offset_pixels = view_offset_cell,
            .vsync_line_position = get_time_us() / 1000 % screen_w,
            .pointer_x = mouse_x,
            .pointer_y = mouse_y
        };

        d3d11_write_buffer((ID3D11Resource*)cell_buffer, cells, cell_count * sizeof(cell));
        d3d11_write_buffer((ID3D11Resource*)d3d11_cbuffer, &cb, sizeof(cbuffer_t));

        ID3D11DeviceContext_Dispatch(device_context, (screen_w + 8 - 1) / 8, (screen_h + 8 - 1) / 8, 1);

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
        arena_reset(frame_arena);

        PROFILE_END();
    }
    #ifdef SPALL_ENABLED
    spall_end_profiling();
    #endif
    ExitProcess(0);
}