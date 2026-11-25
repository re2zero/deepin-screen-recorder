#include "framepumpworker.h"

#include "../media/gifrecorder.h"
#include "../media/ucodecrecorder.h"
#include "../utils/log.h"

#include <QDebug>
#include <QTimer>

#include <algorithm>

namespace {

// GIF 无帧间预测，30fps 全帧写入会让文件体积失控；与旧录制链路保持一致，
// 最高按 12fps 采样，MP4/MKV 仍使用用户选择的帧率。
int effectiveFrameRate(int frameRate, const QString &path)
{
    const int fps = qMax(1, frameRate);
    return path.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive)
            ? qMin(fps, 12) : fps;
}

} // namespace

FramePumpWorker::FramePumpWorker(int frameRate,
                                 const QString &outputPath,
                                 const QRect &cropRect,
                                 LatestFrameStore *store,
                                 const AudioCaptureOptions &audio,
                                 QObject *parent)
    : QObject(parent)
    , m_frameRate(effectiveFrameRate(frameRate, outputPath))
    , m_outputPath(outputPath)
    , m_cropRect(cropRect)
    , m_store(store)
    , m_audio(audio)
    , m_intervalNs(1000000000LL / m_frameRate)
{
}

FramePumpWorker::~FramePumpWorker()
{
    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }
    if (m_recorder) {
        m_recorder->abort();
        m_recorder.reset();
    }
}

void FramePumpWorker::start(int width, int height, int stride)
{
    if (m_started || m_stopping)
        return;

    m_width = width;
    m_height = height;
    m_stride = stride;
    if (m_width <= 0 || m_height <= 0 || m_stride <= 0) {
        fail(QStringLiteral("Invalid frame dimensions for frame pump"));
        return;
    }

    const CaptureFrameSnapshotPtr first = m_store ? m_store->get() : CaptureFrameSnapshotPtr();
    if (!first || first->data.isEmpty()) {
        fail(QStringLiteral("No first frame available for frame pump"));
        return;
    }

    QRect crop = m_cropRect;
    if (crop.isEmpty()) {
        crop = QRect(0, 0, m_width, m_height);
    }
    crop = crop.intersected(QRect(0, 0, m_width, m_height));
    // 编码器只接受偶数尺寸。
    crop.setWidth(crop.width() & ~1);
    crop.setHeight(crop.height() & ~1);
    if (crop.width() < 2 || crop.height() < 2) {
        fail(QStringLiteral("Invalid crop rectangle for frame pump"));
        return;
    }
    m_cropRect = crop;

    if (m_outputPath.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive))
        m_recorder = std::make_unique<GifRecorder>();
    else
        m_recorder = std::make_unique<UcodecRecorder>();

    QString error;
    if (!m_recorder->start(m_outputPath, crop.width(), crop.height(), m_frameRate,
                           0, m_audio, &error)) {
        fail(QStringLiteral("Failed to start recorder: %1").arg(error));
        return;
    }

    m_clock.start();
    m_nextDeadlineNs = m_intervalNs;
    m_lastStatsNs = 0;
    m_lastPts = -1;

    if (!writeCurrentFrame()) {
        fail(QStringLiteral("Failed to write first frame to encoder"));
        return;
    }

    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);
    m_timer->setInterval(qBound(1, 1000 / (m_frameRate * 4), 10));
    connect(m_timer, &QTimer::timeout, this, &FramePumpWorker::onTick);
    m_timer->start();

    m_started = true;
    qCInfo(dsrApp) << "FramePumpWorker started:"
                   << crop.width() << "x" << crop.height()
                   << "fps:" << m_frameRate
                   << "encoder:" << m_recorder->encoderPath()
                   << "audio:" << m_recorder->hasAudio()
                   << "output:" << m_outputPath;
    emit started();
}

void FramePumpWorker::stop()
{
    if (m_stopping)
        return;

    m_stopping = true;
    if (m_timer)
        m_timer->stop();

    // 补写停止瞬间的一帧，让最后一段 CFR 区间有内容，时长与用户停止时刻一致。
    if (m_started && m_recorder && !m_failed) {
        if (!writeCurrentFrame())
            qCWarning(dsrApp) << "FramePumpWorker: failed to write final frame";
    }

    if (m_recorder) {
        if (!m_started) {
            // 启动阶段失败：不落空文件。
            m_recorder->abort();
        } else {
            QString error;
            if (!m_recorder->finish(&error)) {
                qCWarning(dsrApp) << "FramePumpWorker: recorder finish failed:" << error;
                emit errorOccurred(error);
            }
        }
        m_recorder.reset();
    }

    qCInfo(dsrApp) << "FramePumpWorker stopped, output frames:" << m_outputFrames
                   << "repeated frames:" << m_repeatedFrames
                   << "skipped cfr slots:" << m_skippedSlots;
    m_started = false;
    emit finished();
}

void FramePumpWorker::onTick()
{
    if (m_stopping || !m_started)
        return;

    const qint64 now = m_clock.nsecsElapsed();
    if (now >= m_nextDeadlineNs) {
        if (!writeCurrentFrame()) {
            fail(QStringLiteral("Failed to write frame to encoder"));
            return;
        }
        m_nextDeadlineNs += m_intervalNs;
        if (m_nextDeadlineNs <= now) {
            // 编码较慢时不补发陈旧帧，直接对齐到下一个未来时隙；
            // CFR 由“重复最新帧”保持，时间线不会因为跳帧而前移。
            m_skippedSlots += static_cast<int>((now - m_nextDeadlineNs) / m_intervalNs) + 1;
            m_nextDeadlineNs = now + m_intervalNs;
            qCWarning(dsrApp) << "FramePumpWorker aligned to current slot, skipped:"
                              << m_skippedSlots;
        }
    }

    if (now - m_lastStatsNs >= 1000000000LL) {
        m_lastStatsNs = now;
        const qint64 behindMs = now >= m_nextDeadlineNs
                ? (now - m_nextDeadlineNs) / 1000000LL
                : 0;
        emit statsUpdated(m_outputFrames, m_repeatedFrames, behindMs);
    }
}

bool FramePumpWorker::writeCurrentFrame()
{
    if (!m_recorder || !m_recorder->isStarted())
        return false;

    const CaptureFrameSnapshotPtr frame = m_store ? m_store->get() : CaptureFrameSnapshotPtr();
    if (!frame || frame->data.isEmpty())
        return false;

    if (frame->sequence != 0 && frame->sequence == m_lastFrameSequence)
        ++m_repeatedFrames;
    if (frame->sequence != 0)
        m_lastFrameSequence = frame->sequence;

    const qint64 expected = static_cast<qint64>(frame->stride) * frame->height;
    if (expected <= 0 || frame->data.size() < expected)
        return false;

    // 用实际经过时间对应的时隙作为 PTS。编码较慢跳过时隙时，PTS 会留下
    // 空档，封装器据此延长上一帧的显示时长，避免视频时长被压缩。
    qint64 pts = m_clock.nsecsElapsed() / m_intervalNs;
    if (pts <= m_lastPts)
        pts = m_lastPts + 1;

    QString error;
    if (!m_recorder->pushFrame(reinterpret_cast<const uint8_t *>(frame->data.constData()),
                               frame->stride, m_cropRect, pts, &error)) {
        qCWarning(dsrApp) << "FramePumpWorker: pushFrame failed:" << error;
        return false;
    }

    m_lastPts = pts;
    ++m_outputFrames;
    emit frameWritten(m_outputFrames);
    return true;
}

void FramePumpWorker::fail(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    qCCritical(dsrApp) << "FramePumpWorker error:" << message;
    emit errorOccurred(message);
    stop();
}
