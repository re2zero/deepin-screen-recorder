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
 * 每帧为变化区域生成 255 色局部调色板（索引 0 保留给透明），后续帧只写发生
 * 变化的外接矩形，未变化像素使用透明色跳过。GIF 没有帧间预测，这种增量写法
 * 可以避免静态桌面产生巨大文件。
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

    // 调色板直方图条目：统计像素数与各通道累加值。
    // 供调色板生成算法（gifrecorder.cpp 内部的匿名函数）使用。
    // 累加值用 64 位，避免多屏/8K 录制时单桶像素数超过 1680 万后溢出。
    struct ColorBin
    {
        uint32_t count = 0;
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
    };

private:
    bool prepareFrame(const uint8_t *rgba, int stride, const QRect &crop,
                      QString *error);
    QRect changedRect() const;
    void buildPalette(const QRect &box);
    bool writeHeader(QString *error);
    bool writeFrame(qint64 pts, QString *error);
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
    bool m_hasPrevious = false;

    qint64 m_lastPts = -1;
    double m_pendingDelayCs = 0.0;
    double m_delayRemainderCs = 0.0;

    std::vector<uint8_t> m_palette;     // RGB triplets, 256 entries (0 = transparent)
    std::vector<uint16_t> m_colorCache; // 128x128x128 nearest-index cache (7bit/通道)
    std::vector<ColorBin> m_histogram;  // 64x64x64 6bit 直方图，用于生成调色板
    std::vector<uint8_t> m_indexed;     // current frame indices (bbox-local)
    std::vector<uint8_t> m_current;     // current frame, tightly packed RGBA
    std::vector<uint8_t> m_previous;    // previous frame, tightly packed RGBA
};

#endif // GIFRECORDER_H
