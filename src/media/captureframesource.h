// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CAPTUREFRAMESOURCE_H
#define CAPTUREFRAMESOURCE_H

#include <QByteArray>
#include <QMutex>
#include <QRect>
#include <QString>

#include <memory>

/**
 * @brief 采集线程与编码线程之间传递的一帧快照。
 */
struct CaptureFrameSnapshot
{
    QByteArray data;
    int width = 0;
    int height = 0;
    int stride = 0;
    qint64 ptsNs = 0;
    quint64 sequence = 0;
};

using CaptureFrameSnapshotPtr = std::shared_ptr<CaptureFrameSnapshot>;

/**
 * @brief 音频采集选项（与设置项 recorder/audio 对应）。
 */
struct AudioCaptureOptions
{
    enum Type {
        NoAudio = 0,
        Microphone = 1,
        SystemAudio = 2,
        MicrophoneAndSystemAudio = 3,
    };

    int type = NoAudio;
    QString systemDevice;
    QString microphoneDevice = QStringLiteral("default");

    bool hasMicrophone() const
    {
        return type == Microphone || type == MicrophoneAndSystemAudio;
    }
    bool hasSystemAudio() const
    {
        return type == SystemAudio || type == MicrophoneAndSystemAudio;
    }
    bool hasAudio() const
    {
        return hasMicrophone() || hasSystemAudio();
    }
};

/**
 * @brief 线程安全的最新帧槽位：采集端覆盖写，编码端按需读取。
 */
class LatestFrameStore
{
public:
    void set(const CaptureFrameSnapshotPtr &frame)
    {
        QMutexLocker locker(&m_mutex);
        m_frame = frame;
    }

    CaptureFrameSnapshotPtr get() const
    {
        QMutexLocker locker(&m_mutex);
        return m_frame;
    }

    void clear()
    {
        QMutexLocker locker(&m_mutex);
        m_frame.reset();
    }

private:
    mutable QMutex m_mutex;
    CaptureFrameSnapshotPtr m_frame;
};

#endif // CAPTUREFRAMESOURCE_H
