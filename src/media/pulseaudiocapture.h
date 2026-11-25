// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef PULSEAUDIOCAPTURE_H
#define PULSEAUDIOCAPTURE_H

#include "captureframesource.h"

#include <QString>

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/**
 * @brief PulseAudio 采集：麦克风/系统监听源，S16LE 48kHz 双声道。
 *
 * 不依赖 GStreamer；混音（麦克风+系统）在采集线程内完成。
 * 回调每帧固定 1024 个采样点（每声道）。
 */
class PulseAudioCapture
{
public:
    using FrameCallback = std::function<bool(const int16_t *pcm, int frames)>;

    PulseAudioCapture();
    ~PulseAudioCapture();

    PulseAudioCapture(const PulseAudioCapture &) = delete;
    PulseAudioCapture &operator=(const PulseAudioCapture &) = delete;

    bool open(const AudioCaptureOptions &audio, int sampleRate, int channels,
              QString *error = nullptr);
    bool start(FrameCallback callback, QString *error = nullptr);
    void stop();

    bool isRunning() const { return m_running.load(); }
    QString lastError() const;

    static constexpr int kChunkFrames = 1024;

private:
    void captureLoop();

    void *m_micStream = nullptr;
    void *m_systemStream = nullptr;
    int m_sampleRate = 48000;
    int m_channels = 2;

    std::thread m_thread;
    std::atomic<bool> m_running {false};
    FrameCallback m_callback;
    std::atomic<bool> m_hasError {false};
    mutable std::mutex m_errorMutex;
    QString m_error;
};

#endif // PULSEAUDIOCAPTURE_H
