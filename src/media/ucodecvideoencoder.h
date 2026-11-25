// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef UCODECVIDEOENCODER_H
#define UCODECVIDEOENCODER_H

#include <QRect>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

/**
 * @brief ucodec H.264 编码器封装。
 *
 * 只负责“像素 -> 码流”：把捕获得到的紧排 RGBA/BGRA 帧（可选裁剪、
 * 偶数对齐、缩放）转换为 YUV420P，交给 ucodec 编码为 Annex-B 码流，
 * 并按 pts/dts/key 回调每一个编码包。
 *
 * 容器封装由 UcodecRecorder 负责；本类不接触文件、不接触封装器，
 * 便于单测和复用。
 */
class UcodecVideoEncoder
{
public:
    enum PixelFormat {
        RGBA8888 = 0, // R,G,B,A 字节序（紧密排布）
        BGRA8888,     // B,G,R,A 字节序（紧密排布）
    };

    struct Config {
        QString codec = QStringLiteral("h264");
        int width = 0;          // 输出宽度，必须是 >0 的偶数
        int height = 0;         // 输出高度，必须是 >0 的偶数
        int fps = 30;
        qint64 bitrate = 0;     // bits/s；<=0 时按分辨率/帧率估算
        bool preferHw = true;
        bool lowLatency = false; // false: 录制档（允许 B 帧，压缩率更高）
    };

    /**
     * @brief 编码包回调。data 只在回调期间有效。
     * pts/dts 使用调用方传入的 pts 单位（本类按帧序号传入）。
     */
    using PacketCallback = std::function<void(const uint8_t *data, int size,
                                              qint64 pts, qint64 dts, bool key)>;

    UcodecVideoEncoder();
    ~UcodecVideoEncoder();

    UcodecVideoEncoder(const UcodecVideoEncoder &) = delete;
    UcodecVideoEncoder &operator=(const UcodecVideoEncoder &) = delete;

    /**
     * @brief 创建 ucodec 编码器会话。
     * @return 成功返回 true；失败时通过 error 返回原因。
     */
    bool open(const Config &config, PacketCallback callback, QString *error = nullptr);

    /**
     * @brief 编码一帧。
     * @param data   紧排像素起始地址（行间无额外 padding 之外的对齐由 stride 描述）
     * @param stride 源图行字节数
     * @param crop   源图中的裁剪区域；为空表示整帧。输出尺寸在 open() 时确定。
     * @param pts    帧时间戳（通常为帧序号）
     */
    bool encodeFrame(const uint8_t *data, int stride, const QRect &crop,
                     qint64 pts, PixelFormat format = RGBA8888,
                     QString *error = nullptr);

    /**
     * @brief 冲刷编码器并回调剩余编码包。
     */
    bool flush(QString *error = nullptr);

    void close();

    bool isOpen() const { return m_encoder != nullptr; }

    int width() const { return m_width; }
    int height() const { return m_height; }
    int fps() const { return m_fps; }
    qint64 bitrate() const { return m_bitrate; }

    /** 实际使用的编码路径（软件 / VAAPI / ...），用于日志。 */
    QString pathName() const;

private:
    static void packetTrampoline(const uint8_t *data, int size, int64_t pts,
                                 int64_t dts, int key, void *user);
    void convertRgbaToYuv420p(const uint8_t *data, int stride, const QRect &crop,
                              PixelFormat format);
    void convertScaledToYuv420p(const uint8_t *data, int stride, const QRect &crop,
                                PixelFormat format);
    static qint64 defaultBitrate(int width, int height, int fps);

    void *m_encoder = nullptr; // ucodec_encoder*
    Config m_config;
    int m_width = 0;
    int m_height = 0;
    int m_fps = 0;
    qint64 m_bitrate = 0;
    PacketCallback m_callback;

    std::vector<uint8_t> m_yPlane;
    std::vector<uint8_t> m_uPlane;
    std::vector<uint8_t> m_vPlane;
};

#endif // UCODECVIDEOENCODER_H
