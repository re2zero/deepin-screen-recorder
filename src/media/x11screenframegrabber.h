// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef X11SCREENFRAMEGRABBER_H
#define X11SCREENFRAMEGRABBER_H

#include <QByteArray>
#include <QDebug>
#include <QRect>
#include <QString>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

/**
 * @brief X11 屏幕采集：XShmGetImage 抓取区域，可选叠加光标。
 *
 * 输出紧排 RGBA8888。XShm 不可用时退回 XGetImage。
 */
class X11ScreenFrameGrabber
{
public:
    X11ScreenFrameGrabber();
    ~X11ScreenFrameGrabber();

    X11ScreenFrameGrabber(const X11ScreenFrameGrabber &) = delete;
    X11ScreenFrameGrabber &operator=(const X11ScreenFrameGrabber &) = delete;

    bool open(const QRect &region, bool includeCursor, QString *error = nullptr);
    void close();

    bool isOpen() const { return m_display != nullptr; }

    /** 抓取一帧到 rgba（x11 输出为 RGBA8888，stride = width * 4）。 */
    bool grab(QByteArray *rgba, int *stride, QString *error = nullptr);

private:
    bool initShm(QString *error);
    void destroyImage();
    void convert(XImage *image, QByteArray *rgba);
    void blendCursor(QByteArray *rgba);

    Display *m_display = nullptr;
    Window m_root = 0;
    QRect m_region;
    bool m_includeCursor = false;
    bool m_hasXfixes = false;
    bool m_shmAttached = false;
    XImage *m_image = nullptr;
    XShmSegmentInfo m_shm;
};

#endif // X11SCREENFRAMEGRABBER_H
