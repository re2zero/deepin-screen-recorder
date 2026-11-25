// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "h264annexb.h"

#include <QList>

#include <cstring>

namespace {

struct NalUnit
{
    int type = 0;
    const uint8_t *data = nullptr;
    int size = 0;
};

// Annex-B 起始码：00 00 01 或 00 00 00 01。返回起始码之后的 NAL 起点。
int findStartCode(const uint8_t *data, int size, int from, int *startCodeLength)
{
    for (int i = from; i + 2 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *startCodeLength = 3;
                return i + 3;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                *startCodeLength = 4;
                return i + 4;
            }
        }
    }
    return -1;
}

QList<NalUnit> splitNals(const uint8_t *data, int size)
{
    QList<NalUnit> nals;
    if (!data || size <= 0)
        return nals;

    int startCodeLength = 0;
    int nalStart = findStartCode(data, size, 0, &startCodeLength);
    while (nalStart >= 0 && nalStart < size) {
        int nextStartCodeLength = 0;
        int next = findStartCode(data, size, nalStart, &nextStartCodeLength);
        int nalEnd = next >= 0 ? next - nextStartCodeLength : size;
        // 起始码前的 trailing zero 不属于 NAL 负载。
        while (nalEnd > nalStart && data[nalEnd - 1] == 0)
            --nalEnd;
        if (nalEnd > nalStart) {
            NalUnit nal;
            nal.type = data[nalStart] & 0x1f;
            nal.data = data + nalStart;
            nal.size = nalEnd - nalStart;
            nals.append(nal);
        }
        if (next < 0)
            break;
        nalStart = next;
    }
    return nals;
}

void appendU32(QByteArray *buffer, quint32 value)
{
    buffer->append(static_cast<char>((value >> 24) & 0xff));
    buffer->append(static_cast<char>((value >> 16) & 0xff));
    buffer->append(static_cast<char>((value >> 8) & 0xff));
    buffer->append(static_cast<char>(value & 0xff));
}

} // namespace

namespace H264AnnexB {

bool extractParameterSets(const uint8_t *data, int size,
                          QByteArray *sps, QByteArray *pps)
{
    if (!sps || !pps)
        return false;

    for (const NalUnit &nal : splitNals(data, size)) {
        if (nal.type == kNalSps && sps->isEmpty()) {
            *sps = QByteArray(reinterpret_cast<const char *>(nal.data), nal.size);
        } else if (nal.type == kNalPps && pps->isEmpty()) {
            *pps = QByteArray(reinterpret_cast<const char *>(nal.data), nal.size);
        }
    }
    return !sps->isEmpty() && !pps->isEmpty();
}

QByteArray makeAvcC(const QByteArray &sps, const QByteArray &pps)
{
    if (sps.size() < 4 || pps.isEmpty())
        return QByteArray();

    QByteArray avcC;
    avcC.reserve(11 + sps.size() + pps.size());
    avcC.append(char(1));                 // configurationVersion
    avcC.append(sps.at(1));               // AVCProfileIndication
    avcC.append(sps.at(2));               // profile_compatibility
    avcC.append(sps.at(3));               // AVCLevelIndication
    avcC.append(char(0xff));              // lengthSizeMinusOne = 3
    avcC.append(char(0xe1));              // numOfSequenceParameterSets = 1
    avcC.append(static_cast<char>((sps.size() >> 8) & 0xff));
    avcC.append(static_cast<char>(sps.size() & 0xff));
    avcC.append(sps);
    avcC.append(char(1));                 // numOfPictureParameterSets
    avcC.append(static_cast<char>((pps.size() >> 8) & 0xff));
    avcC.append(static_cast<char>(pps.size() & 0xff));
    avcC.append(pps);
    return avcC;
}

QByteArray toAvcc(const uint8_t *data, int size)
{
    QByteArray avcc;
    bool hasSlice = false;
    for (const NalUnit &nal : splitNals(data, size)) {
        if (nal.type == kNalSps || nal.type == kNalPps || nal.type == kNalAud)
            continue;
        if (nal.type >= kNalSlice && nal.type <= kNalIdr)
            hasSlice = true;
        appendU32(&avcc, static_cast<quint32>(nal.size));
        avcc.append(reinterpret_cast<const char *>(nal.data), nal.size);
    }
    return hasSlice ? avcc : QByteArray();
}

} // namespace H264AnnexB
