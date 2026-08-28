/**
 * @file    main_interface.h
 * @brief   桌面 Launcher（App 选择界面）接口声明
 */
#ifndef __MAIN_INTERFACE_H__
#define __MAIN_INTERFACE_H__

extern lv_obj_t * album_screen;        // 电子相册屏（album.c 使用）
extern lv_obj_t * select_app_screen;   // 桌面屏（全局，各子界面返回时复用）

/* 创建并返回桌面屏幕对象 */
lv_obj_t * ui_select_app_screen(void);

#endif
