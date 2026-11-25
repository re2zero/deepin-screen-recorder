// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UCODERRECORDER_H
#define UCODERRECORDER_H

#include "mediaframesink.h"
#include "ucodecvideoencoder.h"

#include <QByteArray>
#include <QRect>
#include <QString>

#include <atomic>
#include <cstdint>
#include <memory>

class MediaMuxer;
class AacEncoder;
class PulseAudioCapture;

/**
 * @brief 基于 ucodec 的自研录制落盘器（不依赖 GStreamer/FFmpeg）。
 *
 * 视频：ucodec 编码为 H.264 Annex-B，自研 MP4/Matroska 封装；
 * 音频：PulseAudio 采集，FDK-AAC 编码，与视频在封装层交织。
 *
 * 对象不是线程安全的：请在同一个线程内 start/pushFrame/finish。
 * 音频采集线程只通过封装器的内部锁与视频线程交汇。
 */
class UcodecRecorder : public MediaFrameSink
{
public:
    UcodecRecorder();
    ~UcodecRecorder() override;

    UcodecRecorder(const UcodecRecorder &) = delete;
    UcodecRecorder &operator=(const UcodecRecorder &) = delete;

    /**
     * @brief 打开输出文件并启动编码/封装。
     * @param bitrate bits/s；<=0 时按分辨率与帧率自动估算。
     */
    bool start(const QString &outputPath, int width, int height, int fps,
               qint64 bitrate, const AudioCaptureOptions &audio,
               QString *error = nullptr) override;

    /**
     * @brief 编码并写入一帧 RGBA8888 紧排图像。
     * @param crop 源图裁剪区域（1:1 或缩放）；空表示整帧。
     * @param pts  帧序号（从 0 开始）。
     */
    bool pushFrame(const uint8_t *rgba, int stride, const QRect &crop, qint64 pts,
                   QString *error = nullptr) override;

    /**
     * @brief 冲刷编码器、收尾音频并回写容器索引。
     */
    bool finish(QString *error = nullptr) override;

    /** 立即中止：销毁管道并删除不完整文件。 */
    void abort() override;

    bool isStarted() const override { return m_started; }
    bool hasAudio() const override { return m_audioEnabled.load(); }
    QString encoderPath() const override;
    int encodedFrames() const { return m_encodedFrames; }
    qint64 bytesWritten() const { return m_bytesWritten; }

private:
    bool setupAudio(const AudioCaptureOptions &audio, QString *error);
    void onVideoPacket(const uint8_t *data, int size, qint64 pts, qint64 dts, bool key);
    bool onAudioPcm(const int16_t *pcm, int frames);
    void releaseResources();

    UcodecVideoEncoder m_encoder;
    UcodecVideoEncoder::Config m_encoderConfig;
    std::unique_ptr<MediaMuxer> m_muxer;
    std::unique_ptr<AacEncoder> m_aac;
    std::unique_ptr<PulseAudioCapture> m_capture;

    QString m_outputPath;
    QString m_audioError;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;

    QByteArray m_sps;
    QByteArray m_pps;
    QByteArray m_avcC;
    std::atomic<bool> m_videoConfigReady {false};
    std::atomic<bool> m_muxerReady {false};
    std::atomic<bool> m_audioEnabled {false};
    std::atomic<bool> m_muxFailed {false};
    std::atomic<qint64> m_audioSampleCount {0};

    bool m_started = false;
    int m_encodedFrames = 0;
    qint64 m_packetCount = 0;
    qint64 m_bytesWritten = 0;
};

#endif // UCODERRECORDER_H
