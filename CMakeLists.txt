cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/fx-anim.c
    src/host.c
    src/meters.c
    src/pipe.c
    src/film-filter.c
    src/mosaic-filter.c
    src/vignette-filter.c
    src/glitch-filter.c
    src/chromatic-filter.c
    src/grade-filter.c
    src/lens-filter.c
    src/skin-filter.c
    src/spectrum-analyzer.c
    src/spectrum.c
    src/stinger-transition.c
    src/vendor.c
)

# 帯の計算だけを libobs 無しで確かめる（tools/smoke/analyzer-test.c）。
# 配布物には入らない（install するのはプラグインの target だけ）
add_executable(spectrum-analyzer-test EXCLUDE_FROM_ALL tools/smoke/analyzer-test.c src/spectrum-analyzer.c)
target_include_directories(spectrum-analyzer-test PRIVATE src)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
