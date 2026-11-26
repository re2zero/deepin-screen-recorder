// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gifrecorder.h"

#include "../utils/log.h"

#include <QDebug>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {

constexpr int kTransparentIndex = 0;
constexpr int kPaletteColors = 255; // 索引 0 保留给透明色

// 每通道 6bit 的 3D 直方图。5bit 会把 #f8f8f8 背景与 #ffffff 合并成一个
// 平均色，录屏里最常见的纯色/文字灰阶因此整体失准；6bit 按 4 级分桶既能
// 区分这些主色，又把直方图控制在 262144 个桶。
constexpr int kHistogramBits = 6;
constexpr int kHistogramSide = 1 << kHistogramBits;
constexpr int kHistogramShift = kHistogramBits;
constexpr int kHistogramSize = 1 << (kHistogramBits * 3);

// 最近色缓存用 7bit/通道（每桶 2 级）。6bit 缓存会把 4 级范围内的颜色
// 强行映射到同一个调色板项，文字/抗锯齿边缘会明显发虚；7bit 的额外误差
// 不超过 1 级，画质与逐像素精确查找基本一致。
constexpr int kCacheBits = 7;
constexpr int kCacheShift = kCacheBits;
constexpr int kCacheSize = 1 << (kCacheBits * 3);

// 3D 直方图上的一个颜色盒（闭区间，单位是桶下标）。
struct Box
{
    int r0 = 0;
    int r1 = -1;
    int g0 = 0;
    int g1 = -1;
    int b0 = 0;
    int b1 = -1;
    uint64_t weight = 0;
    double meanR = 0.0;
    double meanG = 0.0;
    double meanB = 0.0;
    double varR = 0.0;
    double varG = 0.0;
    double varB = 0.0;
    double error = 0.0; // weight * (varR + varG + varB)
};

void evaluateBox(const std::vector<GifRecorder::ColorBin> &histogram, Box &box)
{
    uint64_t count = 0;
    uint64_t sumR = 0;
    uint64_t sumG = 0;
    uint64_t sumB = 0;
    double squareR = 0.0;
    double squareG = 0.0;
    double squareB = 0.0;

    for (int r = box.r0; r <= box.r1; ++r) {
        for (int g = box.g0; g <= box.g1; ++g) {
            const GifRecorder::ColorBin *row =
                    &histogram[(r << (2 * kHistogramShift)) | (g << kHistogramShift)];
            for (int b = box.b0; b <= box.b1; ++b) {
                const GifRecorder::ColorBin &bin = row[b];
                if (bin.count == 0)
                    continue;
                const double inv = 1.0 / static_cast<double>(bin.count);
                const double meanR = static_cast<double>(bin.sumR) * inv;
                const double meanG = static_cast<double>(bin.sumG) * inv;
                const double meanB = static_cast<double>(bin.sumB) * inv;
                const double weight = static_cast<double>(bin.count);
                count += bin.count;
                sumR += bin.sumR;
                sumG += bin.sumG;
                sumB += bin.sumB;
                squareR += weight * meanR * meanR;
                squareG += weight * meanG * meanG;
                squareB += weight * meanB * meanB;
            }
        }
    }

    box.weight = count;
    if (count == 0) {
        box.error = 0.0;
        box.varR = box.varG = box.varB = 0.0;
        return;
    }

    const double inv = 1.0 / static_cast<double>(count);
    box.meanR = static_cast<double>(sumR) * inv;
    box.meanG = static_cast<double>(sumG) * inv;
    box.meanB = static_cast<double>(sumB) * inv;
    box.varR = std::max(0.0, squareR * inv - box.meanR * box.meanR);
    box.varG = std::max(0.0, squareG * inv - box.meanG * box.meanG);
    box.varB = std::max(0.0, squareB * inv - box.meanB * box.meanB);
    box.error = static_cast<double>(count) * (box.varR + box.varG + box.varB);
}

// 沿方差最大的通道按像素中位数把 box 切成两半；切不开时返回 false。
bool splitBox(const std::vector<GifRecorder::ColorBin> &histogram, Box &box,
              Box &lower, Box &upper)
{
    int channel = 0;
    if (box.varR >= box.varG && box.varR >= box.varB)
        channel = 0;
    else if (box.varG >= box.varB)
        channel = 1;
    else
        channel = 2;

    int lo = 0;
    int hi = 0;
    if (channel == 0) {
        lo = box.r0;
        hi = box.r1;
    } else if (channel == 1) {
        lo = box.g0;
        hi = box.g1;
    } else {
        lo = box.b0;
        hi = box.b1;
    }
    if (lo >= hi)
        return false;

    std::array<uint64_t, kHistogramSide> slabs{};
    for (int r = box.r0; r <= box.r1; ++r) {
        for (int g = box.g0; g <= box.g1; ++g) {
            const GifRecorder::ColorBin *row =
                    &histogram[(r << (2 * kHistogramShift)) | (g << kHistogramShift)];
            for (int b = box.b0; b <= box.b1; ++b) {
                if (row[b].count == 0)
                    continue;
                const int coordinate = channel == 0 ? r : channel == 1 ? g : b;
                slabs[coordinate] += row[b].count;
            }
        }
    }

    uint64_t left = 0;
    uint64_t bestDiff = UINT64_MAX;
    int split = -1;
    const uint64_t half = box.weight / 2;
    for (int v = lo; v < hi; ++v) {
        left += slabs[v];
        if (left == 0)
            continue;
        const uint64_t right = box.weight - left;
        if (right == 0)
            break;
        const uint64_t diff = left > right ? left - right : right - left;
        if (diff < bestDiff) {
            bestDiff = diff;
            split = v;
        }
        if (left > half)
            break;
    }
    if (split < 0)
        return false;

    lower = box;
    upper = box;
    if (channel == 0) {
        lower.r1 = split;
        upper.r0 = split + 1;
    } else if (channel == 1) {
        lower.g1 = split;
        upper.g0 = split + 1;
    } else {
        lower.b1 = split;
        upper.b0 = split + 1;
    }
    evaluateBox(histogram, lower);
    evaluateBox(histogram, upper);
    if (lower.weight == 0 || upper.weight == 0)
        return false;
    return true;
}

std::vector<uint8_t> buildMedianCutPalette(const std::vector<GifRecorder::ColorBin> &histogram,
                                           int colorCount)
{
    std::vector<uint8_t> palette(static_cast<size_t>(colorCount) * 3, 0);

    std::vector<Box> boxes;
    Box initial;
    initial.r0 = initial.g0 = initial.b0 = 0;
    initial.r1 = initial.g1 = initial.b1 = kHistogramSide - 1;
    evaluateBox(histogram, initial);
    if (initial.weight == 0) {
        for (int i = 0; i < colorCount; ++i) {
            const uint8_t value = static_cast<uint8_t>(i * 255 / std::max(1, colorCount - 1));
            palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = value;
        }
        return palette;
    }
    boxes.push_back(initial);

    while (static_cast<int>(boxes.size()) < colorCount) {
        // 优先细分人口方差最大的盒子：大面积背景、文字边缘和渐变都能照顾到。
        int target = -1;
        double bestError = 0.0;
        for (int i = 0; i < static_cast<int>(boxes.size()); ++i) {
            if (boxes[i].error > bestError) {
                bestError = boxes[i].error;
                target = i;
            }
        }
        if (target < 0 || bestError <= 0.0)
            break;

        Box lower;
        Box upper;
        if (!splitBox(histogram, boxes[target], lower, upper)) {
            // 该盒已经无法再切：清掉 error 避免反复选中。
            boxes[target].error = 0.0;
            continue;
        }
        boxes[target] = lower;
        boxes.push_back(upper);
    }

    for (int i = 0; i < colorCount; ++i) {
        const Box &box = boxes[std::min<int>(i, static_cast<int>(boxes.size()) - 1)];
        palette[i * 3] = static_cast<uint8_t>(qBound(0.0, box.meanR + 0.5, 255.0));
        palette[i * 3 + 1] = static_cast<uint8_t>(qBound(0.0, box.meanG + 0.5, 255.0));
        palette[i * 3 + 2] = static_cast<uint8_t>(qBound(0.0, box.meanB + 0.5, 255.0));
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

    bool write(const uint8_t *indices, size_t count)
    {
        if (!indices || count == 0)
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
        for (size_t i = 1; i < count; ++i) {
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
    m_hasPrevious = false;
    m_pendingDelayCs = 0.0;
    m_delayRemainderCs = 0.0;
    m_lastPts = -1;
    m_palette.assign(256 * 3, 0);
    m_colorCache.assign(kCacheSize, 0xFFFF);
    m_histogram.assign(kHistogramSize, ColorBin{});
    m_indexed.resize(static_cast<size_t>(m_width) * m_height);
    m_current.assign(static_cast<size_t>(m_width) * m_height * 4, 0);
    m_previous.assign(static_cast<size_t>(m_width) * m_height * 4, 0);

    m_file.setFileName(outputPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("无法创建 GIF 文件：%1").arg(outputPath);
        return false;
    }
    m_started = true;
    return true;
}

bool GifRecorder::prepareFrame(const uint8_t *rgba, int stride, const QRect &crop,
                               QString *error)
{
    if (!rgba || stride <= 0 || !m_current.size()) {
        if (error)
            *error = QStringLiteral("非法的 GIF 帧数据");
        return false;
    }

    if (crop.width() == m_width && crop.height() == m_height) {
        for (int y = 0; y < m_height; ++y) {
            const uint8_t *src = rgba + static_cast<size_t>(crop.y() + y) * stride
                    + static_cast<size_t>(crop.x()) * 4;
            std::memcpy(m_current.data() + static_cast<size_t>(y) * m_width * 4,
                        src, static_cast<size_t>(m_width) * 4);
        }
        return true;
    }

    // 裁剪尺寸与输出不一致时做最近邻缩放（当前录制路径不会走这里，
    // 保留给复用 GifRecorder 做缩略图的场景）。
    for (int y = 0; y < m_height; ++y) {
        const int sourceY = crop.y() + (y * crop.height()) / m_height;
        const uint8_t *src = rgba + static_cast<size_t>(sourceY) * stride;
        uint8_t *dst = m_current.data() + static_cast<size_t>(y) * m_width * 4;
        for (int x = 0; x < m_width; ++x) {
            const int sourceX = crop.x() + (x * crop.width()) / m_width;
            std::memcpy(dst + static_cast<size_t>(x) * 4,
                        src + static_cast<size_t>(sourceX) * 4, 4);
        }
    }
    return true;
}

QRect GifRecorder::changedRect() const
{
    const uint32_t *current = reinterpret_cast<const uint32_t *>(m_current.data());
    const uint32_t *previous = reinterpret_cast<const uint32_t *>(m_previous.data());
    int minX = m_width;
    int minY = m_height;
    int maxX = -1;
    int maxY = -1;

    for (int y = 0; y < m_height; ++y) {
        const uint32_t *currentRow = current + static_cast<size_t>(y) * m_width;
        const uint32_t *previousRow = previous + static_cast<size_t>(y) * m_width;
        for (int x = 0; x < m_width; ++x) {
            if (currentRow[x] == previousRow[x])
                continue;
            minX = std::min(minX, x);
            maxX = std::max(maxX, x);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
    }

    if (maxX < minX || maxY < minY)
        return QRect();
    return QRect(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

void GifRecorder::buildPalette(const QRect &box)
{
    // 统计变化区域内所有像素的 6bit/通道直方图。旧实现按 8 像素间隔抽稀，
    // 细笔画文字、小图标会被漏采，调色板里缺少真正的暗色/亮色，回放时文字
    // 变浅、细节发虚；全量直方图按人口加权取色可以显著改善。
    std::fill(m_histogram.begin(), m_histogram.end(), ColorBin{});
    for (int y = box.top(); y <= box.bottom(); ++y) {
        const uint8_t *row = m_current.data()
                + (static_cast<size_t>(y) * m_width + box.left()) * 4;
        for (int x = 0; x < box.width(); ++x) {
            const uint8_t *pixel = row + static_cast<size_t>(x) * 4;
            ColorBin &bin = m_histogram[((pixel[0] >> (8 - kHistogramBits))
                                          << (2 * kHistogramShift))
                    | ((pixel[1] >> (8 - kHistogramBits)) << kHistogramShift)
                    | (pixel[2] >> (8 - kHistogramBits))];
            ++bin.count;
            bin.sumR += pixel[0];
            bin.sumG += pixel[1];
            bin.sumB += pixel[2];
        }
    }

    const std::vector<uint8_t> colors = buildMedianCutPalette(m_histogram, kPaletteColors);
    m_palette.assign(256 * 3, 0);
    std::memcpy(m_palette.data() + 3, colors.data(), colors.size());
}

uint8_t GifRecorder::paletteIndex(int r, int g, int b)
{
    const int key = ((r >> (8 - kCacheBits)) << (2 * kCacheShift))
            | ((g >> (8 - kCacheBits)) << kCacheShift)
            | (b >> (8 - kCacheBits));
    uint16_t &cached = m_colorCache[key];
    if (cached != 0xFFFF)
        return static_cast<uint8_t>(cached);

    int best = 1;
    int bestDistance = 1 << 30;
    for (int i = 1; i <= kPaletteColors; ++i) {
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
    // 无全局调色板：每帧携带局部调色板，索引 0 保留给透明色。
    writeByte(0x70);
    writeByte(0);    // background color index
    writeByte(0);    // pixel aspect ratio

    // NETSCAPE2.0 循环播放
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

bool GifRecorder::pushFrame(const uint8_t *rgba, int stride, const QRect &crop,
                            qint64 pts, QString *error)
{
    if (!m_started) {
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

    if (!prepareFrame(rgba, stride, source, error))
        return false;
    if (!writeFrame(pts, error))
        return false;
    return true;
}

bool GifRecorder::writeFrame(qint64 pts, QString *error)
{
    const bool first = !m_hasPrevious;
    const QRect box = first ? QRect(0, 0, m_width, m_height) : changedRect();

    const qint64 slotDelta = m_lastPts < 0 ? 1 : std::max<qint64>(1, pts - m_lastPts);
    const double exactCs = slotDelta * 100.0 / std::max(1, m_fps);
    m_lastPts = pts;

    if (!first && box.isEmpty()) {
        // 没有任何像素变化：把这段时间累加到下一帧的显示时长里。
        m_pendingDelayCs += exactCs;
        return true;
    }

    // GIF 的延迟单位是 1/100 秒，12fps 的 8.33cs 无法逐帧精确表示；
    // 用余数累积，长期平均帧率不会漂移（否则 60 帧会少掉约 0.4 秒）。
    const double totalCs = m_pendingDelayCs + m_delayRemainderCs + exactCs;
    const int delay = qBound(1, static_cast<int>(std::floor(totalCs)), 65535);
    m_delayRemainderCs = totalCs - delay;
    m_pendingDelayCs = 0.0;

    buildPalette(box);
    std::fill(m_colorCache.begin(), m_colorCache.end(), 0xFFFF);

    const int boxWidth = box.width();
    const int boxHeight = box.height();
    const uint32_t *current = reinterpret_cast<const uint32_t *>(m_current.data());
    const uint32_t *previous = reinterpret_cast<const uint32_t *>(m_previous.data());
    for (int y = 0; y < boxHeight; ++y) {
        const size_t sourceRow = static_cast<size_t>(box.y() + y) * m_width;
        uint8_t *out = m_indexed.data() + static_cast<size_t>(y) * boxWidth;
        for (int x = 0; x < boxWidth; ++x) {
            const int sourceX = box.x() + x;
            if (!first && current[sourceRow + sourceX] == previous[sourceRow + sourceX]) {
                out[x] = kTransparentIndex;
                continue;
            }
            const uint8_t *pixel = m_current.data() + (sourceRow + sourceX) * 4;
            out[x] = paletteIndex(pixel[0], pixel[1], pixel[2]);
        }
    }

    writeByte(0x21); // graphic control extension
    writeByte(0xF9);
    writeByte(0x04);
    // disposal method 1（保留前帧），后续帧用透明色跳过未变化像素。
    writeByte(first ? 0x04 : 0x05);
    writeShort(static_cast<uint16_t>(delay));
    writeByte(static_cast<uint8_t>(kTransparentIndex));
    writeByte(0x00);

    writeByte(0x2C); // image descriptor
    writeShort(static_cast<uint16_t>(box.x()));
    writeShort(static_cast<uint16_t>(box.y()));
    writeShort(static_cast<uint16_t>(boxWidth));
    writeShort(static_cast<uint16_t>(boxHeight));
    writeByte(0x87); // local color table, 256 entries
    m_file.write(reinterpret_cast<const char *>(m_palette.data()), 256 * 3);

    writeByte(0x08); // LZW minimum code size
    LzwWriter writer(m_file);
    if (!writer.write(m_indexed.data(), static_cast<size_t>(boxWidth) * boxHeight)) {
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

    std::copy(m_current.begin(), m_current.end(), m_previous.begin());
    m_hasPrevious = true;
    ++m_frameCount;
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
        if (!writeHeader(error)) {
            abort();
            return false;
        }
        const int delay = std::max(1, qRound(100.0 / std::max(1, m_fps)));
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
        writeShort(2);
        writeShort(2);
        writeByte(0x87);
        m_file.write(reinterpret_cast<const char *>(m_palette.data()), 256 * 3);
        writeByte(0x08);
        const std::array<uint8_t, 4> black{0, 0, 0, 0};
        LzwWriter writer(m_file);
        writer.write(black.data(), black.size());
        writeByte(0x00);
    } else if (m_pendingDelayCs >= 1.0 && m_hasPrevious) {
        // 最后一个真实帧之后没有新帧：用 1x1 透明帧补齐累计显示时长。
        const uint16_t delay = static_cast<uint16_t>(
                qBound(1, static_cast<int>(m_pendingDelayCs), 65535));
        writeByte(0x21);
        writeByte(0xF9);
        writeByte(0x04);
        writeByte(0x05);
        writeShort(delay);
        writeByte(static_cast<uint8_t>(kTransparentIndex));
        writeByte(0x00);
        writeByte(0x2C);
        writeShort(0);
        writeShort(0);
        writeShort(1);
        writeShort(1);
        writeByte(0x87);
        m_file.write(reinterpret_cast<const char *>(m_palette.data()), 256 * 3);
        writeByte(0x08);
        const uint8_t transparent = kTransparentIndex;
        LzwWriter writer(m_file);
        writer.write(&transparent, 1);
        writeByte(0x00);
        m_pendingDelayCs = 0.0;
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
    m_hasPrevious = false;
    m_pendingDelayCs = 0.0;
    m_delayRemainderCs = 0.0;
    m_lastPts = -1;
}
