// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef X11RECORDWORKER_H
#define X11RECORDWORKER_H

#include "captureframesource.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QString>

class X11ScreenFrameGrabber;
class FramePumpWorker;
class QThread;
class QTimer;

/**
 * @brief X11 屏幕录制 worker（运行在独立 QThread）。
 *
 * 用 XShmGetImage 直接采集目标区域（可选叠加光标），转成紧排 RGBA 后
 * 交给 FramePumpWorker 按固定帧率用 ucodec 编码，自研封装器落盘。
 * 音频由 UcodecRecorder 内的 PulseAudio/AAC 链路采集编码。
 */
class X11RecordWorker : public QObject
{
    Q_OBJECT

public:
    explicit X11RecordWorker(QObject *parent = nullptr);
    ~X11RecordWorker() override;

public slots:
    /**
     * @brief 启动采集与编码。失败时通过 errorOccurred 报告并自行清理。
     */
    void start(const QRect &region, int frameRate, bool includeCursor,
               const QString &outputPath, const AudioCaptureOptions &audio);
    void stop();

signals:
    void started();
    void finished();
    void errorOccurred(const QString &message);
    void frameWritten(int totalFrames);

private slots:
    void captureFrame();

private:
    void startPumpIfNeeded();
    void checkWatchdog();
    void fail(const QString &message);

    X11ScreenFrameGrabber *m_grabber = nullptr;
    QTimer *m_captureTimer = nullptr;
    QTimer *m_watchdog = nullptr;
    LatestFrameStore *m_store = nullptr;
    FramePumpWorker *m_pump = nullptr;
    QThread *m_pumpThread = nullptr;
    QRect m_region;
    QString m_outputPath;
    AudioCaptureOptions m_audio;
    int m_frameRate = 30;
    bool m_running = false;
    bool m_pumpStarted = false;
    bool m_failed = false;
    bool m_finishedEmitted = false;
    quint64 m_sequence = 0;
    qint64 m_lastSampleNs = 0;
    QElapsedTimer m_clock;
};

#endif // X11RECORDWORKER_H
