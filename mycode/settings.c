/**
 * @file    settings.c
 * @brief   设置界面
 *
 * 功能说明：
 *   - 屏幕亮度：滑条调节，写 /sys/class/backlight/backlight/brightness。
 *   - 自动亮度：开关，开启后每 1 秒读 BH1750 环境光传感器，自动调节背光。
 *   - 体感校准：按钮，调用 imu_calibrate() 把当前姿态设为体感零点。
 *   - 关于：版本信息。
 *
 * 硬件说明：
 *   - 背光 PWM 是反极性的（写 255 最暗），所以 set_backlight 里做了反相。
 *   - 环境光传感器 BH1750 挂在 IIO 的 iio:device2。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "imu.h"


/* 中文字体路径 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 背光 sysfs 目录 */
#define BACKLIGHT_DIR "/sys/class/backlight/backlight"
/* 背光极性：1=反相(写255最暗) 0=正向(写255最亮)。滑条往右反而变暗就置1 */
#define BACKLIGHT_INVERTED 1

/* 环境光传感器 BH1750 的 IIO 路径 */
#define ILLUM_INPUT_PATH "/sys/bus/iio/devices/iio:device2/in_illuminance_input"
#define ILLUM_RAW_PATH   "/sys/bus/iio/devices/iio:device2/in_illuminance_raw"
#define ILLUM_SCALE_PATH "/sys/bus/iio/devices/iio:device2/in_illuminance_scale"

/* 自动亮度采样周期（毫秒） */
#define AUTO_BRIGHT_PERIOD 1000

lv_obj_t * settings_screen = NULL;

static lv_timer_t *auto_bright_timer = NULL;   // 自动亮度定时器
static lv_obj_t *bright_slider = NULL;         // 亮度滑条（全局，供自动亮度回调访问）
static lv_obj_t *bright_val = NULL;            // 亮度百分比标签（全局）

/**
 * 创建指定字号的中文字体（带缓存）。
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[4] = {NULL};
    static int cache_size[4] = {0};
    int empty = -1;
    for(int i = 0; i < 4; i++) {
        if(cache[i] != NULL && cache_size[i] == size)
            return cache[i];
        if(cache[i] == NULL && empty < 0)
            empty = i;
    }
    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f) {
        LV_LOG_ERROR("freetype font create failed: %s", CN_FONT_PATH);
        return NULL;
    }
    cache[empty] = f;
    cache_size[empty] = size;
    return f;
}

/*
 * 读取环境光照度（单位 lux）。
 * 优先读 in_illuminance_input（已经是 lux），否则读 in_illuminance_raw 乘 scale。
 */
static int read_illuminance_lux(void)
{
    FILE *fp = fopen(ILLUM_INPUT_PATH, "r");
    if(fp) {
        int v = 0;
        fscanf(fp, "%d", &v);
        fclose(fp);
        return v;
    }
    int raw = 0;
    float scale = 1.0f;
    fp = fopen(ILLUM_RAW_PATH, "r");
    if(fp) { fscanf(fp, "%d", &raw); fclose(fp); }
    fp = fopen(ILLUM_SCALE_PATH, "r");
    if(fp) { fscanf(fp, "%f", &scale); fclose(fp); }
    return (int)(raw * scale);
}

/* 照度(lux) -> 背光百分比：0~1000 lux 线性映射到 30%~100%（最低 30% 兜底） */
static int lux_to_percent(int lux)
{
    if(lux <= 10)    return 30;
    if(lux >= 1000)  return 100;
    return 30 + (lux - 10) * 70 / 990;
}


/* 设置背光亮度（0~100），内部做反相并写入 brightness 文件 */
static void set_backlight(int percent)
{
    char path[128];
    int max = 255;
    snprintf(path, sizeof(path), BACKLIGHT_DIR "/max_brightness");
    FILE *fp = fopen(path, "r");
    if(fp) { fscanf(fp, "%d", &max); fclose(fp); }

    int val = percent * max / 100;
#if BACKLIGHT_INVERTED
    val = max - val;   // 反相：percent 越大 -> 实际写入值越小（越亮）
#endif

    snprintf(path, sizeof(path), BACKLIGHT_DIR "/brightness");
    fp = fopen(path, "w");
    if(fp) { fprintf(fp, "%d", val); fclose(fp); }
    else   { printf("写背光失败: %s\n", path); }
}

/* 亮度滑条回调：更新百分比标签并写入背光 */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *val_lab = lv_event_get_user_data(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(val_lab, "%d%%", v);
    set_backlight(v);
}

/* 自动亮度定时器回调：读照度 -> 映射百分比 -> 写背光并更新界面 */
static void auto_bright_timer_cb(lv_timer_t *t)
{
    int lux = read_illuminance_lux();
    int percent = lux_to_percent(lux);
    set_backlight(percent);
    if(bright_val != NULL)
        lv_label_set_text_fmt(bright_val, "%d%%", percent);
    if(bright_slider != NULL)
        lv_slider_set_value(bright_slider, percent, LV_ANIM_OFF);
    printf("[auto] lux=%d -> backlight %d%%\n", lux, percent);
}

/* 自动亮度开关回调：开启时启动定时器并禁用手动滑条，关闭时反之 */
static void auto_bright_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if(on) {
        if(auto_bright_timer == NULL)
            auto_bright_timer = lv_timer_create(auto_bright_timer_cb, AUTO_BRIGHT_PERIOD, NULL);
        auto_bright_timer_cb(NULL);                       // 立即执行一次，不用等 1 秒
        if(bright_slider != NULL)
            lv_obj_add_state(bright_slider, LV_STATE_DISABLED);   // 自动时禁用手动滑条
        printf("自动亮度：开\n");
    } else {
        if(auto_bright_timer != NULL) {
            lv_timer_delete(auto_bright_timer);
            auto_bright_timer = NULL;
        }
        if(bright_slider != NULL)
            lv_obj_remove_state(bright_slider, LV_STATE_DISABLED); // 恢复手动滑条
        printf("自动亮度：关\n");
    }
}

/* 体感校准按钮回调：把当前姿态设为体感零点 */
static void calibrate_cb(lv_event_t *e)
{
    imu_calibrate();
}


/* 返回按钮回调：先删自动亮度定时器（避免悬空），再切回桌面 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(auto_bright_timer != NULL) {
        lv_timer_delete(auto_bright_timer);
        auto_bright_timer = NULL;
    }
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    if(settings_screen != NULL) {
        lv_obj_delete(settings_screen);
        settings_screen = NULL;
    }
}

/*
 * 设置界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_settings_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(22);   // 正文字号

    /* 创建屏幕和全屏窗口 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题栏：返回按钮 */
    lv_obj_t *back_btn = lv_button_create(win);
    lv_obj_set_size(back_btn, 90, 40);
    lv_obj_set_pos(back_btn, 20, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_t *back_lab = lv_label_create(back_btn);
    lv_label_set_text(back_lab, "返回");
    lv_obj_set_style_text_font(back_lab, font_norm, 0);
    lv_obj_set_style_text_color(back_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, to_select_app_screen_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 屏幕亮度标签 */
    lv_obj_t *lab1 = lv_label_create(win);
    lv_label_set_text(lab1, "屏幕亮度");
    lv_obj_set_style_text_font(lab1, font_norm, 0);
    lv_obj_set_style_text_color(lab1, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab1, 60, 100);

    /* 亮度滑条（用全局变量，供自动亮度回调访问） */
    bright_slider = lv_slider_create(win);
    lv_obj_set_size(bright_slider, 450, 24);
    lv_obj_set_pos(bright_slider, 400, 96);
    lv_slider_set_range(bright_slider, 0, 100);
    lv_slider_set_value(bright_slider, 80, LV_ANIM_OFF);

    /* 亮度百分比标签（用全局变量） */
    bright_val = lv_label_create(win);
    lv_label_set_text(bright_val, "80%");
    lv_obj_set_style_text_font(bright_val, font_norm, 0);
    lv_obj_set_style_text_color(bright_val, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_pos(bright_val, 870, 100);
    lv_obj_add_event_cb(bright_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, bright_val);

    /* 自动亮度标签 */
    lv_obj_t *lab2 = lv_label_create(win);
    lv_label_set_text(lab2, "自动亮度");
    lv_obj_set_style_text_font(lab2, font_norm, 0);
    lv_obj_set_style_text_color(lab2, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab2, 60, 190);

    /* 自动亮度开关 */
    lv_obj_t *auto_sw = lv_switch_create(win);
    lv_obj_set_pos(auto_sw, 800, 185);
    lv_obj_add_event_cb(auto_sw, auto_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 体感校准标签 */
    lv_obj_t *lab4 = lv_label_create(win);
    lv_label_set_text(lab4, "体感校准");
    lv_obj_set_style_text_font(lab4, font_norm, 0);
    lv_obj_set_style_text_color(lab4, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab4, 60, 280);

    /* 体感校准按钮（绿色） */
    lv_obj_t *cal_btn = lv_button_create(win);
    lv_obj_set_size(cal_btn, 140, 44);
    lv_obj_set_pos(cal_btn, 700, 270);
    lv_obj_set_style_bg_color(cal_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(cal_btn, 10, 0);
    lv_obj_t *cal_lab = lv_label_create(cal_btn);
    lv_label_set_text(cal_lab, "校准");
    lv_obj_set_style_text_font(cal_lab, font_norm, 0);
    lv_obj_set_style_text_color(cal_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(cal_lab);
    lv_obj_add_event_cb(cal_btn, calibrate_cb, LV_EVENT_CLICKED, NULL);

    /* 关于：版本信息 */
    lv_obj_t *lab5 = lv_label_create(win);
    lv_label_set_text(lab5, "智趣魔方 · 版本 1.0");
    lv_obj_set_style_text_font(lab5, font_norm, 0);
    lv_obj_set_style_text_color(lab5, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(lab5, 60, 370);

    lv_screen_load(scr);
    settings_screen = scr;
    return scr;
}
