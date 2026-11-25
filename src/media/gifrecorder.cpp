// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gifrecorder.h"

#include "../utils/log.h"

#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstring>

namespace {

struct Rgb
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

struct Box
{
    int begin = 0;
    int end = 0;
    int widest = 0;
    int range = 0;
};

void updateBox(std::vector<Rgb> &samples, Box &box)
{
    int minR = 255, minG = 255, minB = 255;
    int maxR = 0, maxG = 0, maxB = 0;
    for (int i = box.begin; i < box.end; ++i) {
        minR = std::min<int>(minR, samples[i].r);
        minG = std::min<int>(minG, samples[i].g);
        minB = std::min<int>(minB, samples[i].b);
        maxR = std::max<int>(maxR, samples[i].r);
        maxG = std::max<int>(maxG, samples[i].g);
        maxB = std::max<int>(maxB, samples[i].b);
    }
    const int rangeR = maxR - minR;
    const int rangeG = maxG - minG;
    const int rangeB = maxB - minB;
    if (rangeR >= rangeG && rangeR >= rangeB) {
        box.widest = 0;
        box.range = rangeR;
    } else if (rangeG >= rangeB) {
        box.widest = 1;
        box.range = rangeG;
    } else {
        box.widest = 2;
        box.range = rangeB;
    }
}

std::vector<uint8_t> medianCutPalette(std::vector<Rgb> &samples)
{
    std::vector<uint8_t> palette(256 * 3, 0);
    if (samples.empty()) {
        for (int i = 0; i < 256; ++i) {
            palette[i * 3] = static_cast<uint8_t>(i);
            palette[i * 3 + 1] = static_cast<uint8_t>(i);
            palette[i * 3 + 2] = static_cast<uint8_t>(i);
        }
        return palette;
    }

    std::vector<Box> boxes;
    Box initial;
    initial.begin = 0;
    initial.end = static_cast<int>(samples.size());
    updateBox(samples, initial);
    boxes.push_back(initial);

    while (static_cast<int>(boxes.size()) < 256) {
        int target = -1;
        int bestRange = 0;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            if (boxes[i].end - boxes[i].begin < 2)
                continue;
            if (boxes[i].range > bestRange) {
                bestRange = boxes[i].range;
                target = i;
            }
        }
        if (target < 0 || bestRange == 0)
            break;

        Box &box = boxes[target];
        const int channel = box.widest;
        const int mid = box.begin + (box.end - box.begin) / 2;
        std::nth_element(samples.begin() + box.begin, samples.begin() + mid,
                         samples.begin() + box.end,
                         [channel](const Rgb &left, const Rgb &right) {
                             return channel == 0 ? left.r < right.r
                                     : channel == 1 ? left.g < right.g
                                                    : left.b < right.b;
                         });

        Box upper;
        upper.begin = mid;
        upper.end = box.end;
        box.end = mid;
        updateBox(samples, box);
        updateBox(samples, upper);
        boxes.push_back(upper);
    }

    for (int i = 0; i < 256; ++i) {
        const Box &box = boxes[std::min<int>(i, static_cast<int>(boxes.size()) - 1)];
        uint64_t sumR = 0;
        uint64_t sumG = 0;
        uint64_t sumB = 0;
        for (int j = box.begin; j < box.end; ++j) {
            sumR += samples[j].r;
            sumG += samples[j].g;
            sumB += samples[j].b;
        }
        const int count = std::max(1, box.end - box.begin);
        palette[i * 3] = static_cast<uint8_t>(sumR / count);
        palette[i * 3 + 1] = static_cast<uint8_t>(sumG / count);
        palette[i * 3 + 2] = static_cast<uint8_t>(sumB / count);
    }
    return palette;
}

class LzwWriter
{
public:
    explicit LzwWriter(QFile &file)
        : m_file(file)
    {
    }

    bool write(const std::vector<uint8_t> &indices)
    {
        if (indices.empty())
            return false;

        m_keys.assign(8192, 0xFFFFFFFFu);
        m_values.assign(8192, 0);
        m_bitBuffer = 0;
        m_bitCount = 0;
        m_blockLen = 0;

        int codeSize = 9;
        int nextCode = 258;
        emitCode(256, codeSize); // clear

        int prefix = indices[0];
        for (size_t i = 1; i < indices.size(); ++i) {
            const int pixel = indices[i];
            const uint32_t key = (static_cast<uint32_t>(prefix) << 8) | pixel;
            int found = -1;
            if (lookup(key, &found)) {
                prefix = found;
                continue;
            }

            emitCode(prefix, codeSize);
            insert(key, nextCode);
            ++nextCode;
            // 解码器比编码器晚一步建表，因此必须等 nextCode 越过 2^codeSize
            // 一个位置再升位宽，否则码流与解码器错位。
            if (nextCode == (1 << codeSize) + 1 && codeSize < 12)
                ++codeSize;
            if (nextCode == 4096) {
                emitCode(256, codeSize);
                m_keys.assign(8192, 0xFFFFFFFFu);
                codeSize = 9;
                nextCode = 258;
            }
            prefix = pixel;
        }

        emitCode(prefix, codeSize);
        emitCode(257, codeSize); // end of information
        flushBits();
        flushBlock();
        return m_file.error() == QFileDevice::NoError;
    }

private:
    void emitCode(int code, int size)
    {
        m_bitBuffer |= static_cast<uint32_t>(code) << m_bitCount;
        m_bitCount += size;
        while (m_bitCount >= 8) {
            pushByte(static_cast<uint8_t>(m_bitBuffer & 0xff));
            m_bitBuffer >>= 8;
            m_bitCount -= 8;
        }
    }

    void flushBits()
    {
        if (m_bitCount > 0) {
            pushByte(static_cast<uint8_t>(m_bitBuffer & 0xff));
            m_bitBuffer = 0;
            m_bitCount = 0;
        }
    }

    void pushByte(uint8_t value)
    {
        m_block[m_blockLen++] = value;
        if (m_blockLen == 255)
            flushBlock();
    }

    void flushBlock()
    {
        if (m_blockLen == 0)
            return;
        m_file.putChar(static_cast<char>(m_blockLen));
        m_file.write(reinterpret_cast<const char *>(m_block.data()), m_blockLen);
        m_blockLen = 0;
    }

    static uint32_t hashKey(uint32_t key)
    {
        return (key * 2654435761u) >> 19; // 8192 buckets
    }

    bool lookup(uint32_t key, int *value) const
    {
        uint32_t slot = hashKey(key);
        // 表容量 8192、最多 4096 个码字，线性探测必然能遇到空槽；
        // 探测上限取表长以保证读写路径一致（否则编码器/解码器码表会错位）。
        for (int probe = 0; probe < 8192; ++probe) {
            const uint32_t current = m_keys[slot];
            if (current == 0xFFFFFFFFu)
                return false;
            if (current == key) {
                *value = m_values[slot];
                return true;
            }
            slot = (slot + 1) & 8191u;
        }
        return false;
    }

    void insert(uint32_t key, int value)
    {
        uint32_t slot = hashKey(key);
        for (int probe = 0; probe < 8192; ++probe) {
            if (m_keys[slot] == 0xFFFFFFFFu) {
                m_keys[slot] = key;
                m_values[slot] = static_cast<uint32_t>(value);
                return;
            }
            slot = (slot + 1) & 8191u;
        }
    }

    QFile &m_file;
    std::vector<uint32_t> m_keys;
    std::vector<uint32_t> m_values;
    uint32_t m_bitBuffer = 0;
    int m_bitCount = 0;
    std::array<uint8_t, 255> m_block{};
    int m_blockLen = 0;
};

} // namespace

GifRecorder::GifRecorder() = default;

GifRecorder::~GifRecorder()
{
    abort();
}

bool GifRecorder::start(const QString &outputPath, int width, int height, int fps,
                        qint64 bitrate, const AudioCaptureOptions &audio, QString *error)
{
    Q_UNUSED(bitrate)
    Q_UNUSED(audio)

    abort();

    if (outputPath.isEmpty() || width < 2 || height < 2
            || width > 65535 || height > 65535) {
        if (error)
            *error = QStringLiteral("非法的 GIF 录制参数");
        return false;
    }

    m_outputPath = outputPath;
    m_width = width & ~1;
    m_height = height & ~1;
    m_fps = qBound(1, fps, 50);
    m_frameCount = 0;
    m_headerWritten = false;
    m_palette.assign(256 * 3, 0);
    m_colorCache.assign(32 * 32 * 32, 0xFFFF);
    m_indexed.resize(static_cast<size_t>(m_width) * m_height);

    m_file.setFileName(outputPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法创建 GIF 文件：%1").arg(outputPath);
        return false;
    }
    m_started = true;
    return true;
}

bool GifRecorder::buildPalette(const uint8_t *rgba, int stride, const QRect &crop)
{
    const int pixelCount = m_width * m_height;
    const int targetSamples = 32768;
    int step = 1;
    while (step * step * targetSamples < pixelCount)
        ++step;

    const bool direct = crop.width() == m_width && crop.height() == m_height;
    std::vector<Rgb> samples;
    samples.reserve(targetSamples);
    for (int y = 0; y < m_height; y += step) {
        const int sourceY = direct ? crop.y() + y
                                   : crop.y() + (y * crop.height()) / m_height;
        const uint8_t *row = rgba + static_cast<size_t>(sourceY) * stride
                + static_cast<size_t>(crop.x()) * 4;
        for (int x = 0; x < m_width; x += step) {
            const int sourceX = direct ? x : (x * crop.width()) / m_width;
            const uint8_t *pixel = row + static_cast<size_t>(sourceX) * 4;
            samples.push_back({pixel[0], pixel[1], pixel[2]});
        }
    }

    m_palette = medianCutPalette(samples);
    return true;
}

uint8_t GifRecorder::paletteIndex(int r, int g, int b)
{
    const int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
    uint16_t &cached = m_colorCache[key];
    if (cached != 0xFFFF)
        return static_cast<uint8_t>(cached);

    int best = 0;
    int bestDistance = 1 << 30;
    for (int i = 0; i < 256; ++i) {
        const int dr = r - m_palette[i * 3];
        const int dg = g - m_palette[i * 3 + 1];
        const int db = b - m_palette[i * 3 + 2];
        const int distance = dr * dr + dg * dg + db * db;
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
            if (distance == 0)
                break;
        }
    }
    cached = static_cast<uint16_t>(best);
    return static_cast<uint8_t>(best);
}

bool GifRecorder::writeHeader(QString *error)
{
    // GIF89a
    m_file.write("GIF89a", 6);
    writeShort(static_cast<uint16_t>(m_width));
    writeShort(static_cast<uint16_t>(m_height));
    // 无全局调色板：每帧携带 256 色局部调色板，避免首帧色彩覆盖后续画面。
    writeByte(0x70);
    writeByte(0);    // background color index
    writeByte(0);    // pixel aspect ratio

    // NETSCAPE2.0 loop forever
    writeByte(0x21);
    writeByte(0xFF);
    writeByte(0x0B);
    m_file.write("NETSCAPE2.0", 11);
    writeByte(0x03);
    writeByte(0x01);
    writeShort(0);
    writeByte(0x00);

    if (m_file.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("GIF 文件头写入失败");
        return false;
    }
    m_headerWritten = true;
    return true;
}

bool GifRecorder::pushFrame(const uint8_t *rgba, int stride, const QRect &crop, qint64 pts,
                            QString *error)
{
    Q_UNUSED(pts)

    if (!m_started || !rgba || stride <= 0) {
        if (error)
            *error = QStringLiteral("GIF 录制器未就绪");
        return false;
    }

    QRect source = crop;
    if (source.isEmpty())
        source = QRect(0, 0, m_width, m_height);

    if (!m_headerWritten) {
        if (!writeHeader(error))
            return false;
    }

    if (!writeFrame(rgba, stride, source, error))
        return false;

    ++m_frameCount;
    return true;
}

bool GifRecorder::writeFrame(const uint8_t *rgba, int stride, const QRect &crop, QString *error)
{
    buildPalette(rgba, stride, crop);
    std::fill(m_colorCache.begin(), m_colorCache.end(), 0xFFFF);

    // 索引化：裁剪尺寸与输出一致时直接取值，否则最近邻缩放。
    const bool direct = crop.width() == m_width && crop.height() == m_height;
    for (int y = 0; y < m_height; ++y) {
        const int sourceY = direct ? crop.y() + y
                                   : crop.y() + (y * crop.height()) / m_height;
        const uint8_t *row = rgba + static_cast<size_t>(sourceY) * stride
                + static_cast<size_t>(crop.x()) * 4;
        uint8_t *out = m_indexed.data() + static_cast<size_t>(y) * m_width;
        for (int x = 0; x < m_width; ++x) {
            const int sourceX = direct ? x : (x * crop.width()) / m_width;
            const uint8_t *pixel = row + static_cast<size_t>(sourceX) * 4;
            out[x] = paletteIndex(pixel[0], pixel[1], pixel[2]);
        }
    }

    const int delay = std::max(1, qRound(100.0 / m_fps));
    writeByte(0x21); // graphic control extension
    writeByte(0xF9);
    writeByte(0x04);
    writeByte(0x04); // disposal method 1, no transparency
    writeShort(static_cast<uint16_t>(delay));
    writeByte(0x00);
    writeByte(0x00);

    writeByte(0x2C); // image descriptor
    writeShort(0);
    writeShort(0);
    writeShort(static_cast<uint16_t>(m_width));
    writeShort(static_cast<uint16_t>(m_height));
    writeByte(0x87); // local color table, 256 entries
    m_file.write(reinterpret_cast<const char *>(m_palette.data()), 256 * 3);

    writeByte(0x08); // LZW minimum code size
    LzwWriter writer(m_file);
    if (!writer.write(m_indexed)) {
        if (error)
            *error = QStringLiteral("GIF 帧压缩写入失败");
        return false;
    }
    writeByte(0x00); // block terminator

    if (m_file.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("GIF 帧写入失败");
        return false;
    }
    return true;
}

bool GifRecorder::finish(QString *error)
{
    if (!m_started)
        return true;

    if (!m_headerWritten) {
        // 没有帧：写一个 2x2 黑色 GIF，保证文件结构完整。
        m_width = qMax(2, m_width);
        m_height = qMax(2, m_height);
        m_palette.assign(256 * 3, 0);
        for (int i = 0; i < 256; ++i) {
            m_palette[i * 3] = m_palette[i * 3 + 1] = m_palette[i * 3 + 2] = 0;
        }
        if (!writeHeader(error)) {
            abort();
            return false;
        }
        std::vector<uint8_t> black(static_cast<size_t>(m_width) * m_height, 0);
        const int delay = std::max(1, qRound(100.0 / m_fps));
        writeByte(0x21);
        writeByte(0xF9);
        writeByte(0x04);
        writeByte(0x04);
        writeShort(static_cast<uint16_t>(delay));
        writeByte(0x00);
        writeByte(0x00);
        writeByte(0x2C);
        writeShort(0);
        writeShort(0);
        writeShort(static_cast<uint16_t>(m_width));
        writeShort(static_cast<uint16_t>(m_height));
        writeByte(0x87);
        m_file.write(reinterpret_cast<const char *>(m_palette.data()), 256 * 3);
        writeByte(0x08);
        LzwWriter writer(m_file);
        writer.write(black);
        writeByte(0x00);
    }

    writeByte(0x3B); // trailer
    m_file.flush();
    m_file.close();
    m_started = false;

    if (m_file.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("GIF 文件收尾失败");
        return false;
    }
    qCInfo(dsrApp) << "GifRecorder finished:" << m_outputPath
                   << "frames:" << m_frameCount
                   << "bytes:" << QFileInfo(m_outputPath).size();
    return true;
}

void GifRecorder::abort()
{
    const bool wasActive = m_started;
    if (m_file.isOpen())
        m_file.close();
    if (wasActive)
        m_file.remove();
    m_started = false;
    m_headerWritten = false;
}
