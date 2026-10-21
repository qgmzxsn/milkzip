cmake_minimum_required(VERSION 3.16)
project(archiver_jni LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Not built in the phase 1 environment. Android Studio would compile this
# after 7z.so itself has been built with the NDK and placed in jniLibs.
if(NOT SEVENZIP_SRC)
  message(FATAL_ERROR "SEVENZIP_SRC is required. This Android native build was not run.")
endif()

add_library(archiver_jni SHARED
  archiver_jni.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/path_safety.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/safe_write.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/engine.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src/lz4_stream.cpp
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/lz4/lz4.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/lz4/lz4frame.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/lz4/lz4hc.c
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/lz4/xxhash.c
)
target_include_directories(archiver_jni PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/include
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../core/src
  ${CMAKE_CURRENT_LIST_DIR}/../../../../../../third_party/lz4
  ${SEVENZIP_SRC}/CPP
)
target_compile_definitions(archiver_jni PRIVATE ARCHIVER_BUILDING _FILE_OFFSET_BITS=64)
