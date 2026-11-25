// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mp4muxer.h"

#include <QDebug>

#include <algorithm>
#include <cstring>

namespace {

constexpr quint32 kMovieTimescale = 1000;

void appendU8(QByteArray *buffer, quint8 value)
{
    buffer->append(static_cast<char>(value));
}

void appendU16(QByteArray *buffer, quint16 value)
{
    buffer->append(static_cast<char>((value >> 8) & 0xff));
    buffer->append(static_cast<char>(value & 0xff));
}

void appendU24(QByteArray *buffer, quint32 value)
{
    buffer->append(static_cast<char>((value >> 16) & 0xff));
    buffer->append(static_cast<char>((value >> 8) & 0xff));
    buffer->append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray *buffer, quint32 value)
{
    buffer->append(static_cast<char>((value >> 24) & 0xff));
    buffer->append(static_cast<char>((value >> 16) & 0xff));
    buffer->append(static_cast<char>((value >> 8) & 0xff));
    buffer->append(static_cast<char>(value & 0xff));
}

void appendU64(QByteArray *buffer, quint64 value)
{
    appendU32(buffer, static_cast<quint32>(value >> 32));
    appendU32(buffer, static_cast<quint32>(value & 0xffffffff));
}

int beginBox(QByteArray *buffer, const char *type)
{
    const int offset = buffer->size();
    appendU32(buffer, 0);
    buffer->append(type, 4);
    return offset;
}

void endBox(QByteArray *buffer, int offset)
{
    const quint32 size = static_cast<quint32>(buffer->size() - offset);
    (*buffer)[offset] = static_cast<char>((size >> 24) & 0xff);
    (*buffer)[offset + 1] = static_cast<char>((size >> 16) & 0xff);
    (*buffer)[offset + 2] = static_cast<char>((size >> 8) & 0xff);
    (*buffer)[offset + 3] = static_cast<char>(size & 0xff);
}

int beginFullBox(QByteArray *buffer, const char *type, quint8 version, quint32 flags)
{
    const int offset = beginBox(buffer, type);
    appendU8(buffer, version);
    appendU24(buffer, flags);
    return offset;
}

void appendMatrix(QByteArray *buffer)
{
    appendU32(buffer, 0x00010000);
    appendU32(buffer, 0);
    appendU32(buffer, 0);
    appendU32(buffer, 0);
    appendU32(buffer, 0x00010000);
    appendU32(buffer, 0);
    appendU32(buffer, 0);
    appendU32(buffer, 0);
    appendU32(buffer, 0x40000000);
}

void appendDescriptorLength(QByteArray *buffer, int length)
{
    int parts[4] = {0, 0, 0, 0};
    int count = 0;
    do {
        parts[count++] = length & 0x7f;
        length >>= 7;
    } while (length > 0 && count < 4);
    for (int i = count - 1; i >= 0; --i)
        appendU8(buffer, static_cast<quint8>(parts[i] | (i > 0 ? 0x80 : 0)));
}

void appendDescriptor(QByteArray *buffer, quint8 tag, const QByteArray &payload)
{
    appendU8(buffer, tag);
    appendDescriptorLength(buffer, payload.size());
    buffer->append(payload);
}

QByteArray makeDescriptor(quint8 tag, const QByteArray &payload)
{
    QByteArray descriptor;
    appendDescriptor(&descriptor, tag, payload);
    return descriptor;
}

QByteArray makeRunLengthDurations(const std::vector<Mp4Muxer::Sample> &samples, bool useCts)
{
    QByteArray table;
    quint32 previous = 0;
    quint32 runCount = 0;
    bool first = true;
    for (const auto &sample : samples) {
        const quint32 value = useCts ? static_cast<quint32>(sample.cts) : sample.duration;
        if (first) {
            previous = value;
            runCount = 1;
            first = false;
        } else if (value == previous) {
            ++runCount;
        } else {
            appendU32(&table, runCount);
            appendU32(&table, previous);
            previous = value;
            runCount = 1;
        }
    }
    if (!first) {
        appendU32(&table, runCount);
        appendU32(&table, previous);
    }
    return table;
}

QString fourccError(const char *what, const QString &path)
{
    return QStringLiteral("%1：%2").arg(QString::fromUtf8(what), path);
}

} // namespace

Mp4Muxer::Mp4Muxer() = default;

Mp4Muxer::~Mp4Muxer()
{
    abort();
}

bool Mp4Muxer::open(const QString &path, int width, int height, int fps,
                    const MediaAudioConfig *audio, QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_opened)
        return false;

    if (width <= 0 || height <= 0 || fps <= 0) {
        if (error)
            *error = QStringLiteral("MP4 封装参数非法");
        return false;
    }

    m_path = path;
    m_width = width;
    m_height = height;
    m_fps = fps;
    if (audio && !audio->codecConfig.isEmpty()) {
        m_audioEnabled = true;
        m_audio = *audio;
    }

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = fourccError("无法创建 MP4 文件", path);
        return false;
    }

    QByteArray ftyp;
    const int ftypBox = beginBox(&ftyp, "ftyp");
    ftyp.append("isom", 4);
    appendU32(&ftyp, 0x00000200);
    ftyp.append("isom", 4);
    ftyp.append("iso2", 4);
    ftyp.append("avc1", 4);
    ftyp.append("mp41", 4);
    endBox(&ftyp, ftypBox);
    if (m_file.write(ftyp) != ftyp.size()) {
        if (error)
            *error = fourccError("写入 MP4 文件头失败", path);
        m_file.close();
        m_file.remove();
        return false;
    }

    // mdat 使用 64 位长度，录制超过 4GB 时无需回移样本偏移。
    m_mdatStart = m_file.pos();
    QByteArray mdatHeader;
    appendU32(&mdatHeader, 1);
    mdatHeader.append("mdat", 4);
    m_mdatSizePos = m_mdatStart + 8;
    appendU64(&mdatHeader, 0);
    if (m_file.write(mdatHeader) != mdatHeader.size()) {
        if (error)
            *error = fourccError("写入 MP4 mdat 头失败", path);
        m_file.close();
        m_file.remove();
        return false;
    }

    m_videoSamples.clear();
    m_audioSamples.clear();
    m_maxPts = -1;
    m_dtsOffset = 0;
    m_lastDts = -1;
    m_audioSamplesTotal = 0;
    m_hasCts = false;
    m_avcC.clear();
    m_opened = true;
    m_finished = false;
    m_audioEnabled = audio && !audio->codecConfig.isEmpty();
    return true;
}

void Mp4Muxer::setVideoCodecConfig(const QByteArray &avcC)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_avcC = avcC;
}

bool Mp4Muxer::writeSample(Sample *sample, const uint8_t *data, int size, QString *error)
{
    const qint64 offset = m_file.pos();
    if (m_file.write(reinterpret_cast<const char *>(data), size) != size) {
        if (error)
            *error = fourccError("写入 MP4 媒体数据失败", m_path);
        return false;
    }
    sample->offset = static_cast<quint64>(offset);
    sample->size = static_cast<quint32>(size);
    return true;
}

bool Mp4Muxer::addVideoSample(const uint8_t *data, int size, qint64 pts, qint64 dts,
                              bool key, QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished || !data || size <= 0 || m_avcC.isEmpty())
        return false;

    if (m_videoSamples.empty()) {
        m_dtsOffset = dts < 0 ? -dts : 0;
        m_lastDts = dts + m_dtsOffset;
    }

    const qint64 dtsAdjusted = dts + m_dtsOffset;
    if (dtsAdjusted < m_lastDts) {
        if (error)
            *error = QStringLiteral("视频 dts 非单调");
        return false;
    }
    if (!m_videoSamples.empty()) {
        qint64 delta = dtsAdjusted - m_lastDts;
        if (delta <= 0)
            delta = 1;
        m_videoSamples.back().duration = static_cast<quint32>(delta);
    }
    m_lastDts = dtsAdjusted;

    qint64 cts = pts - dtsAdjusted;
    if (cts < 0)
        cts = 0;
    if (cts != 0)
        m_hasCts = true;

    Sample sample;
    sample.duration = 1;
    sample.cts = static_cast<qint32>(cts);
    sample.key = key;
    if (!writeSample(&sample, data, size, error))
        return false;

    m_videoSamples.push_back(sample);
    m_maxPts = std::max(m_maxPts, pts);
    return true;
}

bool Mp4Muxer::addAudioSample(const uint8_t *data, int size, qint64 ptsSamples,
                              qint64 durationSamples, QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished || !m_audioEnabled || !data || size <= 0)
        return false;

    Sample sample;
    sample.duration = static_cast<quint32>(std::max<qint64>(1, durationSamples));
    sample.key = true;
    if (!writeSample(&sample, data, size, error))
        return false;

    m_audioSamples.push_back(sample);
    m_audioSamplesTotal = std::max(m_audioSamplesTotal, ptsSamples + durationSamples);
    return true;
}

QByteArray Mp4Muxer::buildStbl(const std::vector<Sample> &samples, bool video) const
{
    QByteArray stbl;
    const int stblBox = beginBox(&stbl, "stbl");

    QByteArray stsd;
    const int stsdBox = beginFullBox(&stsd, "stsd", 0, 0);
    appendU32(&stsd, 1);
    stsd.append(video ? buildAvc1() : buildMp4a());
    endBox(&stsd, stsdBox);
    stbl.append(stsd);

    QByteArray stts = makeRunLengthDurations(samples, false);
    const int sttsBox = beginFullBox(&stbl, "stts", 0, 0);
    appendU32(&stbl, static_cast<quint32>(stts.size() / 8));
    stbl.append(stts);
    endBox(&stbl, sttsBox);

    if (video && m_hasCts) {
        QByteArray ctts = makeRunLengthDurations(samples, true);
        const int cttsBox = beginFullBox(&stbl, "ctts", 0, 0);
        appendU32(&stbl, static_cast<quint32>(ctts.size() / 8));
        stbl.append(ctts);
        endBox(&stbl, cttsBox);
    }

    if (video) {
        QByteArray keys;
        quint32 keyCount = 0;
        for (quint32 i = 0; i < samples.size(); ++i) {
            if (samples[i].key) {
                appendU32(&keys, i + 1);
                ++keyCount;
            }
        }
        if (keyCount > 0) {
            const int stssBox = beginFullBox(&stbl, "stss", 0, 0);
            appendU32(&stbl, keyCount);
            stbl.append(keys);
            endBox(&stbl, stssBox);
        }
    }

    const int stscBox = beginFullBox(&stbl, "stsc", 0, 0);
    appendU32(&stbl, samples.empty() ? 0 : 1);
    if (!samples.empty()) {
        appendU32(&stbl, 1); // first_chunk
        appendU32(&stbl, 1); // samples_per_chunk
        appendU32(&stbl, 1); // sample_description_index
    }
    endBox(&stbl, stscBox);

    const int stszBox = beginFullBox(&stbl, "stsz", 0, 0);
    appendU32(&stbl, 0);
    appendU32(&stbl, static_cast<quint32>(samples.size()));
    for (const auto &sample : samples)
        appendU32(&stbl, sample.size);
    endBox(&stbl, stszBox);

    const int co64Box = beginFullBox(&stbl, "co64", 0, 0);
    appendU32(&stbl, static_cast<quint32>(samples.size()));
    for (const auto &sample : samples)
        appendU64(&stbl, sample.offset);
    endBox(&stbl, co64Box);

    endBox(&stbl, stblBox);
    return stbl;
}

QByteArray Mp4Muxer::buildAvc1() const
{
    QByteArray avc1;
    const int avc1Box = beginBox(&avc1, "avc1");
    for (int i = 0; i < 6; ++i)
        appendU8(&avc1, 0);
    appendU16(&avc1, 1); // data_reference_index
    appendU16(&avc1, 0);
    appendU16(&avc1, 0);
    appendU32(&avc1, 0);
    appendU32(&avc1, 0);
    appendU32(&avc1, 0);
    appendU16(&avc1, static_cast<quint16>(m_width));
    appendU16(&avc1, static_cast<quint16>(m_height));
    appendU32(&avc1, 0x00480000);
    appendU32(&avc1, 0x00480000);
    appendU32(&avc1, 0);
    appendU16(&avc1, 1); // frame_count
    QByteArray compressor(32, '\0');
    const QByteArray name("ucodec");
    compressor[0] = static_cast<char>(name.size());
    std::memcpy(compressor.data() + 1, name.constData(), name.size());
    avc1.append(compressor);
    appendU16(&avc1, 0x0018); // depth
    appendU16(&avc1, 0xffff); // pre_defined = -1

    const int avcCBox = beginBox(&avc1, "avcC");
    avc1.append(m_avcC);
    endBox(&avc1, avcCBox);

    endBox(&avc1, avc1Box);
    return avc1;
}

QByteArray Mp4Muxer::buildEsds() const
{
    QByteArray dsi;
    appendDescriptor(&dsi, 0x05, m_audio.codecConfig);

    QByteArray decoderConfig;
    appendU8(&decoderConfig, 0x40); // Audio ISO/IEC 14496-3
    appendU8(&decoderConfig, 0x15); // audio stream
    appendU24(&decoderConfig, 0);
    appendU32(&decoderConfig, static_cast<quint32>(m_audio.bitrate));
    appendU32(&decoderConfig, static_cast<quint32>(m_audio.bitrate));
    decoderConfig.append(dsi);

    QByteArray slConfig;
    appendU8(&slConfig, 0x02);
    const QByteArray sl = makeDescriptor(0x06, slConfig);

    QByteArray esPayload;
    appendU16(&esPayload, 0);
    appendU8(&esPayload, 0);
    esPayload.append(makeDescriptor(0x04, decoderConfig));
    esPayload.append(sl);
    const QByteArray es = makeDescriptor(0x03, esPayload);

    QByteArray esds;
    const int esdsBox = beginFullBox(&esds, "esds", 0, 0);
    esds.append(es);
    endBox(&esds, esdsBox);
    return esds;
}

QByteArray Mp4Muxer::buildMp4a() const
{
    QByteArray mp4a;
    const int mp4aBox = beginBox(&mp4a, "mp4a");
    for (int i = 0; i < 6; ++i)
        appendU8(&mp4a, 0);
    appendU16(&mp4a, 1); // data_reference_index
    appendU32(&mp4a, 0);
    appendU32(&mp4a, 0);
    appendU16(&mp4a, static_cast<quint16>(m_audio.channels));
    appendU16(&mp4a, 16); // sample size
    appendU16(&mp4a, 0);
    appendU16(&mp4a, 0);
    appendU32(&mp4a, static_cast<quint32>(m_audio.sampleRate) << 16);
    mp4a.append(buildEsds());
    endBox(&mp4a, mp4aBox);
    return mp4a;
}

QByteArray Mp4Muxer::buildVideoTrak() const
{
    const quint32 videoDuration = static_cast<quint32>(m_maxPts + 1);
    const quint32 videoDurationMovie = static_cast<quint32>(
            (static_cast<quint64>(videoDuration) * kMovieTimescale + m_fps / 2) / m_fps);
    const quint32 mediaDuration = static_cast<quint32>(m_lastDts + 1);

    QByteArray trak;
    const int trakBox = beginBox(&trak, "trak");

    QByteArray tkhd;
    const int tkhdBox = beginFullBox(&tkhd, "tkhd", 0, 0x000007);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 1); // track_ID
    appendU32(&tkhd, 0);
    appendU32(&tkhd, videoDurationMovie);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 0);
    appendU16(&tkhd, 0); // layer
    appendU16(&tkhd, 0); // alternate_group
    appendU16(&tkhd, 0); // volume
    appendU16(&tkhd, 0);
    appendMatrix(&tkhd);
    appendU32(&tkhd, static_cast<quint32>(m_width) << 16);
    appendU32(&tkhd, static_cast<quint32>(m_height) << 16);
    endBox(&tkhd, tkhdBox);
    trak.append(tkhd);

    QByteArray mdia;
    const int mdiaBox = beginBox(&mdia, "mdia");

    QByteArray mdhd;
    const int mdhdBox = beginFullBox(&mdhd, "mdhd", 0, 0);
    appendU32(&mdhd, 0);
    appendU32(&mdhd, 0);
    appendU32(&mdhd, static_cast<quint32>(m_fps));
    appendU32(&mdhd, mediaDuration);
    appendU16(&mdhd, 0x55c4); // und
    appendU16(&mdhd, 0);
    endBox(&mdhd, mdhdBox);
    mdia.append(mdhd);

    QByteArray hdlr;
    const int hdlrBox = beginFullBox(&hdlr, "hdlr", 0, 0);
    appendU32(&hdlr, 0);
    hdlr.append("vide", 4);
    appendU32(&hdlr, 0);
    appendU32(&hdlr, 0);
    appendU32(&hdlr, 0);
    hdlr.append("VideoHandler", 12);
    appendU8(&hdlr, 0);
    endBox(&hdlr, hdlrBox);
    mdia.append(hdlr);

    QByteArray minf;
    const int minfBox = beginBox(&minf, "minf");

    QByteArray vmhd;
    const int vmhdBox = beginFullBox(&vmhd, "vmhd", 0, 1);
    appendU16(&vmhd, 0);
    appendU16(&vmhd, 0);
    appendU16(&vmhd, 0);
    appendU16(&vmhd, 0);
    endBox(&vmhd, vmhdBox);
    minf.append(vmhd);

    QByteArray dinf;
    const int dinfBox = beginBox(&dinf, "dinf");
    QByteArray dref;
    const int drefBox = beginFullBox(&dref, "dref", 0, 0);
    appendU32(&dref, 1);
    const int urlBox = beginFullBox(&dref, "url ", 0, 1);
    endBox(&dref, urlBox);
    endBox(&dref, drefBox);
    dinf.append(dref);
    endBox(&dinf, dinfBox);
    minf.append(dinf);

    minf.append(buildStbl(m_videoSamples, true));
    endBox(&minf, minfBox);
    mdia.append(minf);
    endBox(&mdia, mdiaBox);
    trak.append(mdia);

    endBox(&trak, trakBox);
    return trak;
}

QByteArray Mp4Muxer::buildAudioTrak() const
{
    const quint32 audioDurationMovie = static_cast<quint32>(
            (static_cast<quint64>(m_audioSamplesTotal) * kMovieTimescale
             + m_audio.sampleRate / 2) / m_audio.sampleRate);

    QByteArray trak;
    const int trakBox = beginBox(&trak, "trak");

    QByteArray tkhd;
    const int tkhdBox = beginFullBox(&tkhd, "tkhd", 0, 0x000007);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 2);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, audioDurationMovie);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 0);
    appendU16(&tkhd, 0);
    appendU16(&tkhd, 0);
    appendU16(&tkhd, 0x0100); // volume
    appendU16(&tkhd, 0);
    appendMatrix(&tkhd);
    appendU32(&tkhd, 0);
    appendU32(&tkhd, 0);
    endBox(&tkhd, tkhdBox);
    trak.append(tkhd);

    QByteArray mdia;
    const int mdiaBox = beginBox(&mdia, "mdia");

    QByteArray mdhd;
    const int mdhdBox = beginFullBox(&mdhd, "mdhd", 0, 0);
    appendU32(&mdhd, 0);
    appendU32(&mdhd, 0);
    appendU32(&mdhd, static_cast<quint32>(m_audio.sampleRate));
    appendU32(&mdhd, static_cast<quint32>(m_audioSamplesTotal));
    appendU16(&mdhd, 0x55c4);
    appendU16(&mdhd, 0);
    endBox(&mdhd, mdhdBox);
    mdia.append(mdhd);

    QByteArray hdlr;
    const int hdlrBox = beginFullBox(&hdlr, "hdlr", 0, 0);
    appendU32(&hdlr, 0);
    hdlr.append("soun", 4);
    appendU32(&hdlr, 0);
    appendU32(&hdlr, 0);
    appendU32(&hdlr, 0);
    hdlr.append("SoundHandler", 12);
    appendU8(&hdlr, 0);
    endBox(&hdlr, hdlrBox);
    mdia.append(hdlr);

    QByteArray minf;
    const int minfBox = beginBox(&minf, "minf");

    QByteArray smhd;
    const int smhdBox = beginFullBox(&smhd, "smhd", 0, 0);
    appendU16(&smhd, 0);
    appendU16(&smhd, 0);
    endBox(&smhd, smhdBox);
    minf.append(smhd);

    QByteArray dinf;
    const int dinfBox = beginBox(&dinf, "dinf");
    QByteArray dref;
    const int drefBox = beginFullBox(&dref, "dref", 0, 0);
    appendU32(&dref, 1);
    const int urlBox = beginFullBox(&dref, "url ", 0, 1);
    endBox(&dref, urlBox);
    endBox(&dref, drefBox);
    dinf.append(dref);
    endBox(&dinf, dinfBox);
    minf.append(dinf);

    minf.append(buildStbl(m_audioSamples, false));
    endBox(&minf, minfBox);
    mdia.append(minf);
    endBox(&mdia, mdiaBox);
    trak.append(mdia);

    endBox(&trak, trakBox);
    return trak;
}

QByteArray Mp4Muxer::buildMoov() const
{
    const quint32 videoMs = static_cast<quint32>(
            (static_cast<quint64>(m_maxPts + 1) * kMovieTimescale + m_fps / 2) / m_fps);
    quint32 audioMs = 0;
    if (m_audioEnabled && !m_audioSamples.empty()) {
        audioMs = static_cast<quint32>(
                (static_cast<quint64>(m_audioSamplesTotal) * kMovieTimescale
                 + m_audio.sampleRate / 2) / m_audio.sampleRate);
    }
    const quint32 movieDuration = std::max(videoMs, audioMs);

    QByteArray moov;
    const int moovBox = beginBox(&moov, "moov");

    QByteArray mvhd;
    const int mvhdBox = beginFullBox(&mvhd, "mvhd", 0, 0);
    appendU32(&mvhd, 0);
    appendU32(&mvhd, 0);
    appendU32(&mvhd, kMovieTimescale);
    appendU32(&mvhd, movieDuration);
    appendU32(&mvhd, 0x00010000);
    appendU16(&mvhd, 0x0100);
    appendU16(&mvhd, 0);
    appendU32(&mvhd, 0);
    appendU32(&mvhd, 0);
    appendMatrix(&mvhd);
    for (int i = 0; i < 6; ++i)
        appendU32(&mvhd, 0);
    appendU32(&mvhd, (m_audioEnabled && !m_audioSamples.empty()) ? 3 : 2);
    endBox(&mvhd, mvhdBox);
    moov.append(mvhd);

    moov.append(buildVideoTrak());
    if (m_audioEnabled && !m_audioSamples.empty())
        moov.append(buildAudioTrak());

    endBox(&moov, moovBox);
    return moov;
}

bool Mp4Muxer::finish(QString *error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_opened || m_finished)
        return false;
    m_finished = true;

    if (m_videoSamples.empty() || m_avcC.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有可写入 MP4 的视频帧");
        m_file.close();
        m_file.remove();
        m_opened = false;
        return false;
    }

    m_file.flush();
    const qint64 fileSize = m_file.size();
    const quint64 mdatSize = static_cast<quint64>(fileSize - m_mdatStart);
    if (!m_file.seek(m_mdatSizePos)) {
        if (error)
            *error = fourccError("无法回写 MP4 mdat 长度", m_path);
        m_file.close();
        return false;
    }
    QByteArray sizeBytes;
    appendU64(&sizeBytes, mdatSize);
    if (m_file.write(sizeBytes) != sizeBytes.size()) {
        if (error)
            *error = fourccError("写入 MP4 mdat 长度失败", m_path);
        m_file.close();
        return false;
    }
    if (!m_file.seek(fileSize)) {
        if (error)
            *error = fourccError("无法定位 MP4 moov 写入位置", m_path);
        m_file.close();
        return false;
    }

    const QByteArray moov = buildMoov();
    if (m_file.write(moov) != moov.size()) {
        if (error)
            *error = fourccError("写入 MP4 moov 失败", m_path);
        m_file.close();
        return false;
    }
    m_file.flush();
    m_file.close();
    m_opened = false;
    return true;
}

void Mp4Muxer::abort()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_file.isOpen())
        m_file.close();
    m_opened = false;
    m_finished = true;
    m_videoSamples.clear();
    m_audioSamples.clear();
}

qint64 Mp4Muxer::bytesWritten() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_file.size();
}

int Mp4Muxer::videoSampleCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<int>(m_videoSamples.size());
}
