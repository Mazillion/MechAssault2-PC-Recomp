# The game's compilers under Wine / Proton (wine_host.py): LLVM's Windows tools instead of MSVC.
# The build script's environment (clang-env.cmd) sets MA2_LLVM (the LLVM folder) and MA2_XWIN (the Microsoft C runtime and
# Windows SDK laid out by xwin), plus INCLUDE and LIB. Never used on Windows unless MA2_TOOLCHAIN=clang.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
file(TO_CMAKE_PATH "$ENV{MA2_LLVM}" _ma2_llvm)
file(TO_CMAKE_PATH "$ENV{MA2_XWIN}" _ma2_xwin)
set(CMAKE_C_COMPILER "${_ma2_llvm}/bin/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${_ma2_llvm}/bin/clang-cl.exe")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_LINKER "${_ma2_llvm}/bin/lld-link.exe")
set(CMAKE_AR "${_ma2_llvm}/bin/llvm-lib.exe")
set(CMAKE_RC_COMPILER "${_ma2_llvm}/bin/llvm-rc.exe")
set(CMAKE_MT "${_ma2_llvm}/bin/llvm-mt.exe")
# xwin has release libraries only (no msvcrtd.lib): CMake's test compiles use Release too.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
# The C runtime is linked into the exe (/MT): nothing from Visual Studio's redistributable folder is needed beside the game
# (there is none under Wine), and Wine's own msvcp140/vcruntime140 are never asked for the newer STL's exports.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(_inc "/imsvc\"${_ma2_xwin}/crt/include\" /imsvc\"${_ma2_xwin}/sdk/include/ucrt\" /imsvc\"${_ma2_xwin}/sdk/include/um\" /imsvc\"${_ma2_xwin}/sdk/include/shared\" /imsvc\"${_ma2_xwin}/sdk/include/winrt\"")
# kit/source/game/CMakeLists.txt adds the LINUX-SPIKE source flags when the compiler is clang (implicit declarations,
# -fcommon on two chunks, the STL version check). Here only: the generated code relies on wrapping integer arithmetic, as
# the Xbox CPU does (-fwrapv: clang may otherwise optimise on signed overflow, which MSVC in practice does not), and
# warnings are off (the generated code would print hundreds of thousands).
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_inc} /w /clang:-fwrapv")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc ${_inc} /w /clang:-fwrapv")
set(CMAKE_RC_FLAGS_INIT "/nologo -I \"${_ma2_xwin}/sdk/include/um\" -I \"${_ma2_xwin}/sdk/include/shared\" -I \"${_ma2_xwin}/crt/include\" -I \"${_ma2_xwin}/sdk/include/ucrt\"")
set(_lib "/libpath:\"${_ma2_xwin}/crt/lib/x86_64\" /libpath:\"${_ma2_xwin}/sdk/lib/um/x86_64\" /libpath:\"${_ma2_xwin}/sdk/lib/ucrt/x86_64\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_lib}")
