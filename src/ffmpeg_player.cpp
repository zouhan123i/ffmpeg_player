// mp4_player.cpp

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <SDL2/SDL.h>
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <input.mp4>" << std::endl;
        return -1;
    }

    std::cout<<"FFmpeg and SDL2 MP4 Player" << std::endl;
    const char* input_filename = argv[1];

    // 打开输入文件
    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_filename, nullptr, nullptr) < 0) {
        std::cerr << "Failed to open input file: " << input_filename << std::endl;
        return -1;
    }

    // 获取流信息
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Failed to get stream info." << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 寻找最优视频流
    int video_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video_stream_index < 0) {
        std::cerr << "No video stream found in file." << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVStream* video_stream = fmt_ctx->streams[video_stream_index];
    AVCodecParameters* codecpar = video_stream->codecpar;
    const AVCodec* dec = avcodec_find_decoder(codecpar->codec_id);
    if (!dec) {
        std::cerr << "Failed to find decoder." << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        std::cerr << "Failed to allocate codec context." << std::endl;
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    if (avcodec_parameters_to_context(dec_ctx, codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters to context." << std::endl;
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
        std::cerr << "Failed to open codec." << std::endl;
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 初始化SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Failed to initialize SDL: " << SDL_GetError() << std::endl;
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "FFmpeg MP4 Player", 
        SDL_WINDOWPOS_UNDEFINED, 
        SDL_WINDOWPOS_UNDEFINED, 
        dec_ctx->width, 
        dec_ctx->height, 
        SDL_WINDOW_SHOWN
    );

    if (!window) {
        std::cerr << "Failed to create window: " << SDL_GetError() << std::endl;
        SDL_Quit();
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
    if (!renderer) {
        std::cerr << "Failed to create renderer: " << SDL_GetError() << std::endl;
        SDL_DestroyWindow(window);
        SDL_Quit();
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    SDL_Texture* texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_YV12,
        SDL_TEXTUREACCESS_STREAMING,
        dec_ctx->width,
        dec_ctx->height
    );

    if (!texture) {
        std::cerr << "Failed to create texture: " << SDL_GetError() << std::endl;
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    // 准备转换上下文
    SwsContext* sws_ctx = sws_getContext(
        dec_ctx->width,
        dec_ctx->height,
        dec_ctx->pix_fmt,
        dec_ctx->width,
        dec_ctx->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr, nullptr, nullptr
    );

    AVFrame* frame = av_frame_alloc();
    AVFrame* frame_yuv = av_frame_alloc();
    int buffer_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, dec_ctx->width, dec_ctx->height, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(buffer_size);
    av_image_fill_arrays(frame_yuv->data, frame_yuv->linesize, buffer, AV_PIX_FMT_YUV420P, dec_ctx->width, dec_ctx->height, 1);

    AVPacket* pkt = av_packet_alloc();

    // 1. 查找音频流
    int audio_stream_index = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVCodecContext* audio_dec_ctx = nullptr;
    if (audio_stream_index >= 0) {
        AVStream* audio_stream = fmt_ctx->streams[audio_stream_index];
        AVCodecParameters* audio_codecpar = audio_stream->codecpar;
        const AVCodec* audio_dec = avcodec_find_decoder(audio_codecpar->codec_id);
        audio_dec_ctx = avcodec_alloc_context3(audio_dec);
        avcodec_parameters_to_context(audio_dec_ctx, audio_codecpar);
        avcodec_open2(audio_dec_ctx, audio_dec, nullptr);
    }

    // 2. 初始化SDL音频（推荐用OpenAudioDevice）
    SDL_AudioSpec wanted_spec, obtained_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = audio_dec_ctx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = audio_dec_ctx->channels;
    wanted_spec.silence = 0;
    wanted_spec.samples = 1024;
    wanted_spec.callback = nullptr;
    wanted_spec.userdata = nullptr;

    int dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (dev == 0) {
        std::cerr << "Failed to open audio: " << SDL_GetError() << std::endl;
    } else {
        SDL_PauseAudioDevice(dev, 0); // 开始播放
    }

    // 初始化重采样
    SwrContext* swr_ctx = swr_alloc_set_opts(
        nullptr,
        av_get_default_channel_layout(obtained_spec.channels),
        AV_SAMPLE_FMT_S16,
        obtained_spec.freq,
        av_get_default_channel_layout(audio_dec_ctx->channels),
        audio_dec_ctx->sample_fmt,
        audio_dec_ctx->sample_rate,
        0, nullptr
    );
    swr_init(swr_ctx);

    AVFrame* audio_frame = av_frame_alloc();
    uint8_t* audio_buf = (uint8_t*)av_malloc(192000); // 足够大

    const size_t MAX_QUEUE_SIZE = 200; // 队列最大长度

    std::queue<AVPacket*> audio_queue, video_queue;
    std::mutex audio_mutex, video_mutex;
    std::condition_variable audio_cv, video_cv;
    std::atomic<bool> quit(false);
    static std::atomic<int64_t> audio_written_bytes(0);
    // 读取线程
    auto read_thread_func = [&]() {
        while (!quit) {
            // 队列满则等待
            {
                std::unique_lock<std::mutex> audio_lock(audio_mutex, std::defer_lock);
                std::unique_lock<std::mutex> video_lock(video_mutex, std::defer_lock);
                audio_lock.lock();
                video_lock.lock();
                if (audio_queue.size() >= MAX_QUEUE_SIZE || video_queue.size() >= MAX_QUEUE_SIZE) {
                    audio_lock.unlock();
                    video_lock.unlock();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                audio_lock.unlock();
                video_lock.unlock();
            }

            AVPacket* pkt = av_packet_alloc();
            if (av_read_frame(fmt_ctx, pkt) < 0) {
                av_packet_free(&pkt);
                break;
            }
            if (pkt->stream_index == audio_stream_index) {
                std::lock_guard<std::mutex> lock(audio_mutex);
                audio_queue.push(pkt);
                audio_cv.notify_one();
            } else if (pkt->stream_index == video_stream_index) {
                std::lock_guard<std::mutex> lock(video_mutex);
                video_queue.push(pkt);
                video_cv.notify_one();
            } else {
                av_packet_free(&pkt);
            }
        }
        // 通知解码线程退出
        quit = true;
        audio_cv.notify_all();
        video_cv.notify_all();
    };

    // 音频线程
    auto audio_thread_func = [&]() {
        while (!quit) {
            AVPacket* pkt = nullptr;
            {
                std::unique_lock<std::mutex> lock(audio_mutex);
                audio_cv.wait(lock, [&] { return !audio_queue.empty() || quit; });
                if (quit && audio_queue.empty()) break;
                pkt = audio_queue.front();
                audio_queue.pop();
            }
            if (pkt && audio_dec_ctx) {
                if (avcodec_send_packet(audio_dec_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(audio_dec_ctx, audio_frame) == 0) {
                        int out_samples = swr_convert(
                            swr_ctx,
                            &audio_buf, 192000 / obtained_spec.channels / 2,
                            (const uint8_t**)audio_frame->data, audio_frame->nb_samples
                        );
                        int data_size = out_samples * obtained_spec.channels * 2;
                        if (data_size > 0) {
                            SDL_QueueAudio(dev, audio_buf, data_size);
                              audio_written_bytes += data_size; // 统计写入SDL的字节数
                        }
                    }
                }
                av_packet_free(&pkt);
            }
        }
    };

    // 计算音频时钟（已播放秒数）
    auto get_audio_clock = [&]() -> double {
        Uint32 audio_queued_bytes = SDL_GetQueuedAudioSize(dev);
        int bytes_per_sec = obtained_spec.channels * sizeof(int16_t) * obtained_spec.freq;
        // 已经写入SDL的音频总时长 - 队列中未播放的时长 = 已播放时长
        double played = 0.0;
        if (bytes_per_sec > 0) {
            played = double(audio_written_bytes - audio_queued_bytes) / bytes_per_sec;
        }
        return played;
    };

    // 视频线程（可在此实现音视频同步，见下方注释）
    auto video_thread_func = [&]() {
        while (!quit) {
            AVPacket* pkt = nullptr;
            {
                std::unique_lock<std::mutex> lock(video_mutex);
                video_cv.wait(lock, [&] { return !video_queue.empty() || quit; });
                if (quit && video_queue.empty()) break;
                pkt = video_queue.front();
                video_queue.pop();
            }
            if (pkt) {
                if (avcodec_send_packet(dec_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                        // --- 音视频同步建议 ---
                        // 计算视频帧pts（秒）
                        double video_pts = 0.0;
                        if (frame->pts != AV_NOPTS_VALUE) {
                            video_pts = frame->pts * av_q2d(video_stream->time_base);
                        }
                        // 获取音频时钟
                        double audio_clock = get_audio_clock();

                        // 音视频同步：如果视频帧比音频快，等待
                        while (!quit && video_pts > audio_clock + 0.02) { // 20ms容忍
                            SDL_Delay(1);
                            audio_clock = get_audio_clock();
                        }
                        // --- 同步建议结束 ---
                        sws_scale(sws_ctx, frame->data, frame->linesize, 0, dec_ctx->height, frame_yuv->data, frame_yuv->linesize);
                        SDL_UpdateYUVTexture(texture, nullptr,
                                             frame_yuv->data[0], frame_yuv->linesize[0],
                                             frame_yuv->data[1], frame_yuv->linesize[1],
                                             frame_yuv->data[2], frame_yuv->linesize[2]);
                        SDL_RenderClear(renderer);
                        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
                        SDL_RenderPresent(renderer);
                        //SDL_Delay(25); // 可根据音频同步优化
                    }
                }
                av_packet_free(&pkt);
            }
        }
    };

    // 启动线程
    std::thread read_thread(read_thread_func);
    std::thread audio_thread(audio_thread_func);
    std::thread video_thread(video_thread_func);

    // 主线程只处理事件和退出
    SDL_Event e;
    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
                audio_cv.notify_all();
                video_cv.notify_all();
                break;
            }
        }
        SDL_Delay(10);
    }

    // 等待线程退出
    if (read_thread.joinable()) read_thread.join();
    if (audio_thread.joinable()) audio_thread.join();
    if (video_thread.joinable()) video_thread.join();

    // 清理音频相关资源
    if (audio_dec_ctx) {
        avcodec_free_context(&audio_dec_ctx);
    }
    av_frame_free(&audio_frame);

    // 清理资源
    av_packet_free(&pkt);
    av_free(buffer);
    av_frame_free(&frame);
    av_frame_free(&frame_yuv);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    av_free(audio_buf);
    swr_free(&swr_ctx);

    return 0;
}
