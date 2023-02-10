cmake_minimum_required(VERSION 3.16...3.21)

project(ndi5-texture-filter VERSION 0.0.1)

add_library(${PROJECT_NAME} MODULE)

set(PROJECT_FULL_NAME "NDI5 texture filter")

set(PLUGIN_AUTHOR "mahgu")

target_sources(${PROJECT_NAME} PRIVATE  
  ndi5-texture-filter.h
  ndi5-texture-filter.cpp
)

# Import libobs as main plugin dependency
#find_package(libobs REQUIRED)
#include(cmake/ObsPluginHelpers.cmake)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

if(MSVC)
  target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/../obs-studio/deps/w32-pthreads/)
  target_link_libraries(${PROJECT_NAME} PRIVATE OBS::w32-pthreads)
endif()

find_qt(VERSION 6 COMPONENTS Widgets Core)

target_link_libraries(${PROJECT_NAME} PRIVATE
  OBS::libobs
  Qt::Core
  Qt::Widgets
  OBS::frontend-api)

set_target_properties(${project_name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
)

# --- Windows-specific build settings and tasks ---
if(OS_WINDOWS)
  configure_file(cmake/bundle/windows/installer-Windows.iss.in
                 ${CMAKE_BINARY_DIR}/installer-Windows.generated.iss)

  configure_file(cmake/bundle/windows/resource.rc.in ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.rc)
  target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/${PROJECT_NAME}.rc)

  if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /WX-)
  endif()
  # --- End of section ---


endif()

set_target_properties(${PROJECT_NAME} PROPERTIES FOLDER "plugins/${PLUGIN_AUTHOR}")

setup_plugin_target(${PROJECT_NAME})
