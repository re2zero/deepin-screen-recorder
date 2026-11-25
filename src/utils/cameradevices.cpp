// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cameradevices.h"

#include <QByteArray>
#include <QDir>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace CameraDevices {

bool hasCaptureDevice()
{
    const QStringList nodes = QDir(QStringLiteral("/dev"))
            .entryList({QStringLiteral("video*")}, QDir::System | QDir::Files);
    for (const QString &node : nodes) {
        const QByteArray path = QStringLiteral("/dev/%1").arg(node).toLocal8Bit();
        const int fd = ::open(path.constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;

        v4l2_capability capability;
        bool capture = false;
        if (::ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0) {
            const quint32 caps = capability.device_caps
                    ? capability.device_caps : capability.capabilities;
            capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0
                    || (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
        }
        ::close(fd);
        if (capture)
            return true;
    }
    return false;
}

} // namespace CameraDevices
