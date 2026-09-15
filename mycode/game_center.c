/**
 * @file    game_center.c
 * @brief   体感游戏中心界面
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 游戏中心是「桌面 -> 具体游戏」的中间层菜单。
 *   - 显示三张游戏卡片：2048 体感版、重力滚球、体感贪吃蛇，点击进入对应游戏。
 *   - 顶部有返回按钮（回桌面）和「陀螺仪已就绪」状态提示。
 *
 * =====================================================================
 * 二、全局变量说明（重要）
 * =====================================================================
 *   - select_game_screen：游戏中心屏。本模块每次切走都会把它删掉并置 NULL，
 *     下次再进来重新创建，保证卡片状态干净。
 *   - game_2048_screen：2048 游戏屏。2048.c 返回时要删它，
 *     所以此全局必须保留 —— 下次进 2048 时靠它判断"要不要重新建"。
 *
 * =====================================================================
 * 三、四个跳转回调的统一套路（看懂一个就看懂全部）
 * =====================================================================
 *     ① if(目标屏 == NULL) 目标屏 = 创建它();    // 懒创建，只建一次
 *     ② lv_screen_load(目标屏);                  // 先切走
 *     ③ if(select_game_screen) 删掉它并置 NULL;  // 再删自己
 *
 *   ★ ② 和 ③ 的顺序绝对不能颠倒：
 *     LVGL 不允许删除当前正在显示的 screen（会立刻崩或黑屏）。
 *     必须先把另一个屏显示出来，自己变成"后台屏"了才能删。
 *
 *   这四个回调（返回/2048/滚球/贪吃蛇）除了目标屏和目的屏不同，
 *   结构完全一样，所以只逐个说明差异，公共逻辑见上面。
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
 *
 * 缓存 4 个槽位。本文件实际只用 3 档字体（24/22/14），
 * 所以 4 个槽够用。
 *
 * 【已知隐患，加字号前必看】
 *   下面 if(!f) 那行只挡住了"创建失败"，但**没有挡住"缓存槽全满"**：
 *   如果凑齐 4 档之后又来一个第 5 档字号，循环结束时 empty 仍然是 -1，
 *   于是 `cache[-1] = f;` 会写到数组前面一个 int 的位置 —— 数组越界写，
 *   属于未定义行为（可能踩坏 cache_size 或别的静态变量）。
 *
 *   正确做法是在创建之前补一行：if(empty < 0) return NULL;
 *   （album_cloud.c / chat_ui.c 的同名函数都有这行保护，只有这里和
 *     album.c、settings.c 漏了。）
 *
 *   当前安全的原因：调用点固定，字号种类没超过槽位数。
 *   一旦你要加第 5 种字号，请先把上面那行补上。
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
        if(cache[i] != NULL && cache_size[i] == size)
            return cache[i];                /* 命中缓存，直接复用 */
        if(cache[i] == NULL && empty < 0)
            empty = i;                      /* 记下第一个空槽 */
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


/**
 * 返回按钮回调：切回桌面并删除游戏中心屏。
 *
 * 走的是统一套路：桌面屏已存在就复用，不存在才创建。
 * 注意本函数**不删 game_2048_screen 等游戏屏** —— 那些屏由各自
 * 模块负责（2048.c 返回时自己删），这里只管自己这一屏。
 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();   /* 懒创建桌面 */
    lv_screen_load(select_app_screen);                /* ② 先切走 */
    if(select_game_screen != NULL)
    {
        lv_obj_delete(select_game_screen);             /* ③ 再删自己（此时已不是当前屏） */
        select_game_screen = NULL;                    /* 必须置 NULL，否则成悬垂指针 */
    }
}

/**
 * 进入 2048 游戏。
 *
 * game_2048_screen 的懒创建逻辑：为 NULL 才建，非 NULL 说明
 * 上次的 2048 屏还在（比如用户没从 2048 返回过），直接复用。
 * ui_2048_init() 内部会自己 lv_screen_load，所以后面那句
 * lv_screen_load 在"新建"分支里是重复的 —— 但无害，而且保证了
 * "复用旧屏"分支也能正确切换，所以保留。
 */
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

/** 进入重力滚球游戏（套路同 to_2048_screen_cb，目标屏换成 ball_screen） */
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

/** 进入体感贪吃蛇（套路同 to_2048_screen_cb，目标屏换成 snake_screen） */
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



/**
 * 创建单个游戏卡片（图标 + 游戏名 + 描述 + 标签）。
 *
 * 【卡片内部布局（300×280，内边距 20 → 内容区 260×240）】
 *     内容 y=0    图标 64×64
 *     内容 y=80   游戏名（字号 22）
 *     内容 y=116  描述（宽 260，自动换行）
 *     内容 y=190  标签胶囊（橙色小字）
 *
 * 【坐标为什么从 0 开始而不是从 20 开始】
 *   card 已经设了 pad_all(20)，LVGL 里子对象的 (0,0) 是
 *   **内容区**的左上角（即已经扣掉内边距），所以写 0 就对了。
 *
 * 【为什么点图标/文字也能触发卡片点击】
 *   因为 lv_label 和 lv_image 默认没有 LV_OBJ_FLAG_CLICKABLE，
 *   触摸事件会"穿透"到父对象 card（它是 lv_button_create 建的），
 *   所以整张卡片都是可点区域。
 *
 * @param parent    父对象（本模块传 win）
 * @param icon_path 图标 BMP 路径（'A:' 是 LVGL 虚拟盘符，映射到板子根目录）
 * @param name      游戏名
 * @param desc      描述文字（设了宽度所以会自动换行）
 * @param tag       右上角风格的小标签文字（如"体感 · 甩牌"）
 * @param f_name    游戏名字体
 * @param f_desc    描述/标签字体
 * @param cb        点击回调
 * @param x         卡片左上角 x 坐标（本模块用 30 / 362 / 694，间隔 32）
 */
static void game_card_create(lv_obj_t *parent, const char *icon_path,
                             const char *name, const char *desc, const char *tag,
                             lv_font_t *f_name, lv_font_t *f_desc,
                             lv_event_cb_t cb, int x)
{
    /* 卡片背景：用 lv_button 当卡片，好处是自带点击态和可点击标志 */
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, 300, 280);
    lv_obj_set_pos(card, x, 130);                    /* y=130：让开顶部标题栏 */
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 20, 0);           /* 内容区 = 300-40 = 260 宽 */

    /* 图标（已按 64x64 尺寸生成，直接显示，不做缩放）
     * 注意：LVGL 的 lv_image 不会自动缩放，图片多大就显示多大。
     * 所以图标 BMP 生成时必须就是 64x64（12342 字节），否则会溢出卡片。 */
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, icon_path);
    lv_obj_set_pos(img, 0, 0);

    /* 游戏名 */
    lv_obj_t *name_lab = lv_label_create(card);
    lv_label_set_text(name_lab, name);
    lv_obj_set_style_text_font(name_lab, f_name, 0);
    lv_obj_set_style_text_color(name_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(name_lab, 0, 80);

    /* 描述（设宽度让它自动换行）
     * 260 = 内容区宽度，正好占满一行不溢出 */
    lv_obj_t *desc_lab = lv_label_create(card);
    lv_label_set_text(desc_lab, desc);
    lv_obj_set_style_text_font(desc_lab, f_desc, 0);
    lv_obj_set_style_text_color(desc_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_width(desc_lab, 260);
    lv_obj_set_pos(desc_lab, 0, 116);

    /* 标签（橙色小胶囊）：
     * 做法和聊天室的"当前对象胶囊"一样 —— 直接给 label 加底色+圆角，
     * 不用额外套一个容器对象，省一个对象的内存。 */
    lv_obj_t *tag_lab = lv_label_create(card);
    lv_label_set_text(tag_lab, tag);
    lv_obj_set_style_text_font(tag_lab, f_desc, 0);
    lv_obj_set_style_text_color(tag_lab, lv_color_hex(0xF5A623), 0);   /* 橙字 */
    lv_obj_set_style_bg_color(tag_lab, lv_color_hex(0x2A2410), 0);     /* 深橙底 */
    lv_obj_set_style_bg_opa(tag_lab, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tag_lab, 6, 0);
    lv_obj_set_style_pad_all(tag_lab, 6, 0);
    lv_obj_set_pos(tag_lab, 0, 190);

    /* 事件挂在 card 上（挂 card 而不是挂 label，才能整张卡片可点） */
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
}

/**
 * 游戏中心界面初始化入口。
 *
 * 本函数内部已经 lv_screen_load()，调用方无需再切屏（"创建即显示"风格）。
 * 同时会把屏幕存进全局 select_game_screen，供返回/跳转时删除。
 *
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_select_game_screen(void)
{
    /* 三档字号一次取好，后面复用（cn_font 有缓存，重复取同字号不会再建） */
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_name  = cn_font(22);   // 卡片名字字号
    lv_font_t *font_desc  = cn_font(14);   // 描述/标签字号

    /* 创建屏幕和全屏窗口
     * 套路：scr 是不带样式的空屏，win 是铺满 1024×600 的容器，
     * 所有界面元素都挂在 win 上，背景图也设在 win 上。 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图（BMP 像素字节序按本工程约定生成，别用普通工具重存） */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_game.bmp", 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);         /* 内边距清零，子对象坐标才好算 */

    /* 顶部：返回按钮 */
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
    lv_obj_center(back_lab);                     /* 文字在按钮里居中 */
    lv_obj_add_event_cb(back_btn, to_select_app_screen_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "体感游戏中心");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);              /* 130 = 让开返回按钮(20+90) 再留 20 */

    /* 状态提示胶囊（右上角，绿色）
     * ★ 注意：这是**固定文案**，并没有真的去检测陀螺仪是否可用。
     *   如果传感器没接好，这里照样显示"已就绪"。
     *   要做得更严谨应该在进入本页时读一次 imu_tilt_x() 判断是否异常。 */
    lv_obj_t *pill = lv_label_create(win);
    lv_label_set_text(pill, "陀螺仪已就绪");
    lv_obj_set_style_text_font(pill, font_desc, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0F2A21), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 20, 0);
    lv_obj_set_style_pad_all(pill, 10, 0);
    lv_obj_set_pos(pill, 850, 15);

    /* 三张游戏卡片
     * x 坐标：30 / 362 / 694，卡片宽 300，间隔 32
     * 校验：30+300=330，+32=362；+300=662，+32=694；+300=994
     *       右边距 = 1024-994 = 30，与左边距 30 对称 ✓ */
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


    /* 底部校准提示：用 LV_ALIGN_BOTTOM_MID 定位而不是写死 y，
     * 这样换屏幕分辨率时不用改坐标 */
    lv_obj_t *hint = lv_label_create(win);
    lv_label_set_text(hint, "首次进入请将开发板水平放置 2 秒完成校准");
    lv_obj_set_style_text_font(hint, font_desc, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x6A6A7A), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -24);   /* 底部居中，往上 24px */

    lv_screen_load(scr);        /* 创建完立刻切过去 */
    select_game_screen = scr;   // 记录全局，供返回/切换时删除
    return scr;
}
