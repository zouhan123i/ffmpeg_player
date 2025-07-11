# CMakeLists.txt

cmake_minimum_required(VERSION 3.5)
project(FFmpegPlayer)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
# 启用调试信息
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
# # 查找FFmpeg和SDL2库
# find_package(PkgConfig REQUIRED)
# pkg_check_modules(AVFORMAT REQUIRED libavformat)
# pkg_check_modules(AVCODEC REQUIRED libavcodec)
# pkg_check_modules(AVUTIL REQUIRED libavutil)
# pkg_check_modules(SWSCALE REQUIRED libswscale)
# pkg_check_modules(SDL2 REQUIRED sdl2)

# 查找FFmpeg和SDL2库
find_package(PkgConfig REQUIRED)
pkg_check_modules(AVFORMAT REQUIRED libavformat)
pkg_check_modules(AVCODEC REQUIRED libavcodec)
pkg_check_modules(AVUTIL REQUIRED libavutil)
pkg_check_modules(SWSCALE REQUIRED libswscale)
pkg_check_modules(SWRESAMPLE REQUIRED libswresample)
pkg_check_modules(SDL2 REQUIRED sdl2)

include_directories(
    ${AVFORMAT_INCLUDE_DIRS}
    ${AVCODEC_INCLUDE_DIRS}
    ${AVUTIL_INCLUDE_DIRS}
    ${SWSCALE_INCLUDE_DIRS}
    ${SWRESAMPLE_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

link_directories(
    ${AVFORMAT_LIBRARY_DIRS}
    ${AVCODEC_LIBRARY_DIRS}
    ${AVUTIL_LIBRARY_DIRS}
    ${SWSCALE_LIBRARY_DIRS}
    ${SWRESAMPLE_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
)

# 添加可执行文件
add_executable(ffmpeg_player ./src/ffmpeg_player.cpp)

# 链接FFmpeg和SDL2库
target_link_libraries(ffmpeg_player
    ${AVFORMAT_LIBRARIES}
    ${AVCODEC_LIBRARIES}
    ${AVUTIL_LIBRARIES}
    ${SWSCALE_LIBRARIES}
    ${SWRESAMPLE_LIBRARIES}
    ${SDL2_LIBRARIES}
    pthread
)

# Qt+FFmpeg GUI播放器子项目
add_subdirectory(ffmpeg_player_gui)
