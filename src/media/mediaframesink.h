// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MEDIAFRAMESINK_H
#define MEDIAFRAMESINK_H

#include "captureframesource.h"

#include <QString>

#include <cstdint>

/**
 * @brief 帧落盘接口：ucodec 视频/音频录制器与 GIF 录制器共用。
 *
 * 所有实现都必须按调用方给出的 pts（帧序号）保持恒定帧率。
 */
class MediaFrameSink
{
public:
    virtual ~MediaFrameSink() = default;

    /** bitrate <= 0 表示由实现按分辨率/帧率估算（GIF 忽略该参数）。 */
    virtual bool start(const QString &outputPath, int width, int height, int fps,
                       qint64 bitrate, const AudioCaptureOptions &audio,
                       QString *error = nullptr) = 0;

    virtual bool pushFrame(const uint8_t *rgba, int stride, const QRect &crop,
                           qint64 pts, QString *error = nullptr) = 0;

    virtual bool finish(QString *error = nullptr) = 0;

    virtual void abort() = 0;

    virtual bool isStarted() const = 0;

    virtual bool hasAudio() const = 0;

    virtual QString encoderPath() const = 0;
};

#endif // MEDIAFRAMESINK_H
