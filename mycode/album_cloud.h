/**
 * @file    album_cloud.h
 * @brief   云相册弹层 —— 叠在电子相册页上的远程文件浏览 / 下载界面
 *
 * 定位说明：它不是一个独立的「屏」，而是一层叠在相册屏上的对象，
 * 生命周期由自己管理（open/close 配对），所以不参与 main_interface.h 里
 * 那套「切屏 → 删屏」的返回桌面流程。
 *
 * 数据来源：走 ftp.c 的 FTP 连接向 PC 端拉取文件列表并下载，
 * 下载完成后交给相册模块显示（相册扫描出新文件即可看到）。
 */
#ifndef __ALBUM_CLOUD_H__
#define __ALBUM_CLOUD_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../lvgl/lvgl.h"

/**
 * 打开云相册弹层（在 parent 屏上创建，覆盖全屏）。
 * 已打开时忽略（不会重复创建，见 album_cloud_is_open 的判重作用）。
 * @param parent 宿主屏对象，一般是电子相册的屏
 */
void album_cloud_open(lv_obj_t *parent);

/** 关闭弹层（会断开 FTP 连接、删除定时器与控件） */
void album_cloud_close(void);

/** 查状态：1 = 当前已打开，0 = 未打开 */
int  album_cloud_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* __ALBUM_CLOUD_H__ */
