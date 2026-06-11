cmake_minimum_required(VERSION 3.14)
project(ecewo_multipart VERSION 0.1.0 LANGUAGES C)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

option(ECEWO_MULTIPART_BUILD_SHARED "Build ecewo-multipart as a shared library" OFF)

if(PROJECT_IS_TOP_LEVEL AND NOT TARGET ecewo::ecewo)
  include(FetchContent)

  FetchContent_Declare(
    ecewo
    GIT_REPOSITORY https://github.com/ecewo/ecewo.git
    GIT_TAG v4.0.0
  )

  FetchContent_MakeAvailable(ecewo)
endif()

if(ECEWO_MULTIPART_BUILD_SHARED)
  add_library(ecewo_multipart SHARED src/ecewo-multipart.c)
else()
  add_library(ecewo_multipart STATIC src/ecewo-multipart.c)
endif()

add_library(ecewo::multipart ALIAS ecewo_multipart)

include(GenerateExportHeader)
generate_export_header(ecewo_multipart
  BASE_NAME ECEWO_MULTIPART
  EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/ecewo-multipart-export.h"
)

target_include_directories(ecewo_multipart PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

set_target_properties(ecewo_multipart PROPERTIES
  OUTPUT_NAME ecewo-multipart
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  EXPORT_NAME multipart
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
)

# fs is needed for disk-storage / streaming-to-disk. Pull it via ecewo_add if available.
if(TARGET ecewo::ecewo AND NOT TARGET ecewo::fs)
  ecewo_add(fs@dev)
endif()

target_link_libraries(ecewo_multipart PUBLIC ecewo::ecewo ecewo::fs)

if (PROJECT_IS_TOP_LEVEL AND TARGET ecewo::ecewo)
  ecewo_add(mock@dev)
  enable_testing()

  add_executable(ecewo_multipart_test
    tests/test.c
    tests/runner.c
  )

  target_include_directories(ecewo_multipart_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
      ${ecewo_SOURCE_DIR}/tests
  )

  target_link_libraries(ecewo_multipart_test
    PRIVATE
      ecewo::ecewo
      ecewo::multipart
      ecewo::mock
      ecewo::fs
  )

  if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ecewo_multipart_test PRIVATE
      -Wall
      -Wextra
      -Werror
      -Wstrict-prototypes
      -Wincompatible-pointer-types
      -Wno-unused-parameter
    )
  endif()

  add_test(
    NAME ecewo_multipart_all
    COMMAND ecewo_multipart_test
  )

  add_executable(photo_upload_app
    app/main.c
  )

  target_include_directories(photo_upload_app
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(photo_upload_app
    PRIVATE
      ecewo::ecewo
      ecewo::multipart
      ecewo::fs
  )
endif()
