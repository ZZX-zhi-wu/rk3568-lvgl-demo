/**
 * @file    game_center.c
 * @brief   体感游戏中心界面
 *
 * 功能说明：
 *   - 游戏中心是「桌面 -> 具体游戏」的中间层菜单。
 *   - 显示两张游戏卡片：2048 体感版、重力滚球，点击进入对应游戏。
 *   - 顶部有返回按钮（回桌面）和「陀螺仪已就绪」状态提示。
 *
 * 全局变量说明（重要）：
 *   - select_game_screen：游戏中心屏，主界面和游戏返回时共用，切换/删除后需置 NULL，
 *     避免悬空指针导致二次点击段错误。
 *   - game_2048_screen：2048 游戏屏，2048.c 返回时要删它，所以此全局必须保留。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "2048.h"
#include "game_center.h"
#include <stdio.h>
#include "ball.h"
#include "snake.h"



/* 中文字体路径：微软雅黑 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

lv_obj_t * game_2048_screen = NULL;     // 2048 游戏屏（2048.c 返回时会删它，此全局必须保留）
lv_obj_t * select_game_screen = NULL;   // 游戏中心屏

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


/* 返回按钮回调：切回桌面并删除游戏中心屏 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    if(select_game_screen != NULL)
    {
        lv_obj_delete(select_game_screen);
        select_game_screen = NULL;
    }
}

/* 进入 2048 游戏 */
static void to_2048_screen_cb(lv_event_t *e)
{
    if(game_2048_screen == NULL)
        game_2048_screen = ui_2048_init();
    lv_screen_load(game_2048_screen);
    if(select_game_screen != NULL)
    {
        lv_obj_delete(select_game_screen);
        select_game_screen = NULL;
    }
}

/* 进入重力滚球游戏 */
static void to_ball_screen_cb(lv_event_t *e)
{
    if(ball_screen == NULL)
        ball_screen = ui_ball_init();
    lv_screen_load(ball_screen);
    if(select_game_screen != NULL)
    {
        lv_obj_delete(select_game_screen);
        select_game_screen = NULL;
    }
}

/* 进入体感贪吃蛇 */
static void to_snake_screen_cb(lv_event_t *e)
{
    if(snake_screen == NULL)
        snake_screen = ui_snake_init();
    lv_screen_load(snake_screen);
    if(select_game_screen != NULL)
    {
        lv_obj_delete(select_game_screen);
        select_game_screen = NULL;
    }
}



/*
 * 创建单个游戏卡片（图标 + 游戏名 + 描述 + 标签）。
 * @param parent    父对象
 * @param icon_path 图标 BMP 路径
 * @param name      游戏名
 * @param desc      描述文字（自动换行）
 * @param tag       右上角标签文字
 * @param f_name    名字字号
 * @param f_desc    描述字号
 * @param cb        点击回调
 * @param x         卡片左上角 x 坐标
 */
static void game_card_create(lv_obj_t *parent, const char *icon_path,
                             const char *name, const char *desc, const char *tag,
                             lv_font_t *f_name, lv_font_t *f_desc,
                             lv_event_cb_t cb, int x)
{
    /* 卡片背景 */
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, 300, 280);
    lv_obj_set_pos(card, x, 130);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);

    /* 图标（已按 64x64 尺寸生成，直接显示） */
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, icon_path);
    lv_obj_set_pos(img, 0, 0);

    /* 游戏名 */
    lv_obj_t *name_lab = lv_label_create(card);
    lv_label_set_text(name_lab, name);
    lv_obj_set_style_text_font(name_lab, f_name, 0);
    lv_obj_set_style_text_color(name_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(name_lab, 0, 80);

    /* 描述（设宽度让它自动换行） */
    lv_obj_t *desc_lab = lv_label_create(card);
    lv_label_set_text(desc_lab, desc);
    lv_obj_set_style_text_font(desc_lab, f_desc, 0);
    lv_obj_set_style_text_color(desc_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_width(desc_lab, 260);
    lv_obj_set_pos(desc_lab, 0, 116);

    /* 标签（橙色小胶囊） */
    lv_obj_t *tag_lab = lv_label_create(card);
    lv_label_set_text(tag_lab, tag);
    lv_obj_set_style_text_font(tag_lab, f_desc, 0);
    lv_obj_set_style_text_color(tag_lab, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_bg_color(tag_lab, lv_color_hex(0x2A2410), 0);
    lv_obj_set_style_bg_opa(tag_lab, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tag_lab, 6, 0);
    lv_obj_set_style_pad_all(tag_lab, 6, 0);
    lv_obj_set_pos(tag_lab, 0, 190);

    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
}

/*
 * 游戏中心界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_select_game_screen(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_name  = cn_font(22);   // 卡片名字字号
    lv_font_t *font_desc  = cn_font(14);   // 描述/标签字号

    /* 创建屏幕和全屏窗口 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图 */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_game.bmp", 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题栏：返回按钮 */
    lv_obj_t *back_btn = lv_button_create(win);
    lv_obj_set_size(back_btn, 90, 40);
    lv_obj_set_pos(back_btn, 20, 15);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_t *back_lab = lv_label_create(back_btn);
    lv_label_set_text(back_lab, "返回");
    lv_obj_set_style_text_font(back_lab, font_desc, 0);
    lv_obj_set_style_text_color(back_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, to_select_app_screen_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "体感游戏中心");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 状态提示胶囊（右上角，绿色） */
    lv_obj_t *pill = lv_label_create(win);
    lv_label_set_text(pill, "陀螺仪已就绪");
    lv_obj_set_style_text_font(pill, font_desc, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0F2A21), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 20, 0);
    lv_obj_set_style_pad_all(pill, 10, 0);
    lv_obj_set_pos(pill, 850, 15);

        /* 三个游戏卡片 */
    game_card_create(win, "A:/work_space/bmp_pic/icon/icon_2048.bmp",
                     "2048 体感版", "向四个方向倾斜甩动数字方块，合成更大的数字，冲击 2048。",
                     "体感 · 甩牌", font_name, font_desc, to_2048_screen_cb, 30);
    game_card_create(win, "A:/work_space/bmp_pic/icon/icon_ball.bmp",
                     "重力滚球", "倾斜开发板，用陀螺仪控制小球滚过迷宫到达终点，避开陷阱。",
                     "体感 · 陀螺仪", font_name, font_desc, to_ball_screen_cb, 362);
    game_card_create(win, "A:/work_space/bmp_pic/icon/icon_snake.bmp",
                     "体感贪吃蛇", "倾斜控制蛇的方向，吃食物变长，撞墙或撞自己则失败。",
                     "体感 · 转向", font_name, font_desc, to_snake_screen_cb, 694);


    /* 底部校准提示 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "首次进入请将开发板水平放置 2 秒完成校准");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6A6A7A), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_screen_load(scr);
    select_game_screen = scr;   // 记录全局，供返回/切换时删除
    return scr;
}
