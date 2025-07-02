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

set(AVFORMAT_INCLUDE_DIRS ./include)
set(AVFORMAT_LIBRARIES ./lib)
# 添加头文件路径
include_directories(
    ${AVFORMAT_INCLUDE_DIRS}
)

link_directories(
    ${AVFORMAT_LIBRARIES}
)

# 添加可执行文件
add_executable(ffmpeg_player ./src/ffmpeg_player.cpp)

# 链接FFmpeg和SDL2库
target_link_libraries(ffmpeg_player
    avformat 
    avcodec 
    avutil 
    swscale
    swresample 
    SDL2
    pthread      # <--- 加这一行
)
