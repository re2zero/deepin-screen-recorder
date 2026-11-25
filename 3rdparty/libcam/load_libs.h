// Copyright (C) 2019 ~ 2020 Deepin Technology Co., Ltd.
// SPDX-FileCopyrightText: 2022 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LOAD_LIBS_H
#define LOAD_LIBS_H
#include <stdio.h>
#include <pthread.h>
#include <libusb.h>
#include <libv4l2.h>
#include <libudev.h>

/* 摄像头模块仅动态加载 udev/libusb/libv4l2，不再加载 libav */
typedef struct _LoadLibNames {
    char *chUdev;
    char *chUsb;
    char *chV4l2;
} LoadLibNames;

void setLibNames(LoadLibNames tmp);

//struct udev_enumerate *udev_enumerate_new(struct udev *udev);
typedef struct udev_enumerate *(*uos_udev_enumerate_new)(struct udev *udev);
//struct udev *udev_enumerate_get_udev(struct udev_enumerate *udev_enumerate);
typedef struct udev *(*uos_udev_enumerate_get_udev)(struct udev_enumerate *udev_enumerate);
//int udev_enumerate_add_match_subsystem(struct udev_enumerate *udev_enumerate, const char *subsystem);
typedef int (*uos_udev_enumerate_add_match_subsystem)(struct udev_enumerate *udev_enumerate, const char *subsystem);
//int udev_enumerate_scan_devices(struct udev_enumerate *udev_enumerate);
typedef int (*uos_udev_enumerate_scan_devices)(struct udev_enumerate *udev_enumerate);
//struct udev_list_entry *udev_enumerate_get_list_entry(struct udev_enumerate *udev_enumerate);
typedef struct udev_list_entry *(*uos_udev_enumerate_get_list_entry)(struct udev_enumerate *udev_enumerate);
//const char *udev_list_entry_get_name(struct udev_list_entry *list_entry);
typedef const char *(*uos_udev_list_entry_get_name)(struct udev_list_entry *list_entry);
//struct udev_device *udev_device_new_from_syspath(struct udev *udev, const char *syspath);
typedef struct udev_device *(*uos_udev_device_new_from_syspath)(struct udev *udev, const char *syspath);
//const char *udev_device_get_devnode(struct udev_device *udev_device);
typedef const char *(*uos_udev_device_get_devnode)(struct udev_device *udev_device);
/*struct udev_device *udev_device_get_parent_with_subsystem_devtype(struct udev_device *udev_device,
                                                                  const char *subsystem, const char *devtype);*/
typedef struct udev_device *(*uos_udev_device_get_parent_with_subsystem_devtype)(struct udev_device *udev_device,
                                                                                 const char *subsystem, const char *devtype);
//const char *udev_device_get_sysattr_value(struct udev_device *udev_device, const char *sysattr);
typedef const char *(*uos_udev_device_get_sysattr_value)(struct udev_device *udev_device, const char *sysattr);
//struct udev_device *udev_device_unref(struct udev_device *udev_device);
typedef struct udev_device *(*uos_udev_device_unref)(struct udev_device *udev_device);
//struct udev_enumerate *udev_enumerate_unref(struct udev_enumerate *udev_enumerate);
typedef struct udev_enumerate *(*uos_udev_enumerate_unref)(struct udev_enumerate *udev_enumerate);
//struct udev *udev_new(void);
typedef struct udev *(*uos_udev_new)(void);
//struct udev_monitor *udev_monitor_new_from_netlink(struct udev *udev, const char *name);
typedef struct udev_monitor *(*uos_udev_monitor_new_from_netlink)(struct udev *udev, const char *name);
/*int udev_monitor_filter_add_match_subsystem_devtype(struct udev_monitor *udev_monitor,
                                                    const char *subsystem, const char *devtype);*/
typedef int (*uos_udev_monitor_filter_add_match_subsystem_devtype)(struct udev_monitor *udev_monitor,
                                                                   const char *subsystem, const char *devtype);
//int udev_monitor_enable_receiving(struct udev_monitor *udev_monitor);
typedef int (*uos_udev_monitor_enable_receiving)(struct udev_monitor *udev_monitor);
//int udev_monitor_get_fd(struct udev_monitor *udev_monitor);
typedef int (*uos_udev_monitor_get_fd)(struct udev_monitor *udev_monitor);
//struct udev_device *udev_monitor_receive_device(struct udev_monitor *udev_monitor);
typedef struct udev_device *(*uos_udev_monitor_receive_device)(struct udev_monitor *udev_monitor);
//const char *udev_device_get_subsystem(struct udev_device *udev_device);
typedef const char *(*uos_udev_device_get_subsystem)(struct udev_device *udev_device);
//const char *udev_device_get_devtype(struct udev_device *udev_device);
typedef const char *(*uos_udev_device_get_devtype)(struct udev_device *udev_device);
//const char *udev_device_get_action(struct udev_device *udev_device);
typedef const char *(*uos_udev_device_get_action)(struct udev_device *udev_device);
//struct udev *udev_unref(struct udev *udev);
typedef struct udev *(*uos_udev_unref)(struct udev *udev);
//struct udev_list_entry *udev_list_entry_get_next(struct udev_list_entry *list_entry);
typedef struct udev_list_entry *(*uos_udev_list_entry_get_next)(struct udev_list_entry *list_entry);

typedef struct _LoadUdev {
    uos_udev_enumerate_new m_udev_enumerate_new;
    uos_udev_enumerate_get_udev m_udev_enumerate_get_udev;
    uos_udev_enumerate_add_match_subsystem m_udev_enumerate_add_match_subsystem;
    uos_udev_enumerate_scan_devices m_udev_enumerate_scan_devices;
    uos_udev_enumerate_get_list_entry m_udev_enumerate_get_list_entry;
    uos_udev_list_entry_get_name m_udev_list_entry_get_name;
    uos_udev_device_new_from_syspath m_udev_device_new_from_syspath;
    uos_udev_device_get_devnode m_udev_device_get_devnode;
    uos_udev_device_get_parent_with_subsystem_devtype m_udev_device_get_parent_with_subsystem_devtype;
    uos_udev_device_get_sysattr_value m_udev_device_get_sysattr_value;
    uos_udev_device_unref m_udev_device_unref;
    uos_udev_enumerate_unref m_udev_enumerate_unref;
    uos_udev_new m_udev_new;
    uos_udev_monitor_new_from_netlink m_udev_monitor_new_from_netlink;
    uos_udev_monitor_filter_add_match_subsystem_devtype m_udev_monitor_filter_add_match_subsystem_devtype;
    uos_udev_monitor_enable_receiving m_udev_monitor_enable_receiving;
    uos_udev_monitor_get_fd m_udev_monitor_get_fd;
    uos_udev_monitor_receive_device m_udev_monitor_receive_device;
    uos_udev_device_get_subsystem m_udev_device_get_subsystem;
    uos_udev_device_get_devtype m_udev_device_get_devtype;
    uos_udev_device_get_action m_udev_device_get_action;
    uos_udev_unref m_udev_unref;
    uos_udev_list_entry_get_next m_udev_list_entry_get_next;
} LoadUdev;
LoadUdev *getUdev();


//int LIBUSB_CALL libusb_init(libusb_context **ctx);
typedef int LIBUSB_CALL (*uos_libusb_init)(libusb_context **ctx);
/*ssize_t LIBUSB_CALL libusb_get_device_list(libusb_context *ctx,
    libusb_device ***list);*/
typedef ssize_t LIBUSB_CALL (*uos_libusb_get_device_list)(libusb_context *ctx,
                                                          libusb_device ***list);
//uint8_t LIBUSB_CALL libusb_get_bus_number(libusb_device *dev);
typedef uint8_t LIBUSB_CALL (*uos_libusb_get_bus_number)(libusb_device *dev);
//uint8_t LIBUSB_CALL libusb_get_device_address(libusb_device *dev);
typedef uint8_t LIBUSB_CALL (*uos_libusb_get_device_address)(libusb_device *dev);
//libusb_device * LIBUSB_CALL libusb_ref_device(libusb_device *dev);
typedef libusb_device *LIBUSB_CALL (*uos_libusb_ref_device)(libusb_device *dev);
/*void LIBUSB_CALL libusb_free_device_list(libusb_device **list,
    int unref_devices);*/
typedef void LIBUSB_CALL (*uos_libusb_free_device_list)(libusb_device **list,
                                                        int unref_devices);
/*int LIBUSB_CALL libusb_get_device_descriptor(libusb_device *dev,
    struct libusb_device_descriptor *desc);*/
typedef int LIBUSB_CALL (*uos_libusb_get_device_descriptor)(libusb_device *dev,
                                                            struct libusb_device_descriptor *desc);
/*int LIBUSB_CALL libusb_get_config_descriptor(libusb_device *dev,
    uint8_t config_index, struct libusb_config_descriptor **config);*/
typedef int LIBUSB_CALL (*uos_libusb_get_config_descriptor)(libusb_device *dev,
                                                            uint8_t config_index, struct libusb_config_descriptor **config);
//void LIBUSB_CALL libusb_unref_device(libusb_device *dev);
typedef void LIBUSB_CALL (*uos_libusb_unref_device)(libusb_device *dev);

typedef struct _LoadUSB {
    uos_libusb_init m_libusb_init;
    uos_libusb_get_device_list m_libusb_get_device_list;
    uos_libusb_get_bus_number m_libusb_get_bus_number;
    uos_libusb_get_device_address m_libusb_get_device_address;
    uos_libusb_ref_device m_libusb_ref_device;
    uos_libusb_free_device_list m_libusb_free_device_list;
    uos_libusb_get_device_descriptor m_libusb_get_device_descriptor;
    uos_libusb_get_config_descriptor m_libusb_get_config_descriptor;
    uos_libusb_unref_device m_libusb_unref_device;
} LoadUSB;

LoadUSB *getUSB();

//LIBV4L_PUBLIC int v4l2_ioctl(int fd, unsigned long int request, ...);
typedef LIBV4L_PUBLIC int (*uos_v4l2_ioctl)(int fd, unsigned long int request, ...);
//LIBV4L_PUBLIC int v4l2_munmap(void *_start, size_t length);
typedef LIBV4L_PUBLIC int (*uos_v4l2_munmap)(void *_start, size_t length);
/*LIBV4L_PUBLIC void *v4l2_mmap(void *start, size_t length, int prot, int flags,
int fd, int64_t offset);*/
typedef LIBV4L_PUBLIC void *(*uos_v4l2_mmap)(void *start, size_t length, int prot, int flags,
                                             int fd, int64_t offset);
//LIBV4L_PUBLIC ssize_t v4l2_read(int fd, void *buffer, size_t n);
typedef LIBV4L_PUBLIC ssize_t (*uos_v4l2_read)(int fd, void *buffer, size_t n);
//LIBV4L_PUBLIC int v4l2_close(int fd);
typedef LIBV4L_PUBLIC int (*uos_v4l2_close)(int fd);
//LIBV4L_PUBLIC int v4l2_open(const char *file, int oflag, ...);
typedef LIBV4L_PUBLIC int (*uos_v4l2_open)(const char *file, int oflag, ...);

typedef struct _LoadV4l2 {
    uos_v4l2_ioctl m_v4l2_ioctl;
    uos_v4l2_munmap m_v4l2_munmap;
    uos_v4l2_mmap m_v4l2_mmap;
    uos_v4l2_read m_v4l2_read;
    uos_v4l2_close m_v4l2_close;
    uos_v4l2_open m_v4l2_open;
} LoadV4l2;

LoadV4l2 *getV4l2();
#endif//LOAD_LIBS_H
