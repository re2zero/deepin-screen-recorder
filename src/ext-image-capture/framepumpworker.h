#ifndef FRAMEPUMPWORKER_H
#define FRAMEPUMPWORKER_H

#include "../media/mediaframesink.h"

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QString>

#include <memory>

class QTimer;

/**
 * @brief 固定帧率取帧 + ucodec 编码落盘。
 *
 * 采集线程只更新“最新一帧”，本 worker 按目标帧率取样（重复/丢弃），
 * 用实际经过时间对应的时隙作为 PTS 交给录制器，保证 CFR 与音频时间线对齐。
 */
class FramePumpWorker : public QObject
{
    Q_OBJECT

public:
    FramePumpWorker(int frameRate,
                    const QString &outputPath,
                    const QRect &cropRect,
                    LatestFrameStore *store,
                    const AudioCaptureOptions &audio = AudioCaptureOptions(),
                    QObject *parent = nullptr);
    ~FramePumpWorker() override;

    int outputFrames() const { return m_outputFrames; }
    int repeatedFrames() const { return m_repeatedFrames; }

public slots:
    void start(int width, int height, int stride);
    void stop();

signals:
    void started();
    void frameWritten(int totalFrames);
    void statsUpdated(int outputFrames, int repeatedFrames, qint64 behindMs);
    void errorOccurred(const QString &message);
    void finished();

private slots:
    void onTick();

private:
    bool writeCurrentFrame();
    void fail(const QString &message);

    int m_frameRate;
    QString m_outputPath;
    QRect m_cropRect;
    LatestFrameStore *m_store;
    AudioCaptureOptions m_audio;
    std::unique_ptr<MediaFrameSink> m_recorder;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    qint64 m_intervalNs = 0;
    qint64 m_nextDeadlineNs = 0;
    int m_width = 0;
    int m_height = 0;
    int m_stride = 0;
    int m_outputFrames = 0;
    int m_repeatedFrames = 0;
    int m_skippedSlots = 0;
    quint64 m_lastFrameSequence = 0;
    qint64 m_lastStatsNs = 0;
    qint64 m_lastPts = -1;
    bool m_started = false;
    bool m_stopping = false;
    bool m_failed = false;
};

#endif // FRAMEPUMPWORKER_H
