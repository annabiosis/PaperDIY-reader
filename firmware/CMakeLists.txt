cmake_minimum_required(VERSION 3.16)

# Standalone firmware project. Core components (M5GFX, M5Unified, LGFX_Fonts)
# come from the vendored upstream demo so the board support stays identical.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/M5PaperDIY-UserDemo/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(ereader)
