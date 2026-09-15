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
 * ── 数据模型（两张表一一对应）────────────────────────────────
 *   game_grid[4][4]  纯逻辑数据：0=空格，非 0=该格的数字（2/4/8/.../2048）
 *   tile_image[4][4] 对应位置的 lv_image 控件，只负责把数字画成图片
 *   逻辑与显示分离：所有移动/合并只改 game_grid，改完统一调
 *   update_game_2048() 把 16 张图重刷一遍。这样算法里完全不用碰 LVGL。
 *
 * ── 一次「移动」的算法（本文件的重点）────────────────────────
 *   把二维问题压成一维：每行（或每列）取 4 个数到 temp_arr，然后固定三步
 *       1) rm_zero()  把非 0 数字往移动方向那一端挤紧  → 2 0 2 0  变成 2 2 0 0
 *       2) hebing()   相邻相同就翻倍合并，并累加得分    → 2 2 0 0  变成 4 0 0 0
 *       3) rm_zero()  合并后可能又出现空洞，再挤一次    → 4 0 0 0 保持
 *   三步做完写回 game_grid，若这期间 flag 被置 1（确实动过），才随机补一个新方块。
 *   四个方向共用这套逻辑，区别只在「怎么取进 temp_arr / 怎么写回去」：
 *       左：正序取、正序写        右：倒序取、倒序写
 *       上：按列正序取、正序写    下：按列倒序取、倒序写
 *   注意 hebing() 必须「从左往右只走一遍」才是 2048 的正确规则：
 *   4 4 4 4 一次滑动只能变成 8 8，不能变成 16 4，也不能 4 4 → 8 后再和后面的 8 合并。
 *
 * ── 状态机（game_over_flag + won_before）────────────────────
 *   game_over_flag: 0=进行中  1=失败  2=成功
 *   won_before:     已经弹过一次成功提示的标记。2048 达到 2048 后还能继续玩，
 *                   没有这个标记的话，每次移动都会反复弹「游戏成功」。
 *
 * ── 已知坑（改动时注意）──────────────────────────────────────
 *   1. cn_font() 的缓存只有 4 槽且缺少「缓存满」的保护，本文件用到 3 种字号，
 *      暂时安全；若以后新增第 5 种字号，empty 会一直是 -1，写入 cache[-1] 越界。
 *   2. 屏幕对象 game_2048_screen 声明在 game_center.h（不在 2048.h），
 *      ui_2048_init() 自己不保存指针，由调用方存。见 to_game_center_cb()。
 *   3. 图片路径是板子绝对路径 A:/work_space/...（'A' 由 lv_conf.h 里
 *      LV_FS_POSIX_LETTER 定义映射到根目录），不是 PC 上的路径。
 *
 * 依赖模块：
 *   - imu.h：读取倾斜值（体感控制）。注意用的是加速度计分量，不是陀螺仪：
 *            「哪边朝下」是重力方向决定的，陀螺仪只反映转动角速度。
 *   - game_center.h：返回游戏中心界面，同时持有 game_2048_screen 指针。
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

/* 控制模式枚举：游戏开始前由用户点按钮决定，选定后本局不再改变 */
#define MODE_NONE   0   // 待选择模式（游戏未开始，两个按钮可见）
#define MODE_SLIDE  1   // 滑动模式（触摸拖动）
#define MODE_TILT   2   // 体感模式（倾斜板子）

/* 4x4 游戏棋盘数据：0 表示空格，非 0 表示对应数字的方块。
 * 这里的初值只是为了给静态数组一个合法尺寸，开局时会被 init_2048_game()
 * 的 memset 全部清零再随机放两个方块，改这里不影响游戏。 */
int game_grid[4][4] = {
	0,0,2,4,
	0,0,0,2,
	2,2,0,0,
	0,0,0,0
};

/* 4x4 瓷砖图像控件，与 game_grid 一一对应，用于在界面上显示数字。
 * 下标含义统一为 tile_image[行][列]，和 game_grid[i][j] 完全对应。 */
lv_obj_t *tile_image[4][4];

/* ── 本文件用到的全部状态变量（都是文件内静态，外部只通过 ui_2048_init 进入）── */
static int score = 0;                    // 当前得分（每次合并累加合并后的值）
static lv_obj_t *score_label = NULL;     // 得分显示标签
static lv_timer_t *tilt_timer = NULL;    // 体感检测定时器（仅体感模式创建，切换/退出时必须删）
static int tilt_armed = 1;               // 体感触发开关：1=可触发，0=已触发过、等回中（防连发）
static lv_obj_t *result_label = NULL;    // 结束提示字（"游戏成功！"/"游戏失败！"）
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
 *
 * 为什么要缓存：lv_freetype_font_create() 每次都会真正加载字体文件并占内存，
 * 如果放在刷新函数里反复调用，几秒钟就能把板子内存吃光。这里做 4 个槽的缓存，
 * 相同字号第二次调用直接返回旧指针。
 *
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL
 *
 * @warning 缓存满（4 种不同字号都出现过）时 empty 会保持 -1，
 *          此后 cache[empty] 就是 cache[-1]，属于数组越界写。
 *          本文件只用到 3 种字号（28/18/36），所以碰不到；
 *          若以后要加第 5 种字号，必须先把这里补成
 *          `if(empty < 0) return NULL;` 再做后续操作。
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

/* 刷新得分标签显示。注意 LV_USE_FLOAT=0，所以用 %d 而不是 %f */
static void update_score(void)
{
    lv_label_set_text_fmt(score_label, "得分 %d", score);
}

/* 统计棋盘中空格（0）的个数，供随机生成新方块时使用。
 * 返回 0 表示棋盘已满 —— 调用方 rand_num() 依赖它做取模，务必注意。 */
static int get_arr_zero_count(void)
{
    int count = 0;
    for(int i = 0;i < 4;i++)
        for(int j = 0;j < 4;j++)
            if(game_grid[i][j] == 0) count++;
    return count;
}

/*
 * 在随机一个空格上生成新数字。
 *
 * 做法：先数出空格总数 count，从中随机挑第 k 个空格（k = rand()%count + 1），
 *       再遍历棋盘找到第 k 个空格填进去。
 *
 * 新数字的概率：rand()%10 得到 0~9 共 10 种等概率结果，
 *   条件 >2 覆盖 {3..9} 共 7 种 → 70% 填 2；其余 {0,1,2} 共 3 种 → 30% 填 4。
 *   即 70% 出 2、30% 出 4（不是 90%/10%）。
 *
 * @note 内层那个 break 只能跳出「内层 j 循环」，外层 i 循环还会继续跑完。
 *       不过因为 k 只会命中一次，后面几轮的 q 不可能再等于 k，所以结果是对的，
 *       只是白跑几圈循环而已。
 * @warning 若 count==0（棋盘已满）会执行 rand()%0，除零导致崩溃。
 *          目前所有调用点都是「确认发生过移动/合并」之后才调，彼时必然有空位，
 *          所以实际不会触发；但这里没有防御，改动调用时机时要留意。
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

/*
 * 去掉一行中的 0（把非零数字挤到左侧），若发生移动则置 flag=1。
 *
 * 双指针写法：i 扫描原数组，k 指向「下一个该放非零数的位置」。
 * 例：  [0, 2, 0, 4]  →  i=1 时把 2 放到 k=0，i=3 时把 4 放到 k=1
 *                      →  [2, 4, 0, 0]，且 *flag=1
 *      [2, 2, 0, 0]  →  每个数本来就在位（k==i），一个字节都不动，*flag 保持原样
 *
 * 注意 temp_arr[i] = 0 那句必须在赋值之后执行：先把非零数搬到 k 处覆盖掉
 * 旧值，再把原位置抹成 0，否则会把刚搬过去的数又清掉。
 *
 * @param temp_arr 待处理的一行（4 个元素，原地修改）
 * @param flag     出参：发生过实际位移就置 1（只加不清，多重调用可累加）
 */
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

/*
 * 合并一行中相邻的相同数字（翻倍并累加得分），若发生合并则置 flag=1。
 *
 * 规则细节（这是 2048 最容易写错的地方）：
 *   1. 从下标 0 往后只走一遍（i 只到 2），并且合并后把 i+1 清 0。
 *      这样 [4,4,4,4] 的结果是 [8,8,0,0] —— 第二次合并发生在 i=2 位，
 *      不会出现 [4,4,4,4] → [8,0,4,4] → 再合出 16 这种「连锁吞并」。
 *   2. 判断条件里必须带 temp_arr[i] != 0，否则两个空格（0==0）会被误判成
 *      「相同数字」而合并，score 还会加 0、flag 被误置，凭空多刷一个新方块。
 *   3. 得分按「合并后的值」累加，而不是原值 —— 合出 8 就加 8。
 *
 * @param temp_arr 已经去过 0 的一行（4 个元素，原地修改）
 * @param flag     出参：发生过合并就置 1
 */
static void hebing(int temp_arr[],int *flag)
{
    for(int i = 0; i < 3;i++)
    {
        if(temp_arr[i] == temp_arr[i+1] && temp_arr[i] != 0)
        {
            temp_arr[i] *= 2;
            score += temp_arr[i];   // 累计得分（按合并后的值）
            temp_arr[i+1] = 0;
            *flag = 1;
        }
    }
}

/*
 * 向左移动：逐行处理，先去掉 0、再合并、再去掉 0。
 * 每行都要做「去0 → 合并 → 再去0」三步，第三步不能省：
 * 例如 [2,2,2,0] 去0后 [2,2,2,0]，合并后 [4,0,2,0]，此时中间的空洞要靠再挤一次
 * 才能变成正确的 [4,2,0,0]（合并出来的空位必须让后面的数字补上）。
 *
 * flag 声明在行循环外面，四行共用：只要任意一行动过就为 1，
 * 因此整次滑动最多只补一个新方块（这是 2048 的规则，不能每行补一个）。
 */
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

/*
 * 向右移动：与左移同理，但用户看到的「右边」对应数组下标大的那一头，
 * 所以取数时倒序（temp_arr[3-j] = game_grid[i][j]，右下角对齐），
 * 复用同一套「向 0 下标挤」的逻辑处理完，再倒序写回原位。
 * 这样四个方向只需要一套算法，不用为方向分别写不同的合并规则。
 */
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

/*
 * 向上移动：逐列处理（把每一列当成一行来看待）。
 * 注意取写都是 game_grid[j][i] —— 固定列号 i，让行号 j 变化，
 * 于是原本竖着的一列就变成了 temp_arr 里横着的一维数组。
 */
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

/* 向下移动：逐列处理，且列内倒序（下边对齐，对应数组下标大的行） */
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

/*
 * 判断是否失败：棋盘满了且任何一个方向都没有相邻相同数字可合并。
 * 思路是「找反例」——只要找到一个空格、或一对相邻相同的数字，就说明还能动，
 * 返回 0；全盘扫完都找不到才返回 1。返回 1 表示无路可走，游戏结束。
 * 只检查右边和下边两个方向就够了：相邻关系是对称的，往右/往下看一遍
 * 等价于把左右、上下相邻都覆盖了。
 */
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

/*
 * 判断是否成功：出现 2048 方块即算达成目标。
 * @note 这里的 2048 是写死的。如果以后改了瓷砖图片的最大数字，
 *       记得同步改这里，否则游戏永远判不了成功。
 */
static int game_success(void)
{
    for(int i = 0;i < 4;i++)
        for(int j = 0;j < 4;j++)
            if(game_grid[i][j]==2048) return 1;
    return 0;
}

/*
 * 游戏结束处理：弹出提示和按钮。
 *
 * 成功时按钮要并排两个（重新开始靠右、继续靠左），失败时只有一个
 * 重新开始按钮，所以这里要重新 lv_obj_align 调整位置：
 *   成功 → 重新开始居中偏右(+90)、继续居中偏左(-90)，两个并排
 *   失败 → 重新开始直接居中(0)，继续按钮隐藏
 * 另外成功后要把 won_before 置 1，否则玩家点「继续」再随便一动，
 * 又会检测到 2048 存在而重复弹成功框。
 *
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

/*
 * 根据 game_grid 刷新所有瓷砖图片（图片名就是数字，如 2.bmp、2048.bmp）。
 * 逻辑和显示分离的关键收口点：所有移动/合并函数只管改 game_grid，
 * 改完统一调这里把 16 张图重刷一遍，所以算法部分完全不需要认识 LVGL。
 *
 * 路径 A:/work_space/2048pic/N.bmp 里的 'A' 不是真的盘符，
 * 而是 lv_conf.h 中 LV_FS_POSIX_LETTER 配的字母，映射到文件系统根目录，
 * 因此这条路径在板子上就是 /work_space/2048pic/N.bmp。空格（0）也有对应的
 * 0.bmp，就是一块空底色的图，所以这里不用对 0 做特殊判断。
 *
 * @note 每次调用都会重新打开 16 个 BMP 文件解码（lv_image_set_src 传路径时
 *       不做跨调用缓存），一次移动 16 次文件 IO。4x4 规模下感官上够快，
 *       若以后做更大棋盘，应考虑预先把图片全部解码成 lv_image_dsc_t 常驻内存。
 */
static void update_game_2048(void)
{
    for(int i = 0;i<4;i++)
        for(int j = 0;j<4;j++)
        {
            char bmppathname[1024] = {0};   // 路径实际不到 40 字节，这里给得很宽松
            sprintf(bmppathname,"A:/work_space/2048pic/%d.bmp",game_grid[i][j]);
            lv_image_set_src(tile_image[i][j],bmppathname);
        }
}

/*
 * 根据起点/终点坐标判断滑动方向。
 *
 * 坐标系是 LVGL 的屏幕坐标：原点在左上角，x 向右增大、y 向下增大。
 * 所以 dy > 0 是往下、dy < 0 是往上，别按数学坐标轴去理解。
 *
 * 判定逻辑：比较 |dx| 和 |dy|，谁大就认为主要往那个轴滑。
 * 相等时（|dx| == |dy|，正好 45°）靠 >= 让横向优先，避免出现未定义方向。
 *
 * @return 0=点击未滑动（按下和松开在同一点），1=上，2=下，3=左，4=右
 */
static int get_slide(lv_point_t start_point,lv_point_t end_point)
{
    int dx = end_point.x - start_point.x;
    int dy = end_point.y - start_point.y;
    if(dx == 0 && dy == 0) return 0;
    if(abs(dx) >= abs(dy)) return dx > 0 ? 4 : 3;
    else                   return dy > 0 ? 2 : 1;
}

/*
 * 触摸滑动事件回调。注册时用的是 LV_EVENT_ALL，所以按下、移动、松开
 * 各种事件都会进来，函数里靠 code 自己筛出需要的两个：
 *   LV_EVENT_PRESSED  记录手指按下位置
 *   LV_EVENT_RELEASED 记录抬起位置 → 算方向 → 移动 → 刷图 → 判定胜负
 *
 * 只在「滑动模式且游戏未结束」时才处理，体感模式下这两行会直接 return，
 * 这正是滑动事件能一直挂在 win 上、不用随模式反复注册/注销的原因。
 *
 * start_point / end_point 是 static，跨事件调用保留上一帧的按下位置，
 * 这个函数只在 LVGL 主线程被调用，不存在多线程竞争问题。
 */
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

/*
 * 初始化游戏数据：置空棋盘、得分清零、随机生成两个初始方块。
 * 标准 2048 开局就是 2 个方块，所以 rand_num() 连调两次。
 *
 * @note srand(time(NULL)) 放在这里，等于每次开局都重新播种。
 *       好处是不用额外找地方初始化；代价是「同一秒内点两次重新开始」
 *       会得到一模一样的开局（种子相同 → 随机序列相同）。
 *       真要严格随机，应该把 srand 挪到 ui_2048_init 里只调一次。
 */
static void init_2048_game(void)
{
    srand(time(NULL));
    memset(game_grid,0,sizeof(game_grid));
    score = 0;
    rand_num();
    rand_num();
}

/*
 * 重新开始：把一切恢复到「刚进游戏」的样子。
 * 除了重置数据和隐藏结束提示，有两点容易漏：
 *   1. 必须删掉体感定时器并清空指针。否则旧的定时器还在跑，
 *      会操作已经重置的棋盘，而且下次选体感模式时会再建一个，
 *      出现两个定时器同时触发移动（一步变两步）。
 *   2. 模式要退回 MODE_NONE 并把模式选择按钮重新显示出来，
 *      让玩家重新选滑动/体感，而不是沿用上一局的模式。
 */
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

/* 重新开始按钮回调：直接复用 restart_game 的整套复位逻辑 */
static void restart_btn_cb(lv_event_t *e)
{
    restart_game();
}

/*
 * 继续按钮回调：成功后选择「继续冲更高分」。
 * 只做两件事——把 game_over_flag 清回 0（重新允许移动和判定），
 * 把三个结束相关控件隐藏。
 *
 * 这里刻意不动 game_mode、也不删 tilt_timer：
 * 本局还在继续，玩家用哪种方式操作就还该用哪种，定时器也要继续跑。
 * 这正是它和 restart_game()（一切重来）的本质区别。
 *
 * @note won_before 保持为 1，所以之后再合出 2048 也不会再弹成功框。
 */
static void continue_btn_cb(lv_event_t *e)
{
    game_over_flag = 0;
    lv_obj_add_flag(result_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(restart_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(continue_btn, LV_OBJ_FLAG_HIDDEN);
    // game_mode 保持不变，继续用当前模式玩
}

/*
 * 体感检测定时器回调（每 100ms 一次，仅体感模式创建）。
 *
 * 读加速度计的倾斜值 imu_tilt_x()/imu_tilt_y()，某个方向超过阈值就触发一次移动。
 *
 * ★ 防连发的核心是 tilt_armed + 两个不同阈值（迟滞/回差设计）：
 *     - 触发条件：|倾斜| > TILT_THRESHOLD(6000)     且 tilt_armed == 1
 *     - 解除条件：|tx| 和 |ty| 都 < TILT_THRESHOLD/2(3000) 时把 tilt_armed 置回 1
 *   之所以回中判定用「一半」的阈值，而不是同一个 6000：
 *   如果两边都用 6000，玩家把板子维持在 6000 附近微微抖动时，读数会反复
 *   越过阈值线，一秒内可能触发好几次移动，板子像疯了一样。用一个更低的
 *   回中阈值拉开「触发」和「复位」的差距，玩家必须明显回正才能再触发一次，
 *   手感才是一倾斜走一步。
 *
 * 还有个小技巧：下面方向判断里一旦真的移动了就把 tilt_armed 置 0，
 * 所以紧接着的 if(!tilt_armed) 就等价于「这一轮发生移动了」，
 * 借同一个变量兼做「本次是否移动」的标记，省掉一个额外变量。
 * 反过来，如果倾斜了但没超过 6000，不会置 0，也就不会进刷新分支。
 */
static void tilt_loop_cb(lv_timer_t *t)
{
    if(game_over_flag) return;   // 游戏结束不再响应

    int tx = imu_tilt_x();
    int ty = imu_tilt_y();

    // 倾斜很小时视为回中，重新允许下一次触发（阈值取一半，见函数头说明）
    if(abs(tx) < TILT_THRESHOLD/2 && abs(ty) < TILT_THRESHOLD/2) {
        tilt_armed = 1;
        return;
    }
    if(!tilt_armed) return;      // 还没回中，本次不触发（板子一直斜着不会连走）

    // 两个轴都倾斜时，选倾斜更明显的那个轴作为移动方向
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
 *
 * 「隐藏三个模式控件」是固定的，两种模式差别在控制源怎么起：
 *   体感模式 → 在这里创建 100ms 定时器轮询倾斜
 *   滑动模式 → 什么都不用起。滑动事件在 ui_2048_init 里已经挂到 win 上了，
 *              靠回调开头的 game_mode 判断自然就把事件过滤掉了
 * 所以滑动模式是「事件驱动、零开销」，体感模式才需要周期性轮询。
 *
 * @param mode MODE_SLIDE 或 MODE_TILT
 */
static void start_game(int mode)
{
    game_mode = mode;
    lv_obj_add_flag(mode_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_slide_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(mode_tilt_btn, LV_OBJ_FLAG_HIDDEN);

    if(mode == MODE_TILT) {
        // 体感模式：启动体感定时器（判空防止重复创建出两个定时器）
        if(tilt_timer == NULL)
            tilt_timer = lv_timer_create(tilt_loop_cb, 100, NULL);
    }
    // 滑动模式：不启动体感定时器（滑动事件已在 win 上注册，靠 game_mode 过滤）
}

/* 模式按钮回调：分别对应两个选择按钮，就是把模式号交给 start_game */
static void mode_slide_cb(lv_event_t *e) { start_game(MODE_SLIDE); }
static void mode_tilt_cb(lv_event_t *e)  { start_game(MODE_TILT);  }

/*
 * 返回按钮回调：回到游戏中心。
 *
 * ★ 下面三行的顺序不能调换（换过会黑屏卡死）：
 *   1) 先删体感定时器   —— 定时器回调会操作本屏控件，屏都删了它还在跑就是野指针
 *   2) 再切到游戏中心屏 —— 必须先把「当前活动屏」换成别的屏
 *   3) 最后删本屏       —— LVGL 不允许删除当前正在显示的屏，
 *                          若在还显示着 2048 屏时就 lv_obj_delete，界面会变黑
 *
 * 关于 select_game_screen：它是游戏中心的屏对象，由 game_center.c 管理。
 * 如果判空时发现是 NULL（比如从桌面第一次进 2048 时还没建过），
 * 就现调 ui_select_game_screen() 建一个再切过去。
 *
 * game_2048_screen 这个指针声明在 game_center.h 里，由本屏的调用方保存，
 * 我们这里只用它来删除自己，删完必须置 NULL，否则下次进来会拿它当有效指针用。
 */
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
 * 2048 界面初始化入口。由游戏中心（game_center.c）点击「2048」图标时调用。
 *
 * 整体流程：
 *   1. 先取好三种字号的中文字体（cn_font 带缓存，重复调用不会重复占用内存）
 *   2. 立刻重置游戏数据，保证每次进来都是新的一局
 *   3. 建屏幕 scr 和全屏窗口 win（深色底 0x111119）
 *   4. 摆标题、得分胶囊、棋盘底板、16 张瓷砖图
 *   5. 摆模式选择按钮、结束提示、两个按钮（都先隐藏）
 *   6. 把触摸滑动事件挂到 win 上，最后 lv_screen_load 上屏
 *
 * @return 创建的屏幕对象。注意函数自己不做保存，
 *         调用方（game_center.c）要把它存进 game_2048_screen，
 *         我们才能在 to_game_center_cb 里删掉自己。
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

    /* 创建屏幕和全屏窗口。
     * scr 是「屏」对象（lv_obj_create(NULL) 表示没有父对象，即一块屏幕），
     * 真正的内容都放在全屏窗口 win 里。win 铺满 1024x600、内边距设为 0、
     * 无圆角无边框，这样下面所有控件都可以直接按屏幕绝对坐标摆位置，
     * 不用去算内边距带来的偏移。 */
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

    /* 注册滑动事件。这里用 LV_EVENT_ALL 一次把所有事件都收进来，
     * 由 test12_cb 内部按事件码自己筛选（见该函数说明），
     * 比逐个注册 PRESSED/RELEASED 更省事。
     * 事件挂在 win 而不是 board 上，原因见上面「不可点击」那条注释。
     * 体感定时器不在这里创建，等用户点了「体感模式」按钮才启动。 */
    lv_obj_add_event_cb(win, test12_cb, LV_EVENT_ALL, NULL);

    lv_screen_load(scr);   // 真正把这块屏切到前台显示
    return scr;
}
