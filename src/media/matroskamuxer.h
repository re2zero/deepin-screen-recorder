// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MATROSKAMUXER_H
#define MATROSKAMUXER_H

#include "mediamuxer.h"

#include <QByteArray>
#include <QFile>
#include <QString>

#include <cstdint>
#include <mutex>
#include <vector>

/**
 * @brief 自研 Matroska（MKV）封装器：H.264（AVCC）+ AAC-LC。
 *
 * 使用未知长度 Segment 顺序写入，结束时回写 Segment 长度与 Duration；
 * 每个 Cluster 在内存中拼装，遇到视频关键帧或超过阈值时落盘。
 */
class MatroskaMuxer : public MediaMuxer
{
public:
    MatroskaMuxer();
    ~MatroskaMuxer();

    MatroskaMuxer(const MatroskaMuxer &) = delete;
    MatroskaMuxer &operator=(const MatroskaMuxer &) = delete;

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

private:
    struct PendingSample
    {
        bool video = false;
        qint64 timestampMs = 0;
        bool key = false;
        QByteArray data;
    };

    bool writeHeader(QString *error);
    bool writeCluster(QString *error);
    bool addSample(bool video, qint64 timestampMs, bool key,
                   const QByteArray &data, QString *error);
    void startCluster(qint64 timestampMs);
    bool writeElement(const QByteArray &element, QString *error);

    mutable std::mutex m_mutex;
    QFile m_file;
    QString m_path;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;

    QByteArray m_avcC;
    bool m_audioEnabled = false;
    MediaAudioConfig m_audio;

    std::vector<PendingSample> m_pendingSamples;
    QByteArray m_cluster;
    qint64 m_clusterTimestampMs = 0;
    qint64 m_maxVideoPts = -1;
    qint64 m_audioSamplesTotal = 0;
    int m_videoSampleCount = 0;

    qint64 m_segmentStart = 0;
    qint64 m_segmentSizePos = 0;
    qint64 m_segmentDataStart = 0;
    qint64 m_durationFilePos = 0;
    bool m_headerWritten = false;
    bool m_opened = false;
    bool m_finished = false;
};

#endif // MATROSKAMUXER_H
