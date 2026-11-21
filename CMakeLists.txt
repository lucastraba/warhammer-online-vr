# WARVR build. WAR.exe is a 32-bit process, so the DLL and the injector must be 32-bit (Win32).
#
#   Visual Studio:  double-click build.bat   (or: cmake -S . -B build -A Win32 && cmake --build build --config Release)
#   Output:         build\Release\WARVR.dll, build\Release\WARVRInjector.exe
#   Unit tests:     ctest --test-dir build -C Release
cmake_minimum_required(VERSION 3.20)
project(WARVR LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32 AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "WAR.exe is 32-bit: configure with -A Win32 (Visual Studio) or a 32-bit toolchain.")
endif()

if(MSVC)
  # Static CRT: the DLL then runs on a machine without the Visual C++ redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/W3 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

if(WIN32)
  # The mod itself. d3d11/dxgi are loaded at runtime; d3d9 is only needed for its headers.
  add_library(WARVR SHARED src/WARVR.cpp)
  target_link_libraries(WARVR PRIVATE user32)
  set_target_properties(WARVR PROPERTIES OUTPUT_NAME "WARVR" PREFIX "")
  if(MSVC)
    target_link_options(WARVR PRIVATE /SAFESEH:NO)
  endif()

  add_executable(WARVRInjector src/injector.cpp)
  if(MSVC)
    target_compile_definitions(WARVRInjector PRIVATE _MBCS)
  endif()

  # Put the 32-bit openvr_api.dll next to the build output, so the output folder is the complete install set.
  add_custom_command(TARGET WARVR POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/openvr/bin/win32/openvr_api.dll" "$<TARGET_FILE_DIR:WARVR>")
endif()

# Pure-math unit tests: build and run on any platform.
enable_testing()
foreach(t vrmath_test shaderscan_test hookutil_test)
  add_executable(${t} tests/${t}.cpp)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
