/**
 * @file    2048.c
 * @brief   2048 体感版游戏界面
 *
 * 功能说明：
 *   - 经典 2048 数字合并游戏，4x4 棋盘，瓷砖用 BMP 图片显示。
 *   - 两种控制模式（游戏开始前二选一，过程中不可切换）：
 *       1. 滑动模式：触摸按下拖动，松开时按滑动方向移动/合并方块。
 *       2. 体感模式：倾斜开发板，用 MPU6050 加速度计触发上下左右移动。
 *   - 合并方块时累加得分，棋盘满且无法合并判失败，出现 2048 判成功。
 *   - 游戏结束弹出提示 + 「重新开始」按钮，成功时多一个「继续」按钮。
 *
 * 依赖模块：
 *   - imu.h：读取陀螺仪倾斜值（体感控制）。
 *   - game_center.h：返回游戏中心界面。
 */
#include "../lvgl/lvgl.h"
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include "main_interface.h"
#include "game_center.h"
#include "imu.h"


/* 中文字体路径：微软雅黑（板子上实际存放位置） */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 棋盘布局参数（单位：像素） */
#define TILE_SIZE 100   // 单块瓷砖尺寸
#define TILE_GAP  16    // 瓷砖之间的间距
#define BOARD_PAD 16    // 棋盘内边距

/* 体感倾斜阈值：加速度计读数超过该值才触发一次移动（数值越小越灵敏） */
#define TILT_THRESHOLD 6000

/* 控制模式枚举 */
#define MODE_NONE   0   // 待选择模式（游戏未开始）
#define MODE_SLIDE  1   // 滑动模式
#define MODE_TILT   2   // 体感模式

/* 4x4 游戏棋盘数据：0 表示空格，非 0 表示对应数字的方块 */
int game_grid[4][4] = {
	0,0,2,4,
	0,0,0,2,
	2,2,0,0,
	0,0,0,0
};

/* 4x4 瓷砖图像控件，与 game_grid 一一对应，用于在界面上显示数字 */
lv_obj_t *tile_image[4][4];
static int score = 0;                    // 当前得分
static lv_obj_t *score_label = NULL;     // 得分显示标签
static lv_timer_t *tilt_timer = NULL;    // 体感检测定时器（仅体感模式创建）
static int tilt_armed = 1;               // 体感触发开关：1=可触发，0=等待回中
static lv_obj_t *result_label = NULL;    // 结束提示字
static lv_obj_t *restart_btn = NULL;     // 重新开始按钮
static lv_obj_t *continue_btn = NULL;    // 继续按钮（仅成功时显示）
static int game_over_flag = 0;           // 游戏状态：0=游戏中 1=失败 2=成功
static int won_before = 0;               // 是否已经弹过成功提示（避免反复弹）

/* 控制模式相关 */
static int game_mode = MODE_NONE;        // 当前控制模式：0待选择 1滑动 2体感
static lv_obj_t *mode_label = NULL;      // 模式选择提示文字
static lv_obj_t *mode_slide_btn = NULL;  // 「滑动模式」按钮
static lv_obj_t *mode_tilt_btn = NULL;   // 「体感模式」按钮

/**
 * 创建指定字号的中文字体（带缓存，避免重复创建造成内存泄漏）。
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

/* 刷新得分标签显示 */
static void update_score(void)
{
    lv_label_set_text_fmt(score_label, "得分 %d", score);
}

/* 统计棋盘中空格（0）的个数，供随机生成新方块时使用 */
static int get_arr_zero_count(void)
{
    int count = 0;
    for(int i = 0;i < 4;i++)
        for(int j = 0;j < 4;j++)
            if(game_grid[i][j] == 0) count++;
    return count;
}

/*
 * 在随机一个空格上生成新数字（90% 概率是 2，10% 概率是 4）。
 * 先统计空格个数，再随机选中第 k 个空格填入。
 */
static void rand_num(void)
{
    int count = get_arr_zero_count();
    int k = rand()%count + 1;
    int q = 0;
    for(int i = 0;i < 4;i++)
    {
        for(int j = 0;j < 4;j++)
        {
            if(game_grid[i][j] == 0)
            {
                q++;
                if(q == k)
                {
                    game_grid[i][j] = rand()%10 > 2 ? 2:4;
                    break;
                }
            }
        }
    }
}

/* 去掉一行中的 0（把非零数字挤到左侧），若发生移动则置 flag=1 */
static void rm_zero(int temp_arr[],int *flag)
{
    int k = 0;
    for(int i = 0 ; i < 4;i++)
    {
        if(temp_arr[i] != 0)
        {
            temp_arr[k] = temp_arr[i];
            if(k != i) { temp_arr[i] = 0; *flag = 1; }
            k++;
        }
    }
}

/* 合并一行中相邻的相同数字（翻倍并累加得分），若发生合并则置 flag=1 */
static void hebing(int temp_arr[],int *flag)
{
    for(int i = 0; i < 3;i++)
    {
        if(temp_arr[i] == temp_arr[i+1] && temp_arr[i] != 0)
        {
            temp_arr[i] *= 2;
            score += temp_arr[i];   // 累计得分
            temp_arr[i+1] = 0;
            *flag = 1;
        }
    }
}

/* 向左移动：逐行处理，先去掉 0、再合并、再去掉 0 */
static void slide_left(void)
{
    int temp_arr[4]; int flag = 0;
    for(int i = 0;i < 4;i++)
    {
        for(int j = 0;j < 4;j++) temp_arr[j] = game_grid[i][j];
        rm_zero(temp_arr,&flag); hebing(temp_arr,&flag); rm_zero(temp_arr,&flag);
        for(int j = 0;j < 4;j++) game_grid[i][j] = temp_arr[j];
    }
    if(flag == 1) rand_num();
}

/* 向右移动：与左移同理，只是先把每行倒序，处理完再倒序写回 */
static void slide_right(void)
{
    int temp_arr[4]; int flag = 0;
    for(int i = 0;i < 4;i++)
    {
        for(int j = 0;j < 4;j++) temp_arr[3-j] = game_grid[i][j];
        rm_zero(temp_arr,&flag); hebing(temp_arr,&flag); rm_zero(temp_arr,&flag);
        for(int j = 0;j < 4;j++) game_grid[i][3-j] = temp_arr[j];
    }
    if(flag == 1) rand_num();
}

/* 向上移动：逐列处理（把列当行看待） */
static void slide_up(void)
{
    int temp_arr[4]; int flag = 0;
    for(int i = 0;i < 4;i++)
    {
        for(int j = 0;j < 4;j++) temp_arr[j] = game_grid[j][i];
        rm_zero(temp_arr,&flag); hebing(temp_arr,&flag); rm_zero(temp_arr,&flag);
        for(int j = 0;j < 4;j++) game_grid[j][i] = temp_arr[j];
    }
    if(flag == 1) rand_num();
}

/* 向下移动：逐列倒序处理 */
static void slide_down(void)
{
    int temp_arr[4]; int flag = 0;
    for(int i = 0;i < 4;i++)
    {
        for(int j = 0;j < 4;j++) temp_arr[3-j] = game_grid[j][i];
        rm_zero(temp_arr,&flag); hebing(temp_arr,&flag); rm_zero(temp_arr,&flag);
        for(int j = 0;j < 4;j++) game_grid[3-j][i] = temp_arr[j];
    }
    if(flag == 1) rand_num();
}

/* 判断是否失败：棋盘满了且没有任何相邻相同数字可合并 */
static int game_failed(void)
{
    for(int i = 0;i < 4;i++)
        for(int j = 0;j < 4;j++)
        {
            if(game_grid[i][j]==0) return 0;
            if(j+1<4 && game_grid[i][j]==game_grid[i][j+1]) return 0;
            if(i+1<4 && game_grid[i][j]==game_grid[i+1][j]) return 0;
        }
    return 1;
}

/* 判断是否成功：出现 2048 方块即算达成目标 */
static int game_success(void)
{
    for(int i = 0;i < 4;i++)
        for(int j = 0;j < 4;j++)
            if(game_grid[i][j]==2048) return 1;
    return 0;
}

/*
 * 游戏结束处理：弹出提示和按钮。
 * @param type 1=失败（只显示重新开始），2=成功（显示继续+重新开始）
 */
static void game_over(int type)
{
    game_over_flag = type;
    if(type == 2) {
        lv_label_set_text(result_label, "游戏成功！");
        lv_obj_set_style_text_color(result_label, lv_color_hex(0x2FD3A0), 0);
        won_before = 1;
        lv_obj_align(restart_btn, LV_ALIGN_CENTER, 90, 50);
        lv_obj_remove_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(result_label, "游戏失败！");
        lv_obj_set_style_text_color(result_label, lv_color_hex(0xE74C3C), 0);
        lv_obj_align(restart_btn, LV_ALIGN_CENTER, 0, 50);
        lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
}

/* 根据 game_grid 刷新所有瓷砖图片（图片名就是数字，如 2.bmp、2048.bmp） */
static void update_game_2048(void)
{
    for(int i = 0;i<4;i++)
        for(int j = 0;j<4;j++)
        {
            char bmppathname[1024] = {0};
            sprintf(bmppathname,"A:/work_space/2048pic/%d.bmp",game_grid[i][j]);
            lv_image_set_src(tile_image[i][j],bmppathname);
        }
}

/*
 * 根据起点/终点坐标判断滑动方向。
 * @return 0=点击未滑动，1=上，2=下，3=左，4=右
 */
static int get_slide(lv_point_t start_point,lv_point_t end_point)
{
    int dx = end_point.x - start_point.x;
    int dy = end_point.y - start_point.y;
    if(dx == 0 && dy == 0) return 0;
    if(abs(dx) >= abs(dy)) return dx > 0 ? 4 : 3;
    else                   return dy > 0 ? 2 : 1;
}

/* 触摸滑动事件回调：仅滑动模式且游戏未结束时响应 */
static void test12_cb(lv_event_t *e)
{
    static lv_point_t start_point;
    static lv_point_t end_point;
    lv_event_code_t code = lv_event_get_code(e);
    if(game_over_flag || game_mode != MODE_SLIDE) return;   // 非滑动模式不响应

    if(code == LV_EVENT_PRESSED)
    {
        lv_indev_get_point(lv_indev_active(), &start_point);
    }
    else if(code == LV_EVENT_RELEASED)
    {
        lv_indev_get_point(lv_indev_active(), &end_point);
        int r = get_slide(start_point,end_point);
        if(r == 1) slide_up();
        else if(r == 2) slide_down();
        else if(r == 3) slide_left();
        else if(r == 4) slide_right();
        update_game_2048();
        update_score();
        if(game_failed()) game_over(1);
        else if(game_success() && !won_before) game_over(2);
    }
}

/* 初始化游戏数据：置空棋盘、得分清零、随机生成两个初始方块 */
static void init_2048_game(void)
{
    srand(time(NULL));
    memset(game_grid,0,sizeof(game_grid));
    score = 0;
    rand_num();
    rand_num();
}

/* 重新开始：重置数据、回到待选择模式、隐藏结束提示、重新弹出模式按钮 */
static void restart_game(void)
{
    init_2048_game();
    update_game_2048();
    update_score();
    game_over_flag = 0;
    won_before = 0;
    tilt_armed = 1;
    game_mode = MODE_NONE;                       // 回到待选择模式
    if(tilt_timer != NULL) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }

    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);

    // 重新弹出模式选择
    lv_obj_remove_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_slide_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);
}

/* 重新开始按钮回调 */
static void restart_btn_cb(lv_event_t *e)
{
    restart_game();
}

/* 继续按钮回调：成功后选择继续冲更高分，关闭弹窗，保持当前模式不变 */
static void continue_btn_cb(lv_event_t *e)
{
    game_over_flag = 0;
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    // game_mode 保持不变，继续用当前模式玩
}

/*
 * 体感检测定时器回调（每 100ms 一次，仅体感模式创建）：
 * 读陀螺仪倾斜值，超过阈值触发一次移动；回中后才允许下一次触发。
 */
static void tilt_loop_cb(lv_timer_t *t)
{
    if(game_over_flag) return;   // 游戏结束不再响应

    int tx = imu_tilt_x();
    int ty = imu_tilt_y();

    // 倾斜很小时视为回中，重新允许下一次触发
    if(abs(tx) < TILT_THRESHOLD/2 && abs(ty) < TILT_THRESHOLD/2) {
        tilt_armed = 1;
        return;
    }
    if(!tilt_armed) return;

    if(abs(tx) > abs(ty)) {
        if(tx >  TILT_THRESHOLD)      { slide_right(); tilt_armed = 0; }
        else if(tx < -TILT_THRESHOLD) { slide_left();  tilt_armed = 0; }
    } else {
        if(ty >  TILT_THRESHOLD)      { slide_down(); tilt_armed = 0; }
        else if(ty < -TILT_THRESHOLD) { slide_up();   tilt_armed = 0; }
    }
    if(!tilt_armed) {
        update_game_2048();
        update_score();
        if(game_failed()) game_over(1);
        else if(game_success() && !won_before) game_over(2);
    }
}

/*
 * 开始游戏：选定模式后隐藏模式按钮，按需启动对应控制源。
 * @param mode MODE_SLIDE 或 MODE_TILT
 */
static void start_game(int mode)
{
    game_mode = mode;
    lv_obj_add_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_slide_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);

    if(mode == MODE_TILT) {
        // 体感模式：启动体感定时器
        if(tilt_timer == NULL)
            tilt_timer = lv_timer_create(tilt_loop_cb, 100, NULL);
    }
    // 滑动模式：不启动体感定时器（滑动事件已在 win 上注册，靠 game_mode 过滤）
}

/* 模式按钮回调 */
static void mode_slide_cb(lv_event_t *e) { start_game(MODE_SLIDE); }
static void mode_tilt_cb(lv_event_t *e)  { start_game(MODE_TILT);  }

/* 返回按钮回调：先删体感定时器（避免悬空指针），再切回游戏中心 */
static void to_game_center_cb(lv_event_t *e)
{
    if(tilt_timer != NULL) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }
    if(select_game_screen == NULL)
        select_game_screen = ui_select_game_screen();
    lv_screen_load(select_game_screen);
    if(game_2048_screen != NULL)
    {
        lv_obj_delete(game_2048_screen);
        game_2048_screen = NULL;
    }
}

/*
 * 2048 界面初始化入口。
 * @return 创建的屏幕对象
 */
lv_obj_t* ui_2048_init(void)
{
    lv_font_t *font_title = cn_font(28);   // 标题字号
    lv_font_t *font_norm  = cn_font(18);   // 正文字号
    lv_font_t *font_big   = cn_font(36);   // 结束提示大字

    init_2048_game();
    game_over_flag = 0;
    won_before = 0;
    game_mode = MODE_NONE;   // 初始为待选择模式

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
    lv_obj_add_event_cb(back_btn, to_game_center_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "2048 体感版");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 得分胶囊标签（右上角） */
    score_label = lv_label_create(win);
    lv_label_set_text_fmt(score_label, "得分 %d", score);
    lv_obj_set_style_text_font(score_label, font_norm, 0);
    lv_obj_set_style_text_color(score_label, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_bg_color(score_label, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(score_label, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(score_label, 1, 0);
    lv_obj_set_style_border_color(score_label, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(score_label, 20, 0);
    lv_obj_set_style_pad_all(score_label, 12, 0);
    lv_obj_set_pos(score_label, 840, 12);

    /* 棋盘底板（圆角深色背景，居中） */
    int board_size = 4*TILE_SIZE + 3*TILE_GAP + 2*BOARD_PAD;   // 480
    lv_obj_t *board = lv_obj_create(win);
    lv_obj_set_size(board, board_size, board_size);
    lv_obj_set_pos(board, (1024-board_size)/2, 100);
    lv_obj_set_style_bg_color(board, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(board, 0, 0);
    lv_obj_set_style_radius(board, 16, 0);
    lv_obj_set_style_pad_all(board, 0, 0);

    // 底板设为不可点击，让触摸事件穿透到全屏窗口，滑动回调才能收到事件
    lv_obj_remove_flag(board, LV_OBJ_FLAG_CLICKABLE);

    /* 4x4 瓷砖：创建 16 个图像控件，按棋盘坐标摆放 */
    int bx = (1024-board_size)/2;
    for(int i = 0;i<4;i++)
    {
        for(int j = 0;j<4;j++)
        {
            tile_image[i][j] = lv_image_create(win);
            lv_obj_set_pos(tile_image[i][j],
                bx + BOARD_PAD + j*(TILE_SIZE+TILE_GAP),
                100 + BOARD_PAD + i*(TILE_SIZE+TILE_GAP));
            char bmppathname[1024] = {0};
            sprintf(bmppathname,"A:/work_space/2048pic/%d.bmp",game_grid[i][j]);
            lv_image_set_src(tile_image[i][j],bmppathname);
        }
    }

    /* ===== 模式选择（游戏开始前显示，选定后隐藏）===== */
    mode_label = lv_label_create(win);
    lv_label_set_text(mode_label, "请选择控制模式");
    lv_obj_set_style_text_font(mode_label, font_norm, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xECEAF2), 0);
    lv_obj_align(mode_label, LV_ALIGN_CENTER, 0, -140);

    // 滑动模式（绿色）
    mode_slide_btn = lv_button_create(win);
    lv_obj_set_size(mode_slide_btn, 170, 60);
    lv_obj_set_style_bg_color(mode_slide_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(mode_slide_btn, 12, 0);
    lv_obj_align(mode_slide_btn, LV_ALIGN_CENTER, -95, -20);
    lv_obj_t *slide_lab = lv_label_create(mode_slide_btn);
    lv_label_set_text(slide_lab, "滑动模式");
    lv_obj_set_style_text_font(slide_lab, font_norm, 0);
    lv_obj_set_style_text_color(slide_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(slide_lab);
    lv_obj_add_event_cb(mode_slide_btn, mode_slide_cb, LV_EVENT_CLICKED, NULL);

    // 体感模式（蓝色）
    mode_tilt_btn = lv_button_create(win);
    lv_obj_set_size(mode_tilt_btn, 170, 60);
    lv_obj_set_style_bg_color(mode_tilt_btn, lv_color_hex(0x4DA6FF), 0);
    lv_obj_set_style_radius(mode_tilt_btn, 12, 0);
    lv_obj_align(mode_tilt_btn, LV_ALIGN_CENTER, 95, -20);
    lv_obj_t *tilt_lab = lv_label_create(mode_tilt_btn);
    lv_label_set_text(tilt_lab, "体感模式");
    lv_obj_set_style_text_font(tilt_lab, font_norm, 0);
    lv_obj_set_style_text_color(tilt_lab, lv_color_hex(0x06131E), 0);
    lv_obj_center(tilt_lab);
    lv_obj_add_event_cb(mode_tilt_btn, mode_tilt_cb, LV_EVENT_CLICKED, NULL);

    /* 结束提示字（默认隐藏，游戏结束时显示） */
    result_label = lv_label_create(win);
    lv_label_set_text(result_label, "");
    lv_obj_set_style_text_font(result_label, font_big, 0);
    lv_obj_set_style_text_color(result_label, lv_color_hex(0xF5A623), 0);
    lv_obj_align(result_label, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);

    /* 重新开始按钮（默认隐藏，绿色） */
    restart_btn = lv_button_create(win);
    lv_obj_set_size(restart_btn, 150, 50);
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

    /* 继续按钮（默认隐藏，仅成功时显示，蓝色） */
    continue_btn = lv_button_create(win);
    lv_obj_set_size(continue_btn, 150, 50);
    lv_obj_set_style_bg_color(continue_btn, lv_color_hex(0x4DA6FF), 0);
    lv_obj_set_style_radius(continue_btn, 12, 0);
    lv_obj_align(continue_btn, LV_ALIGN_CENTER, -90, 50);
    lv_obj_t *cnt_lab = lv_label_create(continue_btn);
    lv_label_set_text(cnt_lab, "继续");
    lv_obj_set_style_text_font(cnt_lab, font_norm, 0);
    lv_obj_set_style_text_color(cnt_lab, lv_color_hex(0x06131E), 0);
    lv_obj_center(cnt_lab);
    lv_obj_add_event_cb(continue_btn, continue_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);

    /* 注册滑动事件（滑动模式靠 game_mode 过滤；体感模式不响应） */
    lv_obj_add_event_cb(win, test12_cb, LV_EVENT_ALL, NULL);
    // 注意：体感定时器不在这里创建，等用户选定「体感模式」后才启动

    lv_screen_load(scr);
    return scr;
}
