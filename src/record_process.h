// Copyright (C) 2020 ~ 2021 Uniontech Software Technology Co.,Ltd.
// SPDX-FileCopyrightText: 2022 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RECORDPROCESS_H
#define RECORDPROCESS_H

#include "utils/configsettings.h"
#include "utils/voicevolumewatcher.h"
#include <QObject>
#include <QRect>
#include <QTimer>
#include <QtConcurrent>

//不需要开启线程，用信号槽代替 process->waitForFinished(-1); 避免线程等待浪费系统资源
/**
 * @brief The RecordProcess class 录屏的控制类
 * x11：XShm 采集，ucodec 编码，自研 MP4/Matroska 封装
 * Treeland：ext-image-copy-capture 采集，ucodec 编码，自研封装
 */
class X11RecordWorker;
class QThread;

class RecordProcess  : public QObject
{
    Q_OBJECT

public:
    static const int RECORD_MOUSE_NULL;
    static const int RECORD_MOUSE_CURSE;
    static const int RECORD_MOUSE_CHECK;
    static const int RECORD_MOUSE_CURSE_CHECK;

    static const int RECORD_FRAMERATE_5;
    static const int RECORD_FRAMERATE_10;
    static const int RECORD_FRAMERATE_20;
    static const int RECORD_FRAMERATE_24;
    static const int RECORD_FRAMERATE_30;

    static constexpr quint32 RECORD_SAVING_NOTIFY_ID_INVALID = 0;

    explicit RecordProcess(QObject *parent = nullptr);
    ~RecordProcess();

    /**
     * @brief 设置录屏的基本信息
     * @param 录屏的范围
     * @param filename
     */
    void setRecordInfo(const QRect &recordRect, const QString &filename);
    /**
     * @brief 开始录屏
     */
    void startRecord();
    /**
     * @brief 停止录屏
     */
    void stopRecord();
    /**
     * @brief 退出录屏（先停止插件计时，再弹录屏提示，最后退出计时插件）
     */
    void exitRecord(QString newSavePath);
    /**
     * @brief 定时给任务栏插件发送正在录屏的信号
     */
    void emitRecording();

    /**
     * @brief setFullScreenRecord 设置全屏录制模式的开关
     * @param flag
     */
    void setFullScreenRecord(bool flag);

    /**
     * @brief 由 MainWindow 在弹出「正在保存」通知后传入 Notify 返回的 id，
     *        exitRecord 弹出「录制完成」前会先 CloseNotification 关闭该通知。
     */
    void setRecordSavingNotifyId(quint32 id);
private:
    /**
     * @brief x11 协议下使用 ucodec 录制视频
     */
    void recordVideo();

    /**
     * @brief treeland协议下ext-image-copy-capture录制视频
     */
    void treelandRecord();

    /**
     * @brief 初始化进程
     */
    void initProcess();

public slots:
    /**
     * @brief 从配置文件获取录屏文件的保存目录
     */
    void getScreenRecordSavePath();

    /**
     * @brief 保存到剪切板
     */
    void save2Clipboard(QString file);
private slots:
    /**
     * @brief onRecordFinish:录屏完成
     */
    void onRecordFinish();

private:
    /**
     * @brief 停止 X11 采集/编码线程
     */
    void stopX11Recording();

    /**
     * @brief X11 采集与 ucodec 编码工作线程
     */
    X11RecordWorker *m_x11Worker = nullptr;
    QThread *m_x11Thread = nullptr;

    /**
     * @brief 录屏的类型：gif mkv mp4
     */
    int m_recordType = 0;

    /**
     * @brief 录制的声音类型： 混音 单麦克风音频 单系统音频
     */
    int m_audioType = 0;

    /**
     * @brief 是否录制鼠标
     */
    int m_mouseType = RECORD_MOUSE_CURSE;

    /**
     * @brief 录屏的范围
     */
    QRect m_recordRect;

    QString savePath;
    QString saveBaseName;
    QString saveTempDir;
    QString saveDir;
    QString saveAreaName;
    QString displayNumber;

    ConfigSettings *m_settings = nullptr;

    /**
     * @brief 录屏的帧率
     */
    int m_framerate;
    /**
      * @brief 定时发送录屏正在运行的标志
      */
    bool m_recordingFlag;
    /**
     * @brief m_isFullScreenRecord 是否是快捷全屏录制，默认为false
     */
    bool m_isFullScreenRecord = false;

    quint32 m_recordSavingNotifyId = RECORD_SAVING_NOTIFY_ID_INVALID;

signals:
    void recordingStopped();
    void recordingStarted(); // 录屏开始信号 

};

#endif //RECORDPROCESS_H
