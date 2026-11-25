// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef AACENCODER_H
#define AACENCODER_H

#include <QByteArray>
#include <QList>
#include <QString>

#include <cstdint>

/**
 * @brief AAC-LC 编码器（FDK-AAC），输出裸 AAC 帧与 AudioSpecificConfig。
 */
class AacEncoder
{
public:
    AacEncoder();
    ~AacEncoder();

    AacEncoder(const AacEncoder &) = delete;
    AacEncoder &operator=(const AacEncoder &) = delete;

    bool open(int sampleRate, int channels, int bitrate, QString *error = nullptr);
    void close();

    /** AudioSpecificConfig，用于 MP4 esds / Matroska CodecPrivate。 */
    QByteArray config() const { return m_config; }

    /**
     * @brief 编码一个 AAC 帧所需的 PCM（frames 为每声道采样数）。
     */
    bool encode(const int16_t *pcm, int frames, QList<QByteArray> *outputs,
                QString *error = nullptr);

    /** 冲刷编码器延迟，输出剩余帧。 */
    bool flush(QList<QByteArray> *outputs, QString *error = nullptr);

    bool isOpen() const { return m_handle != nullptr; }

private:
    void *m_handle = nullptr;
    int m_sampleRate = 0;
    int m_channels = 0;
    QByteArray m_config;
};

#endif // AACENCODER_H
