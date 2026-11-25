// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "aacencoder.h"

#include <QDebug>

#include <fdk-aac/aacenc_lib.h>

#include <cstring>
#include <vector>

namespace {

QString fdkError(const char *what, AACENC_ERROR error)
{
    return QStringLiteral("%1（FDK-AAC 错误码 %2）")
            .arg(QString::fromUtf8(what))
            .arg(static_cast<int>(error));
}

} // namespace

AacEncoder::AacEncoder() = default;

AacEncoder::~AacEncoder()
{
    close();
}

bool AacEncoder::open(int sampleRate, int channels, int bitrate, QString *error)
{
    close();
    if (sampleRate <= 0 || channels <= 0) {
        if (error)
            *error = QStringLiteral("非法的 AAC 编码参数");
        return false;
    }

    HANDLE_AACENCODER handle = nullptr;
    AACENC_ERROR status = aacEncOpen(&handle, 0, static_cast<UINT>(channels));
    if (status != AACENC_OK) {
        if (error)
            *error = fdkError("无法创建 AAC 编码器", status);
        return false;
    }

    const CHANNEL_MODE mode = channels == 1 ? MODE_1 : MODE_2;
    status = aacEncoder_SetParam(handle, AACENC_AOT, AOT_AAC_LC);
    if (status == AACENC_OK)
        status = aacEncoder_SetParam(handle, AACENC_SAMPLERATE,
                                     static_cast<UINT>(sampleRate));
    if (status == AACENC_OK)
        status = aacEncoder_SetParam(handle, AACENC_CHANNELMODE, mode);
    if (status == AACENC_OK)
        status = aacEncoder_SetParam(handle, AACENC_CHANNELORDER, 1);
    if (status == AACENC_OK)
        status = aacEncoder_SetParam(handle, AACENC_BITRATE,
                                     static_cast<UINT>(bitrate > 0 ? bitrate : 128000));
    if (status == AACENC_OK)
        status = aacEncoder_SetParam(handle, AACENC_TRANSMUX, TT_MP4_RAW);
    if (status != AACENC_OK) {
        if (error)
            *error = fdkError("AAC 编码器参数设置失败", status);
        aacEncClose(&handle);
        return false;
    }

    status = aacEncEncode(handle, nullptr, nullptr, nullptr, nullptr);
    if (status != AACENC_OK) {
        if (error)
            *error = fdkError("AAC 编码器初始化失败", status);
        aacEncClose(&handle);
        return false;
    }

    AACENC_InfoStruct info;
    std::memset(&info, 0, sizeof(info));
    status = aacEncInfo(handle, &info);
    if (status != AACENC_OK) {
        if (error)
            *error = fdkError("无法获取 AAC 编码配置", status);
        aacEncClose(&handle);
        return false;
    }

    m_handle = handle;
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_config = QByteArray(reinterpret_cast<const char *>(info.confBuf),
                          static_cast<int>(info.confSize));
    return true;
}

void AacEncoder::close()
{
    if (m_handle) {
        HANDLE_AACENCODER handle = static_cast<HANDLE_AACENCODER>(m_handle);
        aacEncClose(&handle);
        m_handle = nullptr;
    }
    m_config.clear();
    m_sampleRate = 0;
    m_channels = 0;
}

bool AacEncoder::encode(const int16_t *pcm, int frames, QList<QByteArray> *outputs,
                        QString *error)
{
    if (!m_handle || !pcm || frames <= 0 || !outputs) {
        if (error)
            *error = QStringLiteral("AAC 编码器尚未就绪");
        return false;
    }

    HANDLE_AACENCODER handle = static_cast<HANDLE_AACENCODER>(m_handle);
    std::vector<unsigned char> output(16384);

    void *inputPtr = const_cast<int16_t *>(pcm);
    INT inputId = IN_AUDIO_DATA;
    INT inputSize = frames * m_channels * static_cast<int>(sizeof(int16_t));
    INT inputElementSize = static_cast<INT>(sizeof(int16_t));
    AACENC_BufDesc inputDesc;
    std::memset(&inputDesc, 0, sizeof(inputDesc));
    inputDesc.numBufs = 1;
    inputDesc.bufs = &inputPtr;
    inputDesc.bufferIdentifiers = &inputId;
    inputDesc.bufSizes = &inputSize;
    inputDesc.bufElSizes = &inputElementSize;

    void *outputPtr = output.data();
    INT outputId = OUT_BITSTREAM_DATA;
    INT outputSize = static_cast<INT>(output.size());
    INT outputElementSize = 1;
    AACENC_BufDesc outputDesc;
    std::memset(&outputDesc, 0, sizeof(outputDesc));
    outputDesc.numBufs = 1;
    outputDesc.bufs = &outputPtr;
    outputDesc.bufferIdentifiers = &outputId;
    outputDesc.bufSizes = &outputSize;
    outputDesc.bufElSizes = &outputElementSize;

    AACENC_InArgs inputArgs;
    std::memset(&inputArgs, 0, sizeof(inputArgs));
    inputArgs.numInSamples = frames * m_channels;
    AACENC_OutArgs outputArgs;
    std::memset(&outputArgs, 0, sizeof(outputArgs));

    const AACENC_ERROR status = aacEncEncode(handle, &inputDesc, &outputDesc,
                                             &inputArgs, &outputArgs);
    if (status != AACENC_OK && status != AACENC_ENCODE_EOF) {
        if (error)
            *error = fdkError("AAC 编码失败", status);
        return false;
    }
    if (outputArgs.numOutBytes > 0) {
        outputs->append(QByteArray(reinterpret_cast<const char *>(output.data()),
                                   outputArgs.numOutBytes));
    }
    return true;
}

bool AacEncoder::flush(QList<QByteArray> *outputs, QString *error)
{
    if (!m_handle || !outputs)
        return false;

    HANDLE_AACENCODER handle = static_cast<HANDLE_AACENCODER>(m_handle);
    std::vector<unsigned char> output(16384);

    for (int i = 0; i < 64; ++i) {
        void *outputPtr = output.data();
        INT outputId = OUT_BITSTREAM_DATA;
        INT outputSize = static_cast<INT>(output.size());
        INT outputElementSize = 1;
        AACENC_BufDesc outputDesc;
        std::memset(&outputDesc, 0, sizeof(outputDesc));
        outputDesc.numBufs = 1;
        outputDesc.bufs = &outputPtr;
        outputDesc.bufferIdentifiers = &outputId;
        outputDesc.bufSizes = &outputSize;
        outputDesc.bufElSizes = &outputElementSize;

        AACENC_InArgs inputArgs;
        std::memset(&inputArgs, 0, sizeof(inputArgs));
        inputArgs.numInSamples = -1;
        AACENC_OutArgs outputArgs;
        std::memset(&outputArgs, 0, sizeof(outputArgs));

        const AACENC_ERROR status = aacEncEncode(handle, nullptr, &outputDesc,
                                                 &inputArgs, &outputArgs);
        if (outputArgs.numOutBytes > 0) {
            outputs->append(QByteArray(reinterpret_cast<const char *>(output.data()),
                                       outputArgs.numOutBytes));
        }
        if (status == AACENC_ENCODE_EOF)
            return true;
        if (status != AACENC_OK) {
            if (error)
                *error = fdkError("AAC 编码器冲刷失败", status);
            return false;
        }
        if (outputArgs.numOutBytes == 0)
            return true;
    }
    if (error)
        *error = QStringLiteral("AAC 编码器冲刷超时");
    return false;
}
