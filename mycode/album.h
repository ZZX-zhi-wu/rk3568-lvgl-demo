/**
 * @file    album.h
 * @brief   电子相册界面接口声明
 *
 * 相比 8-28 版只加了一行 album_rescan()，其余不变。
 */
#ifndef __ALBUM_H__
#define __ALBUM_H__

#ifdef __cplusplus
extern "C" {
#endif

/* 创建并返回电子相册屏幕对象 */
lv_obj_t * ui_album_init(void);

/*
 * 重新扫描 /work_space/bmp_pic/photo/ 下的照片并刷新界面。
 * 要求 photo_N.bmp 与 thumb_N.bmp 成对存在，按编号升序排列；
 * 一张都扫不到时回退到原来的 6 张硬编码路径，保证相册不会空白。
 *
 * 由 album_cloud.c 在下载完成后调用。
 */
void album_rescan(void);

#ifdef __cplusplus
}
#endif

#endif
