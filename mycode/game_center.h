/**
 * @file    game_center.h
 * @brief   体感游戏中心界面接口声明
 */
#ifndef __GAME_CENTER_H__
#define __GAME_CENTER_H__

extern lv_obj_t * game_2048_screen;    // 2048 游戏屏（2048.c 返回时会删它）
extern lv_obj_t * select_game_screen;   // 游戏中心屏（全局，返回时复用）

/* 创建并返回游戏中心屏幕对象 */
lv_obj_t * ui_select_game_screen(void);

#endif
