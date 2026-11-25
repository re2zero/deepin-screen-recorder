// Copyright (C) 2020 ~ 2021 Uniontech Software Technology Co.,Ltd.
// SPDX-FileCopyrightText: 2022 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "record_process.h"
#include "utils/configsettings.h"
#include "utils.h"
#include "utils/audioutils.h"
#include "media/x11recordworker.h"
#include "utils/log.h"
#include "ext-image-capture/extcapturerecorder.h"
#include "ext-image-capture/extcapturebridge.h"
#include <QApplication>
#include <QDate>
#include <QDebug>
#include <QDir>
#include <QStandardPaths>
#include <QtDBus>
#include <QScreen>
#include <QMetaType>
#include <QClipboard>
#include <QUrl>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <dlfcn.h>
#include <signal.h>

const int RecordProcess::RECORD_MOUSE_NULL = 0;
const int RecordProcess::RECORD_MOUSE_CURSE = 1;
const int RecordProcess::RECORD_MOUSE_CHECK = 2;
const int RecordProcess::RECORD_MOUSE_CURSE_CHECK = 3;

const int RecordProcess::RECORD_FRAMERATE_5 = 5;
const int RecordProcess::RECORD_FRAMERATE_10 = 10;
const int RecordProcess::RECORD_FRAMERATE_20 = 20;
const int RecordProcess::RECORD_FRAMERATE_24 = 24;
const int RecordProcess::RECORD_FRAMERATE_30 = 30;

// 用于任务栏图标DBus通信调用的函数
const QString DBUS_FUNC_ON_START = "onStart";
const QString DBUS_FUNC_ON_STOP = "onStop";
const QString DBUS_FUNC_ON_RECORDING = "onRecording";
const QString DBUS_FUNC_ON_PAUSE = "onPause";

/**
   @brief 调用托盘图标"录制中"的对应函数，改变显示状态
 */
static QDBusMessage callTrayTimeIcon(const QString &function)
{
    qCDebug(dsrApp) << "Calling tray time icon function:" << function;
    QDBusInterface timeInterface("com.deepin.ScreenRecorder.time",
                                 "/com/deepin/ScreenRecorder/time",
                                 "com.deepin.ScreenRecorder.time",
                                 QDBusConnection::sessionBus());
    return timeInterface.call(function);
}

/**
   @brief 调用托盘图标"截图"的对应函数，改变显示状态
 */
static QDBusMessage callTrayShotIcon(const QString &function)
{
    qCDebug(dsrApp) << "Calling tray shot icon function:" << function;
    QDBusInterface shotInterface("com.deepin.ShotRecorder.PanelStatus",
                                 "/com/deepin/ShotRecorder/PanelStatus",
                                 "com.deepin.ShotRecorder.PanelStatus",
                                 QDBusConnection::sessionBus());
    return shotInterface.call(function);
}

/**
   @brief 调用托盘图标"录屏"的对应函数，改变显示状态
 */
static QDBusMessage callTrayRecorderIcon(const QString &function)
{
    qCDebug(dsrApp) << "Calling tray recorder icon function:" << function;
    QDBusInterface recorderInterface("com.deepin.ShotRecorder.Recorder.PanelStatus",
                                     "/com/deepin/ShotRecorder/Recorder/PanelStatus",
                                     "com.deepin.ShotRecorder.Recorder.PanelStatus",
                                     QDBusConnection::sessionBus());
    return recorderInterface.call(function);
}

static void closeFdoNotification(quint32 id)
{
    if (id == RecordProcess::RECORD_SAVING_NOTIFY_ID_INVALID)
        return;
    QDBusInterface notification("org.freedesktop.Notifications",
                                "/org/freedesktop/Notifications",
                                "org.freedesktop.Notifications",
                                QDBusConnection::sessionBus());
    notification.call("CloseNotification", id);
}

void RecordProcess::setRecordSavingNotifyId(quint32 id)
{
    m_recordSavingNotifyId = id;
}

RecordProcess::RecordProcess(QObject *parent) : QObject(parent)
{
    qCDebug(dsrApp) << "Record process control class initialized.";
    m_settings = ConfigSettings::instance();
    qCDebug(dsrApp) << "ConfigSettings instance obtained.";

    saveTempDir = QStandardPaths::standardLocations(QStandardPaths::TempLocation).first();
    qCDebug(dsrApp) << "Temporary save directory:" << saveTempDir;
    displayNumber = QString(std::getenv("DISPLAY"));
    qCDebug(dsrApp) << "Display number:" << displayNumber;


    m_recordingFlag = false;
    m_framerate = 0;
    qCDebug(dsrApp) << "Recording flag and framerate initialized.";
}

RecordProcess::~RecordProcess()
{
    qCDebug(dsrApp) << "RecordProcess destructor called.";
    stopX11Recording();
}
//设置录屏的基础信息
void RecordProcess::setRecordInfo(const QRect &recordRect, const QString &filename)
{
    qCDebug(dsrApp) << "Setting record information. Record Rect:" << recordRect << ", Filename:" << filename;
    m_recordRect = recordRect;
    saveAreaName = filename;
    m_recordType = m_settings->getValue("recorder", "format").toInt();
    m_audioType = m_settings->getValue("recorder", "audio").toInt();
    m_mouseType = m_settings->getValue("recorder", "cursor").toInt();
    m_framerate = m_settings->getValue("recorder", "frame_rate").toInt();
    qCDebug(dsrApp) << "Record type, audio type, mouse type, and framerate obtained.";
}

//录屏结束后弹出通知
void RecordProcess::onRecordFinish()
{
    qCDebug(dsrApp) << "Record finish callback: moving file to save directory.";
    // Move file to save directory.
    QString newSavePath = QDir(saveDir).filePath(saveBaseName);
    QFile::remove(newSavePath);
    if (!QFile::rename(savePath, newSavePath)) {
        qCWarning(dsrApp) << "Failed to move recording from" << savePath << "to" << newSavePath;
        newSavePath = savePath;
    }
    exitRecord(newSavePath);
}

//x11录制视频（ucodec + 自研 MP4/Matroska 封装）
void RecordProcess::recordVideo()
{
    qCDebug(dsrApp) << "Starting X11 ucodec video recording.";
    initProcess();

    AudioUtils audioUtils;
    QString systemAudioChannel = audioUtils.currentAudioChannel();
    if (systemAudioChannel == "-1") {
        qCWarning(dsrApp) << "Current system audio channel error!";
        if (m_audioType == Utils::kSystemAudio) {
            m_audioType = Utils::kNoAudio;
            qCWarning(dsrApp) << "Selected audio changed: System audio changed to no audio!";
        } else if (m_audioType == Utils::kMicAndSystemAudio) {
            m_audioType = Utils::kMic;
            qCWarning(dsrApp) << "Selected audio changed: Mix audio changed to microphone only!";
        }
    }

    AudioCaptureOptions audio;
    switch (m_audioType) {
    case Utils::kMic:
        audio.type = AudioCaptureOptions::Microphone;
        break;
    case Utils::kSystemAudio:
        audio.type = AudioCaptureOptions::SystemAudio;
        break;
    case Utils::kMicAndSystemAudio:
        audio.type = AudioCaptureOptions::MicrophoneAndSystemAudio;
        break;
    default:
        audio.type = AudioCaptureOptions::NoAudio;
        break;
    }
    audio.systemDevice = systemAudioChannel;
    audio.microphoneDevice = QStringLiteral("default");

    m_x11Thread = new QThread(this);
    m_x11Worker = new X11RecordWorker;
    m_x11Worker->moveToThread(m_x11Thread);
    connect(m_x11Thread, &QThread::finished, m_x11Worker, &QObject::deleteLater);
    connect(m_x11Worker, &X11RecordWorker::errorOccurred, this,
            [](const QString &message) {
                qCCritical(dsrApp) << "X11 recording error:" << message;
            });
    m_x11Thread->start();

    const QRect region = m_recordRect;
    const int frameRate = m_framerate;
    const bool includeCursor = !(m_mouseType == RECORD_MOUSE_NULL
                                 || m_mouseType == RECORD_MOUSE_CHECK);
    const QString path = savePath;
    X11RecordWorker *worker = m_x11Worker;
    QMetaObject::invokeMethod(worker, [worker, region, frameRate, includeCursor, path, audio]() {
        worker->start(region, frameRate, includeCursor, path, audio);
    }, Qt::QueuedConnection);

    qCInfo(dsrApp) << "X11 ucodec recording started, region:" << region
                   << "fps:" << frameRate << "output:" << path;
}

void RecordProcess::stopX11Recording()
{
    if (m_x11Worker && m_x11Thread && m_x11Thread->isRunning()) {
        X11RecordWorker *worker = m_x11Worker;
        QMetaObject::invokeMethod(worker, [worker]() { worker->stop(); },
                                  Qt::BlockingQueuedConnection);
    }
    if (m_x11Thread) {
        m_x11Thread->quit();
        if (!m_x11Thread->wait(20000))
            qCWarning(dsrApp) << "X11 recording thread did not stop in time";
        delete m_x11Thread;
        m_x11Thread = nullptr;
    }
    m_x11Worker = nullptr;
}

//初始化录制输出路径（mp4/mkv/gif）
void RecordProcess::initProcess()
{
    QString date = QDateTime::currentDateTime().toString("yyyyMMddhhmmss");
    QString fileExtension = "mp4";
    if (m_recordType == Utils::kMKV) {
        fileExtension = "mkv";
    } else if (m_recordType == Utils::kGIF) {
        fileExtension = "gif";
    }
    if (saveAreaName.isEmpty()) {
        saveBaseName = QString("%1_%2.%3").arg(tr("Record")).arg(date).arg(fileExtension);
    } else {
        if (m_isFullScreenRecord) {
            saveBaseName = QString("%1.%2").arg(saveAreaName).arg(fileExtension);
        } else {
            saveBaseName = QString("%1_%2_%3.%4").arg(tr("Record")).arg(saveAreaName).arg(date).arg(fileExtension);
        }
    }
    savePath = QDir(saveTempDir).filePath(saveBaseName);
    // Remove same cache file first.
    QFile::remove(savePath);
}

void RecordProcess::getScreenRecordSavePath()
{
    ConfigSettings *settings = ConfigSettings::instance();
    if (settings->getValue("recorder", "save_dir").toString() == "") {
        qCWarning(dsrApp) << "配置文件中录屏保存路径为空,采用默认路径";
        saveDir = QStandardPaths::standardLocations(QStandardPaths::MoviesLocation).first() + QDir::separator() + "Screen Recordings" + QDir::separator();
    } else {
        int t_saveIndex = settings->getValue("recorder", "save_op").toInt();
        switch (t_saveIndex) {
        case 1: {
            //保存到桌面
            saveDir = settings->getValue("recorder", "save_dir").toString() + QDir::separator();
            qCInfo(dsrApp) << "保存到桌面: " << saveDir;
            break;
        }
        default:
            //保存到视频目录中的Screen Recordings目录
            saveDir = settings->getValue("recorder", "save_dir").toString() + QDir::separator() + "Screen Recordings" + QDir::separator();
            qCInfo(dsrApp) << "保存到视频目录: " << saveDir;
            break;
        }
    }
    if ((!QDir(saveDir).exists() && QDir().mkdir(saveDir) == false) ||   // 文件不存在，且创建失败
            (QDir(saveDir).exists() && !QFileInfo(saveDir).isWritable())) {   // 文件存在，且不能写
        // 兜底分支与其它分支保持一致的后置条件：确保目录存在，且以路径分隔符结尾
        const QString moviesDir = QStandardPaths::standardLocations(QStandardPaths::MoviesLocation).first();
        QDir().mkpath(moviesDir);
        saveDir = moviesDir + QDir::separator();
    }
    qCInfo(dsrApp) << "录屏保存目录: " << saveDir;
}

void RecordProcess::save2Clipboard(QString file)
{
    qCDebug(dsrApp) << "Saving file to clipboard:" << file;
    //录屏文件保存到剪切板
    QClipboard *cb = qApp->clipboard();
    // Ownership of the new data is transferred to the clipboard.
    QMimeData *newMimeData = new QMimeData();

    QByteArray gnomeFormat = QByteArray("copy\n");
    QString text;
    QList<QUrl> dataUrls;

    dataUrls << QUrl::fromLocalFile(file);
    gnomeFormat.append(QUrl::fromLocalFile(file).toEncoded()).append("\n");

    newMimeData->setText(text.endsWith('\n') ? text.left(text.length() - 1) : text);
    newMimeData->setUrls(dataUrls);

    gnomeFormat.remove(gnomeFormat.length() - 1, 1);

    newMimeData->setData("x-special/gnome-copied-files", gnomeFormat);

    cb->setMimeData(newMimeData, QClipboard::Clipboard);
    qCDebug(dsrApp) << "File saved to clipboard.";
}

//treeland录制视频
void RecordProcess::treelandRecord()
{
    qCDebug(dsrApp) << "treeland ext-image-copy-capture 录屏！";
    
    initProcess();
    
    ExtCaptureRecorder *extRecorder = new ExtCaptureRecorder(this);
    
    // 检查协议可用性
    if (!extRecorder->isAvailable()) {
        qCWarning(dsrApp) << "ext-image-copy-capture协议暂未就绪，等待协议初始化...";
        
        QEventLoop waitLoop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);
        timeoutTimer.setInterval(1000); 
        
        connect(&timeoutTimer, &QTimer::timeout, &waitLoop, &QEventLoop::quit);
        
        QMetaObject::Connection protocolConnection;
        protocolConnection = connect(extRecorder, &ExtCaptureRecorder::protocolAvailable, 
                                   [&waitLoop, &protocolConnection]() {
            qCWarning(dsrApp) << "协议现在可用，继续录制";
            QObject::disconnect(protocolConnection);
            waitLoop.quit();
        });
        
        timeoutTimer.start();
        waitLoop.exec();
        
        if (!extRecorder->isAvailable()) {
            qCCritical(dsrApp) << "等待超时，ext-image-copy-capture协议仍不可用";
            delete extRecorder;
            return;
        } else {
            qCWarning(dsrApp) << "协议现在可用，继续录制流程";
        }
    }
    
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        qCCritical(dsrApp) << "无法获取屏幕信息";
        delete extRecorder;
        return;
    }
    
    connect(extRecorder, &ExtCaptureRecorder::recordingStarted, this, [this]() {
        qCDebug(dsrApp) << "TreeLand录制已开始";
    });
    
    connect(extRecorder, &ExtCaptureRecorder::recordingStopped, this, [this]() {
        qCDebug(dsrApp) << "TreeLand录制已停止";
    });
    
    connect(extRecorder, &ExtCaptureRecorder::error, this, [this](const QString &message) {
        qCCritical(dsrApp) << "TreeLand录制错误:" << message;
    });
    
    bool startResult = extRecorder->startRecording(screen, m_mouseType != RECORD_MOUSE_NULL, savePath, m_framerate);
    
    if (!startResult) {
        qCCritical(dsrApp) << "启动TreeLand录制失败";
        delete extRecorder;
        return;
    }
    
    setProperty("extCaptureRecorder", QVariant::fromValue(extRecorder));
    
    qCInfo(dsrApp) << "TreeLand录制启动完成";
}

//开始录屏
void RecordProcess::startRecord()
{
    m_recordSavingNotifyId = RECORD_SAVING_NOTIFY_ID_INVALID;
    getScreenRecordSavePath();
    //使用QtConcurrent::run受cpu核心线程数的影响，线程池默认大小为CPU核心线程数大小，由于程序中通过此方法启动的线程数超出4个，故再次设置线程池大小
    QThreadPool::globalInstance()->setMaxThreadCount(QThreadPool::globalInstance()->maxThreadCount() > 6 ? QThreadPool::globalInstance()->maxThreadCount() : 10);

#ifndef ENABLE_UNIT_TEST
    if (Utils::isTreelandMode) {
        treelandRecord();
    }
    // x11 下的录屏
    else if (!Utils::isWaylandMode) {
#else
    {
#endif
        recordVideo();
#ifndef ENABLE_UNIT_TEST
    }
    // 旧版 KWayland 采集链路已移除；V25 的 Wayland 会话走 Treeland。
    else {
        qCWarning(dsrApp) << "Legacy Wayland recording is not supported";
    }
#else
    }
#endif

    if (Utils::isSysHighVersion1040() == false) {
        qCInfo(dsrApp) << "系统版本小于1040, 不显示录制时长功能。";
        return;
    }
    //1040及以上的版本可通过此方式启动状态栏图标闪烁
    qCDebug(dsrApp) << "通知录屏插件开始录屏! currentTime: " << QTime::currentTime();
    QDBusMessage message = callTrayTimeIcon(DBUS_FUNC_ON_START);
    callTrayShotIcon(DBUS_FUNC_ON_START);
    callTrayRecorderIcon(DBUS_FUNC_ON_START);

    qCDebug(dsrApp) << "已通知录屏插件开始录屏! currentTime: " << QTime::currentTime();
    m_recordingFlag = true;
    
    // 发送录屏开始信号
    emit recordingStarted();
    qCDebug(dsrApp) << "Recording started signal emitted";
    
    QtConcurrent::run([this]() {
        this->emitRecording();
    });
    if (QDBusMessage::ReplyMessage == message.type()) {
        if (!message.arguments().takeFirst().toBool())
            qCDebug(dsrApp) << "dde dock screen-recorder-plugin did not receive start message!";
    }
}
void RecordProcess::emitRecording()
{
    while (m_recordingFlag) {
        //qDebug() << "录屏正在进行中! currentTime: " << QTime::currentTime();

        callTrayTimeIcon(DBUS_FUNC_ON_RECORDING);
        callTrayShotIcon(DBUS_FUNC_ON_RECORDING);
        callTrayRecorderIcon(DBUS_FUNC_ON_RECORDING);

        QThread::msleep(1000);
    }
}

void RecordProcess::setFullScreenRecord(bool flag)
{
    m_isFullScreenRecord = flag;
}

void RecordProcess::stopRecord()
{
    if (Utils::isSysHighVersion1040() == true) {
        qCInfo(dsrApp) << __FUNCTION__ << __LINE__ << "正在暂停录屏计时...";
        qCDebug(dsrApp) << "Pause the screen recording timer!";

        //系统托盘图标停止闪烁，时间暂停，但还没有结束
        QDBusMessage message = callTrayTimeIcon(DBUS_FUNC_ON_PAUSE);

        if (QDBusMessage::ReplyMessage == message.type()) {
            if (!message.arguments().takeFirst().toBool())
                qCDebug(dsrApp) << "dde dock screen-recorder-plugin did not receive stop message!";
        }
        callTrayShotIcon(DBUS_FUNC_ON_PAUSE);
        callTrayRecorderIcon(DBUS_FUNC_ON_PAUSE);

        qCInfo(dsrApp) << __FUNCTION__ << __LINE__ << "录屏计时已暂停";
    }
#ifndef ENABLE_UNIT_TEST
    //停止treeland录屏
    if (Utils::isTreelandMode) {
            // 获取存储的组件实例
            ExtCaptureRecorder *extRecorder = property("extCaptureRecorder").value<ExtCaptureRecorder*>();
            
            // 改为"等待停止完成"的异步流程，确保底层完全停止后再收尾，避免残留帧
            if (extRecorder) {
                // 断开可能的重复连接，确保只触发一次
                disconnect(extRecorder, nullptr, this, nullptr);
                
                // 连接停止完成信号后再调用stopRecording
#if (QT_VERSION_MAJOR == 5)
                connect(extRecorder, &ExtCaptureRecorder::recordingStopped, this, [this, extRecorder]() {
#elif (QT_VERSION_MAJOR == 6)
                connect(extRecorder, &ExtCaptureRecorder::recordingStopped, this, [this, extRecorder]() {
#endif
                    qCInfo(dsrApp) << "TreeLand录屏已停止";
                    
                    // 清理对象引用
                    setProperty("extCaptureRecorder", QVariant());
                    if (extRecorder) extRecorder->deleteLater();
                    
                    // 停止完成后再执行收尾流程（GIF/MP4/MKV 均为直接录制）
                    onRecordFinish();
                });
                
                // 执行停止
                extRecorder->stopRecording();
                
                // 等待异步回调，不要立刻继续后续逻辑
                return;
            } else {
                onRecordFinish();
            }
        }
    //停止x11录屏
    else {
#endif
        stopX11Recording();
        onRecordFinish();
#ifndef ENABLE_UNIT_TEST
    }
#endif
}

//退出录屏（先停止，再弹提示，最后退出）
void RecordProcess::exitRecord(QString newSavePath)
{
    if (!Utils::isRootUser && !m_isFullScreenRecord) {
        qCInfo(dsrApp) << __LINE__ << __func__ << "关闭「正在保存」通知（若有）并弹出保存完成通知";
        if (m_recordSavingNotifyId != RECORD_SAVING_NOTIFY_ID_INVALID) {
            closeFdoNotification(m_recordSavingNotifyId);
            m_recordSavingNotifyId = RECORD_SAVING_NOTIFY_ID_INVALID;
        }
        // Popup notify.
        QDBusInterface notification("org.freedesktop.Notifications",
                                    "/org/freedesktop/Notifications",
                                    "org.freedesktop.Notifications",
                                    QDBusConnection::sessionBus());

        QStringList actions;
        actions << "_open" << tr("View");
        actions << "_open1" << tr("Open Folder");

        QVariantMap hints;
        hints["x-deepin-action-_open"] = QStringList{"xdg-open", newSavePath};
        QStringList savepathcommand = QStringList{"dde-file-manager", "--show-item", newSavePath};
        hints["x-deepin-action-_open1"] = savepathcommand;
        int timeout = -1;
        unsigned int id = 0;

        QList<QVariant> arg;
        arg << Utils::appName  //(QCoreApplication::applicationName())                 // appname
            << id                                                    // id
            << QString("deepin-screen-recorder")                     // icon
            << tr("Recording finished")                              // summary
            << QString(tr("Saved to %1")).arg(newSavePath)           // body
            << actions                                               // actions
            << hints                                                 // hints
            << timeout;                                              // timeout
        notification.callWithArgumentList(QDBus::AutoDetect, "Notify", arg);
        qCInfo(dsrApp) << __LINE__ << __func__ << "已弹出通知消息";
    }
    // GIF 直接由 GifRecorder 写入目标文件，不再有中间转码文件需要清理。
    m_recordingFlag = false;
    
    if (Utils::isSysHighVersion1040() == true) {
        qCInfo(dsrApp) << __LINE__ << __func__ << "正在退出录屏计时图标...";
        qCDebug(dsrApp) << __LINE__ << ": Stop the screen recording timer!";
        //系统托盘图标结束并退出
        callTrayTimeIcon(DBUS_FUNC_ON_STOP);
        callTrayShotIcon(DBUS_FUNC_ON_STOP);
        callTrayRecorderIcon(DBUS_FUNC_ON_STOP);
        qCInfo(dsrApp) << __LINE__ << __func__ << "录屏计时图标已退出";
    }
    
    // 发送录屏的信号
    emit recordingStopped();
    qCDebug(dsrApp) << "Recording stopped signal emitted";

    //保存到剪切板
    save2Clipboard(newSavePath);

    qCInfo(dsrApp) << __LINE__ << __func__ <<"录屏已退出";

#ifndef ENABLE_UNIT_TEST
    // 延迟退出，确保剪切板操作完成（单测构建不调度应用退出，避免击穿测试事件循环）
    QTimer::singleShot(500, []() {
        QApplication::quit();

        // Treeland 和 Wayland 模式下都强制退出，避免残留事件循环导致进程无法退出
        if (Utils::isWaylandMode || Utils::isTreelandMode) {
            qCInfo(dsrApp) << (Utils::isTreelandMode ? "treeland" : "wayland") << "record exit! (_Exit(0))";
            QTimer::singleShot(100, []() {
                _Exit(0);
            });
        }
    });
#endif
}
