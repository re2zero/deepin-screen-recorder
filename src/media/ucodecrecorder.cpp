// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ucodecrecorder.h"

#include "aacencoder.h"
#include "h264annexb.h"
#include "matroskamuxer.h"
#include "mp4muxer.h"
#include "pulseaudiocapture.h"

#include "../utils/log.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace {

constexpr int kAudioSampleRate = 48000;
constexpr int kAudioChannels = 2;
constexpr int kAudioBitrate = 128000;
constexpr int kAacFrameSamples = 1024;

} // namespace

UcodecRecorder::UcodecRecorder() = default;

UcodecRecorder::~UcodecRecorder()
{
    if (m_started)
        abort();
    else
        releaseResources();
}

bool UcodecRecorder::start(const QString &outputPath, int width, int height, int fps,
                           qint64 bitrate, const AudioCaptureOptions &audio,
                           QString *error)
{
    if (m_started)
        return false;

    releaseResources();
    m_outputPath = outputPath;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_encodedFrames = 0;
    m_packetCount = 0;
    m_audioSampleCount = 0;
    m_bytesWritten = 0;
    m_avcC.clear();
    m_sps.clear();
    m_pps.clear();
    m_videoConfigReady = false;
    m_muxerReady = false;
    m_muxFailed = false;
    m_audioEnabled = false;
    m_audioError.clear();

    if (outputPath.isEmpty() || width <= 0 || height <= 0 || fps <= 0) {
        if (error)
            *error = QStringLiteral("非法的录制参数");
        return false;
    }

    UcodecVideoEncoder::Config encoderConfig;
    encoderConfig.codec = QStringLiteral("h264");
    encoderConfig.width = width;
    encoderConfig.height = height;
    encoderConfig.fps = fps;
    encoderConfig.bitrate = bitrate;
    encoderConfig.preferHw = true;
    encoderConfig.lowLatency = false;
    m_encoderConfig = encoderConfig;

    if (!m_encoder.open(encoderConfig,
                        [this](const uint8_t *data, int size, qint64 pts,
                               qint64 dts, bool key) {
                            onVideoPacket(data, size, pts, dts, key);
                        },
                        error)) {
        return false;
    }

    setupAudio(audio, nullptr);

    const bool matroska = outputPath.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive);
    if (matroska)
        m_muxer = std::make_unique<MatroskaMuxer>();
    else
        m_muxer = std::make_unique<Mp4Muxer>();

    MediaAudioConfig audioConfig;
    const MediaAudioConfig *audioPtr = nullptr;
    if (m_audioEnabled.load() && m_aac) {
        audioConfig.codecConfig = m_aac->config();
        audioConfig.sampleRate = kAudioSampleRate;
        audioConfig.channels = kAudioChannels;
        audioConfig.bitrate = kAudioBitrate;
        if (!audioConfig.codecConfig.isEmpty())
            audioPtr = &audioConfig;
    }

    if (!m_muxer->open(outputPath, m_encoder.width(), m_encoder.height(), fps,
                       audioPtr, error)) {
        m_muxer.reset();
        releaseResources();
        return false;
    }
    m_muxerReady = true;

    m_started = true;
    qCInfo(dsrApp) << "UcodecRecorder started:"
                   << outputPath << m_encoder.width() << "x" << m_encoder.height()
                   << "@" << fps
                   << "encoder:" << m_encoder.pathName()
                   << "container:" << (matroska ? "matroska" : "mp4")
                   << "audio:" << (audioPtr ? "aac" : "none");
    return true;
}

bool UcodecRecorder::setupAudio(const AudioCaptureOptions &audio, QString *error)
{
    Q_UNUSED(error)
    if (!audio.hasAudio())
        return false;

    QString aacError;
    auto aac = std::make_unique<AacEncoder>();
    if (!aac->open(kAudioSampleRate, kAudioChannels, kAudioBitrate, &aacError)) {
        qCWarning(dsrApp) << "UcodecRecorder: AAC encoder unavailable:" << aacError;
        return false;
    }

    QString captureError;
    auto capture = std::make_unique<PulseAudioCapture>();
    if (!capture->open(audio, kAudioSampleRate, kAudioChannels, &captureError)) {
        qCWarning(dsrApp) << "UcodecRecorder: audio capture unavailable:" << captureError;
        aac->close();
        return false;
    }

    QString startError;
    if (!capture->start([this](const int16_t *pcm, int frames) {
            return onAudioPcm(pcm, frames);
        }, &startError)) {
        qCWarning(dsrApp) << "UcodecRecorder: audio capture start failed:" << startError;
        aac->close();
        return false;
    }

    m_aac = std::move(aac);
    m_capture = std::move(capture);
    m_audioEnabled = true;
    return true;
}

void UcodecRecorder::onVideoPacket(const uint8_t *data, int size, qint64 pts,
                                   qint64 dts, bool key)
{
    if (!m_started || !m_muxer || !data || size <= 0)
        return;

    if (!m_videoConfigReady.load()) {
        if (!H264AnnexB::extractParameterSets(data, size, &m_sps, &m_pps)) {
            qCWarning(dsrApp) << "UcodecRecorder: key frame without SPS/PPS";
            return;
        }
        m_avcC = H264AnnexB::makeAvcC(m_sps, m_pps);
        if (m_avcC.isEmpty()) {
            qCWarning(dsrApp) << "UcodecRecorder: failed to build avcC";
            return;
        }
        m_muxer->setVideoCodecConfig(m_avcC);
        m_videoConfigReady = true;
    }

    const QByteArray avcc = H264AnnexB::toAvcc(data, size);
    if (avcc.isEmpty())
        return;

    QString error;
    if (!m_muxer->addVideoSample(reinterpret_cast<const uint8_t *>(avcc.constData()),
                                 avcc.size(), pts, dts, key, &error)) {
        qCWarning(dsrApp) << "UcodecRecorder: add video sample failed:" << error;
        m_muxFailed = true;
        return;
    }
    ++m_packetCount;
}

bool UcodecRecorder::onAudioPcm(const int16_t *pcm, int frames)
{
    if (!m_muxerReady.load())
        return true;
    if (!m_aac || !m_muxer)
        return false;

    QList<QByteArray> packets;
    QString error;
    if (!m_aac->encode(pcm, frames, &packets, &error)) {
        m_audioError = error;
        qCWarning(dsrApp) << "UcodecRecorder: AAC encode failed:" << error;
        return false;
    }

    for (const QByteArray &packet : packets) {
        if (packet.isEmpty())
            continue;
        const qint64 pts = m_audioSampleCount.load();
        if (!m_muxer->addAudioSample(
                    reinterpret_cast<const uint8_t *>(packet.constData()),
                    packet.size(), pts, kAacFrameSamples, &error)) {
            m_audioError = error;
            qCWarning(dsrApp) << "UcodecRecorder: add audio sample failed:" << error;
            return false;
        }
        m_audioSampleCount.fetch_add(kAacFrameSamples);
    }
    return true;
}

bool UcodecRecorder::pushFrame(const uint8_t *rgba, int stride, const QRect &crop,
                               qint64 pts, QString *error)
{
    if (!m_started) {
        if (error)
            *error = QStringLiteral("录制器尚未启动");
        return false;
    }
    if (m_muxFailed.load()) {
        if (error)
            *error = QStringLiteral("视频封装失败");
        return false;
    }
    if (!m_encoder.encodeFrame(rgba, stride, crop, pts,
                               UcodecVideoEncoder::RGBA8888, error)) {
        return false;
    }
    ++m_encodedFrames;
    return true;
}

bool UcodecRecorder::finish(QString *error)
{
    if (!m_started)
        return true;

    QString flushError;
    if (!m_encoder.flush(&flushError)) {
        qCWarning(dsrApp) << "UcodecRecorder: encoder flush failed:" << flushError;
        if (error && error->isEmpty())
            *error = flushError;
    }
    m_encoder.close();

    if (m_capture) {
        m_capture->stop();
        const QString captureError = m_capture->lastError();
        if (!captureError.isEmpty()) {
            m_audioError = captureError;
            qCWarning(dsrApp) << "UcodecRecorder: audio capture error:" << captureError;
        }
    }

    if (m_aac && m_muxer && m_muxerReady.load()) {
        QList<QByteArray> packets;
        QString aacError;
        if (m_aac->flush(&packets, &aacError)) {
            for (const QByteArray &packet : packets) {
                if (packet.isEmpty())
                    continue;
                QString addError;
                if (!m_muxer->addAudioSample(
                            reinterpret_cast<const uint8_t *>(packet.constData()),
                            packet.size(), m_audioSampleCount.load(),
                            kAacFrameSamples, &addError)) {
                    m_audioError = addError;
                    qCWarning(dsrApp) << "UcodecRecorder: flush audio sample failed:"
                                      << addError;
                    break;
                }
                m_audioSampleCount.fetch_add(kAacFrameSamples);
            }
        } else {
            m_audioError = aacError;
            qCWarning(dsrApp) << "UcodecRecorder: AAC flush failed:" << aacError;
        }
    }

    bool ok = true;
    QString muxError;
    if (m_muxer && m_muxerReady.load()) {
        ok = m_muxer->finish(&muxError);
        if (!ok)
            qCWarning(dsrApp) << "UcodecRecorder: muxer finish failed:" << muxError;
    } else {
        ok = false;
        muxError = QStringLiteral("封装器尚未启动");
    }
    if (m_muxFailed.load()) {
        ok = false;
        if (muxError.isEmpty())
            muxError = QStringLiteral("视频封装失败");
    }
    if (!ok && error)
        *error = muxError.isEmpty() ? QStringLiteral("录制收尾失败") : muxError;

    m_bytesWritten = QFileInfo(m_outputPath).size();
    const int frames = m_encodedFrames;
    const QString path = m_outputPath;
    m_started = false;
    m_muxerReady = false;
    releaseResources();
    m_outputPath.clear();

    if (ok) {
        qCInfo(dsrApp) << "UcodecRecorder finished:" << path
                       << "frames:" << frames << "bytes:" << m_bytesWritten
                       << "audioSamples:" << m_audioSampleCount.load();
    }
    return ok;
}

void UcodecRecorder::abort()
{
    m_started = false;
    m_muxerReady = false;
    const QString path = m_outputPath;
    releaseResources();
    m_outputPath.clear();
    if (!path.isEmpty())
        QFile::remove(path);
}

void UcodecRecorder::releaseResources()
{
    if (m_capture) {
        m_capture->stop();
        m_capture.reset();
    }
    if (m_aac) {
        m_aac->close();
        m_aac.reset();
    }
    if (m_muxer) {
        m_muxer->abort();
        m_muxer.reset();
    }
    m_encoder.close();
    m_audioEnabled = false;
    m_videoConfigReady = false;
    m_muxerReady = false;
}

QString UcodecRecorder::encoderPath() const
{
    return m_encoder.pathName();
}
