// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ucodecvideoencoder.h"

#include "../utils/log.h"

#include <QByteArray>
#include <QDebug>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <ucodec/ucodec.h>

namespace {

inline uint8_t clampByte(int value)
{
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

inline uint8_t clampLuma(int value)
{
    return static_cast<uint8_t>(value < 16 ? 16 : (value > 235 ? 235 : value));
}

inline uint8_t clampChroma(int value)
{
    return static_cast<uint8_t>(value < 16 ? 16 : (value > 240 ? 240 : value));
}

inline void readRgb(const uint8_t *pixel, bool rgba, int *r, int *g, int *b)
{
    if (rgba) {
        *r = pixel[0];
        *g = pixel[1];
        *b = pixel[2];
    } else {
        *b = pixel[0];
        *g = pixel[1];
        *r = pixel[2];
    }
}

inline void rgbToYuv(int r, int g, int b, uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = clampLuma(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    *u = clampChroma(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
    *v = clampChroma(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
}

} // namespace

void UcodecVideoEncoder::packetTrampoline(const uint8_t *data, int size, int64_t pts,
                                          int64_t dts, int key, void *user)
{
    auto *self = static_cast<UcodecVideoEncoder *>(user);
    if (self && self->m_callback)
        self->m_callback(data, size, pts, dts, key != 0);
}

UcodecVideoEncoder::UcodecVideoEncoder() = default;

UcodecVideoEncoder::~UcodecVideoEncoder()
{
    close();
}

qint64 UcodecVideoEncoder::defaultBitrate(int width, int height, int fps)
{
    // 屏幕内容细节多，取 ~0.12 bit/pixel/frame；下限 1 Mbps，上限 40 Mbps。
    const double raw = static_cast<double>(width) * height * std::max(1, fps) * 0.12;
    const qint64 clamped = static_cast<qint64>(raw);
    return std::clamp<qint64>(clamped, 1000 * 1000, 40 * 1000 * 1000);
}

bool UcodecVideoEncoder::open(const Config &config, PacketCallback callback,
                              QString *error)
{
    close();

    if (config.width <= 0 || config.height <= 0 || config.fps <= 0) {
        if (error)
            *error = QStringLiteral("非法的编码参数：%1x%2@%3")
                             .arg(config.width).arg(config.height).arg(config.fps);
        return false;
    }

    m_config = config;
    m_width = config.width & ~1;
    m_height = config.height & ~1;
    m_fps = config.fps;
    m_bitrate = config.bitrate > 0
            ? config.bitrate
            : defaultBitrate(m_width, m_height, m_fps);
    m_callback = std::move(callback);

    if (m_width < 2 || m_height < 2) {
        if (error)
            *error = QStringLiteral("编码尺寸过小：%1x%2").arg(config.width).arg(config.height);
        return false;
    }

    const QByteArray codec = config.codec.toUtf8();

    ucodec_enc_config encConfig;
    std::memset(&encConfig, 0, sizeof(encConfig));
    encConfig.codec = codec.constData();
    encConfig.width = m_width;
    encConfig.height = m_height;
    encConfig.fps = m_fps;
    encConfig.bitrate = m_bitrate;
    encConfig.low_latency = config.lowLatency ? 1 : 0;
    encConfig.prefer_hw = config.preferHw ? 1 : 0;
    encConfig.allow_sw_fallback = 1;

    ucodec_encoder *encoder = nullptr;
    const ucodec_status status = ucodec_encoder_create(&encConfig, &encoder);
    if (status != UCODEC_OK || !encoder) {
        if (error)
            *error = QStringLiteral("ucodec 编码器创建失败（status=%1）").arg(int(status));
        return false;
    }

    m_encoder = encoder;

    m_yPlane.resize(static_cast<size_t>(m_width) * m_height);
    m_uPlane.resize(static_cast<size_t>(m_width / 2) * (m_height / 2));
    m_vPlane.resize(static_cast<size_t>(m_width / 2) * (m_height / 2));

    qCInfo(dsrApp) << "ucodec encoder ready:"
                   << m_width << "x" << m_height << "@" << m_fps
                   << "bitrate:" << m_bitrate
                   << "path:" << pathName();
    return true;
}

void UcodecVideoEncoder::close()
{
    if (m_encoder) {
        ucodec_encoder_destroy(static_cast<ucodec_encoder *>(m_encoder));
        m_encoder = nullptr;
    }
    m_callback = nullptr;
}

QString UcodecVideoEncoder::pathName() const
{
    if (!m_encoder)
        return QStringLiteral("closed");
    const ucodec_path path = ucodec_encoder_get_path(static_cast<ucodec_encoder *>(m_encoder));
    switch (path) {
    case UCODEC_PATH_VAAPI:
        return QStringLiteral("vaapi");
    case UCODEC_PATH_NVENC:
        return QStringLiteral("nvenc");
    case UCODEC_PATH_NVDEC:
        return QStringLiteral("nvdec");
    case UCODEC_PATH_SW:
    default:
        return QStringLiteral("sw");
    }
}

bool UcodecVideoEncoder::encodeFrame(const uint8_t *data, int stride, const QRect &crop,
                                     qint64 pts, PixelFormat format, QString *error)
{
    if (!m_encoder || !data || stride <= 0) {
        if (error)
            *error = QStringLiteral("编码器未就绪或帧数据为空");
        return false;
    }

    QRect source = crop;
    if (source.isEmpty())
        source = QRect(0, 0, m_width, m_height);
    if (source.width() <= 0 || source.height() <= 0) {
        if (error)
            *error = QStringLiteral("裁剪区域非法：%1x%2")
                             .arg(source.width()).arg(source.height());
        return false;
    }

    const bool fastPath = source.x() == 0 && source.y() == 0
            && source.width() == m_width && source.height() == m_height;
    if (fastPath)
        convertRgbaToYuv420p(data, stride, source, format);
    else
        convertScaledToYuv420p(data, stride, source, format);

    ucodec_frame frame;
    std::memset(&frame, 0, sizeof(frame));
    frame.data[0] = m_yPlane.data();
    frame.data[1] = m_uPlane.data();
    frame.data[2] = m_vPlane.data();
    frame.linesize[0] = m_width;
    frame.linesize[1] = m_width / 2;
    frame.linesize[2] = m_width / 2;
    frame.format = UCODEC_FMT_YUV420P;
    frame.width = m_width;
    frame.height = m_height;
    frame.pts = pts;

    const ucodec_status status = ucodec_encoder_encode_ram(
            static_cast<ucodec_encoder *>(m_encoder), &frame,
            packetTrampoline, this);
    if (status != UCODEC_OK) {
        if (error)
            *error = QStringLiteral("ucodec 编码失败（status=%1, pts=%2）")
                             .arg(int(status)).arg(pts);
        return false;
    }
    return true;
}

bool UcodecVideoEncoder::flush(QString *error)
{
    if (!m_encoder)
        return true;
    const ucodec_status status = ucodec_encoder_flush(
            static_cast<ucodec_encoder *>(m_encoder), packetTrampoline, this);
    if (status != UCODEC_OK) {
        if (error)
            *error = QStringLiteral("ucodec 冲刷失败（status=%1）").arg(int(status));
        return false;
    }
    return true;
}

void UcodecVideoEncoder::convertRgbaToYuv420p(const uint8_t *data, int stride,
                                              const QRect &crop, PixelFormat format)
{
    const int w = m_width;
    const int h = m_height;
    const bool rgba = format == RGBA8888;

    for (int y = 0; y < h; ++y) {
        const uint8_t *row = data + static_cast<size_t>(crop.y() + y) * stride
                + static_cast<size_t>(crop.x()) * 4;
        uint8_t *dst = m_yPlane.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            int r = 0;
            int g = 0;
            int b = 0;
            readRgb(row + static_cast<size_t>(x) * 4, rgba, &r, &g, &b);
            dst[x] = clampLuma(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    const int chromaW = w / 2;
    const int chromaH = h / 2;
    for (int cy = 0; cy < chromaH; ++cy) {
        const uint8_t *row0 = data + static_cast<size_t>(crop.y() + cy * 2) * stride
                + static_cast<size_t>(crop.x()) * 4;
        const uint8_t *row1 = row0 + stride;
        uint8_t *uRow = m_uPlane.data() + static_cast<size_t>(cy) * chromaW;
        uint8_t *vRow = m_vPlane.data() + static_cast<size_t>(cy) * chromaW;
        for (int cx = 0; cx < chromaW; ++cx) {
            const uint8_t *p00 = row0 + static_cast<size_t>(cx) * 8;
            const uint8_t *p10 = p00 + 4;
            const uint8_t *p01 = row1 + static_cast<size_t>(cx) * 8;
            const uint8_t *p11 = p01 + 4;
            int r00 = 0, g00 = 0, b00 = 0;
            int r10 = 0, g10 = 0, b10 = 0;
            int r01 = 0, g01 = 0, b01 = 0;
            int r11 = 0, g11 = 0, b11 = 0;
            readRgb(p00, rgba, &r00, &g00, &b00);
            readRgb(p10, rgba, &r10, &g10, &b10);
            readRgb(p01, rgba, &r01, &g01, &b01);
            readRgb(p11, rgba, &r11, &g11, &b11);
            const int r = r00 + r10 + r01 + r11;
            const int g = g00 + g10 + g01 + g11;
            const int b = b00 + b10 + b01 + b11;
            uRow[cx] = clampChroma(((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128);
            vRow[cx] = clampChroma(((112 * r - 94 * g - 18 * b + 512) >> 10) + 128);
        }
    }
}

void UcodecVideoEncoder::convertScaledToYuv420p(const uint8_t *data, int stride,
                                                const QRect &crop, PixelFormat format)
{
    const int w = m_width;
    const int h = m_height;
    const bool rgba = format == RGBA8888;
    const double scaleX = static_cast<double>(crop.width()) / w;
    const double scaleY = static_cast<double>(crop.height()) / h;
    const int maxX = crop.x() + crop.width() - 1;
    const int maxY = crop.y() + crop.height() - 1;

    auto sample = [&](double fx, double fy, int *r, int *g, int *b) {
        const double sx = crop.x() + (fx + 0.5) * scaleX - 0.5;
        const double sy = crop.y() + (fy + 0.5) * scaleY - 0.5;
        const int x0 = std::clamp(static_cast<int>(std::floor(sx)), crop.x(), maxX);
        const int y0 = std::clamp(static_cast<int>(std::floor(sy)), crop.y(), maxY);
        const int x1 = std::min(x0 + 1, maxX);
        const int y1 = std::min(y0 + 1, maxY);
        const double wx = std::clamp(sx - std::floor(sx), 0.0, 1.0);
        const double wy = std::clamp(sy - std::floor(sy), 0.0, 1.0);
        const uint8_t *p00 = data + static_cast<size_t>(y0) * stride + static_cast<size_t>(x0) * 4;
        const uint8_t *p10 = data + static_cast<size_t>(y0) * stride + static_cast<size_t>(x1) * 4;
        const uint8_t *p01 = data + static_cast<size_t>(y1) * stride + static_cast<size_t>(x0) * 4;
        const uint8_t *p11 = data + static_cast<size_t>(y1) * stride + static_cast<size_t>(x1) * 4;
        int r00 = 0, g00 = 0, b00 = 0;
        int r10 = 0, g10 = 0, b10 = 0;
        int r01 = 0, g01 = 0, b01 = 0;
        int r11 = 0, g11 = 0, b11 = 0;
        readRgb(p00, rgba, &r00, &g00, &b00);
        readRgb(p10, rgba, &r10, &g10, &b10);
        readRgb(p01, rgba, &r01, &g01, &b01);
        readRgb(p11, rgba, &r11, &g11, &b11);
        const double topR = r00 * (1.0 - wx) + r10 * wx;
        const double topG = g00 * (1.0 - wx) + g10 * wx;
        const double topB = b00 * (1.0 - wx) + b10 * wx;
        const double bottomR = r01 * (1.0 - wx) + r11 * wx;
        const double bottomG = g01 * (1.0 - wx) + g11 * wx;
        const double bottomB = b01 * (1.0 - wx) + b11 * wx;
        *r = static_cast<int>(std::lround(topR * (1.0 - wy) + bottomR * wy));
        *g = static_cast<int>(std::lround(topG * (1.0 - wy) + bottomG * wy));
        *b = static_cast<int>(std::lround(topB * (1.0 - wy) + bottomB * wy));
    };

    for (int y = 0; y < h; ++y) {
        uint8_t *dst = m_yPlane.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            int r = 0, g = 0, b = 0;
            sample(x, y, &r, &g, &b);
            dst[x] = clampLuma(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }

    const int chromaW = w / 2;
    const int chromaH = h / 2;
    for (int cy = 0; cy < chromaH; ++cy) {
        uint8_t *uRow = m_uPlane.data() + static_cast<size_t>(cy) * chromaW;
        uint8_t *vRow = m_vPlane.data() + static_cast<size_t>(cy) * chromaW;
        for (int cx = 0; cx < chromaW; ++cx) {
            // 2x2 输出块 -> 双线性采样的 RGB 均值
            int r = 0, g = 0, b = 0;
            int sr = 0, sg = 0, sb = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    sample(cx * 2 + dx, cy * 2 + dy, &r, &g, &b);
                    sr += r;
                    sg += g;
                    sb += b;
                }
            }
            uRow[cx] = clampChroma(((-38 * sr - 74 * sg + 112 * sb + 512) >> 10) + 128);
            vRow[cx] = clampChroma(((112 * sr - 94 * sg - 18 * sb + 512) >> 10) + 128);
        }
    }
}
