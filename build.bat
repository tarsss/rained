@echo off
if not defined DevEnvDir (
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
)

:: -fuse-ld=lld provides better messages for the locations of missing symbols.
:: -clang:-H is very usefull for debugging windows header inclusions and stuff!!

fxc -nologo /O3 /WX /Qstrip_reflect /Qstrip_debug /T cs_5_0 /E shader_cs /Fh shader.h shader.hlsl

set common_compiler_flags=/GS- /Gs9999999 
set common_linker_flags=kernel32.lib user32.lib d3d11.lib dxgi.lib dxguid.lib -incremental:no -nodefaultlib -subsystem:windows -STACK:0x100000,0x100000 

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
clang-cl editor.c %compiler_flags% -link %linker_flags% -verbose:ref -force:unresolved

call C:\raddbg\raddbg --ipc run
::call editor