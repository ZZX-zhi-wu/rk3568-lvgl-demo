/**
 * @file    snake.h
 * @brief   体感贪吃蛇接口声明
 *
 * 对外提供：
 *   - ui_snake_init()：创建贪吃蛇界面并返回屏幕对象。
 *   - snake_screen：贪吃蛇屏幕全局指针（供 game_center.c 跳转和删除时使用）。
 */
#ifndef __SNAKE_H__
#define __SNAKE_H__

extern lv_obj_t * snake_screen;

lv_obj_t * ui_snake_init(void);

#endif
