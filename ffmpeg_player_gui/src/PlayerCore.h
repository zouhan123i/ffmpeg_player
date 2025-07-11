#pragma once
#include <QObject>
#include <QString>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>
#include "ThreadSafeQueue.h"
#include <QTimer>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// 日志等级定义
enum LogLevel {
    LOG_ERROR = 0,    // 错误信息
    LOG_WARN = 1,     // 警告信息
    LOG_INFO = 2,     // 一般信息
    LOG_DEBUG = 3,    // 调试信息
    LOG_VERBOSE = 4   // 详细调试信息
};

class QAudioSink;
class QIODevice;

class PlayerCore : public QObject
{
    Q_OBJECT
public:
    explicit PlayerCore(QObject *parent = nullptr);
    ~PlayerCore();
    
    // 设置日志等级
    static void setLogLevel(LogLevel level) { logLevel = level; }
    static LogLevel getLogLevel() { return logLevel; }
    


    bool openFile(const QString &fileName);
    void play();
    void pause();
    void seek(int position); // position: 秒
    void stop();
    bool isPlaying() const { return playing; }
    void prepareForSeek();
    void doSeek(int position);
    void cleanup(); // 只保留这一处声明
    void clearAllQueues();
    void flushAllDecoders();
    void renderPreciseFrame(int position); // 用独立 AVFormatContext 渲染目标帧
    void parseVideoInfo();
    // 视频文件信息解析
    double getVideoDuration() const;
    double getAudioDuration() const;

public slots:
    // 删除 preciseFrameSeek 声明

signals:
    void positionChanged(int position);
    void durationChanged(int duration);
    void frameReady(const QImage &image);
    void stateChanged(bool playing);
    void frameIndexReady(); // 新增：帧索引构建完成信号
    void frameIndexProgress(int percent); // 新增：索引进度信号
    // 删除 requestPreciseSeek 相关声明

private:
    void decodeLoop();
    // 删除 private 区域的 void cleanup(); 声明（如果有）

    QThread* decodeThread = nullptr;
    std::atomic<bool> playing{false};
    std::atomic<bool> stopFlag{false};
    QMutex stateMutex;
    QWaitCondition pauseCond;
    QString currentFile;

    // FFmpeg相关
    AVFormatContext* fmt_ctx = nullptr;
    AVCodecContext* vctx = nullptr;
    AVCodecContext* actx = nullptr;
    SwsContext* sws_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    int videoStream = -1;
    int audioStream = -1;
    int duration = 0;
    std::mutex vctxMutex;
    std::mutex fmtCtxMutex;

    // 音频输出
    QAudioSink* audioSink = nullptr;
    QIODevice* audioDevice = nullptr;
    double seek_target_time = 0; 

    // 多线程解码优化
    ThreadSafeQueue<AVPacket*> audioPacketQueue{200};
    ThreadSafeQueue<AVPacket*> videoPacketQueue{200};
    ThreadSafeQueue<AVFrame*> audioFrameQueue{100};
    ThreadSafeQueue<AVFrame*> videoFrameQueue{100};
    QThread* demuxThread = nullptr;
    QThread* audioDecodeThread = nullptr;
    QThread* videoDecodeThread = nullptr;
    QThread* audioPlayThread = nullptr;
    QThread* videoRenderThread = nullptr;


    void demuxLoop();
    void audioDecodeLoop();
    void videoDecodeLoop();
    void audioPlayLoop();
    void videoRenderLoop();

    

    
    // 音频时钟管理
    std::atomic<bool> audioClockReset{false};
    std::atomic<double> seekTargetTime{0.0};
    

    

    
    // 智能跳转
    bool smartSeek(double targetTime);
    bool preciseSeek(double targetTime);
    bool approximateSeek(double targetTime);
    
    // 静态日志等级
    static LogLevel logLevel;

    QThread* indexingThread = nullptr; // 新增：索引线程
    void startFrameIndexing();         // 新增：启动索引线程
    //void buildFrameIndex();            // 新增：实际索引构建
    std::atomic<bool> indexReady{false}; // 新增：索引状态
    std::atomic<bool> isSeeking{false}; // 新增：seek状态标志

    struct FrameIndexEntry {
        int64_t pts;
        int64_t pos;
        bool is_key;
        int stream_index;
    };
    std::vector<FrameIndexEntry> videoFrameIndex;
    double lastAudioRenderedPts = 0; // 记录最近一次音频帧PTS（单位秒）
}; 