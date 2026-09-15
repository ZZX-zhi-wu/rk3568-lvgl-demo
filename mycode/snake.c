/**
 * @file    snake.c
 * @brief   体感贪吃蛇游戏界面
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 12×12 网格棋盘，蛇身绿色、食物红色（用 lv_obj 方块着色）。
 *   - 两种控制模式（游戏开始前二选一，过程中不可切换）：
 *       1. 体感模式：倾斜开发板改变蛇的移动方向（读 imu_tilt_x/y）。
 *       2. 按键模式：用屏幕右侧「上/下/左/右」方向键控制。
 *   - 蛇每 300ms 自动前进一步，吃到食物变长，撞墙或撞自己判负。
 *   - 游戏结束弹出提示 + 「重新开始」按钮，重新开始后回到模式选择。
 *
 * =====================================================================
 * 二、核心数据结构
 * =====================================================================
 *   - board[GRID_H][GRID_W]：棋盘状态（0=空 1=蛇身 2=食物）。
 *   - snake_x[]/snake_y[]：蛇身各节坐标，[0] 为蛇头。
 *   - dir / next_dir：当前方向 / 下一步方向（0上 1下 2左 3右）。
 *   - game_mode：控制模式（0待选择 1体感 2按键）。
 *
 *   【为什么坐标数组不用二维、而是两个一维数组】
 *     蛇本质是一串"有顺序的点"，用 snake_x[i]/snake_y[i] 表示第 i 节，
 *     "后移一位"就是数组元素依次拷贝，非常直观。
 *     如果用二维数组存，反而要小心行列顺序。
 *     容量取 GRID_W*GRID_H（=144），即"塞满整个棋盘"的极限长度。
 *
 * =====================================================================
 * 三、依赖模块
 * =====================================================================
 *   - imu.h：读取**加速度计**倾斜值（体感控制）。
 *     ★ 不是陀螺仪：判断"板子往哪边倾"必须用加速度计测重力分量，
 *       陀螺仪测的是角速度（转动快慢），静止倾斜时读数为 0。
 *       （旧注释写的"读取陀螺仪倾斜值"有误，已修正。）
 *   - game_center.h：返回游戏中心界面。
 *
 * =====================================================================
 * 四、两个定时器的分工
 * =====================================================================
 *   move_timer（300ms，创建时就启动，一直存在）
 *       推进蛇前进一步。在 MODE_NONE（还没选模式）时直接 return，
 *       所以它是"空转"的，靠 game_mode 过滤。
 *   tilt_timer（150ms，只在选定"体感模式"后创建）
 *       读倾斜值 → 改方向。按键模式不创建它。
 *
 *   两个周期不同是刻意的：蛇 300ms 走一步，而方向检测要更密（150ms），
 *   这样玩家倾斜一下能立刻被捕捉到，不用等整步走完。
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

/* 屏幕全局指针：本模块创建/删除，game_center.c 读取 */
lv_obj_t * snake_screen = NULL;

/* ---------------------------------------------------------------------
 * 游戏数据与控件（模块级 static / 全局，供定时器回调使用）
 * --------------------------------------------------------------------- */
static int board[GRID_H][GRID_W];           // 0=空 1=蛇身 2=食物
static lv_obj_t *cells[GRID_H][GRID_W];     // 方块控件
static int snake_x[GRID_W * GRID_H];        // 蛇身 x（[0]=头）
static int snake_y[GRID_W * GRID_H];        // 蛇身 y
static int snake_len = 3;                   // 当前蛇长（初始 3 节）
static int dir = 3;                         // 0上 1下 2左 3右（当前正在走的方向）
static int next_dir = 3;                    // 下一步要用的方向
static int game_running = 1;                // 1=进行中 0=已结束

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

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * 缓存 4 槽，本文件用 4 档（24 标题、18 正文、36 结束大字、14 小字），
 * **正好占满** —— 再加第 5 档字号就会触发下面说的越界问题。
 *
 * 【已知隐患】少了 if(empty < 0) return NULL; 这行保护。
 * 凑满 4 档后再来第 5 档时，循环结束 empty 仍为 -1，
 * `cache[-1] = f;` 会写到数组前一个 int 的位置 → 数组越界写。
 * 本文件（和 ball.c）已经把 4 个槽用满，是最接近触发的两个文件。
 * 要加字号请先把保护行补上，或把 cache[4] 改成 cache[8]。
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
 * 刷新整个棋盘方块颜色：蛇身绿、食物红、空格深色。
 *
 * 【为什么"全量重绘"而不是"只改变化的两格"】
 *   每次移动其实只有 2~3 格真的变了（新头、旧尾，可能加食物），
 *   理论上可以只更新这几格。但全量重绘 144 格只是 144 次样式赋值，
 *   在 RK3568 上耗时不到 1ms，而**省掉了所有"漏改某格"的 bug 风险**。
 *   每 300ms 才一次，性能完全不是问题 —— 属于"用性能换正确性"的合理取舍。
 *
 *   如果以后要把蛇做成 50×50 的大棋盘，就该改成增量更新了
 *   （那时 2500 次/步会真的卡）。
 */
static void draw_board(void)
{
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++) {
            lv_color_t c;
            if(board[i][j] == 1)      c = lv_color_hex(0x2FD3A0);   /* 蛇身：绿 */
            else if(board[i][j] == 2) c = lv_color_hex(0xE74C3C);   /* 食物：红 */
            else                      c = lv_color_hex(0x1B1B26);   /* 空：深灰 */
            lv_obj_set_style_bg_color(cells[i][j], c, 0);
        }
    /* 注：这里只改了颜色，没改控件尺寸。方块比格子小 2px（CELL-2），
     * 差值由创建时的 +1 偏移补上，所以看起来格子之间有细缝，网格感更清晰。 */
}

/**
 * 在随机空格生成一个新食物。
 *
 * 【算法：先数空格，再随机选"第 k 个空格"】
 *   典型的水塘抽样思路，但这里更直接：
 *     ① 遍历一遍数出空格总数 empty
 *     ② 随机取 k = 1..empty
 *     ③ 再遍历一遍，第 k 个空格就是目标
 *   两遍遍历都是 144 次，代价可忽略。
 *
 *   为什么不"随机坐标直到撞上空格"：
 *   棋盘快满时（只剩 1~2 个空格）那种做法平均要试几十次，
 *   而这里恒定两遍，最坏情况也可控。
 *
 * 【为什么开头要 if(empty == 0) return】
 *   蛇填满整个棋盘时没有空格可放，直接返回 ——
 *   如果不判，下面 rand()%0 会**除零崩溃**。
 */
static void spawn_food(void)
{
    int empty = 0;
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++)
            if(board[i][j] == 0) empty++;
    if(empty == 0) return;                  /* 棋盘满了，无处可放（也避免除零） */
    int k = rand() % empty + 1, q = 0;      /* k 取 1..empty */
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++)
            if(board[i][j] == 0) {
                q++;
                if(q == k) { board[i][j] = 2; return; }   /* 第 k 个空格放食物 */
            }
}

/**
 * 初始化蛇（3 节横向，头在 (5,5)）。
 *
 * 蛇身从蛇头往左排：头 (5,5)，第 1 节 (4,5)，第 2 节 (3,5)。
 * 初始方向 dir=3（右），所以蛇一开局就朝"身体的反方向"走，
 * 不会第一步就撞到自己身上。
 *
 * 【关于 memset】
 *   这里调用了 memset 但本文件**没有显式 #include <string.h>**，
 *   是靠 lvgl.h 间接引入的。能编过，但属于隐性依赖 ——
 *   规范做法是显式包含自己用到的头文件。
 *   （同理，abs() 来自 <stdlib.h>，那个是显式包含的。）
 */
static void init_snake(void)
{
    memset(board, 0, sizeof(board));        /* 清空棋盘 */
    snake_len = 3;
    dir = 3; next_dir = 3;                  /* 初始朝右 */
    for(int i = 0; i < snake_len; i++) {
        snake_x[i] = 5 - i;
        snake_y[i] = 5;
        board[5][5 - i] = 1;                /* 注意行列顺序：board[y][x] */
    }
    spawn_food();                           /* 放第一个食物 */
    draw_board();
}

/**
 * 移动一步。
 * @return 1=继续，0=死亡（撞墙或撞自己）
 *
 * 【执行顺序（每一步都不能颠倒）】
 *   ① dir = next_dir          把"玩家想走的方向"变成"正在走的方向"
 *   ② 算出新蛇头坐标 ny/nx
 *   ③ 撞墙判定 → 返回 0
 *   ④ 记录旧尾巴坐标（吃食物时要保留它）
 *   ⑤ 蛇身整体后移一位（从尾到头拷贝，才不会覆盖数据）
 *   ⑥ 写入新蛇头 + 棋盘标记
 *   ⑦ 吃食物 → 变长；没吃 → 清掉旧尾巴
 *
 * 【⑤ 为什么必须"从尾到头"拷贝】
 *   如果从头到尾拷：snake_x[0] 先被覆盖，那么拷 snake_x[1] 时读到的
 *   已经是新值了，整条蛇会被"刷成同一节"。从尾到头拷，
 *   每次都读"还没被覆盖的前一节"，数据才正确。
 *   这是数组内移位最容易踩的坑。
 */
static int move_step(void)
{
    dir = next_dir;
    int nx = snake_x[0], ny = snake_y[0];   /* 从旧蛇头出发 */
    if(dir == 0) ny--;                      /* 上：y 减小（屏幕坐标 y 向下为正） */
    else if(dir == 1) ny++;
    else if(dir == 2) nx--;
    else if(dir == 3) nx++;

    if(nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) return 0;  // 撞墙

    int eat = (board[ny][nx] == 2);           // 是否吃到食物
    /* 撞自己判负。注意判定时机在"尾巴被清除之前"：
     * 所以"蛇头进入当前尾巴所在的格子"也算死。
     * 标准贪吃蛇规则里这种情况应该允许（因为尾巴马上就要让开了），
     * 这里更严格一点 —— 属于游戏规则的取舍，不影响可玩性。 */
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
        /* 原理：上面已经把 [0..len-1] 整体后移，原本的尾巴信息
         * 在 snake_x[len-1] 位置已被覆盖（变成了原来 len-2 的值）。
         * 所以要先把它存进 tail_x/tail_y，变长后再写回 len 位置。
         * 容量方面：snake_len 最大 144（塞满棋盘），数组大小正是 144，
         * 而塞满后没有空格也就不会再有食物，所以不会越界。 */
        snake_len++;
        snake_x[snake_len - 1] = tail_x;
        snake_y[snake_len - 1] = tail_y;
        spawn_food();                       /* 吃掉一个补一个 */
    } else {
        // 不吃：清除旧尾巴
        /* 注意这里清的是"棋盘标记"，蛇身坐标数组不需要清 ——
         * 因为下一位已经被后面的元素补齐了，只有棋盘需要反映"这格空了" */
        board[tail_y][tail_x] = 0;
    }
    draw_board();
    return 1;
}

/* 游戏结束：停体感定时器、禁用方向键、弹提示
 *
 * 注意这里删的是 tilt_timer 而不是 move_timer：
 *   move_timer 留着继续跑，但它开头的 `if(!game_running) return;` 会让它空转，
 *   这样重新开始时不用重建定时器（也避免"删了忘记建"导致蛇不动）。
 *   tilt_timer 则必须删 —— 它会读传感器并改方向，游戏结束后不应再响应。 */
static void game_over(void)
{
    game_running = 0;
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }
    lv_label_set_text(result_label, "游戏结束");
    lv_obj_remove_flag(result_label, LV_OBJ_FLAG_HIDDEN);   /* 显示提示 */
    lv_obj_remove_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);    /* 显示重开按钮 */
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);   // 结束禁用方向键
}

/* 重新开始：重置数据、回到待选择模式、重新弹出模式按钮
 *
 * 注意"回到模式选择"是刻意的设计：重开时不沿用上次的模式，
 * 强制玩家重新选一次（毕竟可能想换个玩法）。
 * 对应地也要把方向键重新禁用、把模式按钮重新显示出来。 */
static void restart_game(void)
{
    init_snake();
    game_mode = MODE_NONE;                  /* 回到待选择 */
    game_running = 1;
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }

    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);      /* 藏提示 */
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);       /* 藏重开按钮 */

    // 重新弹出模式选择
    lv_obj_remove_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(mode_key_btn, LV_OBJ_FLAG_HIDDEN);

    // 待选择状态下禁用方向键
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);
}

static void restart_btn_cb(lv_event_t *e) { restart_game(); }

/* 移动定时器：每 300ms 移动一步（待选择/结束时不动）
 *
 * 300ms 是"可玩性"的折中：更快（如 150ms）体感模式来不及反应，
 * 更慢（如 500ms）则显得拖沓。经典诺基亚贪吃蛇也是这个量级。 */
static void move_timer_cb(lv_timer_t *t)
{
    if(game_mode == MODE_NONE || !game_running) return;   /* 还没选模式 / 已结束 → 空转 */
    if(!move_step()) game_over();                         /* 撞了 → 结束 */
}

/**
 * 方向按钮回调：仅按键模式且游戏进行中响应，点击改变方向（不能直接反向）。
 *
 * 【为什么用 user_data 而不是每个方向一个回调】
 *   四个按钮的处理逻辑一模一样，只有方向值不同。
 *   把方向值用 (void *)(long)d 塞进 user_data，一个回调就够了。
 *   注意是 (long) 中转再转指针 —— 直接把 int 转 void* 在 64 位平台上
 *   会触发编译警告（指针 64 位、int 32 位）。
 *
 * 【不能直接反向】
 *   蛇正在向右走时点"左"会让蛇头撞进第一节身体，瞬间自杀。
 *   所以这里挡掉所有"相反方向"的组合。
 *
 * 【比较的是 dir 而不是 next_dir】
 *   意味着同一拍里可以"改一次方向"，但不能连改两次相反方向。
 *   例如向右走时：点"上"→ next_dir=0（允许）；接着点"左"→
 *   检查 d==2 && dir==3 → 命中反向规则被挡掉（因为 dir 还是 3）。
 *   这正好避免了"一拍内把自己转死"。 */
static void dir_btn_cb(lv_event_t *e)
{
    if(game_mode != MODE_KEY || !game_running) return;      /* 非按键模式不响应 */
    int d = (int)(long)lv_event_get_user_data(e);
    if((d == 0 && dir == 1) || (d == 1 && dir == 0) ||
       (d == 2 && dir == 3) || (d == 3 && dir == 2))
        return;                                             /* 反向 → 忽略 */
    next_dir = d;
}

/**
 * 体感定时器：仅体感模式创建，每 150ms 读倾斜改变方向（不能直接反向）。
 *
 * 【判方向的方法：比较"哪一轴的绝对值更大"】
 *   倾斜值 tx / ty 都是双轴的。哪个轴分量更大，说明板子主要在往那个方向倒，
 *   就按那个轴判方向。这是"取主分量"的思路，好处是不用算真正的倾斜角
 *   （省掉 atan2 和开方）。
 *
 *   副作用：45° 斜着拿时会抖（两个轴的分量接近，会在"左右"和"上下"之间跳），
 *   所以实际操作要"明确地往一个方向倾"，不要斜着晃。
 *
 * 【不能直接反向】判的是 dir（当前方向）而不是 next_dir，理由同 dir_btn_cb。
 */
static void tilt_timer_cb(lv_timer_t *t)
{
    if(!game_running) return;
    int tx = imu_tilt_x();
    int ty = imu_tilt_y();
    if(abs(tx) > abs(ty)) {                                 /* X 轴分量更大 → 判左右 */
        if(tx >  TILT_THRESHOLD && dir != 2) next_dir = 3;   // 右
        if(tx < -TILT_THRESHOLD && dir != 3) next_dir = 2;   // 左
    } else {                                                /* Y 轴分量更大 → 判上下 */
        if(ty >  TILT_THRESHOLD && dir != 0) next_dir = 1;   // 下（屏幕 y 向下为正）
        if(ty < -TILT_THRESHOLD && dir != 1) next_dir = 0;   // 上
    }
    /* 注意这里**不直接移动**，只改 next_dir。
     * 真正的移动交给 move_timer_cb（300ms 一次），
     * 所以体感"转向"和"走一步"是解耦的：
     * 倾斜只决定"下一朝哪走"，速度由移动定时器统一控制。 */
}

/*
 * 开始游戏：选定模式后隐藏模式按钮，按需启用对应控制源。
 * @param mode MODE_TILT 或 MODE_KEY
 */
static void start_game(int mode)
{
    game_mode = mode;
    /* 三个模式选择控件一起藏掉，界面上不再有"未开始"的痕迹 */
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
    /* ★ 核心思想：**选了一种输入就彻底关掉另一种**。
     *   体感模式禁用方向键（看得见的禁用，用户不会白点）；
     *   按键模式不创建体感定时器（连读传感器都省了）。
     *   这样就不存在"两种输入打架"的问题。 */
}

/* 模式按钮回调（两个按钮共用一个 start_game，只是参数不同） */
static void mode_tilt_cb(lv_event_t *e) { start_game(MODE_TILT); }
static void mode_key_cb(lv_event_t *e)  { start_game(MODE_KEY);  }

/* 返回游戏中心
 *
 * 顺序：① 删两个定时器 → ② 切屏 → ③ 删屏 → ④ 置 NULL。
 * move_timer 300ms、tilt_timer 150ms 都在持续访问棋盘控件，
 * 所以必须先把两个定时器都停掉，才能删屏。 */
static void to_game_center_cb(lv_event_t *e)
{
    if(move_timer) { lv_timer_delete(move_timer); move_timer = NULL; }
    if(tilt_timer) { lv_timer_delete(tilt_timer); tilt_timer = NULL; }
    if(select_game_screen == NULL)
        select_game_screen = ui_select_game_screen();
    lv_screen_load(select_game_screen);
    if(snake_screen != NULL) { lv_obj_delete(snake_screen); snake_screen = NULL; }
}

/* 创建方向按钮：x/y 左上角，size 边长，text 文字，dir_val 方向值
 *
 * 和 cand 里其他按钮的写法一致：lv_button + 居中 label，
 * 唯一特殊的是 user_data 传了方向值（供共用的 dir_btn_cb 区分）。 */
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

/**
 * 体感贪吃蛇界面初始化入口。
 *
 * 【布局（1024×600）】
 *   棋盘 480×480，水平居中 bx = (1024-480)/2 = 272，纵向从 y=70 开始 →
 *   棋盘占 x 272~752、y 70~550。
 *   每个格子 40×40，方块做成 38×38 再 +1 偏移，于是格子间有 2px 缝隙，
 *   看起来是"网格"而不是一整块色块。
 *
 *   右侧方向键（十字布局）：
 *      上 (852,202)  72×72
 *      左 (780,274)  下 (852,346)  右 (924,274)
 *
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_snake_init(void)
{
    /* 四档字体，正好占满 cn_font 的 4 个缓存槽 */
    lv_font_t *font_title = cn_font(24);
    lv_font_t *font_norm  = cn_font(18);
    lv_font_t *font_big   = cn_font(36);
    lv_font_t *font_desc  = cn_font(14);

    srand(time(NULL));          /* 只在进入时播种一次 */
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

    /* 标题栏（位置样式全工程统一：20,12 / 90×40） */
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

    /* 12x12 棋盘方块
     * 144 个 lv_obj 一次性建好，之后只改颜色不重建（重建 144 个对象太慢）。
     * 初始颜色是 lv_obj 的默认色，紧接着的 init_snake()->draw_board()
     * 会立刻把全部格子刷成正确颜色。 */
    int bx = (1024 - BOARD) / 2;            /* 水平居中 = 272 */
    for(int i = 0; i < GRID_H; i++)
        for(int j = 0; j < GRID_W; j++) {
            cells[i][j] = lv_obj_create(win);
            lv_obj_set_size(cells[i][j], CELL - 2, CELL - 2);       /* 38×38，留 2px 缝隙 */
            lv_obj_set_pos(cells[i][j], bx + j * CELL + 1, 70 + i * CELL + 1);   /* +1 让缝隙居中 */
            lv_obj_set_style_radius(cells[i][j], 4, 0);             /* 小圆角，方块更好看 */
            lv_obj_remove_flag(cells[i][j], LV_OBJ_FLAG_CLICKABLE); /* 格子不接收点击，避免挡住其他事件 */
        }

    /* 右侧方向键（触摸控制上下左右，初始禁用，仅按键模式启用） */
    lv_obj_t *dir_label = lv_label_create(win);
    lv_label_set_text(dir_label, "方向键");
    lv_obj_set_style_text_font(dir_label, font_desc, 0);
    lv_obj_set_style_text_color(dir_label, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(dir_label, 862, 168);

    /* 十字布局：上/下 在中间一列（x=852），左/右 在中间一行（y=274）
     * 按钮 72×72，左(780) 与右(924) 之间正好空出 72 给"下"的位置留通道 */
    dir_btns[0] = create_dir_btn(win, 852, 202, 72, "上", font_norm, 0);   // 上
    dir_btns[1] = create_dir_btn(win, 852, 346, 72, "下", font_norm, 1);   // 下
    dir_btns[2] = create_dir_btn(win, 780, 274, 72, "左", font_norm, 2);   // 左
    dir_btns[3] = create_dir_btn(win, 924, 274, 72, "右", font_norm, 3);   // 右
    for(int i = 0; i < 4; i++)
        lv_obj_add_state(dir_btns[i], LV_STATE_DISABLED);   // 初始禁用
    /* 为什么初始就禁用：游戏还没开始（MODE_NONE），
     * 此时点方向键本来也不会生效（dir_btn_cb 会拦），
     * 但按钮显示成灰的能让用户直观知道"现在不能点"。 */

    /* ===== 模式选择（游戏开始前显示，选定后隐藏）===== */
    mode_label = lv_label_create(win);
    lv_label_set_text(mode_label, "请选择控制模式");
    lv_obj_set_style_text_font(mode_label, font_norm, 0);
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xECEAF2), 0);
    lv_obj_align(mode_label, LV_ALIGN_CENTER, 0, -140);     /* 屏幕中央偏上 */

    // 体感模式（蓝色）
    /* 两个按钮用 -95 / +95 的水平偏移对称摆放，宽 170 → 中间留 20px 间隙 */
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

    /* 结束提示 + 重新开始（默认隐藏）
     * 两个控件都放屏幕中央：提示在 -40，按钮在 +50，互不遮挡。 */
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

    /* 底部提示：用 BOTTOM_MID 定位，换分辨率不用改坐标 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "选择模式后开始游戏，吃到红色食物变长");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);

    init_snake();                                       /* 摆好蛇和第一个食物 */
    move_timer = lv_timer_create(move_timer_cb, 300, NULL);   /* 300ms 走一步 */
    // 注意：体感定时器不在这里创建，等用户选定「体感模式」后才启动

    lv_screen_load(scr);        /* 创建完立刻显示 */
    snake_screen = scr;         /* 记录全局，供 game_center 和返回回调使用 */
    return scr;
}
