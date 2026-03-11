cmake_minimum_required(VERSION 3.12)
project(plugins CXX C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0)

set(PLUGINS fifo lru)  # Add more plugins here when you create them

foreach(plugin IN LISTS PLUGINS)
  add_library(plugin_${plugin} SHARED plugin_${plugin}.cpp)

  target_include_directories(plugin_${plugin} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../libCacheSim/include
    ${GLIB_INCLUDE_DIRS})

  set_target_properties(plugin_${plugin} PROPERTIES
    OUTPUT_NAME "plugin_${plugin}_hooks")
endforeach()
