cmake_minimum_required(VERSION 3.10)
project(MXWrite LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED 
    libavcodec 
    libavformat 
    libavutil 
    libswscale 
)
add_library(mxwrite STATIC mxwrite.cpp)
target_include_directories(mxwrite PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(mxwrite PRIVATE ${FFMPEG_LIBRARIES})
install(FILES mxwrite.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS mxwrite
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})