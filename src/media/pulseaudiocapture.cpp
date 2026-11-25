// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "pulseaudiocapture.h"

#include <QDebug>
#include <QElapsedTimer>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <pulse/introspect.h>
#include <pulse/simple.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace {

QString pulseError(const char *what, int error)
{
    return QStringLiteral("%1：%2")
            .arg(QString::fromUtf8(what), QString::fromUtf8(pa_strerror(error)));
}

struct SourceLookup
{
    int index = -1;
    QString name;
    bool done = false;
};

void sourceInfoCallback(pa_context *, const pa_source_info *info, int eol, void *user)
{
    auto *lookup = static_cast<SourceLookup *>(user);
    if (!lookup)
        return;
    if (eol) {
        lookup->done = true;
        return;
    }
    if (info && info->index == static_cast<uint32_t>(lookup->index))
        lookup->name = QString::fromUtf8(info->name);
}

// pactl 的 source index 在 libpulse 中需要先解析成设备名。
QString resolveSourceName(const QString &device)
{
    bool numeric = false;
    const int index = device.toInt(&numeric);
    if (!numeric || index < 0)
        return device;

    pa_mainloop *mainloop = pa_mainloop_new();
    if (!mainloop)
        return QString();
    pa_context *context = pa_context_new(pa_mainloop_get_api(mainloop),
                                         "deepin-screen-recorder");
    if (!context) {
        pa_mainloop_free(mainloop);
        return QString();
    }

    QString resolved;
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0) {
        QElapsedTimer timer;
        timer.start();
        while (pa_context_get_state(context) != PA_CONTEXT_READY
               && timer.elapsed() < 3000) {
            const pa_context_state_t state = pa_context_get_state(context);
            if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
                break;
            pa_mainloop_iterate(mainloop, 1, nullptr);
        }
        if (pa_context_get_state(context) == PA_CONTEXT_READY) {
            SourceLookup lookup;
            lookup.index = index;
            pa_operation *operation = pa_context_get_source_info_list(
                    context, sourceInfoCallback, &lookup);
            while (operation && !lookup.done && timer.elapsed() < 3000) {
                if (pa_operation_get_state(operation) == PA_OPERATION_DONE)
                    break;
                pa_mainloop_iterate(mainloop, 1, nullptr);
            }
            if (operation)
                pa_operation_unref(operation);
            resolved = lookup.name;
        }
    }
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    return resolved;
}

} // namespace

PulseAudioCapture::PulseAudioCapture() = default;

PulseAudioCapture::~PulseAudioCapture()
{
    stop();
}

bool PulseAudioCapture::open(const AudioCaptureOptions &audio, int sampleRate,
                             int channels, QString *error)
{
    stop();
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_hasError = false;
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error.clear();
    }

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_S16LE;
    spec.rate = static_cast<uint32_t>(sampleRate);
    spec.channels = static_cast<uint8_t>(channels);

    const int chunkBytes = kChunkFrames * channels * static_cast<int>(sizeof(int16_t));
    pa_buffer_attr attribute;
    std::memset(&attribute, 0, sizeof(attribute));
    attribute.maxlength = static_cast<uint32_t>(chunkBytes * 8);
    attribute.fragsize = static_cast<uint32_t>(chunkBytes);

    auto openStream = [&](const QString &device, QString *streamError) -> void * {
        QByteArray resolved = device.toUtf8();
        if (resolved == "default" || resolved == "@DEFAULT_SOURCE@")
            resolved.clear();
        int pulseErr = 0;
        pa_simple *stream = pa_simple_new(nullptr, "deepin-screen-recorder",
                                          PA_STREAM_RECORD,
                                          resolved.isEmpty() ? nullptr : resolved.constData(),
                                          "record", &spec, nullptr, &attribute, &pulseErr);
        if (!stream && streamError)
            *streamError = pulseError("无法打开音频输入设备", pulseErr);
        return stream;
    };

    if (audio.hasMicrophone()) {
        QString micError;
        m_micStream = openStream(audio.microphoneDevice, &micError);
        if (!m_micStream) {
            if (error)
                *error = micError;
            return false;
        }
    }
    if (audio.hasSystemAudio()) {
        QString device = audio.systemDevice.trimmed();
        if (device.isEmpty() || device == QStringLiteral("-1")) {
            if (error)
                *error = QStringLiteral("系统音频设备无效");
            return false;
        }
        if (device.at(0).isDigit()) {
            const QString name = resolveSourceName(device);
            if (!name.isEmpty())
                device = name;
        }
        QString systemError;
        m_systemStream = openStream(device, &systemError);
        if (!m_systemStream) {
            if (error)
                *error = systemError;
            return false;
        }
    }

    if (!m_micStream && !m_systemStream) {
        if (error)
            *error = QStringLiteral("没有可用的音频输入");
        return false;
    }
    return true;
}

bool PulseAudioCapture::start(FrameCallback callback, QString *error)
{
    if (m_running.load())
        return false;
    if (!m_micStream && !m_systemStream) {
        if (error)
            *error = QStringLiteral("音频采集尚未初始化");
        return false;
    }
    m_callback = std::move(callback);
    m_running = true;
    m_thread = std::thread(&PulseAudioCapture::captureLoop, this);
    return true;
}

void PulseAudioCapture::captureLoop()
{
    const int samples = kChunkFrames * m_channels;
    const int bytes = samples * static_cast<int>(sizeof(int16_t));
    std::vector<int16_t> mixed(static_cast<size_t>(samples));
    std::vector<int16_t> scratch(static_cast<size_t>(samples));

    auto readStream = [&](void *stream, int16_t *buffer) -> bool {
        int pulseErr = 0;
        if (pa_simple_read(static_cast<pa_simple *>(stream), buffer, static_cast<size_t>(bytes), &pulseErr) < 0) {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_error = pulseError("读取音频数据失败", pulseErr);
            m_hasError = true;
            return false;
        }
        return true;
    };

    while (m_running.load()) {
        if (m_micStream && m_systemStream) {
            if (!readStream(m_systemStream, mixed.data())
                    || !readStream(m_micStream, scratch.data()))
                break;
            for (int i = 0; i < samples; ++i) {
                const int sum = static_cast<int>(mixed[i]) + static_cast<int>(scratch[i]);
                mixed[i] = static_cast<int16_t>(std::clamp(sum, INT16_MIN, INT16_MAX));
            }
        } else {
            void *stream = m_micStream ? m_micStream : m_systemStream;
            if (!readStream(stream, mixed.data()))
                break;
        }

        if (m_callback && !m_callback(mixed.data(), kChunkFrames))
            break;
    }

    m_running = false;
    if (m_micStream) {
        pa_simple_free(static_cast<pa_simple *>(m_micStream));
        m_micStream = nullptr;
    }
    if (m_systemStream) {
        pa_simple_free(static_cast<pa_simple *>(m_systemStream));
        m_systemStream = nullptr;
    }
}

void PulseAudioCapture::stop()
{
    m_running = false;
    if (m_thread.joinable())
        m_thread.join();
    m_callback = nullptr;
    if (m_micStream) {
        pa_simple_free(static_cast<pa_simple *>(m_micStream));
        m_micStream = nullptr;
    }
    if (m_systemStream) {
        pa_simple_free(static_cast<pa_simple *>(m_systemStream));
        m_systemStream = nullptr;
    }
}

QString PulseAudioCapture::lastError() const
{
    std::lock_guard<std::mutex> lock(m_errorMutex);
    return m_error;
}
