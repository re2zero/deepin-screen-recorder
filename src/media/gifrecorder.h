// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef GIFRECORDER_H
#define GIFRECORDER_H

#include "mediaframesink.h"

#include <QFile>
#include <QRect>
#include <QString>

#include <cstdint>
#include <vector>

/**
 * @brief 应用内 GIF 录制器（替代 ffmpeg mp4->gif 转码）。
 *
 * 首帧抽样后用中位切分生成 256 色全局调色板，后续帧做最近色映射，
 * 以 GIF89a 逐帧写入并循环播放。无音频。
 */
class GifRecorder : public MediaFrameSink
{
public:
    GifRecorder();
    ~GifRecorder() override;

    bool start(const QString &outputPath, int width, int height, int fps,
               qint64 bitrate, const AudioCaptureOptions &audio,
               QString *error = nullptr) override;

    bool pushFrame(const uint8_t *rgba, int stride, const QRect &crop,
                   qint64 pts, QString *error = nullptr) override;

    bool finish(QString *error = nullptr) override;

    void abort() override;

    bool isStarted() const override { return m_started; }
    bool hasAudio() const override { return false; }
    QString encoderPath() const override { return QStringLiteral("gif"); }

    int encodedFrames() const { return m_frameCount; }

private:
    bool buildPalette(const uint8_t *rgba, int stride, const QRect &crop);
    bool writeHeader(QString *error);
    bool writeFrame(const uint8_t *rgba, int stride, const QRect &crop, QString *error);
    uint8_t paletteIndex(int r, int g, int b);
    void writeByte(uint8_t value) { m_file.putChar(static_cast<char>(value)); }
    void writeShort(uint16_t value)
    {
        writeByte(static_cast<uint8_t>(value & 0xff));
        writeByte(static_cast<uint8_t>(value >> 8));
    }

    QFile m_file;
    QString m_outputPath;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 12;
    int m_frameCount = 0;
    bool m_started = false;
    bool m_headerWritten = false;

    std::vector<uint8_t> m_palette;   // RGB triplets, 256 entries
    std::vector<uint16_t> m_colorCache; // 32x32x32 nearest-index cache
    std::vector<uint8_t> m_indexed;
};

#endif // GIFRECORDER_H
