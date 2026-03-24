cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK!")
  endif()
endif()

project(vitascrob C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

add_executable(vitascrob vitascrob.c)

target_compile_options(vitascrob PRIVATE -Wall -O2)

target_link_options(vitascrob PRIVATE -nostdlib -Wl,-q)

target_link_libraries(vitascrob
  taihen_stub
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceLibKernel_stub
  SceLibc_stub
  SceRtc_stub
  gcc
)

vita_create_self(vitascrob.suprx vitascrob
  UNSAFE
)