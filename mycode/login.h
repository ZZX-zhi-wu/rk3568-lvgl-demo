/**
 * @file    login.h
 * @brief   登录界面接口声明
 */
#ifndef __LOGIN_H__
#define __LOGIN_H__

#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "lvgl/examples/lv_examples.h"
#include <stdio.h>
#include "main_interface.h"
#include "2048.h"
#include "album.h"

/* 登录界面入口函数（供 main.c 调用） */
void login(void);

#endif
