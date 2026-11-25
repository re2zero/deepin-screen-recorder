// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "matroskamuxer.h"

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace {

constexpr qint64 kClusterMaxDurationMs = 5000;
constexpr int kClusterMaxBytes = 8 * 1024 * 1024;

void appendId(QByteArray *buffer, quint32 id, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i)
        buffer->append(static_cast<char>((id >> (i * 8)) & 0xff));
}

void appendSize(QByteArray *buffer, quint64 size, bool unknown = false)
{
    if (unknown)
        size = (quint64(1) << 56) - 1;
    buffer->append(char(0x01));
    for (int i = 6; i >= 0; --i)
        buffer->append(static_cast<char>((size >> (i * 8)) & 0xff));
}

void appendUintPayload(QByteArray *buffer, quint64 value)
{
    if (value <= 0xff) {
        buffer->append(static_cast<char>(value));
    } else if (value <= 0xffff) {
        buffer->append(static_cast<char>((value >> 8) & 0xff));
        buffer->append(static_cast<char>(value & 0xff));
    } else if (value <= 0xffffffffULL) {
        for (int i = 3; i >= 0; --i)
            buffer->append(static_cast<char>((value >> (i * 8)) & 0xff));
    } else {
        for (int i = 7; i >= 0; --i)
            buffer->append(static_cast<char>((value >> (i * 8)) & 0xff));
    }
}

QByteArray makeElement(quint32 id, int idBytes, const QByteArray &payload)
{
    QByteArray element;
    appendId(&element, id, idBytes);
    appendSize(&element, static_cast<quint64>(payload.size()));
    element.append(payload);
    return element;
}

QByteArray makeUintElement(quint32 id, int idBytes, quint64 value)
{
    QByteArray payload;
    appendUintPayload(&payload, value);
    return makeElement(id, idBytes, payload);
}

QByteArray makeStringElement(quint32 id, int idBytes, const QByteArray &value)
{
    return makeElement(id, idBytes, value);
}

QByteArray makeFloatElement(quint32 id, int idBytes, double value)
{
    quint64 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    QByteArray payload;
    for (int i = 7; i >= 0; --i)
        payload.append(static_cast<char>((bits >> (i * 8)) & 0xff));
    return makeElement(id, idBytes, payload);
}

QByteArray makeBlock(bool video, qint64 relativeMs, bool key, const QByteArray &data)
{
    QByteArray block;
    block.append(static_cast<char>(video ? 0x81 : 0x82)); // track number vint
    const qint16 relative = static_cast<qint16>(relativeMs);
    block.append(static_cast<char>((relative >> 8) & 0xff));
    block.append(static_cast<char>(relative & 0xff));
    block.append(static_cast<char>(key ? 0x80 : 0x00));
    block.append(data);
    return block;
}

QString muxError(const char *what, const QString &path)
{
    return QStringLiteral("%1：%2").arg(QString::fromUtf8(what), path);
}

} // namespace

MatroskaMuxer::MatroskaMuxer() = default;

MatroskaMuxer::~MatroskaMuxer()
{
    abort();
}

bool MatroskaMuxer::open(const QString &path, int width, int height, int fps,
                         const MediaAudioConfig *audio, QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_opened)
        return false;

    if (width <= 0 || height <= 0 || fps <= 0) {
        if (error)
            *error = QStringLiteral("MKV 封装参数非法");
        return false;
    }

    m_path = path;
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_audioEnabled = audio && !audio->codecConfig.isEmpty();
    if (m_audioEnabled)
        m_audio = *audio;

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = muxError("无法创建 MKV 文件", path);
        return false;
    }

    m_avcC.clear();
    m_pendingSamples.clear();
    m_cluster.clear();
    m_clusterTimestampMs = 0;
    m_maxVideoPts = -1;
    m_audioSamplesTotal = 0;
    m_videoSampleCount = 0;
    m_headerWritten = false;
    m_opened = true;
    m_finished = false;
    return true;
}

bool MatroskaMuxer::writeElement(const QByteArray &element, QString *error)
{
    if (m_file.write(element) != element.size()) {
        if (error)
            *error = muxError("写入 MKV 数据失败", m_path);
        return false;
    }
    return true;
}

bool MatroskaMuxer::writeHeader(QString *error)
{
    QByteArray ebml;
    ebml.append(makeUintElement(0x4286, 2, 1)); // EBMLVersion
    ebml.append(makeUintElement(0x42F7, 2, 1)); // EBMLReadVersion
    ebml.append(makeUintElement(0x42F2, 2, 4)); // EBMLMaxIDLength
    ebml.append(makeUintElement(0x42F3, 2, 8)); // EBMLMaxSizeLength
    ebml.append(makeStringElement(0x4282, 2, "matroska")); // DocType
    ebml.append(makeUintElement(0x4287, 2, 4)); // DocTypeVersion
    ebml.append(makeUintElement(0x4285, 2, 2)); // DocTypeReadVersion
    if (!writeElement(makeElement(0x1A45DFA3, 4, ebml), error))
        return false;

    m_segmentStart = m_file.pos();
    const QByteArray segmentId = QByteArray::fromHex("18538067");
    if (m_file.write(segmentId) != segmentId.size()) {
        if (error)
            *error = muxError("写入 MKV Segment 失败", m_path);
        return false;
    }
    m_segmentSizePos = m_file.pos();
    QByteArray unknownSize;
    appendSize(&unknownSize, 0, true);
    if (m_file.write(unknownSize) != unknownSize.size()) {
        if (error)
            *error = muxError("写入 MKV Segment 长度失败", m_path);
        return false;
    }
    m_segmentDataStart = m_file.pos();

    QByteArray info;
    info.append(makeUintElement(0x2AD7B1, 3, 1000000)); // TimestampScale
    info.append(makeStringElement(0x4D80, 2, "deepin-screen-recorder"));
    info.append(makeStringElement(0x5741, 2, "deepin-screen-recorder"));

    // Duration 先写占位值，结束时回写实际时长。
    const qint64 infoFilePos = m_file.pos() + 4 + 8; // Info ID + 8 字节长度
    QByteArray durationPayload(8, '\0');
    m_durationFilePos = infoFilePos + info.size() + 2 + 8;
    QByteArray durationElement;
    appendId(&durationElement, 0x4489, 2);
    appendSize(&durationElement, durationPayload.size());
    durationElement.append(durationPayload);
    info.append(durationElement);

    if (!writeElement(makeElement(0x1549A966, 4, info), error))
        return false;

    QByteArray tracks;
    QByteArray videoTrack;
    videoTrack.append(makeUintElement(0xD7, 1, 1)); // TrackNumber
    videoTrack.append(makeUintElement(0x73C5, 2, 1)); // TrackUID
    videoTrack.append(makeUintElement(0x83, 1, 1)); // TrackType: video
    videoTrack.append(makeUintElement(0x9C, 1, 0)); // FlagLacing
    videoTrack.append(makeStringElement(0x22B59C, 3, "und"));
    videoTrack.append(makeStringElement(0x86, 1, "V_MPEG4/ISO/AVC"));
    videoTrack.append(makeElement(0x63A2, 2, m_avcC)); // CodecPrivate
    videoTrack.append(makeUintElement(0x23E383, 3,
            static_cast<quint64>((1000000000LL + m_fps / 2) / m_fps)));
    QByteArray video;
    video.append(makeUintElement(0xB0, 1, static_cast<quint64>(m_width)));
    video.append(makeUintElement(0xBA, 1, static_cast<quint64>(m_height)));
    videoTrack.append(makeElement(0xE0, 1, video));
    tracks.append(makeElement(0xAE, 1, videoTrack));

    if (m_audioEnabled) {
        QByteArray audioTrack;
        audioTrack.append(makeUintElement(0xD7, 1, 2));
        audioTrack.append(makeUintElement(0x73C5, 2, 2));
        audioTrack.append(makeUintElement(0x83, 1, 2));
        audioTrack.append(makeUintElement(0x9C, 1, 0));
        audioTrack.append(makeStringElement(0x22B59C, 3, "und"));
        audioTrack.append(makeStringElement(0x86, 1, "A_AAC"));
        audioTrack.append(makeElement(0x63A2, 2, m_audio.codecConfig));
        QByteArray audio;
        audio.append(makeFloatElement(0xB5, 1, m_audio.sampleRate));
        audio.append(makeUintElement(0x9F, 1, static_cast<quint64>(m_audio.channels)));
        audio.append(makeUintElement(0x6264, 2, 16)); // BitDepth
        audioTrack.append(makeElement(0xE1, 1, audio));
        tracks.append(makeElement(0xAE, 1, audioTrack));
    }

    if (!writeElement(makeElement(0x1654AE6B, 4, tracks), error))
        return false;

    m_headerWritten = true;

    const std::vector<PendingSample> pending = std::move(m_pendingSamples);
    m_pendingSamples.clear();
    for (const PendingSample &sample : pending) {
        if (!addSample(sample.video, sample.timestampMs, sample.key,
                       sample.data, error))
            return false;
    }
    return true;
}

void MatroskaMuxer::setVideoCodecConfig(const QByteArray &avcC)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_avcC.isEmpty())
        m_avcC = avcC;
}

void MatroskaMuxer::startCluster(qint64 timestampMs)
{
    m_clusterTimestampMs = timestampMs;
    m_cluster.clear();
    m_cluster.append(makeUintElement(0xE7, 1, static_cast<quint64>(timestampMs)));
}

bool MatroskaMuxer::writeCluster(QString *error)
{
    if (m_cluster.isEmpty())
        return true;
    const bool ok = writeElement(makeElement(0x1F43B675, 4, m_cluster), error);
    m_cluster.clear();
    return ok;
}

bool MatroskaMuxer::addSample(bool video, qint64 timestampMs, bool key,
                              const QByteArray &data, QString *error)
{
    if (!m_headerWritten) {
        PendingSample sample;
        sample.video = video;
        sample.timestampMs = timestampMs;
        sample.key = key;
        sample.data = data;
        m_pendingSamples.push_back(std::move(sample));
        return true;
    }

    if (m_cluster.isEmpty()) {
        startCluster(timestampMs);
    } else {
        const qint64 duration = timestampMs - m_clusterTimestampMs;
        if ((video && key) || duration >= kClusterMaxDurationMs
                || m_cluster.size() >= kClusterMaxBytes
                || duration > 30000 || duration < -30000) {
            if (!writeCluster(error))
                return false;
            startCluster(timestampMs);
        }
    }

    qint64 relative = timestampMs - m_clusterTimestampMs;
    if (relative < -32768 || relative > 32767) {
        if (!writeCluster(error))
            return false;
        startCluster(timestampMs);
        relative = 0;
    }

    m_cluster.append(makeElement(0xA3, 1, makeBlock(video, relative, key, data)));
    return true;
}

bool MatroskaMuxer::addVideoSample(const uint8_t *data, int size, qint64 pts, qint64 dts,
                                   bool key, QString *error)
{
    Q_UNUSED(dts)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished || !data || size <= 0 || m_avcC.isEmpty())
        return false;

    const qint64 timestampMs = (pts * 1000 + m_fps / 2) / m_fps;
    const QByteArray sample(reinterpret_cast<const char *>(data), size);
    m_maxVideoPts = std::max(m_maxVideoPts, pts);
    ++m_videoSampleCount;

    // 第一个视频样本作为关键帧，必须先建立 Tracks。
    if (!m_headerWritten) {
        if (!writeHeader(error))
            return false;
    }
    return addSample(true, timestampMs, key, sample, error);
}

bool MatroskaMuxer::addAudioSample(const uint8_t *data, int size, qint64 ptsSamples,
                                   qint64 durationSamples, QString *error)
{
    Q_UNUSED(durationSamples)
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished || !m_audioEnabled || !data || size <= 0)
        return false;

    const qint64 timestampMs =
            (ptsSamples * 1000 + m_audio.sampleRate / 2) / m_audio.sampleRate;
    QByteArray sample(reinterpret_cast<const char *>(data), size);
    m_audioSamplesTotal = std::max(m_audioSamplesTotal, ptsSamples + durationSamples);
    return addSample(false, timestampMs, true, sample, error);
}

bool MatroskaMuxer::finish(QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished)
        return false;
    m_finished = true;

    if (!m_headerWritten || m_videoSampleCount == 0) {
        if (error)
            *error = QStringLiteral("没有可写入 MKV 的视频帧");
        m_file.close();
        m_file.remove();
        m_opened = false;
        return false;
    }

    if (!writeCluster(error)) {
        m_file.close();
        return false;
    }

    double durationMs = static_cast<double>(m_maxVideoPts + 1) * 1000.0 / m_fps;
    if (m_audioEnabled) {
        const double audioMs = static_cast<double>(m_audioSamplesTotal) * 1000.0
                / m_audio.sampleRate;
        durationMs = std::max(durationMs, audioMs);
    }

    m_file.flush();
    const qint64 fileEnd = m_file.size();

    if (!m_file.seek(m_durationFilePos)) {
        if (error)
            *error = muxError("无法回写 MKV Duration", m_path);
        m_file.close();
        return false;
    }
    quint64 durationBits = 0;
    std::memcpy(&durationBits, &durationMs, sizeof(durationBits));
    QByteArray durationBytes;
    for (int i = 7; i >= 0; --i)
        durationBytes.append(static_cast<char>((durationBits >> (i * 8)) & 0xff));
    if (m_file.write(durationBytes) != durationBytes.size()) {
        if (error)
            *error = muxError("写入 MKV Duration 失败", m_path);
        m_file.close();
        return false;
    }

    if (!m_file.seek(m_segmentSizePos)) {
        if (error)
            *error = muxError("无法回写 MKV Segment 长度", m_path);
        m_file.close();
        return false;
    }
    QByteArray segmentSize;
    appendSize(&segmentSize, static_cast<quint64>(fileEnd - m_segmentDataStart));
    if (m_file.write(segmentSize) != segmentSize.size()) {
        if (error)
            *error = muxError("写入 MKV Segment 长度失败", m_path);
        m_file.close();
        return false;
    }

    if (!m_file.seek(fileEnd)) {
        if (error)
            *error = muxError("无法恢复 MKV 写入位置", m_path);
        m_file.close();
        return false;
    }
    m_file.flush();
    m_file.close();
    m_opened = false;
    return true;
}

void MatroskaMuxer::abort()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.isOpen())
        m_file.close();
    m_opened = false;
    m_finished = true;
    m_cluster.clear();
    m_pendingSamples.clear();
}

qint64 MatroskaMuxer::bytesWritten() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file.size();
}

int MatroskaMuxer::videoSampleCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_videoSampleCount;
}
