// Copyright (C) 2019 ~ 2020 Deepin Technology Co., Ltd.
// SPDX-FileCopyrightText: 2022 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <dlfcn.h>
#include "load_libs.h"

void PrintError(){
    char *error;
    if ((error = dlerror()) != NULL)  {
        fprintf (stderr, "%s ", error);
    }
}
static LoadLibNames g_ldnames;
static LoadUdev *pUdev = NULL;
static LoadUdev *newUdev(void)
{
    pUdev = (LoadUdev *)malloc(sizeof(LoadUdev));
    //libudev
    void *handle = dlopen(g_ldnames.chUdev/*"libudev.so.1"*/,RTLD_LAZY);
    if (!handle) {
        PrintError();
    }
    pUdev->m_udev_enumerate_new = (uos_udev_enumerate_new)dlsym(handle, "udev_enumerate_new");
    PrintError();
    pUdev->m_udev_enumerate_get_udev = (uos_udev_enumerate_get_udev)dlsym(handle, "udev_enumerate_get_udev");
    PrintError();
    pUdev->m_udev_enumerate_add_match_subsystem = (uos_udev_enumerate_add_match_subsystem)dlsym(handle, "udev_enumerate_add_match_subsystem");
    PrintError();
    pUdev->m_udev_enumerate_scan_devices = (uos_udev_enumerate_scan_devices)dlsym(handle, "udev_enumerate_scan_devices");
    PrintError();
    pUdev->m_udev_enumerate_get_list_entry = (uos_udev_enumerate_get_list_entry)dlsym(handle, "udev_enumerate_get_list_entry");
    PrintError();
    pUdev->m_udev_list_entry_get_name = (uos_udev_list_entry_get_name)dlsym(handle, "udev_list_entry_get_name");
    PrintError();
    pUdev->m_udev_device_new_from_syspath = (uos_udev_device_new_from_syspath)dlsym(handle, "udev_device_new_from_syspath");
    PrintError();
    pUdev->m_udev_device_get_devnode = (uos_udev_device_get_devnode)dlsym(handle, "udev_device_get_devnode");
    PrintError();
    pUdev->m_udev_device_get_parent_with_subsystem_devtype = (uos_udev_device_get_parent_with_subsystem_devtype)dlsym(handle, "udev_device_get_parent_with_subsystem_devtype");
    PrintError();
    pUdev->m_udev_device_get_sysattr_value = (uos_udev_device_get_sysattr_value)dlsym(handle, "udev_device_get_sysattr_value");
    PrintError();
    pUdev->m_udev_device_unref = (uos_udev_device_unref)dlsym(handle, "udev_device_unref");
    PrintError();
    pUdev->m_udev_enumerate_unref = (uos_udev_enumerate_unref)dlsym(handle, "udev_enumerate_unref");
    PrintError();
    pUdev->m_udev_new = (uos_udev_new)dlsym(handle, "udev_new");
    PrintError();
    pUdev->m_udev_monitor_new_from_netlink = (uos_udev_monitor_new_from_netlink)dlsym(handle, "udev_monitor_new_from_netlink");
    PrintError();
    pUdev->m_udev_monitor_filter_add_match_subsystem_devtype = (uos_udev_monitor_filter_add_match_subsystem_devtype)dlsym(handle, "udev_monitor_filter_add_match_subsystem_devtype");
    PrintError();
    pUdev->m_udev_monitor_enable_receiving = (uos_udev_monitor_enable_receiving)dlsym(handle, "udev_monitor_enable_receiving");
    PrintError();
    pUdev->m_udev_monitor_get_fd = (uos_udev_monitor_get_fd)dlsym(handle, "udev_monitor_get_fd");
    PrintError();
    pUdev->m_udev_monitor_receive_device = (uos_udev_monitor_receive_device)dlsym(handle, "udev_monitor_receive_device");
    PrintError();
    pUdev->m_udev_device_get_devtype = (uos_udev_device_get_devtype)dlsym(handle, "udev_device_get_devtype");
    PrintError();
    pUdev->m_udev_device_get_action = (uos_udev_device_get_action)dlsym(handle, "udev_device_get_action");
    PrintError();
    pUdev->m_udev_unref = (uos_udev_unref)dlsym(handle, "udev_unref");
    PrintError();
    pUdev->m_udev_list_entry_get_next = (uos_udev_list_entry_get_next)dlsym(handle, "udev_list_entry_get_next");
    PrintError();

    assert(pUdev != NULL);
    return pUdev;
}

LoadUdev *getUdev()
{
    static pthread_mutex_t mutexUdev;
    //双检锁
    if (pUdev == NULL) {
        // 这里要对pUdev加锁
        pthread_mutex_lock(&mutexUdev);
        if (pUdev == NULL)
        {
            pUdev = newUdev();
        }
        //退出时解锁
        pthread_mutex_unlock(&mutexUdev);
    }

    return pUdev;
}

static LoadUSB *pUSB = NULL;
static LoadUSB *newUSB(void)
{
    pUSB = (LoadUSB *)malloc(sizeof(LoadUSB));
    //libusb
    void *handle = dlopen(g_ldnames.chUsb/*"libusb-1.0.so.0"*/,RTLD_LAZY);
    if (!handle) {
        PrintError();
    }
    pUSB->m_libusb_init = (uos_libusb_init)dlsym(handle, "libusb_init");
    PrintError();
    pUSB->m_libusb_get_device_list = (uos_libusb_get_device_list)dlsym(handle, "libusb_get_device_list");
    PrintError();
    pUSB->m_libusb_get_bus_number = (uos_libusb_get_bus_number)dlsym(handle, "libusb_get_bus_number");
    PrintError();
    pUSB->m_libusb_get_device_address = (uos_libusb_get_device_address)dlsym(handle, "libusb_get_device_address");
    PrintError();
    pUSB->m_libusb_ref_device = (uos_libusb_ref_device)dlsym(handle, "libusb_ref_device");
    PrintError();
    pUSB->m_libusb_free_device_list = (uos_libusb_free_device_list)dlsym(handle, "libusb_free_device_list");
    PrintError();
    pUSB->m_libusb_get_device_descriptor = (uos_libusb_get_device_descriptor)dlsym(handle, "libusb_get_device_descriptor");
    PrintError();
    pUSB->m_libusb_get_config_descriptor = (uos_libusb_get_config_descriptor)dlsym(handle, "libusb_get_config_descriptor");
    PrintError();
    pUSB->m_libusb_unref_device = (uos_libusb_unref_device)dlsym(handle, "libusb_unref_device");
    PrintError();

    assert(pUSB != NULL);
    return pUSB;
}

LoadUSB *getUSB()
{
    static pthread_mutex_t mutexUSB;
    //双检锁
    if (pUSB == NULL) {
        // 这里要对pUSB加锁
        pthread_mutex_lock(&mutexUSB);
        if (pUSB == NULL)
        {
            pUSB = newUSB();
        }
        //退出时解锁
        pthread_mutex_unlock(&mutexUSB);
    }

    return pUSB;
}

static LoadV4l2 *pV4l2 = NULL;
static LoadV4l2 *newV4l2(void)
{
    pV4l2 = (LoadV4l2 *)malloc(sizeof(LoadV4l2));
    //libv4l2
    void *handle = dlopen(g_ldnames.chV4l2/*"libv4l2.so.0"*/,RTLD_LAZY);
    if (!handle) {
        PrintError();
    }

    pV4l2->m_v4l2_ioctl = (uos_v4l2_ioctl)dlsym(handle, "v4l2_ioctl");
    PrintError();
    pV4l2->m_v4l2_munmap = (uos_v4l2_munmap)dlsym(handle, "v4l2_munmap");
    PrintError();
    pV4l2->m_v4l2_mmap = (uos_v4l2_mmap)dlsym(handle, "v4l2_mmap");
    PrintError();
    pV4l2->m_v4l2_read = (uos_v4l2_read)dlsym(handle, "v4l2_read");
    PrintError();
    pV4l2->m_v4l2_close = (uos_v4l2_close)dlsym(handle, "v4l2_close");
    PrintError();
    pV4l2->m_v4l2_open = (uos_v4l2_open)dlsym(handle, "v4l2_open");
    PrintError();

    assert(pV4l2 != NULL);
    return pV4l2;
}

LoadV4l2 *getV4l2()
{
    static pthread_mutex_t mutexV4l2;
    //双检锁
    if (pV4l2 == NULL) {
        // 这里要对pUSB加锁
        pthread_mutex_lock(&mutexV4l2);
        if (pV4l2 == NULL)
        {
            pV4l2 = newV4l2();
        }
        //退出时解锁
        pthread_mutex_unlock(&mutexV4l2);
    }

    return pV4l2;
}

void setLibNames(LoadLibNames tmp)
{
    g_ldnames.chUdev = (char *)malloc(strlen(tmp.chUdev) + 1);
    strcpy(g_ldnames.chUdev, tmp.chUdev);

    g_ldnames.chUsb = (char *)malloc(strlen(tmp.chUsb) + 1);
    strcpy(g_ldnames.chUsb, tmp.chUsb);

    g_ldnames.chV4l2 = (char *)malloc(strlen(tmp.chV4l2) + 1);
    strcpy(g_ldnames.chV4l2, tmp.chV4l2);
}
