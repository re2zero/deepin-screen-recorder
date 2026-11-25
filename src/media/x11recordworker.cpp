// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11recordworker.h"

#include "x11screenframegrabber.h"
#include "../ext-image-capture/framepumpworker.h"
#include "../utils/log.h"

#include <QDebug>
#include <QThread>
#include <QTimer>

X11RecordWorker::X11RecordWorker(QObject *parent)
    : QObject(parent)
{
}

X11RecordWorker::~X11RecordWorker()
{
    stop();
}

void X11RecordWorker::start(const QRect &region, int frameRate, bool includeCursor,
                            const QString &outputPath, const AudioCaptureOptions &audio)
{
    if (m_running) {
        emit errorOccurred(QStringLiteral("X11 录制已在运行"));
        return;
    }
    if (region.width() < 2 || region.height() < 2 || outputPath.isEmpty()) {
        fail(QStringLiteral("非法的录制区域或输出路径"));
        return;
    }

    m_region = region;
    m_frameRate = qBound(1, frameRate, 60);
    m_outputPath = outputPath;
    m_audio = audio;
    m_sequence = 0;
    m_lastSampleNs = 0;
    m_pumpStarted = false;
    m_failed = false;
    m_finishedEmitted = false;
    m_store = new LatestFrameStore;

    m_grabber = new X11ScreenFrameGrabber;
    QString error;
    if (!m_grabber->open(region, includeCursor, &error)) {
        fail(QStringLiteral("X11 采集初始化失败：%1").arg(error));
        return;
    }

    m_clock.start();
    m_running = true;

    m_captureTimer = new QTimer(this);
    m_captureTimer->setTimerType(Qt::PreciseTimer);
    m_captureTimer->setInterval(qMax(1, 1000 / m_frameRate));
    connect(m_captureTimer, &QTimer::timeout, this, &X11RecordWorker::captureFrame);

    m_watchdog = new QTimer(this);
    m_watchdog->setInterval(1000);
    connect(m_watchdog, &QTimer::timeout, this, &X11RecordWorker::checkWatchdog);
    m_watchdog->start();

    // 立即抓取首帧，确保帧泵能马上启动。
    captureFrame();
    if (!m_running)
        return;
    m_captureTimer->start();

    qCInfo(dsrApp) << "X11RecordWorker started:" << region << "fps:" << m_frameRate
                   << "cursor:" << includeCursor << "audio:" << audio.hasAudio()
                   << "output:" << outputPath;
}

void X11RecordWorker::captureFrame()
{
    if (!m_running || !m_grabber || !m_store)
        return;

    QByteArray rgba;
    int stride = 0;
    QString error;
    if (!m_grabber->grab(&rgba, &stride, &error)) {
        if (m_lastSampleNs == 0)
            fail(QStringLiteral("X11 首帧采集失败：%1").arg(error));
        else
            qCWarning(dsrApp) << "X11RecordWorker: grab failed:" << error;
        return;
    }
    if (rgba.isEmpty()) {
        if (m_lastSampleNs == 0)
            fail(QStringLiteral("X11 首帧为空"));
        return;
    }

    CaptureFrameSnapshotPtr snapshot = std::make_shared<CaptureFrameSnapshot>();
    snapshot->data = std::move(rgba);
    snapshot->width = m_region.width();
    snapshot->height = m_region.height();
    snapshot->stride = stride;
    snapshot->sequence = ++m_sequence;
    snapshot->ptsNs = m_clock.nsecsElapsed();
    m_store->set(snapshot);
    m_lastSampleNs = snapshot->ptsNs;

    if (!m_pumpStarted)
        startPumpIfNeeded();
}

void X11RecordWorker::startPumpIfNeeded()
{
    if (m_pumpStarted || !m_running || !m_store)
        return;

    const CaptureFrameSnapshotPtr frame = m_store->get();
    if (!frame || frame->data.isEmpty())
        return;

    m_pump = new FramePumpWorker(m_frameRate, m_outputPath,
                                 QRect(0, 0, frame->width, frame->height),
                                 m_store, m_audio);
    m_pumpThread = new QThread(this);
    m_pump->moveToThread(m_pumpThread);
    connect(m_pumpThread, &QThread::finished, m_pump, &QObject::deleteLater);
    connect(m_pump, &FramePumpWorker::frameWritten,
            this, &X11RecordWorker::frameWritten);
    connect(m_pump, &FramePumpWorker::errorOccurred, this,
            [this](const QString &message) {
                qCCritical(dsrApp) << "X11RecordWorker: pump error:" << message;
                fail(message);
            });
    connect(m_pump, &FramePumpWorker::started, this, [this]() {
        emit started();
    });

    FramePumpWorker *pump = m_pump;
    const int width = frame->width;
    const int height = frame->height;
    const int stride = frame->stride;
    m_pumpThread->start();
    // 编码/封装在独立线程运行，抓屏定时器不会被 H.264 或 GIF 编码阻塞。
    QMetaObject::invokeMethod(pump, [pump, width, height, stride]() {
        pump->start(width, height, stride);
    }, Qt::QueuedConnection);
    m_pumpStarted = true;
}

void X11RecordWorker::checkWatchdog()
{
    if (!m_running)
        return;

    if (m_lastSampleNs == 0) {
        if (m_clock.elapsed() > 3000)
            fail(QStringLiteral("X11 采集在 3 秒内没有产生任何帧"));
        return;
    }

    const qint64 now = m_clock.nsecsElapsed();
    if (now - m_lastSampleNs > 5000000000LL)
        fail(QStringLiteral("X11 采集停滞超过 5 秒"));
}

void X11RecordWorker::fail(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    qCCritical(dsrApp) << "X11RecordWorker error:" << message;
    emit errorOccurred(message);
    stop();
}

void X11RecordWorker::stop()
{
    // 启动失败时也要清理并发出 finished，避免调用方悬挂。
    if (!m_running && !m_pump && !m_grabber && !m_store) {
        if (!m_finishedEmitted && m_failed) {
            m_finishedEmitted = true;
            emit finished();
        }
        return;
    }

    m_running = false;
    if (m_captureTimer) {
        m_captureTimer->stop();
        delete m_captureTimer;
        m_captureTimer = nullptr;
    }
    if (m_watchdog) {
        m_watchdog->stop();
        delete m_watchdog;
        m_watchdog = nullptr;
    }

    if (m_pump) {
        if (m_pumpThread && m_pumpThread->isRunning()) {
            FramePumpWorker *pump = m_pump;
            QMetaObject::invokeMethod(pump, [pump]() { pump->stop(); },
                                      Qt::BlockingQueuedConnection);
        }
        m_pump = nullptr;
    }
    if (m_pumpThread) {
        m_pumpThread->quit();
        if (!m_pumpThread->wait(20000))
            qCWarning(dsrApp) << "X11RecordWorker: pump thread did not stop in time";
        delete m_pumpThread;
        m_pumpThread = nullptr;
    }
    m_pumpStarted = false;

    if (m_grabber) {
        m_grabber->close();
        delete m_grabber;
        m_grabber = nullptr;
    }
    if (m_store) {
        m_store->clear();
        delete m_store;
        m_store = nullptr;
    }

    qCInfo(dsrApp) << "X11RecordWorker stopped, output:" << m_outputPath;
    if (!m_finishedEmitted) {
        m_finishedEmitted = true;
        emit finished();
    }
}
