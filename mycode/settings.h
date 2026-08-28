/**
 * @file    settings.h
 * @brief   设置界面接口声明
 */
#ifndef __SETTINGS_H__
#define __SETTINGS_H__

extern lv_obj_t * settings_screen;   // 设置界面屏（全局）

/* 创建并返回设置屏幕对象 */
lv_obj_t * ui_settings_init(void);

#endif
