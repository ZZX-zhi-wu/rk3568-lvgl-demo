/**
 * @file    ball.c
 * @brief   重力滚球体感游戏界面
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 倾斜开发板，用 MPU6050 加速度计控制小球在屏幕上滚动。
 *   - 屏幕上有 1 个绿色终点圆环和若干红色陷阱圆环，位置每次随机生成。
 *   - 小球滚进绿色终点圆环 = 游戏成功；滚进红色陷阱 = 游戏失败。
 *   - 游戏结束弹出提示 + 「重新开始」按钮，点击后重新随机生成圆环并复位小球。
 *
 * =====================================================================
 * 二、物理模型（每帧做四件事）
 * =====================================================================
 *   ① 速度累加：v += tilt × ACCEL_SCALE     倾斜越大，加速越猛
 *   ② 摩擦衰减：v ×= FRICTION (0.90)        松手后慢慢停
 *   ③ 限速    ：|v| ≤ MAX_SPEED (30)        防止倾斜过猛把球甩飞
 *   ④ 位置更新：pos += v
 *
 *   撞到屏幕边界时：位置夹回边界，速度反向并乘 0.5（模拟能量损失）。
 *
 * 【推导一下"最快能多快"（答辩可以用）】
 *   每帧先加速再衰减，稳定速度满足 v = (v + a) × 0.9，解得
 *       终端速度 = 9 × a = 9 × tilt × ACCEL_SCALE
 *   倾斜读数 5000 时 a = 5000 × 0.0002 = 1.0 像素/帧²，
 *   终端速度 = 9 像素/帧。定时器 10ms/帧 → 约 900 像素/秒，
 *   横穿 1024 宽的屏幕约 1.1 秒，手感适中。
 *   MAX_SPEED = 30 像素/帧（3000 像素/秒）是"上限保护"，
 *   正常倾斜远到不了，只有猛甩板子才会触发。
 *
 * =====================================================================
 * 三、依赖模块
 * =====================================================================
 *   - imu.h：读取**加速度计**倾斜值。
 *     ★ 注意：不是陀螺仪！imu.c 读的是 in_accel_x_raw / in_accel_y_raw。
 *       游戏中心那张卡片的文案写的是"用陀螺仪控制小球"，
 *       那是宣传口径不严谨 —— 体感倾斜判断靠的是加速度计测量重力方向，
 *       陀螺仪测的是角速度（转得多快），两个东西不一样。
 *       答辩如果被问"你用的陀螺仪还是加速度计"，答案是加速度计。
 *   - game_center.h：返回游戏中心界面。
 *
 * =====================================================================
 * 四、必须校准才能玩
 * =====================================================================
 *   小球的速度完全由倾斜值驱动，而倾斜值 = 原始读数 - 零点偏移。
 *   如果没校准过（或校准时不水平），零点就是错的，
 *   小球会一直往一个方向漂。
 *   校准途径：设置页的「校准」按钮，或进一次"传感器实验室"（会自动校准）。
 */
#include "../lvgl/lvgl.h"
#include "game_center.h"
#include "ball.h"
#include "imu.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 物理参数（越小越慢/越灵敏可自行改）
 *
 * 调参提示：
 *   觉得球太慢/太迟钝 → 把 ACCEL_SCALE 调大（如 0.0003）
 *   觉得球滑得停不下来 → 把 FRICTION 调小（如 0.85，衰减更快）
 *   觉得球会突然飞出去 → 把 MAX_SPEED 调小
 *   注意这三个参数是配合定时器周期（10ms）调出来的，
 *   如果改了 game_loop_cb 的定时周期，手感会整体变化，需要重新调。 */
#define ACCEL_SCALE 0.0002f   // 倾斜读数 -> 加速度 的换算系数
#define FRICTION    0.90f     // 摩擦系数（每帧速度乘这个，越小越快停）
#define MAX_SPEED   30.0f     // 最大速度（像素/帧）
#define BALL_R      15        // 小球半径（像素）

/* 起点（小球初始位置）—— 屏幕正中央 */
#define START_X 512
#define START_Y 300

/* 圆环的数量与半径 */
#define TRAP_COUNT 4          // 陷阱圈数量（可改成别的数，数组会自动跟着变）
#define TARGET_R   40         // 终点圆环半径
#define TRAP_R     35         // 陷阱圆环半径

/* 离起点的最小距离（避免开局就撞上）
 * 用"距离的平方"比较，所以下面写 START_CLEAR * START_CLEAR。 */
#define START_CLEAR 140

/* 屏幕全局指针：本模块创建/删除，game_center.c 读取 */
lv_obj_t * ball_screen = NULL;

/* ---------------------------------------------------------------------
 * 游戏状态（模块级 static，供定时器回调访问）
 * --------------------------------------------------------------------- */
static lv_obj_t *ball_obj = NULL;            // 小球控件
static lv_obj_t *target_obj = NULL;          // 终点圆环控件
static lv_obj_t *trap_objs[TRAP_COUNT];      // 陷阱圆环控件数组
static lv_obj_t *result_label = NULL;        // 结果提示标签
static lv_obj_t *restart_btn = NULL;         // 重新开始按钮

static float ball_x = START_X, ball_y = START_Y;  // 小球当前位置（球心坐标，浮点）
static float ball_vx = 0, ball_vy = 0;            // 小球当前速度（像素/帧）
static lv_timer_t *game_timer = NULL;             // 游戏循环定时器

/* 游戏状态枚举：PLAYING 时物理才推进，WIN/LOSE 时冻结画面等用户点重开 */
typedef enum { GS_PLAYING, GS_WIN, GS_LOSE } game_state_t;
static game_state_t state = GS_PLAYING;

/* 圆心坐标结构
 * 注意存的是**圆心**坐标，而 LVGL 控件的 set_pos 用的是**左上角**坐标，
 * 所以摆放时要减去半径（见 apply_circle_pos）。 */
typedef struct { int x, y; } pt_t;
static pt_t target_pos;              // 终点圆心坐标
static pt_t trap_pos[TRAP_COUNT];    // 陷阱圆心坐标数组

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * 缓存 4 槽，本文件用 4 档（24 标题、36 结束大字、20 按钮、14 小字），
 * **正好占满** —— 再加第 5 档字号就会触发下面说的越界问题。
 *
 * 【已知隐患，加字号前必看】
 *   少了 if(empty < 0) return NULL; 这行保护。
 *   凑满 4 档后再来第 5 档时，循环结束 empty 仍为 -1，
 *   于是 `cache[-1] = f;` 写到数组前一个 int 的位置 → 数组越界写（未定义行为）。
 *   本文件是最接近触发的（4 槽用满 4 档），所以尤其要注意：
 *   **如果要加第 5 种字号，先把保护行补上，或把 cache[4] 改成 cache[8]。**
 *
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

/**
 * 两点距离的平方（用于碰撞判断，避免开平方运算）。
 *
 * 【为什么要"平方"】
 *   判断"距离 < R" 等价于 "距离² < R²"，而距离²只需要两次乘法和一次加法，
 *   完全避开 sqrtf。在 100fps 的循环里，乘以每秒几百次调用的次数，
 *   这点优化是值得的，也是碰撞检测的通用写法。
 *
 * 精度提示：dx、dy 最大约 1024，平方后约 10⁶，int（21 亿）完全装得下，不会溢出。
 */
static int dist2(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2, dy = y1 - y2;
    return dx * dx + dy * dy;
}

/*
 * 随机生成终点和陷阱圈的位置。
 * 约束：不贴边、彼此不重叠、离起点有足够距离（START_CLEAR），
 * 保证有通路让小球滚过去，不会开局必死。
 *
 * 【为什么留 50 / 40 像素边距】
 *   圆环控件本身有半径（40/35），再加 50/40 的额外边距，
 *   保证圆环完整地落在屏幕内，并且和标题栏、底部提示文字不重叠。
 *   rand() % (1024 - 2*(TARGET_R+50)) 再整体 + (TARGET_R+50)，
 *   就是把随机范围压缩到"左右各留出 TARGET_R+50"的区间内。
 *
 * 【终点为什么用 do-while 而不是加重试上限】
 *   只要求"离起点够远"这一个条件，在 1024×600 的屏幕里，
 *   起点在正中、要求距离 >140px，随机点满足概率极高（约 90%），
 *   do-while 平均 1.1 次就退出。
 *   理论上存在"一直不满足 → 死循环"的极端情况，但概率低到可以忽略。
 *   （相比之下，陷阱圈有 4 个且约束互相牵制，就必须设重试上限，见下。）
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
        int ok = 0;   /* 用"标志位 + 内层循环"表达多重约束，比 goto 清晰 */
        for(int tries = 0; tries < 300 && !ok; tries++) {
            trap_pos[i].x = TRAP_R + 40 + rand() % (1024 - 2 * (TRAP_R + 40));
            trap_pos[i].y = TRAP_R + 40 + rand() % (600  - 2 * (TRAP_R + 40));
            ok = 1;                              /* 先假设合格，下面逐条否决 */
            /* 约束一：离起点 > START_CLEAR(140) */
            if(dist2(trap_pos[i].x, trap_pos[i].y, START_X, START_Y) < START_CLEAR * START_CLEAR) ok = 0;
            /* 约束二：离终点 > 150，避免"陷阱贴着终点"，玩家分辨不出 */
            else if(dist2(trap_pos[i].x, trap_pos[i].y, target_pos.x, target_pos.y) < 150 * 150) ok = 0;
            /* 约束三：和之前已放置的陷阱互不重叠（间距 > 130） */
            else {
                for(int j = 0; j < i; j++)
                    if(dist2(trap_pos[i].x, trap_pos[i].y, trap_pos[j].x, trap_pos[j].y) < 130 * 130) { ok = 0; break; }
            }
        }
        /* ★ 注意这里的边界行为：
         *   如果 300 次都没找到合格位置（屏幕很挤时可能发生），
         *   循环退出时 ok 仍是 0，但**代码不会报错也不会重试**，
         *   而是直接采用最后一次尝试的位置（可能离起点太近或和别的圈重叠）。
         *   后果：偶尔出现"开局就有陷阱贴脸"或"两个圈叠在一起"。
         *   概率很低，所以就这样了；要更严谨可以：失败时把条件放松（比如
         *   把 130 逐步降到 100）再试，或者干脆减少 TRAP_COUNT。 */
    }
}

/* 把 UI 圆环控件摆到新生成的坐标位置（圆心坐标转左上角坐标）
 *
 * 【为什么要 -R】
 *   target_pos 存的是圆心坐标，而 lv_obj_set_pos 要的是控件左上角坐标。
 *   控件是 (2R)×(2R) 的正方形（靠圆角变圆），
 *   所以左上角 = 圆心 - R。这一步搞错的表现是"整个圆环偏了半个直径"。 */
static void apply_circle_pos(void)
{
    lv_obj_set_pos(target_obj, target_pos.x - TARGET_R, target_pos.y - TARGET_R);
    for(int i = 0; i < TRAP_COUNT; i++)
        lv_obj_set_pos(trap_objs[i], trap_pos[i].x - TRAP_R, trap_pos[i].y - TRAP_R);
}

/* 游戏结束处理：设置状态、显示提示和重新开始按钮
 *
 * 两个控件默认是 HIDDEN 的，结束时去掉隐藏标志让它们出现。
 * 同时把 state 设成 WIN/LOSE —— 关键作用：game_loop_cb 开头会
 * `if(state != GS_PLAYING) return;`，于是物理立刻冻结，
 * 玩家就不会看到小球穿过终点继续滚的怪现象。 */
static void game_over(game_state_t s)
{
    state = s;
    lv_label_set_text(result_label, s == GS_WIN ? "游戏成功！" : "游戏失败！");
    lv_obj_remove_flag(result_label, LV_OBJ_FLAG_HIDDEN);   /* 显示提示 */
    lv_obj_remove_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);    /* 显示重开按钮 */
    printf("%s\n", s == GS_WIN ? "游戏成功！" : "游戏失败！");   /* 串口日志 */
}

/* 重新开始：重新随机生成圆环、复位小球、隐藏结束提示
 *
 * 注意这里**不重新 srand()** —— srand 只能在整个程序里调用一次，
 * 放在 ui_ball_init 里就够了（每次进入游戏换一批随机位置）。
 * 如果在 restart_game 里也 srand(time(NULL))，而重开间隔不足 1 秒，
 * time(NULL) 返回值相同 → 随机序列被重置 → 圆环位置和上一局一模一样。 */
static void restart_game(void)
{
    randomize_circles();        /* 重新随机位置 */
    apply_circle_pos();         /* 摆到界面上 */
    ball_x = START_X; ball_y = START_Y; ball_vx = 0; ball_vy = 0;   /* 小球回起点、速度归零 */
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);   /* 藏起提示 */
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);    /* 藏起重开按钮 */
    state = GS_PLAYING;         /* 恢复物理 */
}

/* 重新开始按钮回调：只是一层包装（按钮事件带参数，restart_game 不带） */
static void restart_btn_cb(lv_event_t *e)
{
    restart_game();
}

/*
 * 游戏循环（每 10ms 一次 ≈ 100fps）：
 * 读加速度计 -> 更新速度（累加加速度、摩擦衰减、限速）-> 更新位置
 * -> 边界反弹 -> 更新小球控件位置 -> 判断是否滚进终点或陷阱。
 *
 * 【关于帧率】
 *   定时器周期在 ui_ball_init 里是 **10ms**：
 *       lv_timer_create(game_loop_cb, 10, NULL);
 *   也就是约 100fps。（早先版本的注释写成 20ms/50fps，与实际不符，已修正。）
 *   帧率直接决定手感：物理参数 ACCEL_SCALE / FRICTION / MAX_SPEED
 *   都是按 10ms 调出来的，改周期会让游戏整体变快或变慢。
 *
 * 【为什么每帧都读传感器而不是"变化时读"】
 *   倾斜值是连续变化的，必须每帧采样才能得到平滑的加速效果；
 *   一次 imu_tilt_x() 就是一次 fopen/fscanf/fclose，约几十微秒，
 *   100fps 下每秒 200 次读文件，对 RK3568 完全无压力。
 */
static void game_loop_cb(lv_timer_t *t)
{
    if(state != GS_PLAYING) return;   // 结束状态暂停物理

    /* 读当前倾斜值（原始整数，已减零点、已做方向修正） */
    int tx = imu_tilt_x();
    int ty = imu_tilt_y();

    // 加速度计读数累加到速度（倾斜越大加速越快）
    ball_vx += tx * ACCEL_SCALE;
    ball_vy += ty * ACCEL_SCALE;
    // 摩擦衰减
    ball_vx *= FRICTION;
    ball_vy *= FRICTION;
    // 限制最大速度
    /* 四个方向都要夹，不能用 fabsf—— 因为要保留符号。
     * 这里写成"上界夹一次、下界夹一次"，是通用的限幅写法。 */
    if(ball_vx >  MAX_SPEED) ball_vx =  MAX_SPEED;
    if(ball_vx < -MAX_SPEED) ball_vx = -MAX_SPEED;
    if(ball_vy >  MAX_SPEED) ball_vy =  MAX_SPEED;
    if(ball_vy < -MAX_SPEED) ball_vy = -MAX_SPEED;

    // 更新位置
    ball_x += ball_vx;
    ball_y += ball_vy;

    // 边界反弹（速度反向并减半，模拟能量损失）
    /* 先把位置夹回边界内，再让速度反向 —— 顺序很重要：
     * 如果只反速度不夹位置，球可能已经出界很多，反转后还要好几帧才回来，
     * 视觉上会看到球"黏"在边缘抖动。 */
    if(ball_x < BALL_R)         { ball_x = BALL_R;          ball_vx = -ball_vx * 0.5f; }
    if(ball_x > 1024 - BALL_R)  { ball_x = 1024 - BALL_R;   ball_vx = -ball_vx * 0.5f; }
    if(ball_y < BALL_R)         { ball_y = BALL_R;          ball_vy = -ball_vy * 0.5f; }
    if(ball_y > 600 - BALL_R)   { ball_y = 600 - BALL_R;    ball_vy = -ball_vy * 0.5f; }

    // 更新小球控件位置（球心转左上角）
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);

    // 滚进终点圆环 -> 成功
    /* 【碰撞判定规则说明】
     *   用球心到圆心的距离和 TARGET_R 比较，**没有加 BALL_R**。
     *   意思是"球心进入环内即算成功"，视觉上只要球中心压到环线上就算过。
     *   如果按严格几何应该比 (TARGET_R + BALL_R)（球边缘碰到环）——
     *   那样会更难。现在的规则偏宽松，对演示友好。
     *   想让游戏更难：改成 TARGET_R*TARGET_R 之外再减/加 BALL_R 相关项。 */
    if(dist2((int)ball_x, (int)ball_y, target_pos.x, target_pos.y) < TARGET_R * TARGET_R) {
        game_over(GS_WIN);
        return;                 /* 提前返回，避免同一帧又判陷阱 */
    }
    // 滚进任意陷阱圆环 -> 失败
    for(int i = 0; i < TRAP_COUNT; i++) {
        if(dist2((int)ball_x, (int)ball_y, trap_pos[i].x, trap_pos[i].y) < TRAP_R * TRAP_R) {
            game_over(GS_LOSE);
            return;
        }
    }
}

/* 返回按钮回调：先删游戏循环定时器，再切回游戏中心
 *
 * 顺序：① 删定时器 → ② 切屏 → ③ 删屏。
 * 定时器 10ms 就触发一次，如果先删屏，几乎必然会撞上
 * "定时器访问已释放控件" → 段错误。所以删定时器必须排第一。
 *
 * 最后把 ball_screen 置 NULL —— 这一点很关键：
 * game_center.c 的 to_ball_screen_cb 是 `if(ball_screen == NULL) 创建`，
 * 不置 NULL 的话下次进来会拿已释放的地址去 lv_screen_load。 */
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
 *
 * 【怎么做出"圆环"】
 *   1) 尺寸设为 (2r)×(2r) 的正方形；
 *   2) radius 设成 LV_RADIUS_CIRCLE —— 这是 LVGL 的一个特殊宏（值 65535），
 *      表示"圆角半径取到最大"，正方形就变成了正圆；
 *   3) 背景透明度设为 TRANSP（全透明），只保留边框；
 *   4) 边框颜色和宽度由参数决定。
 *   于是看起来就是一个空心圆环。做"实心球"就是反着来：背景不透明、边框为 0。
 *
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
    /* 四档字体：注意正好占满 cn_font 的 4 个缓存槽 */
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_big   = cn_font(36);   // 结束提示大字
    lv_font_t *font_norm  = cn_font(20);   // 按钮字号
    lv_font_t *font_desc  = cn_font(14);   // 提示小字

    srand(time(NULL));       // 初始化随机数种子（只在进入时调一次）
    state = GS_PLAYING;      // 每次进入重置为"进行中"

    /* 创建屏幕和全屏窗口（纯色底，无背景图） */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题栏：返回按钮（位置样式全工程统一：20,12 / 90×40） */
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

    /* 终点圆环（绿色）—— 绿色表示"安全/目标" */
    target_obj = create_circle(win, TARGET_R, lv_color_hex(0x2FD3A0), 3);

    /* 陷阱圆环（红色）—— 红色表示"危险"，符合直觉
     * 注意：这里只是创建控件，位置由后面的 randomize_circles + apply_circle_pos 决定 */
    for(int i = 0; i < TRAP_COUNT; i++)
        trap_objs[i] = create_circle(win, TRAP_R, lv_color_hex(0xE74C3C), 3);

    /* 小球（橙色实心圆）
     * 和圆环相反：背景不透明（默认就是 COVER，所以这里不设 bg_opa）、
     * 边框不设，用 radius 把正方形变圆。 */
    ball_obj = lv_obj_create(win);
    lv_obj_set_size(ball_obj, BALL_R * 2, BALL_R * 2);
    lv_obj_set_style_bg_color(ball_obj, lv_color_hex(0xF5A623), 0);
    lv_obj_set_style_radius(ball_obj, LV_RADIUS_CIRCLE, 0);
    ball_x = START_X; ball_y = START_Y; ball_vx = 0; ball_vy = 0;
    lv_obj_set_pos(ball_obj, (int)ball_x - BALL_R, (int)ball_y - BALL_R);

    /* 结果提示标签（默认隐藏，游戏结束才显示）
     * LV_ALIGN_CENTER + y偏移 -40：屏幕中央偏上；
     * 重开按钮则放在中央偏下 +50，两者不重叠。 */
    result_label = lv_label_create(win);
    lv_label_set_text(result_label, "");
    lv_obj_set_style_text_font(result_label, font_big, 0);
    lv_obj_set_style_text_color(result_label, lv_color_hex(0xF5A623), 0);
    lv_obj_align(result_label, LV_ALIGN_CENTER, 0, -40);
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);

    /* 重新开始按钮（默认隐藏，绿色主操作色） */
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

    /* 底部操作提示：用 BOTTOM_MID 定位，换分辨率不用改坐标 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "倾斜开发板，滚进绿色圆环获胜，避开红色陷阱");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    /* 随机生成圆环位置并摆放到界面 */
    randomize_circles();
    apply_circle_pos();

    /* 启动游戏循环定时器（10ms 周期 ≈ 100fps）
     * 这个周期是"手感"的关键：周期越长，每帧位移越大、越像"跳"；
     * 太短（比如 2ms）则 CPU 占用上升而看不出区别。10ms 是折中。 */
    game_timer = lv_timer_create(game_loop_cb, 10, NULL);

    lv_screen_load(scr);        /* 创建完立刻显示 */
    ball_screen = scr;          /* 记录全局，供 game_center 和返回回调使用 */
    return scr;
}
