/**
 * @file    album.c
 * @brief   电子相册界面
 *
 * 功能说明：
 *   - 全屏显示照片（1024x600 BMP），底部有缩略图条 + 上一张/播放/下一张按钮。
 *   - 支持自动播放（每 3 秒切换一张）。
 *   - 点击大图可让底部栏（缩略图 + 按键）整体滑下隐藏 / 滑上显示（升降动画）。
 *   - 缩略图带高亮边框标识当前照片。
 *
 * 全局变量说明：
 *   - album_screen：相册屏（定义在 main_interface.c）。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include <stdio.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"
#define PHOTO_COUNT 6   // 照片总数

/* 底部栏（缩略图+按键容器）的两个 y 坐标 */
#define BAR_VISIBLE_Y 440   // 显示时的 y
#define BAR_HIDDEN_Y  610   // 隐藏时的 y（滑到屏幕外）

/* 大图路径 */
static char *photo_path[PHOTO_COUNT] = {
    "A:/work_space/bmp_pic/photo/photo_1.bmp",
    "A:/work_space/bmp_pic/photo/photo_2.bmp",
    "A:/work_space/bmp_pic/photo/photo_3.bmp",
    "A:/work_space/bmp_pic/photo/photo_4.bmp",
    "A:/work_space/bmp_pic/photo/photo_5.bmp",
    "A:/work_space/bmp_pic/photo/photo_6.bmp",
};

/* 缩略图路径（140x82） */
static char *thumb_path[PHOTO_COUNT] = {
    "A:/work_space/bmp_pic/photo/thumb_1.bmp",
    "A:/work_space/bmp_pic/photo/thumb_2.bmp",
    "A:/work_space/bmp_pic/photo/thumb_3.bmp",
    "A:/work_space/bmp_pic/photo/thumb_4.bmp",
    "A:/work_space/bmp_pic/photo/thumb_5.bmp",
    "A:/work_space/bmp_pic/photo/thumb_6.bmp",
};

static int current_index = 0;                // 当前显示的照片索引
static lv_obj_t *big_image = NULL;           // 大图控件
static lv_obj_t *counter_label = NULL;       // 计数标签（x / 总数）
static lv_obj_t *play_lab = NULL;            // 播放/暂停按钮文字
static lv_obj_t *thumb_objs[PHOTO_COUNT];    // 缩略图控件数组
static lv_obj_t *ctrl_bar = NULL;            // 底部栏容器（缩略图 + 按键）
static lv_timer_t *play_timer = NULL;        // 自动播放定时器
static int is_playing = 0;                   // 是否正在自动播放
static int bar_visible = 1;                  // 底部栏是否可见

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

/* 显示第 index 张照片，并更新计数标签和缩略图高亮 */
static void show_photo(int index)
{
    lv_image_set_src(big_image, photo_path[index]);
    lv_label_set_text_fmt(counter_label, "%d / %d", index + 1, PHOTO_COUNT);
    for(int i = 0; i < PHOTO_COUNT; i++) {
        if(i == index) {
            // 当前照片的缩略图加粗绿色边框
            lv_obj_set_style_border_width(thumb_objs[i], 3, 0);
            lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2FD3A0), 0);
        } else {
            lv_obj_set_style_border_width(thumb_objs[i], 2, 0);
            lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2B2B3A), 0);
        }
    }
}

/* 上一张按钮回调 */
static void prev_cb(lv_event_t *e)
{
    current_index = (current_index - 1 + PHOTO_COUNT) % PHOTO_COUNT;
    show_photo(current_index);
}

/* 下一张按钮回调 */
static void next_cb(lv_event_t *e)
{
    current_index = (current_index + 1) % PHOTO_COUNT;
    show_photo(current_index);
}

/* 自动播放定时器回调：每 3 秒切下一张 */
static void play_timer_cb(lv_timer_t *t)
{
    current_index = (current_index + 1) % PHOTO_COUNT;
    show_photo(current_index);
}

/* 播放/暂停按钮回调：切换定时器暂停/恢复，并更新按钮文字 */
static void play_pause_cb(lv_event_t *e)
{
    if(is_playing) {
        lv_timer_pause(play_timer);
        is_playing = 0;
        lv_label_set_text(play_lab, "播放");
    } else {
        if(play_timer == NULL)
            play_timer = lv_timer_create(play_timer_cb, 3000, NULL);
        lv_timer_resume(play_timer);
        is_playing = 1;
        lv_label_set_text(play_lab, "暂停");
    }
}

/* 升降动画的执行回调：每帧把底部栏的 y 设为 v */
static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/* 切换底部栏升降：显示<->隐藏之间做 250ms 缓动动画 */
static void toggle_bar(void)
{
    int32_t from = bar_visible ? BAR_VISIBLE_Y : BAR_HIDDEN_Y;
    int32_t to   = bar_visible ? BAR_HIDDEN_Y : BAR_VISIBLE_Y;
    bar_visible = !bar_visible;

    lv_anim_delete(ctrl_bar, anim_y_cb);   // 先取消上一次动画，避免冲突
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctrl_bar);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 点击大图 -> 升降底部栏 */
static void photo_click_cb(lv_event_t *e)
{
    toggle_bar();
}

/* 返回按钮回调：先停掉自动播放定时器（避免悬空），再切回桌面 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(play_timer != NULL) {
        lv_timer_delete(play_timer);
        play_timer = NULL;
    }
    is_playing = 0;

    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    if(album_screen != NULL) {
        lv_obj_delete(album_screen);
        album_screen = NULL;
    }
}

/*
 * 电子相册界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_album_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(18);   // 正文字号

    /* 每次进入重置状态 */
    current_index = 0;
    is_playing = 0;
    play_timer = NULL;
    bar_visible = 1;

    /* 创建屏幕和全屏窗口 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 大图（全屏，可点击用于升降底部栏） */
    big_image = lv_image_create(win);
    lv_image_set_src(big_image, photo_path[0]);
    lv_obj_add_flag(big_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(big_image, photo_click_cb, LV_EVENT_CLICKED, NULL);

    /* 标题栏：返回按钮 */
    lv_obj_t *back_btn = lv_button_create(win);
    lv_obj_set_size(back_btn, 90, 40);
    lv_obj_set_pos(back_btn, 20, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_90, 0);
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
    lv_label_set_text(title, "电子相册");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 计数标签（右上角，"当前/总数"） */
    counter_label = lv_label_create(win);
    lv_label_set_text_fmt(counter_label, "1 / %d", PHOTO_COUNT);
    lv_obj_set_style_text_font(counter_label, font_norm, 0);
    lv_obj_set_style_text_color(counter_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(counter_label, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(counter_label, LV_OPA_90, 0);
    lv_obj_set_style_radius(counter_label, 20, 0);
    lv_obj_set_style_pad_all(counter_label, 10, 0);
    lv_obj_set_pos(counter_label, 890, 12);

    /* 底部栏容器（缩略图 + 按键），整体随点击升降 */
    ctrl_bar = lv_obj_create(win);
    lv_obj_set_size(ctrl_bar, 1024, 160);
    lv_obj_set_pos(ctrl_bar, 0, BAR_VISIBLE_Y);
    lv_obj_set_style_bg_opa(ctrl_bar, LV_OPA_TRANSP, 0);   // 透明，不挡照片
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_pad_all(ctrl_bar, 0, 0);

    /* 缩略图条（ctrl_bar 的子对象，用相对坐标） */
    int thumb_x[PHOTO_COUNT] = {62, 214, 366, 518, 670, 822};
    for(int i = 0; i < PHOTO_COUNT; i++) {
        thumb_objs[i] = lv_image_create(ctrl_bar);
        lv_image_set_src(thumb_objs[i], thumb_path[i]);
        lv_obj_set_pos(thumb_objs[i], thumb_x[i], 15);
        lv_obj_set_style_border_width(thumb_objs[i], 2, 0);
        lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2B2B3A), 0);
        lv_obj_set_style_radius(thumb_objs[i], 6, 0);
    }

    /* 上一张按钮 */
    lv_obj_t *prev_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(prev_btn, 100, 44);
    lv_obj_set_pos(prev_btn, 342, 105);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(prev_btn, 1, 0);
    lv_obj_set_style_border_color(prev_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(prev_btn, 10, 0);
    lv_obj_t *prev_lab = lv_label_create(prev_btn);
    lv_label_set_text(prev_lab, "上一张");
    lv_obj_set_style_text_font(prev_lab, font_norm, 0);
    lv_obj_set_style_text_color(prev_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(prev_lab);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);

    /* 播放/暂停按钮（绿色） */
    lv_obj_t *play_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(play_btn, 100, 44);
    lv_obj_set_pos(play_btn, 462, 105);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(play_btn, 10, 0);
    play_lab = lv_label_create(play_btn);
    lv_label_set_text(play_lab, "播放");
    lv_obj_set_style_text_font(play_lab, font_norm, 0);
    lv_obj_set_style_text_color(play_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(play_lab);
    lv_obj_add_event_cb(play_btn, play_pause_cb, LV_EVENT_CLICKED, NULL);

    /* 下一张按钮 */
    lv_obj_t *next_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(next_btn, 100, 44);
    lv_obj_set_pos(next_btn, 582, 105);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(next_btn, 1, 0);
    lv_obj_set_style_border_color(next_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(next_btn, 10, 0);
    lv_obj_t *next_lab = lv_label_create(next_btn);
    lv_label_set_text(next_lab, "下一张");
    lv_obj_set_style_text_font(next_lab, font_norm, 0);
    lv_obj_set_style_text_color(next_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(next_lab);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, NULL);

    /* 初始显示第一张（顺便高亮第一张缩略图） */
    show_photo(0);
    lv_screen_load(scr);
    return scr;
}
