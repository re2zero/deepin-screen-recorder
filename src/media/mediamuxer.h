// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MEDIAMUXER_H
#define MEDIAMUXER_H

#include <QByteArray>
#include <QString>

#include <cstdint>

/** 音轨配置：AAC-LC 的 AudioSpecificConfig 与采样参数。 */
struct MediaAudioConfig
{
    QByteArray codecConfig;
    int sampleRate = 48000;
    int channels = 2;
    int bitrate = 128000;
};

/**
 * @brief 自研封装器接口（MP4 / Matroska）。
 *
 * addVideoSample/addAudioSample 可分别从编码线程与音频线程并发调用，
 * 实现内部必须自行加锁。视频样本为 AVCC（4 字节长度前缀）。
 */
class MediaMuxer
{
public:
    virtual ~MediaMuxer() = default;

    virtual bool open(const QString &path, int width, int height, int fps,
                      const MediaAudioConfig *audio, QString *error = nullptr) = 0;
    virtual void setVideoCodecConfig(const QByteArray &avcC) = 0;
    virtual bool addVideoSample(const uint8_t *data, int size, qint64 pts, qint64 dts,
                                bool key, QString *error = nullptr) = 0;
    virtual bool addAudioSample(const uint8_t *data, int size, qint64 ptsSamples,
                                qint64 durationSamples, QString *error = nullptr) = 0;
    virtual bool finish(QString *error = nullptr) = 0;
    virtual void abort() = 0;
    virtual qint64 bytesWritten() const = 0;
    virtual int videoSampleCount() const = 0;
    virtual bool hasAudio() const = 0;
};

#endif // MEDIAMUXER_H
