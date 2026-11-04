cmake_minimum_required(VERSION 3.16)
project(milkzip VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(archiver_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()

if(NOT SEVENZIP_SRC)
  message(FATAL_ERROR
    "Set -DSEVENZIP_SRC= to the 7-Zip 26.04 source tree.\n"
    "Run scripts/fetch-7zip.sh. The core includes 7-Zip's interface headers\n"
    "and loads 7z.so / 7z.dll / lib7z.dylib at runtime. It does not link 7-Zip in.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

add_library(archiver_lz4 STATIC
  third_party/lz4/lz4.c third_party/lz4/lz4frame.c
  third_party/lz4/lz4hc.c third_party/lz4/xxhash.c)
target_include_directories(archiver_lz4 PUBLIC third_party/lz4)
set_target_properties(archiver_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden)

add_library(archiver SHARED
  core/src/path_safety.cpp
  core/src/safe_write.cpp
  core/src/engine.cpp
  core/src/lz4_stream.cpp
)
target_link_libraries(archiver PRIVATE archiver_lz4)
target_include_directories(archiver
  PUBLIC core/include
  PRIVATE ${SEVENZIP_SRC}/CPP
)
target_compile_definitions(archiver PRIVATE ARCHIVER_BUILDING _FILE_OFFSET_BITS=64)
archiver_warnings(archiver)
if(NOT MSVC)
  target_compile_options(archiver PRIVATE -Wno-deprecated -Wno-unused-parameter)
endif()
if(WIN32)
  # Static C++ runtime only. 7-Zip stays a separate DLL.
  target_link_libraries(archiver PRIVATE ole32 oleaut32 uuid ntdll)
  if(MINGW)
    target_link_options(archiver PRIVATE -static-libgcc -static-libstdc++)
  endif()
else()
  target_link_libraries(archiver PRIVATE ${CMAKE_DL_LIBS})
endif()
set_target_properties(archiver PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
)

function(archiver_origin target)
  if(UNIX AND NOT APPLE)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "\$ORIGIN"
      INSTALL_RPATH "\$ORIGIN")
  elseif(APPLE)
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "@loader_path"
      INSTALL_RPATH "@loader_path")
  endif()
endfunction()

if(NOT ANDROID)
  enable_testing()
  add_executable(archiver-cli tools/cli/main.cpp)
  target_link_libraries(archiver-cli PRIVATE archiver)
  target_include_directories(archiver-cli PRIVATE core/src)
  archiver_warnings(archiver-cli)
  archiver_origin(archiver-cli)

  add_executable(test_path_safety tests/test_path_safety.cpp)
  target_link_libraries(test_path_safety PRIVATE archiver)
  archiver_warnings(test_path_safety)
  archiver_origin(test_path_safety)
  add_test(NAME path_safety COMMAND test_path_safety)
  add_executable(test_utf tests/test_utf.cpp)
  target_include_directories(test_utf PRIVATE core/src)
  archiver_warnings(test_utf)
  add_test(NAME unicode COMMAND test_utf)
  if(MINGW)
    target_link_options(archiver-cli PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(test_path_safety PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(test_utf PRIVATE -static-libgcc -static-libstdc++)
    target_link_options(archiver-cli PRIVATE -municode)
  endif()
endif()

if(ANDROID)
  # NDK sysroot provides jni.h. This is the .so the Kotlin shell loads.
  add_library(archiver_jni SHARED apps/android/app/src/main/cpp/archiver_jni.cpp)
  target_include_directories(archiver_jni PRIVATE core/include core/src)
  target_link_libraries(archiver_jni PRIVATE archiver)
  archiver_warnings(archiver_jni)
elseif(NOT CMAKE_CROSSCOMPILING)
  find_package(JNI QUIET)
  if(JNI_FOUND)
    add_library(archiver_jni_check OBJECT apps/android/app/src/main/cpp/archiver_jni.cpp)
    target_compile_definitions(archiver_jni_check PRIVATE ARCHIVER_JNI_TEST_CALLBACKS=1)
    target_include_directories(archiver_jni_check PRIVATE ${JNI_INCLUDE_DIRS} core/include core/src)
    archiver_warnings(archiver_jni_check)
    option(ARCHIVER_TEST_JNI "Build a host JNI library for runtime regression tests" OFF)
    if(ARCHIVER_TEST_JNI)
      set_target_properties(archiver_jni_check PROPERTIES POSITION_INDEPENDENT_CODE ON)
      add_library(archiver_jni_host SHARED $<TARGET_OBJECTS:archiver_jni_check>)
      target_link_libraries(archiver_jni_host PRIVATE archiver)
      archiver_origin(archiver_jni_host)
      if(MINGW)
        target_link_options(archiver_jni_host PRIVATE -static-libgcc -static-libstdc++)
      endif()
    endif()
  endif()
endif()

if(NOT ANDROID AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  set(ARCHIVER_TEST_ENGINE "" CACHE FILEPATH "7-Zip shared library for runtime tests")
  set(ARCHIVER_TEST_7Z "" CACHE FILEPATH "Optional 7z command for creating test archives")
  if(Python3_FOUND AND ARCHIVER_TEST_ENGINE)
    set(test_7z_args)
    if(ARCHIVER_TEST_7Z)
      list(APPEND test_7z_args "${ARCHIVER_TEST_7Z}")
    endif()
    add_test(NAME disguised_archives
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_disguise.py"
              $<TARGET_FILE:archiver-cli> "${ARCHIVER_TEST_ENGINE}")
    add_test(NAME format_detection
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_formats.py"
              $<TARGET_FILE:archiver> "${ARCHIVER_TEST_ENGINE}" ${test_7z_args})
    if(ARCHIVER_TEST_7Z)
      add_test(NAME product_workflows
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_product.py"
                $<TARGET_FILE:archiver-cli> "${ARCHIVER_TEST_ENGINE}" "${ARCHIVER_TEST_7Z}")
      add_test(NAME nested_workflows
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_nested.py"
                $<TARGET_FILE:archiver> "${ARCHIVER_TEST_ENGINE}" "${ARCHIVER_TEST_7Z}")
    endif()
    add_test(NAME extraction_regressions
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_regressions.py"
              $<TARGET_FILE:archiver-cli> "${ARCHIVER_TEST_ENGINE}" ${test_7z_args})
    add_test(NAME extraction_progress
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_progress.py"
              $<TARGET_FILE:archiver> "${ARCHIVER_TEST_ENGINE}")
    find_package(Java QUIET COMPONENTS Runtime Development)
    if(ARCHIVER_TEST_JNI AND TARGET archiver_jni_host AND Java_FOUND)
      add_test(NAME jni_unicode
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/test_jni.py"
                "${Java_JAVA_EXECUTABLE}" "${Java_JAVAC_EXECUTABLE}"
                $<TARGET_FILE:archiver> $<TARGET_FILE:archiver_jni_host> "${ARCHIVER_TEST_ENGINE}"
                ${test_7z_args})
    endif()
  endif()
endif()

# Host Qt is an ELF library. A Windows cross build links Qt only when
# ARCHIVER_WINDOWS_QT=ON and CMAKE_PREFIX_PATH points at a MinGW Qt kit.
if(NOT CMAKE_CROSSCOMPILING OR ARCHIVER_WINDOWS_QT)
  find_package(Qt6 QUIET COMPONENTS Widgets)
  if(Qt6_FOUND)
    add_subdirectory(apps/desktop)
  else()
    message(STATUS "Qt 6 Widgets not found. The shared desktop shell was not built.")
  endif()
else()
  message(STATUS "Cross build: Qt shell is not linked. Pass -DARCHIVER_WINDOWS_QT=ON with a MinGW Qt kit to try it.")
endif()
