/**
 * @file    ball.c
 * @brief   重力滚球体感游戏界面
 *
 * 功能说明：
 *   - 倾斜开发板，用 MPU6050 加速度计控制小球在屏幕上滚动。
 *   - 屏幕上有 1 个绿色终点圆环和若干红色陷阱圆环，位置每次随机生成。
 *   - 小球滚进绿色终点圆环 = 游戏成功；滚进红色陷阱 = 游戏失败。
 *   - 游戏结束弹出提示 + 「重新开始」按钮，点击后重新随机生成圆环并复位小球。
 *
 * 物理模型：
 *   - 加速度计读数乘 ACCEL_SCALE 转化为加速度，累加得到速度；
 *   - 每帧乘 FRICTION 做摩擦衰减，并限制最大速度；
 *   - 小球撞到屏幕边界反弹（速度反向并减半）。
 *
 * 依赖模块：
 *   - imu.h：读取陀螺仪倾斜值。
 *   - game_center.h：返回游戏中心界面。
 */
#include "../lvgl/lvgl.h"
#include "game_center.h"
#include "ball.h"
#include "imu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 物理参数（越小越慢/越灵敏可自行改） */
#define ACCEL_SCALE 0.0002f   // 倾斜读数 -> 加速度 的换算系数
#define FRICTION    0.90f     // 摩擦系数（每帧速度乘这个，越小越快停）
#define MAX_SPEED   30.0f     // 最大速度（像素/帧）
#define BALL_R      15        // 小球半径（像素）

/* 起点（小球初始位置） */
#define START_X 512
#define START_Y 300

/* 圆环的数量与半径 */
#define TRAP_COUNT 4          // 陷阱圈数量（可改成别的数）
#define TARGET_R   40         // 终点圆环半径
#define TRAP_R     35         // 陷阱圆环半径

/* 离起点的最小距离（避免开局就撞上） */
#define START_CLEAR 140

lv_obj_t * ball_screen = NULL;

static lv_obj_t *ball_obj = NULL;            // 小球控件
static lv_obj_t *target_obj = NULL;          // 终点圆环控件
static lv_obj_t *trap_objs[TRAP_COUNT];      // 陷阱圆环控件数组
static lv_obj_t *result_label = NULL;        // 结果提示标签
static lv_obj_t *restart_btn = NULL;         // 重新开始按钮

static float ball_x = START_X, ball_y = START_Y;  // 小球当前位置
static float ball_vx = 0, ball_vy = 0;            // 小球当前速度
static lv_timer_t *game_timer = NULL;             // 游戏循环定时器

/* 游戏状态枚举 */
typedef enum { GS_PLAYING, GS_WIN, GS_LOSE } game_state_t;
static game_state_t state = GS_PLAYING;

/* 圆心坐标结构 */
typedef struct { int x, y; } pt_t;
static pt_t target_pos;              // 终点圆心坐标
static pt_t trap_pos[TRAP_COUNT];    // 陷阱圆心坐标数组

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

/* 两点距离的平方（用于碰撞判断，避免开平方运算） */
static int dist2(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2, dy = y1 - y2;
    return dx * dx + dy * dy;
}

/*
 * 随机生成终点和陷阱圈的位置。
 * 约束：不贴边、彼此不重叠、离起点有足够距离（START_CLEAR），
 * 保证有通路让小球滚过去，不会开局必死。
 */
static void randomize_circles(void)
{
    // 终点：随机位置，但要离起点足够远
    do {
        target_pos.x = TARGET_R + 50 + rand() % (1024 - 2 * (TARGET_R + 50));
        target_pos.y = TARGET_R + 50 + rand() % (600  - 2 * (TARGET_R + 50));
    } while(dist2(target_pos.x, target_pos.y, START_X, START_Y) < START_CLEAR * START_CLEAR);

    // 陷阱圈：每个都要满足约束（离起点远、离终点远、彼此不重叠），最多尝试 300 次
    for(int i = 0; i < TRAP_COUNT; i++) {
        int ok = 0;
        for(int tries = 0; tries < 300 && !ok; tries++) {
            trap_pos[i].x = TRAP_R + 40 + rand() % (1024 - 2 * (TRAP_R + 40));
            trap_pos[i].y = TRAP_R + 40 + rand() % (600  - 2 * (TRAP_R + 40));
            ok = 1;
            if(dist2(trap_pos[i].x, trap_pos[i].y, START_X, START_Y) < START_CLEAR * START_CLEAR) ok = 0;
            else if(dist2(trap_pos[i].x, trap_pos[i].y, target_pos.x, target_pos.y) < 150 * 150) ok = 0;
            else {
                for(int j = 0; j < i; j++)
                    if(dist2(trap_pos[i].x, trap_pos[i].y, trap_pos[j].x, trap_pos[j].y) < 130 * 130) { ok = 0; break; }
            }
        }
    }
}

/* 把 UI 圆环控件摆到新生成的坐标位置（圆心坐标转左上角坐标） */
static void apply_circle_pos(void)
{
    lv_obj_set_pos(target_obj, target_pos.x - TARGET_R, target_pos.y - TARGET_R);
    for(int i = 0; i < TRAP_COUNT; i++)
        lv_obj_set_pos(trap_objs[i], trap_pos[i].x - TRAP_R, trap_pos[i].y - TRAP_R);
}

/* 游戏结束处理：设置状态、显示提示和重新开始按钮 */
static void game_over(game_state_t s)
{
    state = s;
    lv_label_set_text(result_label, s == GS_WIN ? "游戏成功！" : "游戏失败！");
    lv_obj_remove_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    printf("%s\n", s == GS_WIN ? "游戏成功！" : "游戏失败！");
}

/* 重新开始：重新随机生成圆环、复位小球、隐藏结束提示 */
static void restart_game(void)
{
    randomize_circles();
    apply_circle_pos();
    ball_x = START_X; ball_y = START_Y; ball_vx = 0; ball_vy = 0;
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    state = GS_PLAYING;
}

/* 重新开始按钮回调 */
static void restart_btn_cb(lv_event_t *e)
{
    restart_game();
}

/*
 * 游戏循环（每 20ms 一次 ≈ 50fps）：
 * 读加速度计 -> 更新速度（累加加速度、摩擦衰减、限速）-> 更新位置
 * -> 边界反弹 -> 更新小球控件位置 -> 判断是否滚进终点或陷阱。
 */
static void game_loop_cb(lv_timer_t *t)
{
    if(state != GS_PLAYING) return;   // 结束状态暂停物理

    int tx = imu_tilt_x();
    int ty = imu_tilt_y();

    // 加速度计读数累加到速度（倾斜越大加速越快）
    ball_vx += tx * ACCEL_SCALE;
    ball_vy += ty * ACCEL_SCALE;
    // 摩擦衰减
    ball_vx *= FRICTION;
    ball_vy *= FRICTION;
    // 限制最大速度
    if(ball_vx >  MAX_SPEED) ball_vx =  MAX_SPEED;
    if(ball_vx < -MAX_SPEED) ball_vx = -MAX_SPEED;
    if(ball_vy >  MAX_SPEED) ball_vy =  MAX_SPEED;
    if(ball_vy < -MAX_SPEED) ball_vy = -MAX_SPEED;

    // 更新位置
    ball_x += ball_vx;
    ball_y += ball_vy;

    // 边界反弹（速度反向并减半，模拟能量损失）
    if(ball_x < BALL_R)         { ball_x = BALL_R;          ball_vx = -ball_vx * 0.5f; }
    if(ball_x > 1024 - BALL_R)  { ball_x = 1024 - BALL_R;   ball_vx = -ball_vx * 0.5f; }
    if(ball_y < BALL_R)         { ball_y = BALL_R;          ball_vy = -ball_vy * 0.5f; }
    if(ball_y > 600 - BALL_R)   { ball_y = 600 - BALL_R;    ball_vy = -ball_vy * 0.5f; }

    // 更新小球控件位置（球心转左上角）
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);

    // 滚进终点圆环 -> 成功
    if(dist2((int)ball_x, (int)ball_y, target_pos.x, target_pos.y) < TARGET_R * TARGET_R) {
        game_over(GS_WIN);
        return;
    }
    // 滚进任意陷阱圆环 -> 失败
    for(int i = 0; i < TRAP_COUNT; i++) {
        if(dist2((int)ball_x, (int)ball_y, trap_pos[i].x, trap_pos[i].y) < TRAP_R * TRAP_R) {
            game_over(GS_LOSE);
            return;
        }
    }
}

/* 返回按钮回调：先删游戏循环定时器，再切回游戏中心 */
static void to_game_center_cb(lv_event_t *e)
{
    if(game_timer != NULL) { lv_timer_delete(game_timer); game_timer = NULL; }
    if(select_game_screen == NULL)
        select_game_screen = ui_select_game_screen();
    lv_screen_load(select_game_screen);
    if(ball_screen != NULL) { lv_obj_delete(ball_screen); ball_screen = NULL; }
}

/*
 * 创建一个空心圆环控件（透明背景 + 彩色边框 + 圆形）。
 * @param parent    父对象
 * @param r         半径（像素）
 * @param color     边框颜色
 * @param border_w  边框宽度
 * @return 圆环控件指针
 */
static lv_obj_t *create_circle(lv_obj_t *parent, int r, lv_color_t color, int border_w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, r * 2, r * 2);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);      // 背景透明，只留边框
    lv_obj_set_style_border_width(c, border_w, 0);
    lv_obj_set_style_border_color(c, color, 0);
    return c;
}

/*
 * 重力滚球界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_ball_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_big   = cn_font(36);   // 结束提示大字
    lv_font_t *font_norm  = cn_font(20);   // 按钮字号
    lv_font_t *font_desc  = cn_font(14);   // 提示小字

    srand(time(NULL));       // 初始化随机数种子（只在进入时调一次）
    state = GS_PLAYING;

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
    lv_obj_set_style_text_font(back_lab, font_desc, 0);
    lv_obj_set_style_text_color(back_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, to_game_center_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "重力滚球");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 终点圆环（绿色） */
    target_obj = create_circle(win, TARGET_R, lv_color_hex(0x2FD3A0), 3);

    /* 陷阱圆环（红色） */
    for(int i = 0; i < TRAP_COUNT; i++)
        trap_objs[i] = create_circle(win, TRAP_R, lv_color_hex(0xE74C3C), 3);

    /* 小球（橙色实心圆） */
    ball_obj = lv_obj_create(win);
    lv_obj_set_size(ball_obj, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_bg_color(ball_obj, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_radius(ball_obj, LV_RADIUS_CIRCLE, 0);
    ball_x = START_X; ball_y = START_Y; ball_vx = 0; ball_vy = 0;
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);

    /* 结果提示标签（默认隐藏） */
    result_label = lv_label_create(win);
    lv_label_set_text(result_label, "");
    lv_obj_set_style_text_font(result_label, font_big, 0);
    lv_obj_set_style_text_color(result_label, lv_color_hex(0xF5A623), 0);
    lv_obj_align(result_label, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);

    /* 重新开始按钮（默认隐藏，绿色） */
    restart_btn = lv_button_create(win);
    lv_obj_set_size(restart_btn, 160, 50);
    lv_obj_set_style_bg_color(restart_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(restart_btn, 12, 0);
    lv_obj_align(restart_btn, LV_ALIGN_CENTER, 0, 50);
    lv_obj_t *rst_lab = lv_label_create(restart_btn);
    lv_label_set_text(rst_lab, "重新开始");
    lv_obj_set_style_text_font(rst_lab, font_norm, 0);
    lv_obj_set_style_text_color(rst_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(rst_lab);
    lv_obj_add_event_cb(restart_btn, restart_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);

    /* 底部操作提示 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "倾斜开发板，滚进绿色圆环获胜，避开红色陷阱");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* 随机生成圆环位置并摆放到界面 */
    randomize_circles();
    apply_circle_pos();

    /* 启动游戏循环定时器（20ms 周期） */
    game_timer = lv_timer_create(game_loop_cb, 10, NULL);

    lv_screen_load(scr);
    ball_screen = scr;
    return scr;
}
