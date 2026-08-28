/**
 * @file    sensor_lab.c
 * @brief   传感器实验室界面（实时数据可视化）
 *
 * 功能说明：
 *   - 实时显示 MPU6050 加速度/姿态角 和 BH1750 照度。
 *   - 左侧：俯仰角 Pitch、横滚角 Roll 两个对称仪表条（-90° ~ +90°）。
 *   - 右侧：加速度三轴实时滚动曲线（红 ax / 绿 ay / 蓝 az）。
 *   - 底部：照度仪表条 + 三轴加速度/陀螺仪数值。
 *   - 每 200ms 刷新一次。
 *
 * 注意：
 *   - 姿态角用已读的 ax/ay/az 直接算（不调 sensor_pitch_roll，避免重复读文件）。
 *   - 板上 MPU6050 的 X 轴对应"左右"、Y 轴对应"前后"，所以俯仰用 ay、横滚用 ax。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "sensor_lab.h"
#include "sensor.h"
#include <stdio.h>
#include <math.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

lv_obj_t * sensor_lab_screen = NULL;

static lv_obj_t *chart = NULL;               // 加速度曲线控件
static lv_chart_series_t *ser_ax, *ser_ay, *ser_az;  // 三条曲线（ax/ay/az）
static lv_obj_t *pitch_bar, *roll_bar, *lux_bar;     // 三个仪表条
static lv_obj_t *pitch_val, *roll_val, *lux_val;     // 三个数值标签
static lv_obj_t *accel_lab, *gyro_lab;               // 加速度/陀螺仪数值标签
static lv_timer_t *sensor_timer = NULL;              // 刷新定时器

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
        if(cache[i] != NULL && cache_size[i] == size) return cache[i];
        if(cache[i] == NULL && empty < 0) empty = i;
    }
    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f) { LV_LOG_ERROR("freetype font create failed"); return NULL; }
    cache[empty] = f;
    cache_size[empty] = size;
    return f;
}

/* 每 200ms 刷新一次：读传感器 -> 更新曲线、仪表条、数值标签 */
static void sensor_timer_cb(lv_timer_t *t)
{
    char buf[64];
    float ax = sensor_accel_x();
    float ay = sensor_accel_y();
    float az = sensor_accel_z();
    float gx = sensor_gyro_x();
    float gy = sensor_gyro_y();
    float gz = sensor_gyro_z();
    int lux = sensor_illuminance();

    // 姿态角：直接用已读的 ax/ay/az 算（板上 X=左右、Y=前后，所以 ay 对应俯仰、ax 对应横滚）
    float pitch = atan2f(-ay, sqrtf(ax*ax + az*az)) * 180.0f / 3.14159265f;  // 俯仰（前后）
    float roll  = atan2f( ax, sqrtf(ay*ay + az*az)) * 180.0f / 3.14159265f;  // 横滚（左右）

    // 曲线（加速度 g 值放大 100 存整数，±2g 对应 ±200）
    lv_chart_set_next_value(chart, ser_ax, (int)(ax * 100));
    lv_chart_set_next_value(chart, ser_ay, (int)(ay * 100));
    lv_chart_set_next_value(chart, ser_az, (int)(az * 100));

    // 姿态角：更新仪表条 + 数值（用标准 snprintf 格式化 float，因为 LVGL 的 set_text_fmt 不支持 %f）
    lv_bar_set_value(pitch_bar, (int)pitch, LV_ANIM_OFF);
    lv_bar_set_value(roll_bar,  (int)roll,  LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%+.1f°", pitch);
    lv_label_set_text(pitch_val, buf);
    snprintf(buf, sizeof(buf), "%+.1f°", roll);
    lv_label_set_text(roll_val, buf);

    // 照度
    lv_bar_set_value(lux_bar, lux, LV_ANIM_OFF);
    lv_label_set_text_fmt(lux_val, "%d lux", lux);

    // 加速度/陀螺仪数值
    snprintf(buf, sizeof(buf), "加速度  ax %+.2f  ay %+.2f  az %+.2f g", ax, ay, az);
    lv_label_set_text(accel_lab, buf);
    snprintf(buf, sizeof(buf), "陀螺仪  gx %+.1f  gy %+.1f  gz %+.1f °/s", gx, gy, gz);
    lv_label_set_text(gyro_lab, buf);
}

/* 返回按钮回调：先删刷新定时器（避免悬空），再切回桌面 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(sensor_timer != NULL) { lv_timer_delete(sensor_timer); sensor_timer = NULL; }
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    if(sensor_lab_screen != NULL) { lv_obj_delete(sensor_lab_screen); sensor_lab_screen = NULL; }
}

/*
 * 传感器实验室界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_sensor_lab_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(18);   // 正文字号
    lv_font_t *font_desc  = cn_font(14);   // 提示小字

    sensor_init();        // 读量程比例因子
    sensor_calibrate();   // 进入时校准零点（板子水平放）

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
    lv_label_set_text(title, "传感器实验室");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 右上角「实时」状态胶囊 */
    lv_obj_t *pill = lv_label_create(win);
    lv_label_set_text(pill, "实时");
    lv_obj_set_style_text_font(pill, font_desc, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0F2A21), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 20, 0);
    lv_obj_set_style_pad_all(pill, 10, 0);
    lv_obj_set_pos(pill, 880, 15);

    /* 左侧：俯仰角标签 */
    lv_obj_t *lab_pitch = lv_label_create(win);
    lv_label_set_text(lab_pitch, "俯仰角 Pitch");
    lv_obj_set_style_text_font(lab_pitch, font_norm, 0);
    lv_obj_set_style_text_color(lab_pitch, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_pitch, 40, 84);

    /* 俯仰角仪表条（对称模式，中间为 0°，绿色） */
    pitch_bar = lv_bar_create(win);
    lv_obj_set_size(pitch_bar, 220, 24);
    lv_obj_set_pos(pitch_bar, 40, 116);
    lv_bar_set_range(pitch_bar, -90, 90);
    lv_bar_set_mode(pitch_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_obj_set_style_bg_color(pitch_bar, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_color(pitch_bar, lv_color_hex(0x2FD3A0), LV_PART_INDICATOR);

    /* 俯仰角数值 */
    pitch_val = lv_label_create(win);
    lv_label_set_text(pitch_val, "+0.0°");
    lv_obj_set_style_text_font(pitch_val, font_norm, 0);
    lv_obj_set_style_text_color(pitch_val, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_pos(pitch_val, 270, 108);

    /* 左侧：横滚角标签 */
    lv_obj_t *lab_roll = lv_label_create(win);
    lv_label_set_text(lab_roll, "横滚角 Roll");
    lv_obj_set_style_text_font(lab_roll, font_norm, 0);
    lv_obj_set_style_text_color(lab_roll, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_roll, 40, 174);

    /* 横滚角仪表条（橙色） */
    roll_bar = lv_bar_create(win);
    lv_obj_set_size(roll_bar, 220, 24);
    lv_obj_set_pos(roll_bar, 40, 206);
    lv_bar_set_range(roll_bar, -90, 90);
    lv_bar_set_mode(roll_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_obj_set_style_bg_color(roll_bar, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_color(roll_bar, lv_color_hex(0xF5A623), LV_PART_INDICATOR);

    /* 横滚角数值 */
    roll_val = lv_label_create(win);
    lv_label_set_text(roll_val, "+0.0°");
    lv_obj_set_style_text_font(roll_val, font_norm, 0);
    lv_obj_set_style_text_color(roll_val, lv_color_hex(0xF5A623), 0);
    lv_obj_set_pos(roll_val, 270, 198);

    /* 右侧：加速度曲线标题（含图例说明） */
    lv_obj_t *lab_chart = lv_label_create(win);
    lv_label_set_text(lab_chart, "加速度曲线 (g)  ax红 ay绿 az蓝");
    lv_obj_set_style_text_font(lab_chart, font_desc, 0);
    lv_obj_set_style_text_color(lab_chart, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(lab_chart, 340, 84);

    /* 加速度曲线（折线图，滚动更新，±2g 范围） */
    chart = lv_chart_create(win);
    lv_obj_set_size(chart, 650, 300);
    lv_obj_set_pos(chart, 340, 116);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 50);                 // 显示最近 50 个点
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);  // 滚动模式
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -200, 200); // ±2g
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x2B2B3A), 0);

    /* 三条曲线：红 ax、绿 ay、蓝 az */
    ser_ax = lv_chart_add_series(chart, lv_color_hex(0xE74C3C), LV_CHART_AXIS_PRIMARY_Y);
    ser_ay = lv_chart_add_series(chart, lv_color_hex(0x2FD3A0), LV_CHART_AXIS_PRIMARY_Y);
    ser_az = lv_chart_add_series(chart, lv_color_hex(0x4DA6FF), LV_CHART_AXIS_PRIMARY_Y);

    /* 底部：照度标签 */
    lv_obj_t *lab_lux = lv_label_create(win);
    lv_label_set_text(lab_lux, "照度");
    lv_obj_set_style_text_font(lab_lux, font_norm, 0);
    lv_obj_set_style_text_color(lab_lux, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_lux, 40, 440);

    /* 照度仪表条（0~1000 lux，蓝色） */
    lux_bar = lv_bar_create(win);
    lv_obj_set_size(lux_bar, 220, 20);
    lv_obj_set_pos(lux_bar, 40, 470);
    lv_bar_set_range(lux_bar, 0, 1000);
    lv_obj_set_style_bg_color(lux_bar, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_color(lux_bar, lv_color_hex(0x4DA6FF), LV_PART_INDICATOR);

    /* 照度数值 */
    lux_val = lv_label_create(win);
    lv_label_set_text(lux_val, "0 lux");
    lv_obj_set_style_text_font(lux_val, font_norm, 0);
    lv_obj_set_style_text_color(lux_val, lv_color_hex(0x4DA6FF), 0);
    lv_obj_set_pos(lux_val, 270, 465);

    /* 底部：加速度数值标签 */
    accel_lab = lv_label_create(win);
    lv_label_set_text(accel_lab, "加速度  ax 0.00  ay 0.00  az 0.00 g");
    lv_obj_set_style_text_font(accel_lab, font_norm, 0);
    lv_obj_set_style_text_color(accel_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(accel_lab, 340, 445);

    /* 底部：陀螺仪数值标签 */
    gyro_lab = lv_label_create(win);
    lv_label_set_text(gyro_lab, "陀螺仪  gx 0.0  gy 0.0  gz 0.0 °/s");
    lv_obj_set_style_text_font(gyro_lab, font_norm, 0);
    lv_obj_set_style_text_color(gyro_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(gyro_lab, 340, 495);

    /* 启动刷新定时器（200ms 周期，约 5Hz，足够且不卡顿） */
    sensor_timer = lv_timer_create(sensor_timer_cb, 200, NULL);

    lv_screen_load(scr);
    sensor_lab_screen = scr;
    return scr;
}
