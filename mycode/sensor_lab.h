/**
 * @file    sensor_lab.h
 * @brief   传感器实验室界面接口声明
 */
#ifndef __SENSOR_LAB_H__
#define __SENSOR_LAB_H__

extern lv_obj_t * sensor_lab_screen;   // 传感器实验室屏（全局）

/* 创建并返回传感器实验室屏幕对象 */
lv_obj_t * ui_sensor_lab_init(void);

#endif
