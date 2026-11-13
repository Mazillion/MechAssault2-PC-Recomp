# Unmodified libsamplerate 0.2.2 sources; BSD license in COPYING.
# Build the fastest sinc converter used by xemu, without unused coefficient
# tables, external DLLs, examples, or a build-time network dependency.
add_library(apu_samplerate STATIC samplerate.c src_sinc.c src_linear.c src_zoh.c)
target_include_directories(apu_samplerate PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(apu_samplerate PRIVATE
    ENABLE_SINC_FAST_CONVERTER HAVE_STDBOOL_H
    CPU_CLIPS_NEGATIVE=0 CPU_CLIPS_POSITIVE=0
    PACKAGE="libsamplerate" VERSION="0.2.2")
if(MSVC)
    target_compile_options(apu_samplerate PRIVATE /O2 /W3 /wd4244 /wd4267)
endif()
if(NOT WIN32)
    target_link_libraries(apu_samplerate PRIVATE m)
endif()
