// Copyright (C) 2017-2026 ~ 2018 Deepin Technology Co., Ltd.
// SPDX-FileCopyrightText: 2022 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "extcapturerecorder.h"
#include "extcaptureintegration.h"
#include "extcaptureframebuffer.h"
#include "framepumpworker.h"
#include "capturepipelineworker.h"
#include "dmaframegpuconverter.h"
#include "shmformat.h"
#include "../media/ucodecrecorder.h"
#include "../utils/log.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QFileInfo>
#include <cmath>
#include <drm/drm_fourcc.h>

ExtCaptureRecorder::ExtCaptureRecorder(QObject *parent)
    : QObject(parent)
    , m_extCapture(new ExtCaptureIntegration(this))
    , m_frameBuffer(new ExtCaptureFrameBuffer(this))
    , m_captureTimer(new QTimer(this))
    , m_captureWatchdog(new QTimer(this))
    , m_state(Stopped)
    , m_frameRate(30)
    , m_frameCount(0)
    , m_startTime(0)
    , m_includeCursor(false)
    , m_frameWidth(0)
    , m_frameHeight(0)
    , m_frameStride(0)
    , m_streamingMode(true)
    , m_framePump(nullptr)
    , m_framePumpThread(nullptr)
    , m_latestFrame(new LatestFrameStore)
    , m_lastCaptureRequestNs(-1)
    , m_captureIntervalNs(1000000000LL / 30)
    , m_captureSequence(0)
    , m_pumpOutputFrames(0)
    , m_pumpRepeatedFrames(0)
    , m_pumpBehindMs(0)
    , m_firstFrameWidth(0)
    , m_firstFrameHeight(0)
    , m_firstFrameStride(0)
{
    // 连接ext-capture信号
    connect(m_extCapture, &ExtCaptureIntegration::available,
            this, &ExtCaptureRecorder::onExtCaptureAvailable);
    connect(m_extCapture, &ExtCaptureIntegration::unavailable,
            this, &ExtCaptureRecorder::onExtCaptureUnavailable);
    connect(m_extCapture, &ExtCaptureIntegration::recordingStarted,
            this, &ExtCaptureRecorder::onRecordingStarted);
    connect(m_extCapture, &ExtCaptureIntegration::recordingStopped,
            this, &ExtCaptureRecorder::onRecordingStopped);
    connect(m_extCapture, &ExtCaptureIntegration::frameReady,
            this, [this](const void *data, size_t size, int width, int height, int stride,
                         uint32_t format, uint64_t timestamp) {
                if (m_streamingMode || m_state != Recording)
                    return;
                const auto rgba = Dsr::ExtImageCapture::shmToRgba(data, size, width, height,
                                                                stride, format);
                if (rgba.isEmpty()) {
                    emit error(QStringLiteral("SHM 帧无法转换为 RGBA"));
                    scheduleNextCapture();
                    return;
                }
                onFrameReady(rgba.constData(), rgba.size(), width, height,
                             Dsr::ExtImageCapture::rgbaBytesPerLine(width), timestamp);
            });
    connect(m_extCapture, &ExtCaptureIntegration::shmFrameReady,
            this, &ExtCaptureRecorder::onShmFrameReady);
    connect(m_extCapture, &ExtCaptureIntegration::dmaFrameReady,
            this, &ExtCaptureRecorder::onDmaFrameReady);
    connect(m_extCapture, &ExtCaptureIntegration::multiScreenFrameReady,
            this, &ExtCaptureRecorder::onMultiScreenFrameReady);
    connect(m_extCapture, &ExtCaptureIntegration::error,
            this, &ExtCaptureRecorder::onExtCaptureError);

    // 设置捕获定时器
    m_captureTimer->setSingleShot(true);
    connect(m_captureTimer, &QTimer::timeout,
            this, &ExtCaptureRecorder::onCaptureTimer);
    m_captureWatchdog->setSingleShot(true);
    m_captureWatchdog->setInterval(3000);
    connect(m_captureWatchdog, &QTimer::timeout,
            this, &ExtCaptureRecorder::onCaptureWatchdog);

    qCDebug(dsrApp) << "ExtCaptureRecorder initialized";
}

ExtCaptureRecorder::~ExtCaptureRecorder()
{
    if (m_state == Recording) {
        stopRecording();
    }

    stopFramePump();
    stopCapturePipeline();
    delete m_latestFrame;
    m_latestFrame = nullptr;

}

void ExtCaptureRecorder::setAudioOptions(int audioType,
                                         const QString &systemAudioDevice)
{
    m_audioOptions.type = qBound(static_cast<int>(AudioCaptureOptions::NoAudio),
                                 audioType,
                                 static_cast<int>(AudioCaptureOptions::MicrophoneAndSystemAudio));
    m_audioOptions.systemDevice = systemAudioDevice.trimmed();
    qCWarning(dsrApp) << "ExtCaptureRecorder::setAudioOptions:"
                      << "type:" << m_audioOptions.type
                      << "systemDevice:" << m_audioOptions.systemDevice;
}

bool ExtCaptureRecorder::isAvailable() const
{
    return m_extCapture && m_extCapture->isAvailable();
}

bool ExtCaptureRecorder::startRecording(QScreen *screen, bool includeCursor,
                                       const QString &outputPath, int frameRate,
                                       const QRect &captureRegion)
{
    qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: *** START RECORDING REQUEST *** current state:" << m_state;

    if (m_state != Stopped) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: Already recording or in transition, state:" << m_state;
        return false;
    }

    if (!isAvailable()) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: ext-image-copy-capture not available";
        emit error("ext-image-copy-capture protocol not available");
        return false;
    }

    if (!screen) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: Invalid screen provided";
        emit error("Invalid screen");
        return false;
    }

    // 设置录制参数
    m_multiScreenRecording = false;
    m_includeCursor = includeCursor;
    m_frameRate = qMax(1, qMin(frameRate, 60)); // 限制在1-60fps
    m_captureIntervalNs = 1000000000LL / m_frameRate;
    m_outputPath = outputPath.isEmpty() ? generateDefaultOutputPath() : outputPath;
    m_captureOutputGeometry = screen->geometry();
    m_captureRegionGlobal = captureRegion.normalized().intersected(m_captureOutputGeometry);
    m_cropRect = QRect();

    qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: Parameters - includeCursor:" << m_includeCursor << "FPS:" << m_frameRate << "Output:" << m_outputPath;

    // 重置计数器
    m_frameCount = 0;
    m_captureSequence = 0;
    m_pipelineJobId = 0;
    m_pipelineBusy = false;
    m_lastCaptureRequestNs = -1;
    m_pumpOutputFrames = 0;
    m_pumpRepeatedFrames = 0;
    m_pumpBehindMs = 0;
    m_firstFrameWidth = 0;
    m_firstFrameHeight = 0;
    m_firstFrameStride = 0;
    m_firstFrameBuffer.clear();
    if (m_latestFrame)
        m_latestFrame->clear();

    // 清空帧缓冲区
    if (m_frameBuffer) {
        m_frameBuffer->clear();
    }

    setState(Starting);

    if (!ensureCapturePipeline()) {
        setState(Error);
        emit error(QStringLiteral("无法创建采集工作线程"));
        return false;
    }

    // 开始ext-capture录制
    if (!m_extCapture->startScreenRecording(screen, includeCursor)) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: Failed to start ext-capture";
        stopCapturePipeline();
        setState(Error);
        emit error("Failed to start screen recording");
        return false;
    }

    qCWarning(dsrApp) << "ExtCaptureRecorder::startRecording: *** RECORDING START SUCCESS ***";
    return true;
}

bool ExtCaptureRecorder::startMultiScreenRecording(const QList<QScreen *> &screens,
                                                   bool includeCursor,
                                                   const QString &outputPath,
                                                   int frameRate,
                                                   const QRect &captureRegion)
{
    qCWarning(dsrApp) << "ExtCaptureRecorder::startMultiScreenRecording: screens:"
                      << screens.size() << "region:" << captureRegion;

    if (m_state != Stopped) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::startMultiScreenRecording: already active";
        return false;
    }
    if (!isAvailable()) {
        emit error(QStringLiteral("ext-image-copy-capture protocol not available"));
        return false;
    }
    if (screens.isEmpty() || captureRegion.isEmpty()) {
        emit error(QStringLiteral("Invalid cross-output recording parameters"));
        return false;
    }

    m_multiScreenRecording = true;
    m_includeCursor = includeCursor;
    m_frameRate = qMax(1, qMin(frameRate, 60));
    m_captureIntervalNs = 1000000000LL / m_frameRate;
    m_outputPath = outputPath.isEmpty() ? generateDefaultOutputPath() : outputPath;
    m_captureRegionGlobal = captureRegion.normalized();
    m_captureOutputGeometry = QRect();
    m_cropRect = QRect();
    m_frameCount = 0;
    m_captureSequence = 0;
    m_lastCaptureRequestNs = -1;
    m_pumpOutputFrames = 0;
    m_pumpRepeatedFrames = 0;
    m_pumpBehindMs = 0;
    m_firstFrameWidth = 0;
    m_firstFrameHeight = 0;
    m_firstFrameStride = 0;
    m_firstFrameBuffer.clear();

    if (m_latestFrame)
        m_latestFrame->clear();

    if (m_frameBuffer)
        m_frameBuffer->clear();

    setState(Starting);
    if (!m_extCapture->startMultiScreenRecording(screens, includeCursor, m_captureRegionGlobal)) {
        setState(Error);
        emit error(QStringLiteral("Failed to start multi-output recording"));
        return false;
    }

    return true;
}

void ExtCaptureRecorder::stopRecording()
{
    if (m_state != Recording && m_state != Starting && m_state != Stopping && m_state != Error) {
        return;
    }

    setState(Stopping);

    // 不在这里断开 m_extCapture 的帧信号。ExtCaptureRecorder 会被同一
    // 进程复用，断开后下一次录制就再也收不到帧；停止期间的残留帧由
    // m_state != Recording 的守卫丢弃。

    // 停止定时器
    m_captureTimer->stop();
    m_captureWatchdog->stop();
    // 确保移除可能的挂起触发
    m_captureTimer->disconnect(this);
    connect(m_captureTimer, &QTimer::timeout, this, &ExtCaptureRecorder::onCaptureTimer);

    // 停止ext-capture
    if (m_extCapture) {
        m_extCapture->stopRecording();
    }

    // 先让工作线程结束并丢弃所有在途结果，再进入编码收尾。
    stopCapturePipeline();

    // 停止帧缓冲区
    if (m_frameBuffer) {
        m_frameBuffer->setGetFrame(false);
    }

    // 底层正常会发出 recordingStopped 并在 onRecordingStopped 中收尾。
    // 若会话尚未真正启动（例如启动失败后调用停止），这里补一次收尾。
    if (m_state == Stopping) {
        finalizeRecording();
        emit recordingStopped();
    }
}

ExtCaptureRecorder::RecordState ExtCaptureRecorder::state() const
{
    return m_state;
}

int ExtCaptureRecorder::frameCount() const
{
    return m_frameCount;
}

qint64 ExtCaptureRecorder::recordingDuration() const
{
    if (m_state != Recording || m_startTime == 0) {
        return 0;
    }

    qint64 currentTime = (m_state == Recording) ?
        QDateTime::currentMSecsSinceEpoch() : m_startTime;
    return currentTime - m_startTime;
}

void ExtCaptureRecorder::setOutputPath(const QString &path)
{
    if (m_state == Stopped) {
        m_outputPath = path;
    }
}

QString ExtCaptureRecorder::outputPath() const
{
    return m_outputPath;
}

ExtCaptureFrameBuffer* ExtCaptureRecorder::getFrameBuffer() const
{
    return m_frameBuffer;
}

void ExtCaptureRecorder::onExtCaptureAvailable()
{
    qCWarning(dsrApp) << "ExtCaptureRecorder::onExtCaptureAvailable: *** EXT-CAPTURE AVAILABLE ***";
    emit protocolAvailable();
}

void ExtCaptureRecorder::onExtCaptureUnavailable()
{
    qCDebug(dsrApp) << "ExtCaptureRecorder: ext-capture unavailable";

    if (m_state == Recording) {
        stopRecording();
        emit error("ext-capture became unavailable");
    }
}

void ExtCaptureRecorder::onRecordingStarted()
{
    qCWarning(dsrApp) << "ExtCaptureRecorder::onRecordingStarted: *** RECORDING ACTUALLY STARTED ***";
    setState(Recording);
    m_startTime = QDateTime::currentMSecsSinceEpoch();
    m_captureClock.start();
    m_lastCaptureRequestNs = -1;
    m_pendingFrame = false;
    m_captureTimer->stop();
    m_captureWatchdog->stop();
    m_statTicks = 0;
    m_statRequests = 0;
    m_statBusy = 0;
    m_statFrames = 0;
    m_statLatencyTotalMs = 0;
    m_statLatencyMaxMs = 0;
    m_statProcessTotalMs = 0;
    m_statProcessMaxMs = 0;
    m_captureStatsTimer.start();

    if (m_streamingMode) {
        // 流式编码模式：等待第一帧到达时启动 ucodec 编码落盘
        qCWarning(dsrApp) << "ExtCaptureRecorder::onRecordingStarted: Streaming mode - waiting for first frame to start ucodec recorder";
    } else {
        // 缓冲模式：启用帧缓冲区
        if (m_frameBuffer) {
            m_frameBuffer->setGetFrame(true);
            qCWarning(dsrApp) << "ExtCaptureRecorder::onRecordingStarted: Frame buffer enabled";
        }
    }

    // 采集由“上一帧就绪”驱动；首帧直接发起，避免启动阶段 GUI 初始化把
    // 0ms 定时器推迟到数百毫秒之后。
    onCaptureTimer();
    qCWarning(dsrApp) << "ExtCaptureRecorder::onRecordingStarted: ready-driven capture started, target fps:" << m_frameRate;
    emit recordingStarted();
}

void ExtCaptureRecorder::onRecordingStopped()
{
    // 底层已停止，进行收尾
    setState(Stopped);

    stopCapturePipeline();

    // 先完成编码资源的收尾
    finalizeRecording();

    // 再对外发出停止信号，保证上层收到时已经真正完成
    emit recordingStopped();
}

void ExtCaptureRecorder::onFrameReady(const void *data, size_t size, int width, int height, int stride, uint64_t timestamp)
{
    // 流式录制统一走 onShmFrameReady() 的 memfd 工作线程路径；这里保留
    // 旧指针信号，仅用于历史缓冲模式/单测。
    if (m_streamingMode) {
        return;
    }

    if (!data || !m_frameBuffer) {
        return;
    }

    // 停止或非录制状态下一律丢弃
    if (m_state != Recording) {
        return;
    }
    if (m_pipelineBusy) {
        qCDebug(dsrApp) << "ExtCaptureRecorder::onFrameReady: duplicate frame while pipeline busy, ignored";
        return;
    }
    recordFrameArrival();
    m_captureWatchdog->stop();

    const qint64 expectedSize = static_cast<qint64>(stride) * height;
    if (expectedSize <= 0 || static_cast<qint64>(size) < expectedSize) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::onFrameReady: short frame"
                          << size << "expected:" << expectedSize;
        scheduleNextCapture();
        return;
    }

    // 初始化帧缓冲区（第一帧时）
    if (m_frameCount == 0) {
        m_frameWidth = width;
        m_frameHeight = height;
        m_frameStride = stride;
        updateCropRect(width, height);

        if (!m_frameBuffer->initialize(width, height, stride)) {
            qCWarning(dsrApp) << "ExtCaptureRecorder: Failed to initialize frame buffer";
            emit error("Failed to initialize frame buffer");
            scheduleNextCapture();
            return;
        }

        qCDebug(dsrApp) << "ExtCaptureRecorder: Frame format:" << width << "x" << height << "stride:" << stride;
    }

    QElapsedTimer processTimer;
    processTimer.start();
    if (!m_streamingMode) {
        int64_t frameTimestamp = (timestamp > 0) ? static_cast<int64_t>(timestamp) : QDateTime::currentMSecsSinceEpoch() * 1000000LL;
        m_frameBuffer->appendBuffer(static_cast<const unsigned char*>(data), size, width, height, stride, frameTimestamp);
    }
    const qint64 processMs = processTimer.elapsed();
    m_statProcessTotalMs += processMs;
    m_statProcessMaxMs = qMax(m_statProcessMaxMs, processMs);
    ++m_statFrames;

    m_frameCount++;

    // 发送进度更新
    emit progressUpdated(m_frameCount, recordingDuration());
    scheduleNextCapture();
}

void ExtCaptureRecorder::onShmFrameReady(int shmFd, size_t size, int width, int height,
                                         int stride, uint32_t format, uint64_t timestamp)
{
    if (!m_streamingMode || shmFd < 0)
        return;
    if (m_state != Recording)
        return;
    if (m_pipelineBusy) {
        qCDebug(dsrApp) << "ExtCaptureRecorder::onShmFrameReady: duplicate frame while pipeline busy, ignored";
        return;
    }
    recordFrameArrival();
    m_captureWatchdog->stop();

    if (!ensureCapturePipeline()) {
        QMetaObject::invokeMethod(this, [this]() {
            onExtCaptureError(QStringLiteral("采集工作线程不可用"));
        }, Qt::QueuedConnection);
        return;
    }

    const quint64 jobId = ++m_pipelineJobId;
    ShmFrameSourcePtr source;
    QString error;
    if (!makeShmFrameSource(shmFd, size, width, height, stride, format, timestamp, jobId,
                            &source, &error)) {
        QMetaObject::invokeMethod(this, [this, error]() {
            onExtCaptureError(error.isEmpty()
                                      ? QStringLiteral("SHM 帧描述失败")
                                      : error);
        }, Qt::QueuedConnection);
        return;
    }

    m_pipelineBusy = true;
    QMetaObject::invokeMethod(m_capturePipeline, "convertSingleShm",
                              Qt::QueuedConnection,
                              Q_ARG(ShmFrameSourcePtr, source),
                              Q_ARG(quint64, jobId));
}

void ExtCaptureRecorder::onDmaFrameReady(int dmaBufferFd, void *gbmBo, size_t size, int width, int height, int stride, uint64_t timestamp)
{
    Q_UNUSED(size)
    if (dmaBufferFd < 0 || !gbmBo) {
        return;
    }

    // 停止或非录制状态下一律丢弃
    if (m_state != Recording) {
        return;
    }
    if (m_pipelineBusy) {
        qCDebug(dsrApp) << "ExtCaptureRecorder::onDmaFrameReady: duplicate frame while pipeline busy, ignored";
        return;
    }
    recordFrameArrival();
    m_captureWatchdog->stop();

    if (!m_streamingMode) {
        m_frameCount++;
        emit progressUpdated(m_frameCount, recordingDuration());
        scheduleNextCapture();
        return;
    }

    if (!ensureCapturePipeline()) {
        QMetaObject::invokeMethod(this, [this]() {
            onExtCaptureError(QStringLiteral("采集工作线程不可用"));
        }, Qt::QueuedConnection);
        return;
    }

    const QString devicePath = m_extCapture ? m_extCapture->dmaDevicePath() : QString();
    const uint64_t modifier = m_extCapture ? m_extCapture->dmaModifier()
                                           : DRM_FORMAT_MOD_LINEAR;
    const quint64 jobId = ++m_pipelineJobId;
    DmaFrameSourcePtr source;
    QString error;
    if (!makeDmaFrameSource(dmaBufferFd, gbmBo, width, height, stride, devicePath,
                            modifier, timestamp, jobId, &source, &error)) {
        // 当前仍在 Session 的帧就绪回调中，异步报错，避免上层同步停止
        // 录制并销毁 Session 造成重入。
        QMetaObject::invokeMethod(this, [this, error]() {
            onExtCaptureError(error.isEmpty()
                                      ? QStringLiteral("DMA-BUF 帧描述失败")
                                      : error);
        }, Qt::QueuedConnection);
        return;
    }

    m_pipelineBusy = true;
    QMetaObject::invokeMethod(m_capturePipeline, "convertSingleDma",
                              Qt::QueuedConnection,
                              Q_ARG(DmaFrameSourcePtr, source),
                              Q_ARG(quint64, jobId));
}

void ExtCaptureRecorder::onExtCaptureError(const QString &message)
{
    setState(Error);
    emit error(message);
}

void ExtCaptureRecorder::onMultiScreenFrameReady(const QByteArray &data,
                                                 int width,
                                                 int height,
                                                 int stride,
                                                 uint64_t timestamp)
{
    if (m_state != Recording || data.isEmpty() || width <= 0 || height <= 0 || stride <= 0)
        return;

    recordFrameArrival();
    m_captureWatchdog->stop();

    const qint64 expectedSize = static_cast<qint64>(stride) * height;
    if (data.size() < expectedSize) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::onMultiScreenFrameReady: short frame"
                          << data.size() << "expected:" << expectedSize;
        scheduleNextCapture();
        return;
    }

    m_frameWidth = width;
    m_frameHeight = height;
    m_frameStride = stride;

    QElapsedTimer processTimer;
    processTimer.start();
    storeFrameSnapshot(data, width, height, stride, timestamp);
    const qint64 processMs = processTimer.elapsed();
    m_statProcessTotalMs += processMs;
    m_statProcessMaxMs = qMax(m_statProcessMaxMs, processMs);
    ++m_statFrames;

    ++m_frameCount;
    emit progressUpdated(m_frameCount, recordingDuration());
    scheduleNextCapture();
}

void ExtCaptureRecorder::onPipelineSingleFrameReady(const QByteArray &data, int width,
                                                    int height, int stride,
                                                    uint64_t timestamp, quint64 jobId,
                                                    qint64 convertMs)
{
    if (m_state != Recording || jobId != m_pipelineJobId)
        return;

    m_pipelineBusy = false;
    consumeCapturedFrame(data, width, height, stride, timestamp, convertMs);
}

void ExtCaptureRecorder::onPipelineSingleFrameFailed(const QString &message, quint64 jobId)
{
    if (m_state != Recording || jobId != m_pipelineJobId)
        return;

    m_pipelineBusy = false;
    onExtCaptureError(message.isEmpty() ? QStringLiteral("采集帧转换失败") : message);
}

void ExtCaptureRecorder::consumeCapturedFrame(const QByteArray &data, int width, int height,
                                              int stride, uint64_t timestamp,
                                              qint64 convertMs)
{
    if (m_state != Recording || data.isEmpty())
        return;

    if (m_frameCount == 0) {
        m_frameWidth = width;
        m_frameHeight = height;
        m_frameStride = stride;
        updateCropRect(width, height);
        qCDebug(dsrApp) << "ExtCaptureRecorder: captured frame format:" << width << "x"
                        << height << "stride:" << stride;
    }

    QElapsedTimer processTimer;
    processTimer.start();
    storeFrameSnapshot(data, width, height, stride, timestamp);
    const qint64 processMs = processTimer.elapsed() + qMax<qint64>(0, convertMs);
    m_statProcessTotalMs += processMs;
    m_statProcessMaxMs = qMax(m_statProcessMaxMs, processMs);
    ++m_statFrames;

    ++m_frameCount;
    emit progressUpdated(m_frameCount, recordingDuration());
    scheduleNextCapture();
}

void ExtCaptureRecorder::onCaptureTimer()
{
    if (m_state != Recording || !m_extCapture) {
        return;
    }
    if (m_pendingFrame || m_pipelineBusy) {
        return;
    }

    ++m_statTicks;
    QElapsedTimer requestCallTimer;
    requestCallTimer.start();
    const bool requested = m_multiScreenRecording
            ? m_extCapture->captureMultiScreenFrame()
            : m_extCapture->captureFrame();
    const qint64 requestCallNs = requestCallTimer.nsecsElapsed();
    m_statRequestCallTotalNs += requestCallNs;
    m_statRequestCallMaxNs = qMax(m_statRequestCallMaxNs, requestCallNs);
    if (requested) {
        ++m_statRequests;
        m_pendingFrame = true;
        m_pendingFrameTimer.start();
        m_lastCaptureRequestNs = m_captureClock.nsecsElapsed();
        m_captureWatchdog->start();
    } else {
        ++m_statBusy;
        // 会话可能仍在切换状态，稍后重试；帧就绪后会直接触发下一帧请求。
        m_captureTimer->start(qBound(2, 1000 / qMax(1, m_frameRate), 10));
    }

    if (qEnvironmentVariableIsSet("DSR_CAPTURE_STATS")
            && m_captureStatsTimer.elapsed() >= 1000) {
        logCaptureStats();
    }
}

void ExtCaptureRecorder::onCaptureWatchdog()
{
    if (m_state != Recording || !m_pendingFrame)
        return;

    qCWarning(dsrApp) << "ExtCaptureRecorder: frame request timed out, retrying";
    m_pendingFrame = false;
    scheduleNextCapture();
}

void ExtCaptureRecorder::scheduleNextCapture()
{
    if (m_state != Recording || m_pendingFrame)
        return;

    qint64 delayMs = 0;
    if (m_lastCaptureRequestNs >= 0) {
        const qint64 elapsedNs = m_captureClock.nsecsElapsed() - m_lastCaptureRequestNs;
        if (elapsedNs < m_captureIntervalNs) {
            delayMs = (m_captureIntervalNs - elapsedNs + 999999LL) / 1000000LL;
        }
    }
    // 至少让事件循环跑一轮，确保上一帧的 destroy/flush 已交给 compositor。
    m_captureTimer->start(static_cast<int>(qMax<qint64>(1, delayMs)));
}

void ExtCaptureRecorder::logCaptureStats()
{
    const qint64 elapsedMs = m_captureStatsTimer.restart();
    const double fps = elapsedMs > 0
            ? m_statFrames * 1000.0 / elapsedMs
            : 0.0;
    const double avgLatency = m_statFrames > 0
            ? static_cast<double>(m_statLatencyTotalMs) / m_statFrames
            : 0.0;
    const double avgProcess = m_statFrames > 0
            ? static_cast<double>(m_statProcessTotalMs) / m_statFrames
            : 0.0;
    const double avgRequestCallMs = m_statTicks > 0
            ? static_cast<double>(m_statRequestCallTotalNs) / m_statTicks / 1000000.0
            : 0.0;
    const double maxRequestCallMs = static_cast<double>(m_statRequestCallMaxNs) / 1000000.0;
    qCWarning(dsrApp) << "capture stats:"
                      << "windowMs:" << elapsedMs
                      << "ticks:" << m_statTicks
                      << "requests:" << m_statRequests
                      << "busy:" << m_statBusy
                      << "frames:" << m_statFrames
                      << "fps:" << fps
                      << "avgRequestToReadyMs:" << avgLatency
                      << "maxRequestToReadyMs:" << m_statLatencyMaxMs
                      << "avgRequestCallMs:" << avgRequestCallMs
                      << "maxRequestCallMs:" << maxRequestCallMs
                      << "avgProcessMs:" << avgProcess
                      << "maxProcessMs:" << m_statProcessMaxMs
                      << "outputFrames:" << m_pumpOutputFrames
                      << "repeatedFrames:" << m_pumpRepeatedFrames
                      << "pumpBehindMs:" << m_pumpBehindMs;

    m_statTicks = 0;
    m_statRequests = 0;
    m_statBusy = 0;
    m_statFrames = 0;
    m_statLatencyTotalMs = 0;
    m_statLatencyMaxMs = 0;
    m_statProcessTotalMs = 0;
    m_statProcessMaxMs = 0;
    m_statRequestCallTotalNs = 0;
    m_statRequestCallMaxNs = 0;
}

void ExtCaptureRecorder::onFramePumpStats(int outputFrames, int repeatedFrames, qint64 behindMs)
{
    m_pumpOutputFrames = outputFrames;
    m_pumpRepeatedFrames = repeatedFrames;
    m_pumpBehindMs = behindMs;
}

void ExtCaptureRecorder::onFramePumpError(const QString &message)
{
    qCCritical(dsrApp) << "ExtCaptureRecorder: frame pump failed:" << message;
    emit error(message);
    if (m_state == Recording || m_state == Starting)
        stopRecording();
}

void ExtCaptureRecorder::recordFrameArrival()
{
    if (!m_pendingFrame) {
        return;
    }
    const qint64 latencyMs = m_pendingFrameTimer.elapsed();
    m_statLatencyTotalMs += latencyMs;
    m_statLatencyMaxMs = qMax(m_statLatencyMaxMs, latencyMs);
    m_pendingFrame = false;
}

bool ExtCaptureRecorder::ensureCapturePipeline()
{
    if (m_capturePipeline && m_capturePipelineThread
            && m_capturePipelineThread->isRunning()) {
        return true;
    }

    stopCapturePipeline();

    m_capturePipelineThread = new QThread(this);
    m_capturePipeline = new CapturePipelineWorker;
    m_capturePipeline->moveToThread(m_capturePipelineThread);

    connect(m_capturePipeline, &CapturePipelineWorker::singleFrameReady,
            this, &ExtCaptureRecorder::onPipelineSingleFrameReady);
    connect(m_capturePipeline, &CapturePipelineWorker::singleFrameFailed,
            this, &ExtCaptureRecorder::onPipelineSingleFrameFailed);

    m_capturePipelineThread->start();
    qCWarning(dsrApp) << "ExtCaptureRecorder: capture pipeline thread started";
    return true;
}

void ExtCaptureRecorder::stopCapturePipeline()
{
    if (!m_capturePipeline && !m_capturePipelineThread) {
        m_pipelineBusy = false;
        return;
    }

    // 使所有在途结果失效；随后阻塞等待工作线程把当前任务处理完。
    ++m_pipelineJobId;
    m_pipelineBusy = false;

    if (m_capturePipelineThread && m_capturePipelineThread->isRunning()) {
        if (m_capturePipeline) {
            QMetaObject::invokeMethod(m_capturePipeline,
                                      [worker = m_capturePipeline]() { delete worker; },
                                      Qt::BlockingQueuedConnection);
            m_capturePipeline = nullptr;
        }
        m_capturePipelineThread->quit();
        if (!m_capturePipelineThread->wait(8000))
            qCWarning(dsrApp) << "ExtCaptureRecorder: capture pipeline thread did not stop in time";
    } else if (m_capturePipeline) {
        delete m_capturePipeline;
        m_capturePipeline = nullptr;
    }

    delete m_capturePipelineThread;
    m_capturePipelineThread = nullptr;
    qCWarning(dsrApp) << "ExtCaptureRecorder: capture pipeline thread stopped";
}

bool ExtCaptureRecorder::ensureFramePump(int width, int height, int stride)
{
    if (m_framePump)
        return true;

    QDir outputDir = QFileInfo(m_outputPath).dir();
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        qCCritical(dsrApp) << "ExtCaptureRecorder: failed to create output directory" << outputDir.path();
        return false;
    }

    m_framePumpThread = new QThread(this);
    m_framePump = new FramePumpWorker(m_frameRate, m_outputPath, m_cropRect,
                                      m_latestFrame, m_audioOptions);
    m_framePump->moveToThread(m_framePumpThread);
    connect(m_framePump, &FramePumpWorker::statsUpdated,
            this, &ExtCaptureRecorder::onFramePumpStats);
    connect(m_framePump, &FramePumpWorker::errorOccurred,
            this, &ExtCaptureRecorder::onFramePumpError);
    m_framePumpThread->start();

    QMetaObject::invokeMethod(m_framePump, "start", Qt::QueuedConnection,
                              Q_ARG(int, width), Q_ARG(int, height), Q_ARG(int, stride));
    return true;
}

void ExtCaptureRecorder::stopFramePump()
{
    if (!m_framePump && !m_framePumpThread)
        return;

    if (m_framePumpThread && m_framePumpThread->isRunning()) {
        if (m_framePump) {
            QMetaObject::invokeMethod(m_framePump, "stop", Qt::BlockingQueuedConnection);
            int outputFrames = 0;
            int repeatedFrames = 0;
            QMetaObject::invokeMethod(m_framePump, [worker = m_framePump, &outputFrames, &repeatedFrames]() {
                outputFrames = worker->outputFrames();
                repeatedFrames = worker->repeatedFrames();
                delete worker;
            },
                                      Qt::BlockingQueuedConnection);
            m_pumpOutputFrames = outputFrames;
            m_pumpRepeatedFrames = repeatedFrames;
            m_framePump = nullptr;
        }
        m_framePumpThread->quit();
        if (!m_framePumpThread->wait(8000))
            qCWarning(dsrApp) << "ExtCaptureRecorder: frame pump thread did not stop in time";
    } else if (m_framePump) {
        delete m_framePump;
        m_framePump = nullptr;
    }

    delete m_framePumpThread;
    m_framePumpThread = nullptr;
}

bool ExtCaptureRecorder::storeFrameSnapshot(const QByteArray &data, int width, int height,
                                            int stride, uint64_t timestamp)
{
    if (!m_latestFrame || data.isEmpty() || width <= 0 || height <= 0 || stride <= 0)
        return false;

    const qint64 expectedSize = static_cast<qint64>(stride) * height;
    if (data.size() < expectedSize) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::storeFrameSnapshot: short frame"
                          << data.size() << "expected:" << expectedSize;
        return false;
    }

    CaptureFrameSnapshotPtr snapshot = std::make_shared<CaptureFrameSnapshot>();
    if (data.size() == expectedSize)
        snapshot->data = data;
    else
        snapshot->data = QByteArray(data.constData(), expectedSize);
    snapshot->width = width;
    snapshot->height = height;
    snapshot->stride = stride;
    snapshot->ptsNs = static_cast<qint64>(timestamp);
    snapshot->sequence = ++m_captureSequence;
    m_latestFrame->set(snapshot);

    return ensureFramePump(width, height, stride);
}

QByteArray ExtCaptureRecorder::copyDmaFrame(int dmaBufferFd, void *gbmBo, int width,
                                            int height, int stride)
{
#ifdef ENABLE_UNIT_TEST
    Q_UNUSED(dmaBufferFd)
    Q_UNUSED(gbmBo)
    Q_UNUSED(width)
    Q_UNUSED(height)
    Q_UNUSED(stride)
    return QByteArray();
#else
    if (dmaBufferFd < 0 || !gbmBo || width <= 0 || height <= 0)
        return QByteArray();

    gbm_bo *bo = static_cast<gbm_bo *>(gbmBo);
    const uint32_t boWidth = gbm_bo_get_width(bo);
    const uint32_t boHeight = gbm_bo_get_height(bo);
    if (boWidth == 0 || boHeight == 0)
        return QByteArray();

    uint64_t modifier = gbm_bo_get_modifier(bo);
    if (modifier == DRM_FORMAT_MOD_INVALID)
        modifier = DRM_FORMAT_MOD_LINEAR;

    DmaFrameGpuConverter converter(gbm_bo_get_device(bo));
    QByteArray result;
    QString error;
    if (!converter.convertToRgba(dmaBufferFd, static_cast<int>(boWidth),
                                 static_cast<int>(boHeight),
                                 stride > 0 ? stride : static_cast<int>(gbm_bo_get_stride(bo)),
                                 gbm_bo_get_offset(bo, 0), gbm_bo_get_format(bo),
                                 modifier, &result, &error)) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::copyDmaFrame: GPU conversion failed:"
                          << error;
        return QByteArray();
    }
    return result;
#endif
}

void ExtCaptureRecorder::setState(RecordState newState)
{
    if (m_state != newState) {
        m_state = newState;
    }
}

void ExtCaptureRecorder::initializeCapture()
{
    // 这里可以添加录制初始化逻辑
    // 例如：创建输出文件、初始化编码器等
    qCDebug(dsrApp) << "ExtCaptureRecorder: Initialize capture for output:" << m_outputPath;
}

void ExtCaptureRecorder::finalizeRecording()
{
    m_captureTimer->stop();
    m_captureWatchdog->stop();

    if (m_framePump) {
        stopFramePump();
            if (!QFile::exists(m_outputPath) || QFileInfo(m_outputPath).size() == 0) {
            qCCritical(dsrApp) << "ExtCaptureRecorder::finalizeRecording: frame pump produced no video";
        } else {
            qCWarning(dsrApp) << "ExtCaptureRecorder::finalizeRecording: frame pump finalized without remux,"
                              << "output frames:" << m_pumpOutputFrames
                              << "repeated frames:" << m_pumpRepeatedFrames;
        }

    } else if (m_frameBuffer && m_frameBuffer->hasFrames()) {
        // 缓冲模式：创建视频文件
        if (!createVideoFile()) {
            qCCritical(dsrApp) << "ExtCaptureRecorder::finalizeRecording: Failed to create video file";
        }
    }

    // 重置frame buffer以便下次录制
    if (m_frameBuffer) {
        m_frameBuffer->reset();
    }
    if (m_latestFrame)
        m_latestFrame->clear();

    // 重置录制状态和计数器，为下次录制做准备
    m_frameCount = 0;
    m_startTime = 0;
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_frameStride = 0;
    m_lastCaptureRequestNs = -1;

    setState(Stopped);
}

void ExtCaptureRecorder::processFrameQueue()
{
    // 不再需要处理队列，帧数据直接存储在ExtCaptureFrameBuffer中
    // 外部编码器可以通过getFrameBuffer()获取帧数据
    if (m_frameBuffer) {
        qCDebug(dsrApp) << "ExtCaptureRecorder: Frame buffer status: " << m_frameBuffer->frameCount() << " frames available";
    }
}

bool ExtCaptureRecorder::createVideoFile()
{
    if (!m_frameBuffer || !m_frameBuffer->hasFrames()) {
        qCWarning(dsrApp) << "ExtCaptureRecorder::createVideoFile: No frames available";
        return false;
    }

    qCWarning(dsrApp) << "ExtCaptureRecorder::createVideoFile: Creating video file with"
                      << m_frameBuffer->frameCount() << "frames";

    QDir outputDir = QFileInfo(m_outputPath).dir();
    if (!outputDir.exists() && !outputDir.mkpath(".")) {
        qCCritical(dsrApp) << "ExtCaptureRecorder::createVideoFile: Failed to create output directory:"
                           << outputDir.path();
        return false;
    }

    QRect crop = m_cropRect;
    if (crop.isEmpty())
        crop = QRect(0, 0, m_frameWidth, m_frameHeight);
    crop = crop.intersected(QRect(0, 0, m_frameWidth, m_frameHeight));
    crop.setWidth(crop.width() & ~1);
    crop.setHeight(crop.height() & ~1);
    if (crop.width() < 2 || crop.height() < 2) {
        qCCritical(dsrApp) << "ExtCaptureRecorder::createVideoFile: Invalid crop region" << crop;
        return false;
    }

    UcodecRecorder recorder;
    AudioCaptureOptions audio;
    audio.type = AudioCaptureOptions::NoAudio;
    QString error;
    if (!recorder.start(m_outputPath, crop.width(), crop.height(), m_frameRate, 0,
                        audio, &error)) {
        qCCritical(dsrApp) << "ExtCaptureRecorder::createVideoFile: Failed to start recorder:"
                           << error;
        return false;
    }

    int framesWritten = 0;
    qint64 pts = 0;
    while (m_frameBuffer->hasFrames()) {
        ExtFrameData frame;
        if (!m_frameBuffer->getFrame(frame))
            break;
        if (!recorder.pushFrame(frame.data, frame.stride, crop, pts++, &error)) {
            qCWarning(dsrApp) << "ExtCaptureRecorder::createVideoFile: Failed to encode frame"
                              << frame.index << error;
            break;
        }
        ++framesWritten;
    }

    if (!recorder.finish(&error)) {
        qCCritical(dsrApp) << "ExtCaptureRecorder::createVideoFile: recorder finish failed:"
                           << error;
        return false;
    }

    qCWarning(dsrApp) << "ExtCaptureRecorder::createVideoFile: wrote" << framesWritten
                      << "frames to" << m_outputPath;
    return QFile::exists(m_outputPath) && QFileInfo(m_outputPath).size() > 0;
}

QString ExtCaptureRecorder::generateDefaultOutputPath()
{
    QString documentsPath = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documentsPath.isEmpty()) {
        documentsPath = QDir::homePath();
    }

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    QString filename = QString("ext-capture-recording-%1.mp4").arg(timestamp);

    return QDir(documentsPath).filePath(filename);
}

void ExtCaptureRecorder::updateCropRect(int frameWidth, int frameHeight)
{
    m_cropRect = QRect();
    if (frameWidth <= 0 || frameHeight <= 0 || m_captureRegionGlobal.isEmpty()
            || m_captureOutputGeometry.isEmpty()) {
        return;
    }

    const qreal scaleX = static_cast<qreal>(frameWidth) / m_captureOutputGeometry.width();
    const qreal scaleY = static_cast<qreal>(frameHeight) / m_captureOutputGeometry.height();
    const QRect local = m_captureRegionGlobal.translated(-m_captureOutputGeometry.topLeft());

    int x = qBound(0, qRound(local.x() * scaleX), frameWidth - 2);
    int y = qBound(0, qRound(local.y() * scaleY), frameHeight - 2);
    int width = qMin(qRound(local.width() * scaleX), frameWidth - x);
    int height = qMin(qRound(local.height() * scaleY), frameHeight - y);
    width = qMax(2, width - (width % 2));
    height = qMax(2, height - (height % 2));
    x -= x % 2;
    y -= y % 2;

    m_cropRect = QRect(x, y, width, height);
}
