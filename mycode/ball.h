/**
 * @file    ball.h
 * @brief   重力滚球游戏界面接口声明
 */
#ifndef __BALL_H__
#define __BALL_H__

extern lv_obj_t * ball_screen;   // 重力滚球屏幕对象（全局）

/* 创建并返回重力滚球屏幕对象 */
lv_obj_t * ui_ball_init(void);

#endif
