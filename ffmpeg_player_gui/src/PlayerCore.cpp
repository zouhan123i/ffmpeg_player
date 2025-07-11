#include "PlayerCore.h"
#include <QObject>
#include <QString>
#include <QImage>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QTimer>
#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QLabel>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QMediaDevices>
#include <QtMultimedia/QAudioDevice>
#include <QtMultimedia/QAudioSink>
#include <QFileDialog>
#include <QMessageBox>
#include <thread>
#include <mutex>
#include <atomic>
extern "C" {
#include "libavformat/avformat.h"
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
}

// 初始化静态日志等级
LogLevel PlayerCore::logLevel = LOG_INFO;

// 日志宏定义
#define LOG_ERROR(msg) if (PlayerCore::getLogLevel() >= LOG_ERROR) qDebug() << "[ERROR]" << msg
#define LOG_WARN(msg) if (PlayerCore::getLogLevel() >= LOG_WARN) qDebug() << "[WARN]" << msg
#define LOG_INFO(msg) if (PlayerCore::getLogLevel() >= LOG_INFO) qDebug() << "[INFO]" << msg
#define LOG_DEBUG(msg) if (PlayerCore::getLogLevel() >= LOG_DEBUG) qDebug() << "[DEBUG]" << msg
#define LOG_VERBOSE(msg) if (PlayerCore::getLogLevel() >= LOG_VERBOSE) qDebug() << "[VERBOSE]" << msg

PlayerCore::PlayerCore(QObject *parent) : QObject(parent) {}

PlayerCore::~PlayerCore() {
    stop();
    cleanup();
}

void PlayerCore::demuxLoop() {
    LOG_INFO("demuxLoop started");
    int packetCount = 0;
    
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) {
            LOG_INFO("demuxLoop: paused, waiting for resume");
            pauseCond.wait(&stateMutex);
            LOG_INFO("demuxLoop: resumed");
        }
        stateMutex.unlock();
        if (stopFlag) break;
        
        // Check if fmt_ctx is valid before using it
        {
            std::lock_guard<std::mutex> lock(fmtCtxMutex);
            if (!fmt_ctx) {
                LOG_ERROR("demuxLoop: fmt_ctx is null, exiting");
                break;
            }
        }
        
        AVPacket* pkt = av_packet_alloc();
        int ret;
        {
            std::lock_guard<std::mutex> lock(fmtCtxMutex);
            if (!fmt_ctx) {
                av_packet_free(&pkt);
                break;
            }
            ret = av_read_frame(fmt_ctx, pkt);
        }
        
        if (ret < 0) {
            LOG_INFO("demuxLoop: av_read_frame finished, ret=" << ret);
            av_packet_free(&pkt);
            break;
        }
        
        packetCount++;
        if (packetCount % 100 == 0) {
            LOG_INFO("demuxLoop: processed" << packetCount << "packets");
        }
        
        if (pkt->stream_index == videoStream) {
            videoPacketQueue.push(pkt);
        } else if (pkt->stream_index == audioStream) {
            audioPacketQueue.push(pkt);
        } else {
            av_packet_free(&pkt);
        }
    }
    LOG_INFO("demuxLoop exit, processed" << packetCount << "packets");
}

void PlayerCore::audioDecodeLoop() {
    qDebug() << "[audioDecodeLoop] started";
    int packetCount = 0;
    int frameCount = 0;
    int noPacketCount = 0;  // 统计没有包的情况
    
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) {
            qDebug() << "[audioDecodeLoop] paused, waiting for resume";
            pauseCond.wait(&stateMutex);
            qDebug() << "[audioDecodeLoop] resumed";
            noPacketCount = 0;  // 重置计数器
        }
        stateMutex.unlock();
        if (stopFlag) break;
        
        AVPacket* pkt = nullptr;
        if (!audioPacketQueue.try_pop(pkt)) {
            noPacketCount++;
            if (noPacketCount % 100 == 0) {  // 每100次没有包时打印一次
                qDebug() << "[audioDecodeLoop] no packet in queue, count=" << noPacketCount;
            }
            QThread::msleep(10);
            continue;
        }
        
        noPacketCount = 0;  // 重置计数器
        
        if (stopFlag) break;
        if (!pkt || !actx) {
            if (pkt) av_packet_free(&pkt);
            continue;
        }
        packetCount++;
        if (packetCount % 100 == 0) {
            qDebug() << "[audioDecodeLoop] processed" << packetCount << "packets," << frameCount << "frames";
        }
        
        // 添加包时间戳调试信息
        if (packetCount % 50 == 0) {
            double pts = pkt->pts * av_q2d(fmt_ctx->streams[audioStream]->time_base);
            qDebug() << "[audioDecodeLoop] packet" << packetCount << "PTS=" << pts << "s";
        }
        
        int ret = avcodec_send_packet(actx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            qDebug() << "[audioDecodeLoop] avcodec_send_packet error, ret=" << ret;
            continue;
        }
        AVFrame* frame = av_frame_alloc();
        while (avcodec_receive_frame(actx, frame) == 0) {
            if (stopFlag) { av_frame_free(&frame); break; }
            frameCount++;
            audioFrameQueue.push(frame);
            
            // 添加帧时间戳调试信息
            if (frameCount % 50 == 0) {
                double pts = frame->pts * av_q2d(fmt_ctx->streams[audioStream]->time_base);
                qDebug() << "[audioDecodeLoop] frame" << frameCount << "PTS=" << pts << "s";
            }
            
            frame = av_frame_alloc();
        }
        av_frame_free(&frame);
    }
    qDebug() << "[audioDecodeLoop] exit, processed" << packetCount << "packets," << frameCount << "frames";
}

void PlayerCore::videoDecodeLoop() {
    qDebug() << "[videoDecodeLoop] started, thread=" << QThread::currentThread();
    int packetCount = 0;
    int frameCount = 0;
    
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) {
            qDebug() << "[videoDecodeLoop] paused, waiting for resume";
            pauseCond.wait(&stateMutex);
            qDebug() << "[videoDecodeLoop] resumed";
        }
        stateMutex.unlock();
        if (stopFlag) break;
        
        AVPacket* pkt = nullptr;
        static int video_no_packet_count = 0;
        if (videoPacketQueue.empty()) {
            video_no_packet_count++;
            if (video_no_packet_count % 100 == 0) {
                qDebug() << "[videoDecodeLoop] no packet in queue, sleeping";
            }
            QThread::msleep(10);
            continue;
        }
        if (!videoPacketQueue.try_pop(pkt)) {
            video_no_packet_count++;
            if (video_no_packet_count % 100 == 0) {
                qDebug() << "[videoDecodeLoop] no packet in queue, sleeping";
            }
            QThread::msleep(10);
            continue;
        }
        video_no_packet_count = 0;
        
        if (stopFlag) break;
        
        std::lock_guard<std::mutex> lock(vctxMutex);
        if (!pkt || !vctx) {
            if (pkt) av_packet_free(&pkt);
            qDebug() << "[videoDecodeLoop] invalid packet or context, pkt=" << pkt << " vctx=" << vctx;
            continue;
        }
        
        packetCount++;
        if (packetCount % 50 == 0) {
            qDebug() << "[videoDecodeLoop] processed" << packetCount << "packets," << frameCount << "frames";
        }
        
        if (!pkt->data || pkt->size <= 0) {
            av_packet_free(&pkt);
            continue;
        }
        
        if (vctx->codec_id == AV_CODEC_ID_NONE) {
            av_packet_free(&pkt);
            continue;
        }
        
        int ret = avcodec_send_packet(vctx, pkt);
        av_packet_free(&pkt);
        if (ret < 0) {
            qDebug() << "[videoDecodeLoop] avcodec_send_packet error, ret=" << ret;
            continue;
        }
        
        AVFrame* frame = av_frame_alloc();
        int frameReceived = 0;
        while (avcodec_receive_frame(vctx, frame) == 0) {
            if (stopFlag) { av_frame_free(&frame); break; }
            frameCount++;
            frameReceived++;
            videoFrameQueue.push(frame);
            static int video_push_log_count = 0;
            video_push_log_count++;
            if (video_push_log_count % 100 == 0) {
                qDebug() << "[videoDecodeLoop] push frame to videoFrameQueue, pts=" << frame->pts;
            }
            frame = av_frame_alloc();
        }
        
        av_frame_free(&frame);
    }
    qDebug() << "[videoDecodeLoop] exit, processed" << packetCount << "packets," << frameCount << "frames";
}

void PlayerCore::audioPlayLoop() {
    qDebug() << "[audioPlayLoop] started";
    double playback_start_time = 0.0;  // 播放开始的时间点（系统时间）
    double audio_start_pts = 0.0;      // 音频PTS的起始点
    bool first_frame = true;
    
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) pauseCond.wait(&stateMutex);
        stateMutex.unlock();
        if (stopFlag) break;
        
        AVFrame* frame = nullptr;
        static int audio_no_frame_count = 0;
        if (!audioFrameQueue.try_pop(frame)) {
            audio_no_frame_count++;
            if (audio_no_frame_count % 100 == 0) {
                qDebug() << "[audioPlayLoop] no frame in queue, sleeping";
            }
            QThread::msleep(10);
            continue;
        }
        audio_no_frame_count = 0;
        if (stopFlag) break;
        if (!frame || !swr_ctx || !actx) {
            if (frame) av_frame_free(&frame);
            continue;
        }
        if (frame->nb_samples <= 0) {
            av_frame_free(&frame);
            continue;
        }
        
        // 检查暂停状态
        if (!playing) {
            av_frame_free(&frame);
            continue;
        }
        
        // 计算音频帧的时间戳
        double audio_pts = frame->pts * av_q2d(fmt_ctx->streams[audioStream]->time_base);
        double current_time = QDateTime::currentMSecsSinceEpoch() / 1000.0;
        
        // 添加音频PTS调试信息
        static int pts_debug_count = 0;
        pts_debug_count++;
        if (pts_debug_count % 50 == 0) {
            qDebug() << "[audioPlayLoop] PTS debug: frame_pts=" << frame->pts 
                     << "time_base=" << av_q2d(fmt_ctx->streams[audioStream]->time_base)
                     << "audio_pts=" << audio_pts;
        }
        
        // 检查是否需要重置音频时钟（seek后）
        if (audioClockReset.load()) {
            double targetTime = seekTargetTime.load();
            playback_start_time = current_time;
            audio_start_pts = audio_pts;  // 使用当前帧的PTS作为起始点
            first_frame = false;
            audioClockReset = false;
            qDebug() << "[audioPlayLoop] audio clock reset after seek, playback_start_time=" << playback_start_time 
                     << " audio_start_pts=" << audio_start_pts << " target_time=" << targetTime << " audio_pts=" << audio_pts;
            // 直接播放当前帧，不再丢弃任何帧
        }
        
        // 计算应该播放的时间
        double relative_audio_pts = audio_pts - audio_start_pts;
        double target_time = playback_start_time + relative_audio_pts;
        double wait_time = target_time - current_time;
        
        // 添加调试信息
        static int frame_count = 0;
        frame_count++;
        if (frame_count % 100 == 0) {
            qDebug() << "[audioPlayLoop] frame" << frame_count << "audio_pts=" << audio_pts 
                     << "relative_pts=" << relative_audio_pts << "current_time=" << current_time 
                     << "playback_start=" << playback_start_time << "target_time=" << target_time 
                     << "wait_time=" << wait_time;
        }
        
        if (first_frame) {
            playback_start_time = current_time;
            audio_start_pts = audio_pts;
            first_frame = false;
            qDebug() << "[audioPlayLoop] first frame, playback_start_time=" << playback_start_time 
                     << " audio_start_pts=" << audio_start_pts << " audio_pts=" << audio_pts;
        }
        
        // 如果还没到播放时间，等待（使用Qt条件变量）
        if (wait_time > 0.001) { // 1ms精度
            // 使用Qt条件变量等待，而不是sleep循环
            QMutexLocker locker(&stateMutex);
            auto start = std::chrono::steady_clock::now();
            auto timeout = std::chrono::milliseconds(static_cast<int>(wait_time * 1000));
            
            // Qt的QWaitCondition没有wait_for，我们使用循环检查
            while (!stopFlag && playing) {
                double current_time = QDateTime::currentMSecsSinceEpoch() / 1000.0;
                if (current_time >= target_time) break;
                
                if (std::chrono::steady_clock::now() - start >= timeout) {
                    qDebug() << "[audioPlayLoop] wait timeout, continuing";
                    break;
                }
                
                pauseCond.wait(&stateMutex, 10);  // 等待10ms
            }
        }
        
        // 再次检查暂停状态
        if (!playing) {
            av_frame_free(&frame);
            continue;
        }
        
        // 使用栈上分配的缓冲区，避免频繁的内存分配/释放
        uint8_t audioBuf[192000];
        int audioBufSize = 192000;
        uint8_t* audioBufPtr = audioBuf;  // 创建指向缓冲区的指针
        int out_samples = swr_convert(
            swr_ctx,
            &audioBufPtr, audioBufSize / actx->ch_layout.nb_channels / 2,
            (const uint8_t**)frame->data, frame->nb_samples
        );
        int data_size = out_samples * actx->ch_layout.nb_channels * 2;
        if (data_size > 0 && audioDevice && audioSink && playing) {
            // 添加缓冲区状态调试信息
            static int buffer_timeout_count = 0;
            int bytes_free = audioSink->bytesFree();
            
            // 每100帧打印一次缓冲区状态
            static int frame_debug_count = 0;
            frame_debug_count++;
            if (frame_debug_count % 100 == 0) {
                qDebug() << "[audioPlayLoop] buffer status: bytes_free=" << bytes_free 
                         << "data_size=" << data_size << "required=" << data_size / 2;
            }
            
            // 等待音频缓冲区有足够空间（使用Qt条件变量）
            QMutexLocker locker(&stateMutex);
            auto start = std::chrono::steady_clock::now();
            auto timeout = std::chrono::milliseconds(10);  // 减少超时时间到10ms
            
            // Qt的QWaitCondition没有wait_for，我们使用循环检查
            while (!stopFlag && playing && audioSink->bytesFree() < data_size / 2 &&
                   std::chrono::steady_clock::now() - start < timeout) {
                pauseCond.wait(&stateMutex, 2);  // 减少等待时间到2ms
            }
            
            if (playing && audioSink->bytesFree() >= data_size / 2) {
                audioDevice->write((const char*)audioBuf, data_size);
                lastAudioRenderedPts = audio_pts; // 记录音频帧PTS
                buffer_timeout_count = 0;  // 重置计数器
            } else {
                // 超时或状态改变，跳过这一帧
                buffer_timeout_count++;
                if (buffer_timeout_count % 5 == 0) {  // 每5次超时打印一次详细信息
                    qDebug() << "[audioPlayLoop] buffer wait timeout #" << buffer_timeout_count 
                             << "bytes_free=" << audioSink->bytesFree() << "data_size=" << data_size 
                             << "required=" << data_size / 2 << "playing=" << playing;
                } else {
                    qDebug() << "[audioPlayLoop] buffer wait timeout, skipping frame";
                }
            }
        }
        av_frame_free(&frame);
    }
    qDebug() << "[audioPlayLoop] ended";
}

void PlayerCore::videoRenderLoop() {
    qDebug() << "[videoRenderLoop] started, thread=" << QThread::currentThread();
    
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) pauseCond.wait(&stateMutex);
        stateMutex.unlock();
        if (stopFlag) break;
        
        AVFrame* frame = nullptr;
        static int video_no_frame_count = 0;
        if (videoFrameQueue.empty()) {
            video_no_frame_count++;
            if (video_no_frame_count % 100 == 0) {
                qDebug() << "[videoRenderLoop] no frame in queue, sleeping";
            }
            QThread::msleep(10);
            continue;
        }
        if (!videoFrameQueue.try_pop(frame)) {
            video_no_frame_count++;
            if (video_no_frame_count % 100 == 0) {
                qDebug() << "[videoRenderLoop] no frame in queue, sleeping";
            }
            QThread::msleep(10);
            continue;
        }
        video_no_frame_count = 0;
        
        // 检查暂停状态
        if (!playing) {
            av_frame_free(&frame);
            continue;
        }
        
        int w = frame->width;
        int h = frame->height;
        
        // 初始化 sws_ctx（如果还没有初始化）
        if (!sws_ctx) {
            qDebug() << "[videoRenderLoop] initializing sws_ctx for format:" << frame->format;
            sws_ctx = sws_getContext(w, h, (AVPixelFormat)frame->format, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws_ctx) {
                qWarning() << "[videoRenderLoop] failed to create sws_ctx!";
                av_frame_free(&frame);
                continue;
            }
            qDebug() << "[videoRenderLoop] sws_ctx initialized successfully";
        }
        
        // 音视频同步：根据时间戳和音频时钟控制渲染时机
        double audio_clock = lastAudioRenderedPts;
        double video_pts = frame->pts * av_q2d(fmt_ctx->streams[videoStream]->time_base);
        double sync_threshold = 0.2; // 200ms 同步阈值，跳转后更宽松
        
        static int video_sync_log_count = 0;
        video_sync_log_count++;
        if (video_sync_log_count % 100 == 0) {
            LOG_INFO("videoRenderLoop: sync: audio_clock=" << audio_clock << " video_pts=" << video_pts << " diff=" << (video_pts - audio_clock));
        }
        
        // 跳转后，给视频更多时间重新同步
        static bool seekJustHappened = false;
        if (audioClockReset.load()) {
            seekJustHappened = true;
            sync_threshold = 0.5; // 跳转后500ms阈值
        }
        
        // 如果视频超前音频太多，等待音频（使用Qt条件变量）
        if (video_pts > audio_clock + sync_threshold) {
            if (video_sync_log_count % 100 == 0) {
                LOG_INFO("videoRenderLoop: video ahead, waiting for audio. sleep_ms=" << (video_pts - audio_clock - sync_threshold) * 1000);
            }
            double wait_time = video_pts - audio_clock - sync_threshold;
            
            // 使用Qt条件变量等待，而不是sleep循环
            QMutexLocker locker(&stateMutex);
            auto start = std::chrono::steady_clock::now();
            auto timeout = std::chrono::milliseconds(static_cast<int>(wait_time * 1000));
            
            // Qt的QWaitCondition没有wait_for，我们使用循环检查
            while (!stopFlag && playing && video_pts > audio_clock + sync_threshold &&
                   std::chrono::steady_clock::now() - start < timeout) {
                pauseCond.wait(&stateMutex, 10);  // 等待10ms
            }
            
            if (std::chrono::steady_clock::now() - start >= timeout) {
                LOG_DEBUG("videoRenderLoop: wait timeout, continuing");
            }
        }
        // 如果视频落后音频太多，跳过这一帧（但阈值更宽松）
        else if (video_pts < audio_clock - sync_threshold * 2) {  // 400ms落后才跳过
            qDebug() << "[videoRenderLoop] video behind, skipping frame, video_pts=" << video_pts << ", audio_clock=" << audio_clock;
            av_frame_free(&frame);
            continue;
        }
        
        // 如果跳转刚发生，逐渐恢复正常阈值
        if (seekJustHappened && !audioClockReset.load()) {
            seekJustHappened = false;
        }
        
        // 再次检查暂停状态
        if (playing) {
            // 假设frame为AVFrame*，sws_ctx已初始化
            int w = frame->width;
            int h = frame->height;
            QImage img(w, h, QImage::Format_RGB888);
            uint8_t *dst_data[4] = { img.bits(), nullptr, nullptr, nullptr };
            int dst_linesize[4] = { static_cast<int>(img.bytesPerLine()), 0, 0, 0 };
            sws_scale(sws_ctx, frame->data, frame->linesize, 0, h, dst_data, dst_linesize);
            static int video_emit_log_count = 0;
            video_emit_log_count++;
            if (video_emit_log_count % 100 == 0) {
                qDebug() << "[videoRenderLoop] emit frameReady, pts=" << frame->pts;
            }
            emit frameReady(img);
            
            // 发送进度更新信号
            if (frame->pts != AV_NOPTS_VALUE) {
                int pos = frame->pts * av_q2d(fmt_ctx->streams[videoStream]->time_base);
                emit positionChanged(pos);
            }
            
        }
        
        av_frame_free(&frame);
    }
    qDebug() << "[videoRenderLoop] ended";
}

bool PlayerCore::openFile(const QString &fileName) {
    // 如果已经有文件在播放，先停止当前播放
    if (fmt_ctx) {
        stopFlag = true;
        playing = false;
        pauseCond.wakeAll();
        videoPacketQueue.wakeAll();
        audioPacketQueue.wakeAll();
        audioFrameQueue.wakeAll();
        videoFrameQueue.wakeAll();
        
        // 等待所有线程结束
        if (demuxThread) {
            demuxThread->quit();
            demuxThread->wait();
            delete demuxThread;
            demuxThread = nullptr;
        }
        if (audioDecodeThread) {
            audioDecodeThread->quit();
            audioDecodeThread->wait();
            delete audioDecodeThread;
            audioDecodeThread = nullptr;
        }
        if (videoDecodeThread) {
            videoDecodeThread->quit();
            videoDecodeThread->wait();
            delete videoDecodeThread;
            videoDecodeThread = nullptr;
        }
        if (audioPlayThread) {
            audioPlayThread->quit();
            audioPlayThread->wait();
            delete audioPlayThread;
            audioPlayThread = nullptr;
        }
        if (videoRenderThread) {
            videoRenderThread->quit();
            videoRenderThread->wait();
            delete videoRenderThread;
            videoRenderThread = nullptr;
        }
        
        // 清理资源
        cleanup();
    }
    
    currentFile = fileName;
    // 打开文件
    if (avformat_open_input(&fmt_ctx, fileName.toUtf8().data(), nullptr, nullptr) < 0) {
        qWarning() << "Failed to open file:" << fileName;
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        qWarning() << "Failed to get stream info.";
        return false;
    }
    // 查找视频流
    videoStream = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStream >= 0) {
        AVCodecParameters* vpar = fmt_ctx->streams[videoStream]->codecpar;
        const AVCodec* vdec = avcodec_find_decoder(vpar->codec_id);
        if (!vdec) {
            qWarning() << "Failed to find video decoder for codec_id:" << vpar->codec_id;
            return false;
        }
        vctx = avcodec_alloc_context3(vdec);
        if (!vctx) {
            qWarning() << "Failed to allocate video codec context";
            return false;
        }
        if (avcodec_parameters_to_context(vctx, vpar) < 0) {
            qWarning() << "Failed to copy video codec parameters";
            avcodec_free_context(&vctx);
            return false;
        }
        if (avcodec_open2(vctx, vdec, nullptr) < 0) {
            qWarning() << "Failed to open video codec";
            avcodec_free_context(&vctx);
            return false;
        }
        qDebug() << "[openFile] video codec initialized successfully, codec_id=" << vctx->codec_id;
    }
    // 查找音频流
    audioStream = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (audioStream >= 0) {
        AVCodecParameters* apar = fmt_ctx->streams[audioStream]->codecpar;
        const AVCodec* adec = avcodec_find_decoder(apar->codec_id);
        if (!adec) {
            qWarning() << "Failed to find audio decoder for codec_id:" << apar->codec_id;
            return false;
        }
        actx = avcodec_alloc_context3(adec);
        if (!actx) {
            qWarning() << "Failed to allocate audio codec context";
            return false;
        }
        if (avcodec_parameters_to_context(actx, apar) < 0) {
            qWarning() << "Failed to copy audio codec parameters";
            avcodec_free_context(&actx);
            return false;
        }
        if (avcodec_open2(actx, adec, nullptr) < 0) {
            qWarning() << "Failed to open audio codec";
            avcodec_free_context(&actx);
            return false;
        }
        qDebug() << "[openFile] audio codec initialized successfully, codec_id=" << actx->codec_id;
    }
    
    // 初始化音频输出设备
    if (actx) {
        QAudioFormat fmt;
        fmt.setSampleRate(actx->sample_rate);
        fmt.setChannelCount(actx->ch_layout.nb_channels);
        fmt.setSampleFormat(QAudioFormat::Int16);
        QAudioDevice device = QMediaDevices::defaultAudioOutput();
        audioSink = new QAudioSink(device, fmt, this);
        audioSink->setBufferSize(81920);
        audioDevice = audioSink->start();
        
        // 重采样器
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, actx->ch_layout.nb_channels);
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            qWarning() << "Failed to allocate SwrContext.";
        } else if (swr_alloc_set_opts2(
            &swr_ctx,
            &out_layout,
            AV_SAMPLE_FMT_S16,
            actx->sample_rate,
            &actx->ch_layout,
            actx->sample_fmt,
            actx->sample_rate,
            0, nullptr
        ) < 0) {
            qWarning() << "Failed to set SwrContext options.";
        }
        swr_init(swr_ctx);
        av_channel_layout_uninit(&out_layout);
        qDebug() << "[openFile] audio device initialized, sample_rate=" << actx->sample_rate << " channels=" << actx->ch_layout.nb_channels;
    }
    
    // 解析视频文件信息
    parseVideoInfo();
    
    // 建立关键帧索引表 - 暂时禁用，避免线程安全问题
    // QThread::create([this]{
    //     qDebug() << "[openFile] starting async key frame index building";
    //     buildKeyFrameIndex();
    //     qDebug() << "[openFile] async key frame index building completed";
    // })->start();
    
    // 修正duration赋值逻辑
    duration = 0;
    if (fmt_ctx && fmt_ctx->duration > 0) {
        duration = fmt_ctx->duration / AV_TIME_BASE;
        qDebug() << "[openFile] use container duration:" << duration;
    }
    if (duration <= 0) {
        duration = getVideoDuration();
        qDebug() << "[openFile] use video duration:" << duration;
    }
    if (duration <= 0) {
        duration = getAudioDuration();
        qDebug() << "[openFile] use audio duration:" << duration;
    }
    qDebug() << "[openFile] final duration:" << duration;
    emit durationChanged(duration);
    stopFlag = false;
    playing = true;
    // 启动Demux线程
    qDebug() << "[openFile] starting demux thread, fmt_ctx=" << fmt_ctx;
    demuxThread = QThread::create([this]{ demuxLoop(); });
    demuxThread->start();
    // 启动音频解码线程
    audioDecodeThread = QThread::create([this]{ audioDecodeLoop(); });
    audioDecodeThread->start();
    // 启动视频解码线程
    videoDecodeThread = QThread::create([this]{ videoDecodeLoop(); });
    videoDecodeThread->start();
    // 启动音频播放线程
    audioPlayThread = QThread::create([this]{ audioPlayLoop(); });
    audioPlayThread->start();
    // 启动视频渲染线程
    qDebug() << "[openFile] starting video render thread";
    videoRenderThread = QThread::create([this]{ videoRenderLoop(); });
    videoRenderThread->start();
    
    // FFmpeg自动处理索引，无需额外线程
    qDebug() << "[openFile] using FFmpeg's built-in keyframe index";
    
    // 启动帧索引线程
    startFrameIndexing();
    
    // 移除重复的decodeThread启动
    // decodeThread = QThread::create([this]{ decodeLoop(); });
    // decodeThread->start();
    emit stateChanged(true);
    return true;
}

void PlayerCore::play() {
    playing = true;
    pauseCond.wakeAll();
    emit stateChanged(true);
}

void PlayerCore::pause() {
    playing = false;
    emit stateChanged(false);
}

void PlayerCore::seek(int position) {
    qDebug() << "[seek] called, position=" << position;
    if (!fmt_ctx) {
        qWarning() << "[seek] fmt_ctx is null, cannot seek";
        return;
    }
    if (position < 0 || position > duration) {
        qWarning() << "[seek] invalid position:" << position << "duration:" << duration;
        return;
    }
    seek_target_time = position;
    playing = false;
    isSeeking = true;
    //pauseCond.wakeAll();
    videoPacketQueue.wakeAll();
    audioPacketQueue.wakeAll();
    videoFrameQueue.wakeAll();
    audioFrameQueue.wakeAll();
    QThread::msleep(50);
    audioPacketQueue.clear();
    videoPacketQueue.clear();
    audioFrameQueue.clear();
    videoFrameQueue.clear();
    LOG_INFO("seek: using FFmpeg's built-in keyframe index");
    bool seekSuccess = smartSeek(position);
    if (!seekSuccess) {
        LOG_ERROR("seek: smart seek failed for position" << position);
        isSeeking = false;
        playing = true;
        pauseCond.wakeAll();
        videoPacketQueue.wakeAll();
        audioPacketQueue.wakeAll();
        videoFrameQueue.wakeAll();
        audioFrameQueue.wakeAll();
        return;
    }
    LOG_INFO("seek: smart seek operation successful");
    if (vctx) {
        avcodec_flush_buffers(vctx);
        qDebug() << "[seek] flushed video decoder buffers";
    }
    if (actx) {
        avcodec_flush_buffers(actx);
        qDebug() << "[seek] flushed audio decoder buffers";
    }
    seekTargetTime = position;
    audioClockReset = true;
    lastAudioRenderedPts = seek_target_time; // 同步 lastAudioRenderedPts
    if (audioSink) {
        audioSink->reset();
        audioDevice = audioSink->start();
        QThread::msleep(50);
        qDebug() << "[seek] audio buffer status: bytes_free=" << audioSink->bytesFree() 
                 << "buffer_size=" << audioSink->bufferSize();
    }
    isSeeking = false;
    playing = true;
    pauseCond.wakeAll();
    videoPacketQueue.wakeAll();
    audioPacketQueue.wakeAll();
    videoFrameQueue.wakeAll();
    audioFrameQueue.wakeAll();
    emit positionChanged(position);
    QThread::msleep(100);
    qDebug() << "[seek] completed, position=" << position << "resumed all threads";
}

void PlayerCore::prepareForSeek() {
    playing = false;
    pauseCond.wakeAll();
    //QThread::msleep(50); // 等待所有线程响应暂停

    // 只在这里清空队列
    audioPacketQueue.clear();
    videoPacketQueue.clear();
    audioFrameQueue.clear();
    videoFrameQueue.clear();

    // 其他reset/flush操作...
}

void PlayerCore::doSeek(int position) {
    if (!fmt_ctx) return;
    if (position < 0 || position > duration) return;
    playing = false;
    isSeeking = true;
    pauseCond.wakeAll();
    videoPacketQueue.wakeAll();
    audioPacketQueue.wakeAll();
    videoFrameQueue.wakeAll();
    audioFrameQueue.wakeAll();
    QThread::msleep(50);
    smartSeek(position);
    if (vctx) avcodec_flush_buffers(vctx);
    if (actx) avcodec_flush_buffers(actx);
    if (audioSink) {
        audioSink->reset();
        audioDevice = audioSink->start();
        qDebug() << "[doSeek] audio device reset, bytes_free=" << audioSink->bytesFree();
    }
    seekTargetTime = position;
    audioClockReset = true;
    lastAudioRenderedPts = seek_target_time; // 同步 lastAudioRenderedPts
    isSeeking = false;
    playing = true;
    pauseCond.wakeAll();
    videoPacketQueue.wakeAll();
    audioPacketQueue.wakeAll();
    videoFrameQueue.wakeAll();
    audioFrameQueue.wakeAll();
    emit positionChanged(position);
}



void PlayerCore::stop() {
    qDebug() << "[stop] called, thread=" << QThread::currentThread();
    if (QThread::currentThread() != qApp->thread()) {
        qWarning() << "PlayerCore::stop() must only be called from the main (UI) thread!";
        return;
    }
    stopFlag = true;
    playing = false;
    pauseCond.wakeAll();
    videoPacketQueue.wakeAll();
    audioPacketQueue.wakeAll();
    audioFrameQueue.wakeAll();
    videoFrameQueue.wakeAll();
    QThread* current = QThread::currentThread();
    if (demuxThread) {
        demuxThread->quit();
        if (demuxThread != current) demuxThread->wait();
        delete demuxThread;
        demuxThread = nullptr;
    }
    if (audioDecodeThread) {
        audioDecodeThread->quit();
        if (audioDecodeThread != current) audioDecodeThread->wait();
        delete audioDecodeThread;
        audioDecodeThread = nullptr;
    }
    if (videoDecodeThread) {
        videoDecodeThread->quit();
        if (videoDecodeThread != current) videoDecodeThread->wait();
        delete videoDecodeThread;
        videoDecodeThread = nullptr;
    }
    if (audioPlayThread) {
        audioPlayThread->quit();
        if (audioPlayThread != current) audioPlayThread->wait();
        delete audioPlayThread;
        audioPlayThread = nullptr;
    }
    if (videoRenderThread) {
        videoRenderThread->quit();
        if (videoRenderThread != current) videoRenderThread->wait();
        delete videoRenderThread;
        videoRenderThread = nullptr;
    }
    // 索引构建线程已移除，FFmpeg自动处理
    audioPacketQueue.clear();
    videoPacketQueue.clear();
    audioFrameQueue.clear();
    videoFrameQueue.clear();
    {
        std::lock_guard<std::mutex> lock(vctxMutex);
        if (vctx) { avcodec_free_context(&vctx); vctx = nullptr; }
    }
    if (actx) { avcodec_free_context(&actx); actx = nullptr; }
    {
        std::lock_guard<std::mutex> lock(fmtCtxMutex);
        if (fmt_ctx) { avformat_close_input(&fmt_ctx); fmt_ctx = nullptr; }
    }
    if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
    if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (audioSink) { delete audioSink; audioSink = nullptr; }
    audioDevice = nullptr;
}

void PlayerCore::cleanup() {
    {
        std::lock_guard<std::mutex> lock(vctxMutex);
        if (vctx) { avcodec_free_context(&vctx); vctx = nullptr; }
    }
    if (actx) { avcodec_free_context(&actx); actx = nullptr; }
    {
        std::lock_guard<std::mutex> lock(fmtCtxMutex);
        if (fmt_ctx) { avformat_close_input(&fmt_ctx); fmt_ctx = nullptr; }
    }
    if (sws_ctx) { sws_freeContext(sws_ctx); sws_ctx = nullptr; }
    if (swr_ctx) { swr_free(&swr_ctx); swr_ctx = nullptr; }
    if (audioSink) { delete audioSink; audioSink = nullptr; }
    audioDevice = nullptr;
}

void PlayerCore::decodeLoop() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* rgbFrame = av_frame_alloc();
    uint8_t* rgbBuf = nullptr;
    int rgbBufSize = 0;
    int64_t lastVideoPts = 0;
    int64_t lastAudioPts = 0;
    if (vctx) {
        int w = vctx->width, h = vctx->height;
        sws_ctx = sws_getContext(w, h, vctx->pix_fmt, w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        // rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, w, h, 1);
        // rgbBuf = (uint8_t*)av_malloc(rgbBufSize);
        // av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuf, AV_PIX_FMT_RGB24, w, h, 1);
    }
    // 音频设备已在openFile中初始化，这里不需要重复初始化
    AVFrame* audioFrame = av_frame_alloc();
    // 使用栈上分配的缓冲区，避免内存泄漏
    uint8_t audioBuf[192000];
    int audioBufSize = 192000;
    uint8_t* audioBufPtr = audioBuf;  // 创建指向缓冲区的指针
    while (!stopFlag) {
        // 播放/暂停控制
        stateMutex.lock();
        if (!playing) pauseCond.wait(&stateMutex);
        stateMutex.unlock();
        if (stopFlag) break;
        
        // 检查队列状态
        if (audioPacketQueue.empty() && videoPacketQueue.empty()) {
            QThread::msleep(10);
            continue;
        }

        AVPacket* pkt = nullptr;
        if (!audioPacketQueue.empty()) {
            audioPacketQueue.pop(pkt);
        } else {
            videoPacketQueue.pop(pkt);
        }

        if (pkt->stream_index == videoStream && vctx) {
            if (avcodec_send_packet(vctx, pkt) == 0) {
                while (avcodec_receive_frame(vctx, frame) == 0) {
                    if (stopFlag) { av_frame_free(&frame); break; }
                    
                    // 检查暂停状态
                    if (!playing) break;
                    
                    // 转为QImage
                    sws_scale(sws_ctx, frame->data, frame->linesize, 0, vctx->height, rgbFrame->data, rgbFrame->linesize);
                    QImage img(rgbFrame->data[0], vctx->width, vctx->height, rgbFrame->linesize[0], QImage::Format_RGB888);
                    
                    // 音视频同步
                    double audio_clock = audioSink ? audioSink->processedUSecs() / 1e6 : 0.0;
                    double video_pts = frame->pts * av_q2d(fmt_ctx->streams[videoStream]->time_base);
                    
                    // 如果视频超前音频，等待音频（使用Qt条件变量）
                    if (video_pts > audio_clock) {
                        double wait_time = video_pts - audio_clock;
                        if (wait_time > 0.1) { // 超过100ms才等待
                            QMutexLocker locker(&stateMutex);
                            auto start = std::chrono::steady_clock::now();
                            auto timeout = std::chrono::milliseconds(100);
                            
                            // Qt的QWaitCondition没有wait_for，我们使用循环检查
                            while (!stopFlag && playing && video_pts > audio_clock &&
                                   std::chrono::steady_clock::now() - start < timeout) {
                                pauseCond.wait(&stateMutex, 10);  // 等待10ms
                                audio_clock = audioSink ? audioSink->processedUSecs() / 1e6 : 0.0;
                            }
                        }
                    }
                    
                    if (playing) {  // 只有在播放状态才发送帧
                        emit frameReady(img.copy());
                        // 进度同步
                        if (frame->pts != AV_NOPTS_VALUE) {
                            int pos = frame->pts * av_q2d(fmt_ctx->streams[videoStream]->time_base);
                            emit positionChanged(pos);
                            lastVideoPts = frame->pts;
                        }
                    }
                }
            }
        } else if (pkt->stream_index == audioStream && actx) {
            if (avcodec_send_packet(actx, pkt) == 0) {
                while (avcodec_receive_frame(actx, audioFrame) == 0) {
                    if (stopFlag) { av_frame_free(&audioFrame); break; }
                    
                    // 检查暂停状态
                    if (!playing) break;
                    
                    // 重采样到S16
                    int out_samples = swr_convert(
                        swr_ctx,
                        &audioBufPtr, audioBufSize / actx->ch_layout.nb_channels / 2,
                        (const uint8_t**)audioFrame->data, audioFrame->nb_samples
                    );
                    int data_size = out_samples * actx->ch_layout.nb_channels * 2;
                    if (data_size > 0 && audioDevice && audioSink && playing) {
                        // 等待音频缓冲区有足够空间（使用Qt条件变量）
                        QMutexLocker locker(&stateMutex);
                        auto start = std::chrono::steady_clock::now();
                        auto timeout = std::chrono::milliseconds(50);
                        
                        // Qt的QWaitCondition没有wait_for，我们使用循环检查
                        while (!stopFlag && playing && audioSink->bytesFree() < data_size / 2 &&
                               std::chrono::steady_clock::now() - start < timeout) {
                            pauseCond.wait(&stateMutex, 10);  // 等待10ms
                        }
                        
                        if (playing && audioSink->bytesFree() >= data_size / 2) {
                            audioDevice->write((const char*)audioBuf, data_size);
                        } else {
                            // 超时或状态改变，跳过这一帧
                            qDebug() << "[decodeLoop] audio buffer wait timeout, skipping frame";
                        }
                    }
                    // 进度同步
                    if (audioFrame->pts != AV_NOPTS_VALUE) {
                        int pos = audioFrame->pts * av_q2d(fmt_ctx->streams[audioStream]->time_base);
                        emit positionChanged(pos);
                        lastAudioPts = audioFrame->pts;
                    }
                }
            }
        }
        av_packet_unref(pkt);
    }
    // 播放结束处理
    emit stateChanged(false);
    if (rgbBuf) av_free(rgbBuf);
    av_frame_free(&rgbFrame);
    av_frame_free(&frame);
    av_frame_free(&audioFrame);
    av_packet_free(&pkt);
    // audioBuf 现在是栈上分配的，不需要手动释放
}

void PlayerCore::parseVideoInfo() {
    if (!fmt_ctx) return;
    
    qDebug() << "[parseVideoInfo] ===== Video File Information =====";
    qDebug() << "[parseVideoInfo] Format:" << fmt_ctx->iformat->name;
    qDebug() << "[parseVideoInfo] Container duration:" << (fmt_ctx->duration / AV_TIME_BASE) << "seconds";
    qDebug() << "[parseVideoInfo] Number of streams:" << fmt_ctx->nb_streams;
    
    // 解析每个流的信息
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;
        
        qDebug() << "[parseVideoInfo] Stream" << i << ":";
        qDebug() << "  - Type:" << av_get_media_type_string(codecpar->codec_type);
        qDebug() << "  - Codec:" << avcodec_get_name(codecpar->codec_id);
        qDebug() << "  - Time base:" << stream->time_base.num << "/" << stream->time_base.den;
        qDebug() << "  - Duration (packets):" << stream->nb_frames;
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            qDebug() << "  - Resolution:" << codecpar->width << "x" << codecpar->height;
            qDebug() << "  - Frame rate:" << stream->avg_frame_rate.num << "/" << stream->avg_frame_rate.den;
            
            // 计算视频时长
            if (stream->duration != AV_NOPTS_VALUE) {
                double video_duration = stream->duration * av_q2d(stream->time_base);
                qDebug() << "  - Video duration:" << video_duration << "seconds";
            }
        } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            qDebug() << "  - Sample rate:" << codecpar->sample_rate;
            qDebug() << "  - Channels:" << codecpar->ch_layout.nb_channels;
            qDebug() << "  - Sample format:" << av_get_sample_fmt_name((AVSampleFormat)codecpar->format);
            
            // 计算音频时长
            if (stream->duration != AV_NOPTS_VALUE) {
                double audio_duration = stream->duration * av_q2d(stream->time_base);
                qDebug() << "  - Audio duration:" << audio_duration << "seconds";
            }
        }
    }
    qDebug() << "[parseVideoInfo] ==================================";
}

double PlayerCore::getVideoDuration() const {
    if (!fmt_ctx || videoStream < 0 || videoStream >= (int)fmt_ctx->nb_streams) {
        return 0.0;
    }
    
    AVStream* stream = fmt_ctx->streams[videoStream];
    if (stream->duration != AV_NOPTS_VALUE) {
        return stream->duration * av_q2d(stream->time_base);
    }
    
    // 如果stream duration不可用，尝试从帧数计算
    if (stream->nb_frames > 0 && stream->avg_frame_rate.num > 0) {
        double frame_rate = av_q2d(stream->avg_frame_rate);
        return stream->nb_frames / frame_rate;
    }
    
    return 0.0;
}









bool PlayerCore::smartSeek(double targetTime) {
    LOG_INFO("smartSeek: seeking to" << targetTime << "s");
    
    // 首先尝试精确跳转
    if (preciseSeek(targetTime)) {
        LOG_INFO("smartSeek: precise seek successful");
        return true;
    }
    
    // 如果精确跳转失败，尝试近似跳转
    LOG_INFO("smartSeek: precise seek failed, trying approximate seek");
    if (approximateSeek(targetTime)) {
        LOG_INFO("smartSeek: approximate seek successful");
        return true;
    }
    
    LOG_ERROR("smartSeek: both precise and approximate seek failed");
    return false;
}

bool PlayerCore::preciseSeek(double targetTime) {
    LOG_INFO("preciseSeek: attempting precise seek to" << targetTime << "s");
    
    std::lock_guard<std::mutex> lock(fmtCtxMutex);
    if (!fmt_ctx) {
        LOG_ERROR("preciseSeek: fmt_ctx is null");
        return false;
    }
    
    // 使用视频流的时间基准进行seek，通常更精确
    AVStream* videoStreamPtr = fmt_ctx->streams[videoStream];
    int64_t targetPts = av_rescale_q(targetTime, AV_TIME_BASE_Q, videoStreamPtr->time_base);

    int left = 0, right = videoFrameIndex.size() - 1, result = 0;
    int64_t target_pts1 = static_cast<int64_t>(targetTime / av_q2d(fmt_ctx->streams[videoStream]->time_base));
    while (left <= right) {
        int mid = (left + right) / 2;
        if (videoFrameIndex[mid].pts <= target_pts1) {
            result = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    while (result > 0 && !videoFrameIndex[result].is_key) --result;
    int64_t seek_pts = videoFrameIndex[result].pts;
    
    // 执行seek操作，使用视频流索引
    int ret = av_seek_frame(fmt_ctx, videoStream, seek_pts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        LOG_ERROR("preciseSeek: av_seek_frame failed, ret=" << ret);
        return false;
    }
    
    LOG_INFO("preciseSeek: successful, jumped to time" << targetTime << "s");
    
 
    
    return true;
}

bool PlayerCore::approximateSeek(double targetTime) {
    {
        std::lock_guard<std::mutex> lock(fmtCtxMutex);
        if (!fmt_ctx) return false;
        
        // 使用时间戳进行近似跳转
        int64_t ts = av_rescale_q(targetTime, AV_TIME_BASE_Q, 
                                  fmt_ctx->streams[videoStream]->time_base);
        
        int ret = av_seek_frame(fmt_ctx, videoStream, ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            LOG_ERROR("approximateSeek: av_seek_frame failed, ret=" << ret);
            return false;
        }
    }
    
    // 刷新解码器缓冲区（在锁外进行，避免死锁）
    {
        std::lock_guard<std::mutex> lock(vctxMutex);
        if (vctx) avcodec_flush_buffers(vctx);
    }
    if (actx) avcodec_flush_buffers(actx);
    
    LOG_INFO("approximateSeek: successful, jumped to time" << targetTime << "s");
    return true;
}

double PlayerCore::getAudioDuration() const {
    if (!fmt_ctx || audioStream < 0 || audioStream >= (int)fmt_ctx->nb_streams) {
        return 0.0;
    }
    
    AVStream* stream = fmt_ctx->streams[audioStream];
    if (stream->duration != AV_NOPTS_VALUE) {
        return stream->duration * av_q2d(stream->time_base);
    }
    
    // 如果stream duration不可用，尝试从采样数计算
    if (stream->nb_frames > 0 && stream->codecpar->sample_rate > 0) {
        return stream->nb_frames / (double)stream->codecpar->sample_rate;
    }
    
    return 0.0;
}

void PlayerCore::clearAllQueues() {
    audioPacketQueue.clear();
    videoPacketQueue.clear();
    audioFrameQueue.clear();
    videoFrameQueue.clear();
}
void PlayerCore::flushAllDecoders() {
    if (vctx) avcodec_flush_buffers(vctx);
    if (actx) avcodec_flush_buffers(actx);
}

void PlayerCore::renderPreciseFrame(int position) {
    // 用独立 AVFormatContext 渲染目标帧，emit frameReady
    double targetTime = static_cast<double>(position);
    int left = 0, right = videoFrameIndex.size() - 1, result = 0;
    int64_t target_pts = static_cast<int64_t>(targetTime / av_q2d(fmt_ctx->streams[videoStream]->time_base));
    while (left <= right) {
        int mid = (left + right) / 2;
        if (videoFrameIndex[mid].pts <= target_pts) {
            result = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }
    while (result > 0 && !videoFrameIndex[result].is_key) --result;
    int64_t seek_pts = videoFrameIndex[result].pts;
    AVFormatContext* seek_ctx = nullptr;
    avformat_open_input(&seek_ctx, currentFile.toStdString().c_str(), nullptr, nullptr);
    avformat_find_stream_info(seek_ctx, nullptr);
    av_seek_frame(seek_ctx, videoStream, seek_pts, AVSEEK_FLAG_BACKWARD);
    av_seek_frame(fmt_ctx, videoStream, seek_pts, AVSEEK_FLAG_BACKWARD);
    avformat_flush(seek_ctx);

    avformat_close_input(&seek_ctx);
}


// 新增：异步启动帧索引线程
void PlayerCore::startFrameIndexing() {
    if (indexingThread) {
        indexingThread->quit();
        indexingThread->wait();
        delete indexingThread;
        indexingThread = nullptr;
    }
    indexReady = false;
    indexingThread = QThread::create([this]{
        int total = 0, count = 0;
        // 先统计总帧数
        AVFormatContext* ctx = nullptr;
        avformat_open_input(&ctx, currentFile.toStdString().c_str(), nullptr, nullptr);
        avformat_find_stream_info(ctx, nullptr);
        int vStream = -1;
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                vStream = i; break;
            }
        }
        AVPacket* pkt = av_packet_alloc();
        while (av_read_frame(ctx, pkt) >= 0) {
            if (pkt->stream_index == vStream) ++total;
            av_packet_unref(pkt);
        }
        av_seek_frame(ctx, vStream, 0, AVSEEK_FLAG_BACKWARD);
        videoFrameIndex.clear();
        count = 0;
        while (av_read_frame(ctx, pkt) >= 0) {
            if (pkt->stream_index == vStream) {
                FrameIndexEntry entry;
                entry.pts = pkt->pts;
                entry.pos = pkt->pos;
                entry.is_key = pkt->flags & AV_PKT_FLAG_KEY;
                entry.stream_index = pkt->stream_index;
                videoFrameIndex.push_back(entry);
                ++count;
                if (count % 100 == 0 || count == total) {
                    int percent = total > 0 ? (count * 100 / total) : 0;
                    emit frameIndexProgress(percent);
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt);
        avformat_close_input(&ctx);
        indexReady = true;
        emit frameIndexProgress(100);
        emit frameIndexReady();
    });
    indexingThread->start();
}

 
 