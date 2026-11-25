// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CAMERADEVICES_H
#define CAMERADEVICES_H

/**
 * @brief 摄像头枚举（V4L2），替代 QtMultimedia 的 QMediaDevices。
 */
namespace CameraDevices {

/** 是否存在可用的 V4L2 采集设备。 */
bool hasCaptureDevice();

} // namespace CameraDevices

#endif // CAMERADEVICES_H
