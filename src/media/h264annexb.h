// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef H264ANNEXB_H
#define H264ANNEXB_H

#include <QByteArray>

#include <cstdint>

/**
 * @brief H.264 Annex-B 码流辅助：拆分 NAL、生成 avcC、转 AVCC 样本。
 *
 * ucodec 输出 Annex-B，并在关键帧包里携带 SPS/PPS；MP4 与 Matroska 都需要
 * 把参数集放进 CodecPrivate（avcC），样本本身使用 4 字节长度前缀。
 */
namespace H264AnnexB {

constexpr int kNalSlice = 1;
constexpr int kNalIdr = 5;
constexpr int kNalSei = 6;
constexpr int kNalSps = 7;
constexpr int kNalPps = 8;
constexpr int kNalAud = 9;

/**
 * @brief 从 Annex-B 访问单元中提取第一组 SPS/PPS。
 * @return 找到 SPS 与 PPS 时返回 true；参数集可跨包保留由调用方负责。
 */
bool extractParameterSets(const uint8_t *data, int size,
                          QByteArray *sps, QByteArray *pps);

/**
 * @brief 生成 AVCDecoderConfigurationRecord（avcC box 的负载）。
 */
QByteArray makeAvcC(const QByteArray &sps, const QByteArray &pps);

/**
 * @brief Annex-B 访问单元转 AVCC（4 字节长度前缀）。
 *
 * 参数集与 AUD 不进入样本（参数集已写入 avcC）。没有 VCL NAL 时返回空。
 */
QByteArray toAvcc(const uint8_t *data, int size);

} // namespace H264AnnexB

#endif // H264ANNEXB_H
