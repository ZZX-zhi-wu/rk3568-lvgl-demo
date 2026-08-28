/**
 * @file    main_interface.c
 * @brief   桌面 Launcher（App 选择界面）
 *
 * 功能说明：
 *   - 登录后的主菜单界面，显示 4 个 App 图标卡片：
 *       传感器实验室 / 体感游戏 / 电子相册 / 设置。
 *   - 点击卡片切换到对应界面，并删除桌面屏（避免重复创建）。
 *   - 顶部有标题「桌面」和「玩家小明」状态胶囊。
 *
 * 全局变量说明：
 *   - select_app_screen：桌面屏，各子界面返回时复用；切换/删除后置 NULL，
 *     避免悬空指针导致二次点击段错误。
 *   - album_screen：电子相册屏（album.c 使用）。
 */
#include "../lvgl/lvgl.h"
#include "album.h"
#include "2048.h"
#include "game_center.h"
#include <stdio.h>
#include "main_interface.h"
#include "settings.h"
#include "sensor_lab.h"


/* 中文字体路径：微软雅黑 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

lv_obj_t * album_screen = NULL;        // 电子相册屏
lv_obj_t * select_app_screen = NULL;   // 桌面屏（全局，供各界面返回复用）

/**
 * 创建指定字号的中文字体（带缓存，避免重复新建造成内存泄漏）。
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

/* 切换到电子相册 */
static void to_album_screen_cb(lv_event_t *e)
{
    if(album_screen == NULL)
        album_screen = ui_album_init();
    lv_screen_load(album_screen);
    if(select_app_screen != NULL)
    {
        lv_obj_delete(select_app_screen);
        select_app_screen = NULL;
    }
}

/* 切换到体感游戏（游戏中心） */
static void to_game_screen_cb(lv_event_t *e)
{
    if(select_game_screen == NULL)              // 游戏中心屏复用
        select_game_screen = ui_select_game_screen();
    lv_screen_load(select_game_screen);
    if(select_app_screen != NULL)
    {
        lv_obj_delete(select_app_screen);
        select_app_screen = NULL;
    }
}

/* 切换到传感器实验室 */
static void to_sensor_lab_cb(lv_event_t *e)
{
    if(sensor_lab_screen == NULL)
        sensor_lab_screen = ui_sensor_lab_init();
    lv_screen_load(sensor_lab_screen);
    if(select_app_screen != NULL)
    {
        lv_obj_delete(select_app_screen);
        select_app_screen = NULL;
    }
}

/* 切换到设置界面 */
static void to_settings_cb(lv_event_t *e)
{
    if(settings_screen == NULL)
        settings_screen = ui_settings_init();
    lv_screen_load(settings_screen);
    if(select_app_screen != NULL)
    {
        lv_obj_delete(select_app_screen);
        select_app_screen = NULL;
    }
}


/*
 * 创建单个 App 图标卡片（图标 + App 名 + 描述）。
 * @param parent    父对象
 * @param icon_path 图标 BMP 路径
 * @param name      App 名
 * @param desc      描述文字
 * @param f_name    名字字号
 * @param f_desc    描述字号
 * @param cb        点击回调
 * @param x         卡片左上角 x 坐标
 */
static void app_tile_create(lv_obj_t *parent, const char *icon_path,
                            const char *name, const char *desc,
                            lv_font_t *f_name, lv_font_t *f_desc,
                            lv_event_cb_t cb, int x)
{
    /* 卡片背景 */
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_size(tile, 200, 230);
    lv_obj_set_pos(tile, x, 215);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(tile, 16, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);

    /* 图标（已按 96x96 尺寸生成，直接显示） */
    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, icon_path);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 18);

    /* App 名 */
    lv_obj_t *name_lab = lv_label_create(tile);
    lv_label_set_text(name_lab, name);
    lv_obj_set_style_text_font(name_lab, f_name, 0);
    lv_obj_set_style_text_color(name_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_align(name_lab, LV_ALIGN_TOP_MID, 0, 140);

    /* 描述 */
    lv_obj_t *desc_lab = lv_label_create(tile);
    lv_label_set_text(desc_lab, desc);
    lv_obj_set_style_text_font(desc_lab, f_desc, 0);
    lv_obj_set_style_text_color(desc_lab, lv_color_hex(0x6A6A7A), 0);
    lv_obj_align(desc_lab, LV_ALIGN_TOP_MID, 0, 175);

    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
}

/*
 * 桌面界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_select_app_screen(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_name  = cn_font(20);   // App 名字号
    lv_font_t *font_desc  = cn_font(14);   // 描述字号

    /* 创建屏幕和全屏窗口 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图（铺满全屏） */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_home.bmp", 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题「桌面」 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "桌面");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 30, 15);

    /* 右上角「玩家小明」状态胶囊 */
    lv_obj_t *pill = lv_label_create(win);
    lv_label_set_text(pill, "玩家小明");
    lv_obj_set_style_text_font(pill, font_name, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(pill, 20, 0);
    lv_obj_set_style_pad_all(pill, 10, 0);
    lv_obj_set_pos(pill, 900, 12);

    /* 四个 App 图标卡片（横向排列） */
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_sensor.bmp",
                    "传感器实验室", "姿态 · 照度", font_name, font_desc, to_sensor_lab_cb, 52);
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_game.bmp",
                    "体感游戏", "陀螺仪操控", font_name, font_desc, to_game_screen_cb, 292);
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_album.bmp",
                    "电子相册", "自动播放", font_name, font_desc, to_album_screen_cb, 532);
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_settings.bmp",
                    "设置", "亮度 · 体感", font_name, font_desc, to_settings_cb, 772);

    lv_screen_load(scr);
    select_app_screen = scr;   // 记录全局，供返回时复用
    return scr;
}
