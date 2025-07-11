# ffmpeg_player

## 1. 命令行播放器
- 见 src/ffmpeg_player.cpp

## 2. Qt+FFmpeg 跨平台GUI播放器

### 依赖
- Qt 6.x (推荐用官方安装器或 Homebrew 安装)
- FFmpeg (需开发头文件和动态库)
- CMake 3.16+

### 安装依赖 (macOS)
```sh
brew install qt cmake ffmpeg
```

### 安装依赖 (Windows)
- 推荐用 Qt 官方安装器安装 Qt 6.x
- FFmpeg 可用预编译包，需配置 include/lib 路径
- CMake 官网下载安装

### 编译
```sh
cd ffmpeg_player_gui
mkdir build && cd build
cmake ..
make
```

### 运行
```sh
./ffmpeg_player_gui
```

---
