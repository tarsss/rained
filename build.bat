@echo off

set vs_build_tools_path=C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\

fxc -nologo /O3 /WX /Qstrip_reflect /Qstrip_debug /T cs_5_0 /E shader_cs /Fh shader.h shader.hlsl

set warnings=-Wno-incompatible-pointer-types-discards-qualifiers
set common_compiler_flags=/GS- /Gs9999999 %warnings%
set common_linker_flags=kernel32.lib user32.lib d3d11.lib dxgi.lib dxguid.lib dwrite.lib gdi32.lib shell32.lib hid.lib ole32.lib uuid.lib -incremental:no -nodefaultlib -subsystem:windows -STACK:0x100000,0x100000 

set debug_compiler_flags=-Zi -DDEBUG
set debug_linker_flags=-debug:full

set release_compiler_flags=-O2 -arch:AVX2
set release_linker_flags=

IF "%1" == "release" (
    set compiler_flags=%common_compiler_flags% %release_compiler_flags%
    set linker_flags=%common_linker_flags% %release_linker_flags%
) ELSE (
    set compiler_flags=%common_compiler_flags% %debug_compiler_flags%
    set linker_flags=%common_linker_flags% %debug_linker_flags%
)

:: todo: hangs for a couple seconds if raddbg isn't there... WHY
C:\raddbg\raddbg.exe --ipc kill_all

clang-cl rained_win32.c -o rained.exe %compiler_flags% -link %linker_flags%

IF %ERRORLEVEL% == 0 (
    C:\raddbg\raddbg.exe --ipc run
)