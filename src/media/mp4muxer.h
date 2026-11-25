// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MP4MUXER_H
#define MP4MUXER_H

#include "mediamuxer.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdint>
#include <mutex>
#include <vector>

/**
 * @brief 自研 MP4 封装器：H.264（AVCC）+ AAC-LC。
 *
 * 只在录制结束时回写 moov；mdat 使用 64 位长度，样本按到达顺序落盘，
 * 每样本单独成 chunk（co64），避免在内存中缓存媒体数据。
 */
class Mp4Muxer : public MediaMuxer
{
public:
    Mp4Muxer();
    ~Mp4Muxer();

    Mp4Muxer(const Mp4Muxer &) = delete;
    Mp4Muxer &operator=(const Mp4Muxer &) = delete;

    bool open(const QString &path, int width, int height, int fps,
              const MediaAudioConfig *audio, QString *error = nullptr) override;
    void setVideoCodecConfig(const QByteArray &avcC) override;

    bool addVideoSample(const uint8_t *data, int size, qint64 pts, qint64 dts,
                        bool key, QString *error = nullptr) override;
    bool addAudioSample(const uint8_t *data, int size, qint64 ptsSamples,
                        qint64 durationSamples, QString *error = nullptr) override;

    bool finish(QString *error = nullptr) override;
    void abort() override;

    qint64 bytesWritten() const override;
    int videoSampleCount() const override;
    bool hasAudio() const override { return m_audioEnabled; }

public:
    struct Sample
    {
        quint32 size = 0;
        quint64 offset = 0;
        quint32 duration = 0;
        qint32 cts = 0;
        bool key = false;
    };

private:
    bool writeSample(Sample *sample, const uint8_t *data, int size, QString *error);
    QByteArray buildMoov() const;
    QByteArray buildVideoTrak() const;
    QByteArray buildAudioTrak() const;
    QByteArray buildStbl(const std::vector<Sample> &samples, bool video) const;
    QByteArray buildAvc1() const;
    QByteArray buildMp4a() const;
    QByteArray buildEsds() const;

    mutable std::mutex m_mutex;
    QFile m_file;
    QString m_path;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;

    QByteArray m_avcC;
    bool m_audioEnabled = false;
    MediaAudioConfig m_audio;

    std::vector<Sample> m_videoSamples;
    std::vector<Sample> m_audioSamples;
    qint64 m_maxPts = -1;
    qint64 m_dtsOffset = 0;
    qint64 m_lastDts = -1;
    qint64 m_audioSamplesTotal = 0;
    bool m_hasCts = false;

    qint64 m_mdatStart = 0;
    qint64 m_mdatSizePos = 0;
    bool m_opened = false;
    bool m_finished = false;
};

#endif // MP4MUXER_H
