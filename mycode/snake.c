/**
 * @file    snake.c
 * @brief   体感贪吃蛇游戏界面
 *
 * 功能说明：
 *   - 12×12 网格棋盘，蛇身绿色、食物红色（用 lv_obj 方块着色）。
 *   - 两种控制模式（游戏开始前二选一，过程中不可切换）：
 *       1. 体感模式：倾斜开发板改变蛇的移动方向（读 imu_tilt_x/y）。
 *       2. 按键模式：用屏幕右侧「上/下/左/右」方向键控制。
 *   - 蛇每 300ms 自动前进一步，吃到食物变长，撞墙或撞自己判负。
 *   - 游戏结束弹出提示 + 「重新开始」按钮，重新开始后回到模式选择。
 *
 * 核心数据结构：
 *   - board[GRID_H][GRID_W]：棋盘状态（0=空 1=蛇身 2=食物）。
 *   - snake_x[]/snake_y[]：蛇身各节坐标，[0] 为蛇头。
 *   - dir / next_dir：当前方向 / 下一步方向（0上 1下 2左 3右）。
 *   - game_mode：控制模式（0待选择 1体感 2按键）。
 *
 * 依赖模块：
 *   - imu.h：读取陀螺仪倾斜值（体感控制）。
 *   - game_center.h：返回游戏中心界面。
 */
#include "../lvgl/lvgl.h"
#include "game_center.h"
#include "snake.h"
#include "imu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

#define GRID_W 12              // 横向格数
#define GRID_H 12              // 纵向格数
#define CELL    40             // 每格像素
#define BOARD   (GRID_W * CELL)   // 480

#define TILT_THRESHOLD 6000    // 倾斜触发阈值

/* 控制模式枚举 */
#define MODE_NONE  0   // 待选择模式（游戏未开始）
#define MODE_TILT  1   // 体感模式
#define MODE_KEY   2   // 按键模式

lv_obj_t * snake_screen = NULL;

static int board[GRID_H][GRID_W];           // 0=空 1=蛇身 2=食物
static lv_obj_t *cells[GRID_H][GRID_W];     // 方块控件
static int snake_x[GRID_W * GRID_H];        // 蛇身 x（[0]=头）
static int snake_y[GRID_W * GRID_H];        // 蛇身 y
static int snake_len = 3;
static int dir = 3;                         // 0上 1下 2左 3右
static int next_dir = 3;
static int game_running = 1;

/* 控制模式相关 */
static int game_mode = MODE_NONE;           // 当前模式：0待选择 1体感 2按键
static lv_obj_t *mode_label = NULL;         // 模式选择提示文字
static lv_obj_t *mode_tilt_btn = NULL;      // 「体感模式」按钮
static lv_obj_t *mode_key_btn = NULL;       // 「按键模式」按钮
static lv_obj_t *dir_btns[4] = {NULL, NULL, NULL, NULL};  // 上/下/左/右方向键

static lv_obj_t *result_label = NULL;
static lv_obj_t *restart_btn = NULL;
static lv_timer_t *move_timer = NULL;
static lv_timer_t *tilt_timer = NULL;

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

/* 刷新整个棋盘方块颜色：蛇身绿、食物红、空格深色 */
static void draw_board(void)
{
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++) {
            lv_color_t c;
            if(board[i][j] == 1)      c = lv_color_hex(0x2FD3A0);
            else if(board[i][j] == 2) c = lv_color_hex(0xE74C3C);
            else                      c = lv_color_hex(0x1B1B26);
            lv_obj_set_style_bg_color(cells[i][j], c, 0);
        }
}

/* 在随机空格生成一个新食物 */
static void spawn_food(void)
{
    int empty = 0;
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++)
            if(board[i][j] == 0) empty++;
    if(empty == 0) return;
    int k = rand() % empty + 1, q = 0;
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++)
            if(board[i][j] == 0) {
                q++;
                if(q == k) { board[i][j] = 2; return; }
            }
}

/* 初始化蛇（3 节横向，头在 (5,5)） */
static void init_snake(void)
{
    memset(board, 0, sizeof(board));
    snake_len = 3;
    dir = 3; next_dir = 3;
    for(int i = 0; i < snake_len; i++) {
        snake_x[i] = 5 - i;
        snake_y[i] = 5;
        board[5][5 - i] = 1;
    }
    spawn_food();
    draw_board();
}

/* 移动一步：返回 1=继续，0=死亡 */
static int move_step(void)
{
    dir = next_dir;
    int nx = snake_x[0], ny = snake_y[0];
    if(dir == 0) ny--;
    else if(dir == 1) ny++;
    else if(dir == 2) nx--;
    else if(dir == 3) nx++;

    if(nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) return 0;  // 撞墙

    int eat = (board[ny][nx] == 2);           // 是否吃到食物
    if(!eat && board[ny][nx] == 1) return 0;  // 撞自己

    // 保存旧尾巴位置（吃食物时保留，不吃时清除）
    int tail_x = snake_x[snake_len - 1];
    int tail_y = snake_y[snake_len - 1];

    // 蛇身整体后移一位（从尾到头，给新头腾位置）
    for(int i = snake_len - 1; i > 0; i--) {
        snake_x[i] = snake_x[i - 1];
        snake_y[i] = snake_y[i - 1];
    }

    // 新蛇头
    snake_x[0] = nx;
    snake_y[0] = ny;
    board[ny][nx] = 1;

    if(eat) {
        // 吃食物：蛇变长，旧尾巴保留成为新的最后一节
        snake_len++;
        snake_x[snake_len - 1] = tail_x;
        snake_y[snake_len - 1] = tail_y;
        spawn_food();
    } else {
        // 不吃：清除旧尾巴
        board[tail_y][tail_x] = 0;
    }
    draw_board();
    return 1;
}

/* 游戏结束：停体感定时器、禁用方向键、弹提示 */
static void game_over(void)
{
    game_running = 0;
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }
    lv_label_set_text(result_label, "游戏结束");
    lv_obj_remove_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);   // 结束禁用方向键
}

/* 重新开始：重置数据、回到待选择模式、重新弹出模式按钮 */
static void restart_game(void)
{
    init_snake();
    game_mode = MODE_NONE;
    game_running = 1;
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }

    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);

    // 重新弹出模式选择
    lv_obj_remove_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_key_btn, LV_OBJ_FLAG_HIDDEN);

    // 待选择状态下禁用方向键
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);
}

static void restart_btn_cb(lv_event_t *e) { restart_game(); }

/* 移动定时器：每 300ms 移动一步（待选择/结束时不动） */
static void move_timer_cb(lv_timer_t *t)
{
    if(game_mode == MODE_NONE || !game_running) return;
    if(!move_step()) game_over();
}

/* 方向按钮回调：仅按键模式且游戏进行中响应，点击改变方向（不能直接反向） */
static void dir_btn_cb(lv_event_t *e)
{
    if(game_mode != MODE_KEY || !game_running) return;
    int d = (int)(long)lv_event_get_user_data(e);
    if((d == 0 && dir == 1) || (d == 1 && dir == 0) ||
       (d == 2 && dir == 3) || (d == 3 && dir == 2))
        return;
    next_dir = d;
}

/* 体感定时器：仅体感模式创建，每 150ms 读倾斜改变方向（不能直接反向） */
static void tilt_timer_cb(lv_timer_t *t)
{
    if(!game_running) return;
    int tx = imu_tilt_x();
    int ty = imu_tilt_y();
    if(abs(tx) > abs(ty)) {
        if(tx >  TILT_THRESHOLD && dir != 2) next_dir = 3;   // 右
        if(tx < -TILT_THRESHOLD && dir != 3) next_dir = 2;   // 左
    } else {
        if(ty >  TILT_THRESHOLD && dir != 0) next_dir = 1;   // 下
        if(ty < -TILT_THRESHOLD && dir != 1) next_dir = 0;   // 上
    }
}

/*
 * 开始游戏：选定模式后隐藏模式按钮，按需启用对应控制源。
 * @param mode MODE_TILT 或 MODE_KEY
 */
static void start_game(int mode)
{
    game_mode = mode;
    lv_obj_add_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_key_btn, LV_OBJ_FLAG_HIDDEN);

    if(mode == MODE_TILT) {
        // 体感模式：启动体感定时器，禁用方向键
        if(tilt_timer == NULL)
            tilt_timer = lv_timer_create(tilt_timer_cb, 150, NULL);
        for(int i = 0; i < 4; i++)
            lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);
    } else {
        // 按键模式：启用方向键（体感定时器不创建）
        for(int i = 0; i < 4; i++)
            lv_obj_remove_state(dir_btns[i], LV_STATE_DISABLED);
    }
}

/* 模式按钮回调 */
static void mode_tilt_cb(lv_event_t *e) { start_game(MODE_TILT); }
static void mode_key_cb(lv_event_t *e)  { start_game(MODE_KEY);  }

/* 返回游戏中心 */
static void to_game_center_cb(lv_event_t *e)
{
    if(move_timer) { lv_timer_delete(move_timer); move_timer = NULL; }
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }
    if(select_game_screen == NULL)
        select_game_screen = ui_select_game_screen();
    lv_screen_load(select_game_screen);
    if(snake_screen != NULL) { lv_obj_delete(snake_screen); snake_screen = NULL; }
}

/* 创建方向按钮：x/y 左上角，size 边长，text 文字，dir_val 方向值 */
static lv_obj_t *create_dir_btn(lv_obj_t *parent, int x, int y, int size,
                                const char *text, lv_font_t *font, int dir_val)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(lab);
    lv_obj_add_event_cb(btn, dir_btn_cb, LV_EVENT_CLICKED, (void *)(long)dir_val);
    return btn;
}

lv_obj_t * ui_snake_init(void)
{
    lv_font_t *font_title = cn_font(24);
    lv_font_t *font_norm  = cn_font(18);
    lv_font_t *font_big   = cn_font(36);
    lv_font_t *font_desc  = cn_font(14);

    srand(time(NULL));
    game_running = 1;
    game_mode = MODE_NONE;   // 初始为待选择模式

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题栏 */
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
    lv_obj_add_event_cb(back_btn, to_game_center_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "体感贪吃蛇");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 12x12 棋盘方块 */
    int bx = (1024 - BOARD) / 2;
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++) {
            cells[i][j] = lv_obj_create(win);
            lv_obj_set_size(cells[i][j], CELL - 2, CELL - 2);
            lv_obj_set_pos(cells[i][j], bx + j * CELL + 1, 70 + i * CELL + 1);
            lv_obj_set_style_radius(cells[i][j], 4, 0);
            lv_obj_remove_flag(cells[i][j], LV_OBJ_FLAG_CLICKABLE);
        }

    /* 右侧方向键（触摸控制上下左右，初始禁用，仅按键模式启用） */
    lv_obj_t *dir_label = lv_label_create(win);
    lv_label_set_text(dir_label, "方向键");
    lv_obj_set_style_text_font(dir_label, font_desc, 0);
    lv_obj_set_style_text_color(dir_label, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(dir_label, 862, 168);

    dir_btns[0] = create_dir_btn(win, 852, 202, 72, "上", font_norm, 0);   // 上
    dir_btns[1] = create_dir_btn(win, 852, 346, 72, "下", font_norm, 1);   // 下
    dir_btns[2] = create_dir_btn(win, 780, 274, 72, "左", font_norm, 2);   // 左
    dir_btns[3] = create_dir_btn(win, 924, 274, 72, "右", font_norm, 3);   // 右
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);   // 初始禁用

    /* ===== 模式选择（游戏开始前显示，选定后隐藏）===== */
    mode_label = lv_label_create(win);
    lv_label_set_text(mode_label, "请选择控制模式");
    lv_obj_set_style_text_font(mode_label, font_norm, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xECEAF2), 0);
    lv_obj_align(mode_label, LV_ALIGN_CENTER, 0, -140);

    // 体感模式（蓝色）
    mode_tilt_btn = lv_button_create(win);
    lv_obj_set_size(mode_tilt_btn, 170, 60);
    lv_obj_set_style_bg_color(mode_tilt_btn, lv_color_hex(0x4DA6FF), 0);
    lv_obj_set_style_radius(mode_tilt_btn, 12, 0);
    lv_obj_align(mode_tilt_btn, LV_ALIGN_CENTER, -95, -20);
    lv_obj_t *tilt_lab = lv_label_create(mode_tilt_btn);
    lv_label_set_text(tilt_lab, "体感模式");
    lv_obj_set_style_text_font(tilt_lab, font_norm, 0);
    lv_obj_set_style_text_color(tilt_lab, lv_color_hex(0x06131E), 0);
    lv_obj_center(tilt_lab);
    lv_obj_add_event_cb(mode_tilt_btn, mode_tilt_cb, LV_EVENT_CLICKED, NULL);

    // 按键模式（绿色）
    mode_key_btn = lv_button_create(win);
    lv_obj_set_size(mode_key_btn, 170, 60);
    lv_obj_set_style_bg_color(mode_key_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(mode_key_btn, 12, 0);
    lv_obj_align(mode_key_btn, LV_ALIGN_CENTER, 95, -20);
    lv_obj_t *key_lab = lv_label_create(mode_key_btn);
    lv_label_set_text(key_lab, "按键模式");
    lv_obj_set_style_text_font(key_lab, font_norm, 0);
    lv_obj_set_style_text_color(key_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(key_lab);
    lv_obj_add_event_cb(mode_key_btn, mode_key_cb, LV_EVENT_CLICKED, NULL);

    /* 结束提示 + 重新开始（默认隐藏） */
    result_label = lv_label_create(win);
    lv_label_set_text(result_label, "");
    lv_obj_set_style_text_font(result_label, font_big, 0);
    lv_obj_set_style_text_color(result_label, lv_color_hex(0xF5A623), 0);
    lv_obj_align(result_label, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);

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

    /* 底部提示 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "选择模式后开始游戏，吃到红色食物变长");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    init_snake();
    move_timer = lv_timer_create(move_timer_cb, 300, NULL);
    // 注意：体感定时器不在这里创建，等用户选定「体感模式」后才启动

    lv_screen_load(scr);
    snake_screen = scr;
    return scr;
}
