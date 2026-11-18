cmake_minimum_required(VERSION 3.10)
project(PlutoSDR_Workspace CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -Wall")
set(OpenGL_GL_PREFERENCE GLVND)

find_package(SoapySDR REQUIRED)
find_package(SDL2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

add_executable(main main.cpp)
target_include_directories(main PRIVATE ${SoapySDR_INCLUDE_DIRS})
target_link_libraries(main PRIVATE ${SoapySDR_LIBRARIES} m)

set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)
set(IMPLOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot)

set(GUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${IMPLOT_DIR}/implot.cpp
    ${IMPLOT_DIR}/implot_items.cpp
)

add_executable(gui gui.cpp ${GUI_SOURCES})
target_include_directories(gui PRIVATE
    ${SoapySDR_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${IMPLOT_DIR}
)
target_link_libraries(gui PRIVATE
    ${SoapySDR_LIBRARIES}
    ${SDL2_LIBRARIES}
    OpenGL::GL
    Threads::Threads
    m
)
