cmake_minimum_required(VERSION 3.22)
project(popup16quest CXX C)
set(CMAKE_CXX_STANDARD 20)
set(S9X ${CMAKE_CURRENT_SOURCE_DIR}/../snes9x)

# patched snes9x core (libretro build), compiled straight into the app library
set(CORE_SRC
  apu/apu.cpp apu/bapu/dsp/sdsp.cpp apu/bapu/smp/smp.cpp apu/bapu/smp/smp_state.cpp
  bsx.cpp c4.cpp c4emu.cpp cheats.cpp cheats2.cpp clip.cpp conffile.cpp controls.cpp cpu.cpp
  cpuexec.cpp cpuops.cpp crosshairs.cpp dma.cpp dsp.cpp dsp1.cpp dsp2.cpp dsp3.cpp dsp4.cpp
  fxinst.cpp fxemu.cpp gfx.cpp globals.cpp memmap.cpp obc1.cpp msu1.cpp ppu.cpp stream.cpp
  sa1.cpp sa1cpu.cpp screenshot.cpp sdd1.cpp sdd1emu.cpp seta.cpp seta010.cpp seta011.cpp
  seta018.cpp snapshot.cpp snes9x.cpp spc7110.cpp srtc.cpp tile.cpp tileimpl-n1x1.cpp
  tileimpl-n2x1.cpp tileimpl-h2x1.cpp sha256.cpp bml.cpp movie.cpp fscompat.cpp
  libretro/libretro.cpp filter/snes_ntsc.c)
list(TRANSFORM CORE_SRC PREPEND ${S9X}/)
add_library(snes9x STATIC ${CORE_SRC})
target_compile_definitions(snes9x PRIVATE ANDROID __LIBRETRO__ HAVE_STRINGS_H RIGHTSHIFT_IS_SAR)
target_include_directories(snes9x PRIVATE ${S9X}/libretro ${S9X}/libretro/libretro-common/include ${S9X} ${S9X}/apu ${S9X}/apu/bapu)
target_compile_options(snes9x PRIVATE -O3 -fPIC -Wno-everything)
set_target_properties(snes9x PROPERTIES CXX_STANDARD 17)

set(GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
add_library(native_app_glue STATIC ${GLUE}/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${GLUE})

set(OXR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/oxr/prefab/modules)
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION ${OXR}/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so)

add_library(popup16quest SHARED src/main.cpp)
target_include_directories(popup16quest PRIVATE ${OXR}/headers/include ${S9X}/libretro)
target_compile_options(popup16quest PRIVATE -O2 -Wall -Wno-missing-field-initializers)
target_link_options(popup16quest PRIVATE -u ANativeActivity_onCreate)
target_link_libraries(popup16quest snes9x native_app_glue openxr_loader android log EGL GLESv3 aaudio jnigraphics)
