// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11screenframegrabber.h"

#include <QDebug>

#include "../utils/log.h"

#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <cstring>

namespace {

int maskShift(unsigned long mask)
{
    if (!mask)
        return 0;
    int shift = 0;
    while ((mask & 1UL) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

int maskBits(unsigned long mask)
{
    int bits = 0;
    while (mask) {
        bits += static_cast<int>(mask & 1UL);
        mask >>= 1;
    }
    return bits;
}

inline quint8 scaleChannel(unsigned long pixel, unsigned long mask, int shift, int maxValue)
{
    if (!mask || maxValue <= 0)
        return 0;
    const unsigned long value = (pixel & mask) >> shift;
    return static_cast<quint8>((value * 255UL + static_cast<unsigned long>(maxValue) / 2)
                               / static_cast<unsigned long>(maxValue));
}

} // namespace

X11ScreenFrameGrabber::X11ScreenFrameGrabber()
{
    std::memset(&m_shm, 0, sizeof(m_shm));
}

X11ScreenFrameGrabber::~X11ScreenFrameGrabber()
{
    close();
}

bool X11ScreenFrameGrabber::open(const QRect &region, bool includeCursor, QString *error)
{
    close();

    if (region.width() < 2 || region.height() < 2) {
        if (error)
            *error = QStringLiteral("X11 采集区域非法");
        return false;
    }

    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        if (error)
            *error = QStringLiteral("无法连接 X11 显示");
        return false;
    }

    const int screen = DefaultScreen(m_display);
    m_root = DefaultRootWindow(m_display);
    const int screenWidth = DisplayWidth(m_display, screen);
    const int screenHeight = DisplayHeight(m_display, screen);
    if (region.x() < 0 || region.y() < 0
            || region.right() >= screenWidth || region.bottom() >= screenHeight) {
        if (error)
            *error = QStringLiteral("X11 采集区域超出屏幕范围");
        close();
        return false;
    }

    m_region = region;
    m_includeCursor = includeCursor;
    int eventBase = 0;
    int errorBase = 0;
    m_hasXfixes = XFixesQueryExtension(m_display, &eventBase, &errorBase);

    // XShm 不可用时保持 m_image 为空，grab() 会退回 XGetImage。
    if (XShmQueryExtension(m_display)) {
        QString shmError;
        if (!initShm(&shmError))
            qCWarning(dsrApp) << "X11ScreenFrameGrabber: XShm 不可用:" << shmError;
    }
    return true;
}

bool X11ScreenFrameGrabber::initShm(QString *error)
{
    const int screen = DefaultScreen(m_display);
    const int depth = DefaultDepth(m_display, screen);
    Visual *visual = DefaultVisual(m_display, screen);

    m_image = XShmCreateImage(m_display, visual, depth, ZPixmap, nullptr, &m_shm,
                              static_cast<unsigned int>(m_region.width()),
                              static_cast<unsigned int>(m_region.height()));
    if (!m_image) {
        if (error)
            *error = QStringLiteral("无法创建 XShm 图像");
        return false;
    }

    m_shm.shmid = shmget(IPC_PRIVATE,
                         static_cast<size_t>(m_image->bytes_per_line) * m_image->height,
                         IPC_CREAT | 0600);
    if (m_shm.shmid < 0) {
        if (error)
            *error = QStringLiteral("无法创建共享内存");
        destroyImage();
        return false;
    }

    m_shm.shmaddr = static_cast<char *>(shmat(m_shm.shmid, nullptr, 0));
    if (m_shm.shmaddr == reinterpret_cast<char *>(-1)) {
        if (error)
            *error = QStringLiteral("无法映射共享内存");
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        destroyImage();
        return false;
    }
    m_shm.readOnly = False;
    m_image->data = m_shm.shmaddr;

    if (!XShmAttach(m_display, &m_shm)) {
        if (error)
            *error = QStringLiteral("XShmAttach 失败");
        shmdt(m_shm.shmaddr);
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        m_shm.shmaddr = nullptr;
        destroyImage();
        return false;
    }
    XSync(m_display, False);
    m_shmAttached = true;
    // 标记删除，最后一个 detach 的内核对象自动回收。
    shmctl(m_shm.shmid, IPC_RMID, nullptr);
    return true;
}

void X11ScreenFrameGrabber::destroyImage()
{
    if (m_image) {
        if (m_shmAttached) {
            XShmDetach(m_display, &m_shm);
            m_shmAttached = false;
        }
        XDestroyImage(m_image);
        m_image = nullptr;
    }
    if (m_shm.shmaddr) {
        shmdt(m_shm.shmaddr);
        m_shm.shmaddr = nullptr;
    }
}

void X11ScreenFrameGrabber::close()
{
    destroyImage();
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
    m_root = 0;
    m_hasXfixes = false;
    m_shmGrabFailed = false;
}

void X11ScreenFrameGrabber::convert(XImage *image, QByteArray *rgba)
{
    const int width = m_region.width();
    const int height = m_region.height();
    rgba->resize(width * height * 4);

    const int redShift = maskShift(image->red_mask);
    const int greenShift = maskShift(image->green_mask);
    const int blueShift = maskShift(image->blue_mask);
    const int redMax = static_cast<int>((1UL << maskBits(image->red_mask)) - 1);
    const int greenMax = static_cast<int>((1UL << maskBits(image->green_mask)) - 1);
    const int blueMax = static_cast<int>((1UL << maskBits(image->blue_mask)) - 1);

    uchar *out = reinterpret_cast<uchar *>(rgba->data());
    const bool fast32 = image->bits_per_pixel == 32 && image->byte_order == LSBFirst;
    const bool fast24 = image->bits_per_pixel == 24 && image->byte_order == LSBFirst;
    const bool standardMask = image->red_mask == 0xff0000UL
            && image->green_mask == 0x00ff00UL
            && image->blue_mask == 0x000000ffUL;

    // 常见的 X11 32/24bpp 布局就是内存里的 B,G,R(,X)。直接按字节重排，
    // 避免每个像素做三次掩码移位和缩放，2M 像素区域可省下约 20ms/帧。
    if (fast32 && standardMask) {
        for (int y = 0; y < height; ++y) {
            const uchar *row = reinterpret_cast<const uchar *>(image->data)
                    + static_cast<qint64>(y) * image->bytes_per_line;
            uchar *dst = out + static_cast<qint64>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                dst[0] = row[2];
                dst[1] = row[1];
                dst[2] = row[0];
                dst[3] = 255;
                row += 4;
                dst += 4;
            }
        }
        return;
    }
    if (fast24 && standardMask) {
        for (int y = 0; y < height; ++y) {
            const uchar *row = reinterpret_cast<const uchar *>(image->data)
                    + static_cast<qint64>(y) * image->bytes_per_line;
            uchar *dst = out + static_cast<qint64>(y) * width * 4;
            for (int x = 0; x < width; ++x) {
                dst[0] = row[2];
                dst[1] = row[1];
                dst[2] = row[0];
                dst[3] = 255;
                row += 3;
                dst += 4;
            }
        }
        return;
    }

    for (int y = 0; y < height; ++y) {
        const uchar *row = reinterpret_cast<const uchar *>(image->data)
                + static_cast<qint64>(y) * image->bytes_per_line;
        uchar *dst = out + static_cast<qint64>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            unsigned long pixel = 0;
            if (fast32) {
                std::memcpy(&pixel, row + static_cast<qint64>(x) * 4, 4);
            } else if (fast24) {
                uchar bytes[4] = {0, 0, 0, 0};
                std::memcpy(bytes, row + static_cast<qint64>(x) * 3, 3);
                std::memcpy(&pixel, bytes, 4);
            } else {
                pixel = XGetPixel(image, x, y);
            }
            dst[x * 4 + 0] = scaleChannel(pixel, image->red_mask, redShift, redMax);
            dst[x * 4 + 1] = scaleChannel(pixel, image->green_mask, greenShift, greenMax);
            dst[x * 4 + 2] = scaleChannel(pixel, image->blue_mask, blueShift, blueMax);
            dst[x * 4 + 3] = 255;
        }
    }
}

void X11ScreenFrameGrabber::blendCursor(QByteArray *rgba)
{
    if (!m_hasXfixes || !rgba)
        return;

    XFixesCursorImage *cursor = XFixesGetCursorImage(m_display);
    if (!cursor)
        return;

    const int width = m_region.width();
    const int height = m_region.height();
    uchar *pixels = reinterpret_cast<uchar *>(rgba->data());
    const int originX = cursor->x - cursor->xhot - m_region.x();
    const int originY = cursor->y - cursor->yhot - m_region.y();

    for (int y = 0; y < static_cast<int>(cursor->height); ++y) {
        const int dstY = originY + y;
        if (dstY < 0 || dstY >= height)
            continue;
        for (int x = 0; x < static_cast<int>(cursor->width); ++x) {
            const int dstX = originX + x;
            if (dstX < 0 || dstX >= width)
                continue;
            const unsigned long pixel = cursor->pixels[y * cursor->width + x];
            const int alpha = static_cast<int>((pixel >> 24) & 0xff);
            if (alpha == 0)
                continue;
            const int srcR = static_cast<int>((pixel >> 16) & 0xff);
            const int srcG = static_cast<int>((pixel >> 8) & 0xff);
            const int srcB = static_cast<int>(pixel & 0xff);
            uchar *dst = pixels + static_cast<qint64>(dstY) * width * 4 + dstX * 4;
            for (int c = 0; c < 3; ++c) {
                const int src = c == 0 ? srcR : (c == 1 ? srcG : srcB);
                dst[c] = static_cast<uchar>((src * alpha + dst[c] * (255 - alpha) + 127) / 255);
            }
            dst[3] = 255;
        }
    }
    XFree(cursor);
}

bool X11ScreenFrameGrabber::grab(QByteArray *rgba, int *stride, QString *error)
{
    if (!m_display || !rgba || !stride) {
        if (error)
            *error = QStringLiteral("X11 采集尚未初始化");
        return false;
    }

    Status status = 0;
    if (m_shmAttached && !m_shmGrabFailed) {
        status = XShmGetImage(m_display, m_root, m_image, m_region.x(), m_region.y(),
                              AllPlanes);
        if (status == 0) {
            m_shmGrabFailed = true;
            qCWarning(dsrApp) << "X11ScreenFrameGrabber: XShmGetImage 失败，改用 XGetImage";
        }
    }

    XImage *image = m_image;
    bool owned = false;
    if (status == 0) {
        image = XGetImage(m_display, m_root, m_region.x(), m_region.y(),
                          static_cast<unsigned int>(m_region.width()),
                          static_cast<unsigned int>(m_region.height()),
                          AllPlanes, ZPixmap);
        if (!image) {
            if (error)
                *error = QStringLiteral("XGetImage 抓取失败");
            return false;
        }
        owned = true;
    }

    convert(image, rgba);
    if (owned)
        XDestroyImage(image);

    if (m_includeCursor)
        blendCursor(rgba);

    *stride = m_region.width() * 4;
    return true;
}
