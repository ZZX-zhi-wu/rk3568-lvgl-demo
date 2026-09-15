/* ============================================================================
 * @file    main_interface.c
 * @brief   桌面 Launcher（App 选择界面）
 *
 * 【这个界面在流程里的位置】
 *
 *   main.c → login.c（登录页）→【本文件：桌面】→ 五个子模块之一
 *                                   ▲                    │
 *                                   └────────────────────┘
 *                                       各子模块的返回回调
 *
 * 【它做什么】
 *   - 登录后的主菜单界面，显示 5 个 App 图标卡片：
 *       传感器实验室 / 体感游戏 / 电子相册 / 设置 / 网络聊天室。
 *   - 点击卡片切换到对应界面，并删除桌面屏（避免重复创建）。
 *   - 顶部有标题「桌面」和「玩家小明」状态胶囊。
 *
 * 【本文件最重要的两个设计约定】
 *
 *   ① 桌面是「一次性屏」，返回时会被重建。
 *      点任一张卡片都会把自己的屏 lv_obj_delete 掉，
 *      所以从子界面返回时，是重新执行一遍 ui_select_app_screen()，
 *      而不是「恢复之前的桌面」。这意味着桌面上的状态不会保留
 *      （本工程也确实不需要保留：桌面本身没有可变状态）。
 *      好处是内存占用恒定 —— 任何时刻只有一块屏活着。
 *
 *   ② 每张卡片的点击回调都是同一个四步套路（详见各回调函数头部注释）：
 *      判空创建 → 切屏 → 删自己 → 置 NULL。
 *      ★ 顺序不能改动，第 ④ 步置 NULL 不能省，原因见回调函数注释。
 *
 * 【相比 8-28 版改了 3 处（网络聊天室接入带来的连带修改）】
 *   1. 加 #include "chat_ui.h"
 *   2. 追加 to_chat_cb 回调
 *   3. 卡片宽度 200 → 176，4 张卡的 x 从 52/292/532/772 改成 5 张的
 *      32/228/424/620/816（左右边距各 32，对称）
 *
 *   宽度账（卡宽 176，间距 20）—— 改尺寸时照这个算法重算，别手填坐标：
 *       32 + 176 = 208  (+20) = 228
 *      228 + 176 = 404  (+20) = 424
 *      424 + 176 = 600  (+20) = 620
 *      620 + 176 = 796  (+20) = 816
 *      816 + 176 = 992                 右边距 1024 - 992 = 32 ✓
 *   屏宽 1024 - 右边距 32 - 左边距 32 = 960 可用；
 *   5 张卡 5*176 = 880，4 个间隙 4*20 = 80，880 + 80 = 960 ✓ 正好塞满。
 *   （这也是为什么卡宽必须是 176：176*5 + 20*4 = 960，多一点都放不下。）
 *
 * 【全局变量说明】
 *   - select_app_screen：桌面屏，各子界面返回时复用；切换/删除后置 NULL，
 *     避免悬空指针导致二次点击段错误。
 *   - album_screen：电子相册屏（album.c 使用）。
 *   - chat_conn_screen / chat_room_screen：聊天室两个屏（chat_ui.c 使用）。
 *   这几个全局变量是「屏的所有权」登记表：谁创建谁负责置 NULL。
 * ========================================================================== */

#include "../lvgl/lvgl.h"
#include "album.h"
#include "2048.h"
#include "game_center.h"
#include <stdio.h>
#include "main_interface.h"
#include "settings.h"
#include "sensor_lab.h"
#include "chat_ui.h"          /* 新增 */

/* 中文字体路径：微软雅黑。
 * 注意这是板子上的绝对路径，不是 Windows 路径。
 * 资源统一放在板子的 /work_space/ 目录下（上板时通过 scp/U 盘拷进去）。 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 卡片尺寸与排布（改成一个地方，算错的概率小一点）
 * 下面 5 张卡片的 x 坐标全部用 TILE_X0 + i*(TILE_W + TILE_GAP) 算出来，
 * 不手写具体数字 —— 这样以后改卡宽只要动这里，布局自动重算。 */
#define TILE_W      176
#define TILE_H      230
#define TILE_GAP    20
#define TILE_X0     32

lv_obj_t * album_screen = NULL;        // 电子相册屏
lv_obj_t * select_app_screen = NULL;   // 桌面屏（全局，供各界面返回复用）

/**
 * 创建指定字号的中文字体（带缓存，避免重复新建造成内存泄漏）。
 *
 * 【为什么需要缓存】
 *   lv_freetype_font_create() 每次调用都会从 TTF 文件重新加载字形数据并
 *   分配内存，代价很高（msyh.ttc 是 10MB 级的字体文件）。
 *   桌面要 3 种字号，如果每次重建桌面都新建一遍，来回切几次桌面就会
 *   吃掉大量内存且永不释放。所以用 4 个静态槽把已建过的字体留着复用：
 *   先线性查找，命中就直接返回，未命中才创建。
 *
 * 【缓存策略】
 *   cache[] 存字体指针，cache_size[] 存对应字号，两个数组下标一一对应。
 *   empty 记录「第一个空槽」——注意是第一个，靠 `empty < 0` 保证只记一次。
 *
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL
 *
 * ★ 已知隐患（本文件与 album.c 都有，chat_ui.c / album_cloud.c 已修）：
 *   循环结束后没有检查 `if(empty < 0) return NULL;`。
 *   当 4 个槽全被占满、又来了第 5 种字号时，empty 仍然是 -1，
 *   下面的 `cache[empty] = f` 就会变成 cache[-1] 越界写。
 *   本文件只用 3 种字号（24/20/14），离 4 个槽还有余量，所以目前不触发。
 *   属于「改动触发型」隐患：以后多调一次 cn_font(新字号) 就可能踩到。
 *   稳妥做法是在 for 循环之后补一行判空。
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[4] = {NULL};
    static int cache_size[4] = {0};
    int empty = -1;

    /* 线性查缓存：命中同字号直接返回；顺便记下第一个空槽位置 */
    for(int i = 0; i < 4; i++) {
        if(cache[i] != NULL && cache_size[i] == size)
            return cache[i];
        if(cache[i] == NULL && empty < 0)
            empty = i;
    }

    /* BITMAP 渲染模式：freetype 把字形栅格化成位图交给 LVGL 绘制。
     * 另一种模式是 VECTOR（运行时矢量绘制），更清晰但更耗 CPU，
     * 本工程屏只有 1024x600、MCU 级性能，用 BITMAP 更稳。 */
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


/* ==========================================================================
 * 五个卡片的点击回调
 *
 * ★ 五个函数的代码结构完全一样，只有「目标屏全局变量」和「创建函数」不同。
 *   这里没有抽成一个带参数的公共函数，是因为每个回调都直接引用自己的
 *   全局屏指针（album_screen / select_game_screen / ...），
 *   在 C 里要做到「引用不同全局变量」只能传指针的地址，反而更难读。
 *   保持展开写更直观 —— 看到 to_chat_cb 就知道它管聊天室。
 *
 * ★ 四步套路（顺序是有原因的，别调换）：
 *
 *   ① if(目标屏 == NULL) 目标屏 = 创建函数();
 *      —— 判空是因为「同一块屏可能已经存在」。本工程里桌面被删后
 *         子屏通常也已被删，所以实际总是重新创建，但判空是必要的防御。
 *
 *   ② lv_screen_load(目标屏);
 *      —— 切到新屏。必须先切，再删旧的，否则中间会出现「没有任何屏」
 *         的瞬间，LVGL 会绘制空白，表现为闪黑。

 *   ③ if(桌面屏 != NULL) { lv_obj_delete(桌面屏); 桌面屏 = NULL; }
 *      —— 删掉自己（桌面）。为什么要删？因为不删的话，用户每次从
 *         子界面回到桌面都会再新建一块桌面屏，旧屏没人引用但内存还在，
 *         来回几十次就会内存耗尽。删掉是「一次性屏」策略的核心。
 *
 *   ④ 置 NULL
 *      —— NULL 在这里是「这块屏现在不存在」的标记。
 *         各模块返回时都会先判 `if(xxx_screen == NULL)` 再决定要不要创建
 *         （见 login.c / main_interface.c 的开头），置 NULL 才能让下次
 *         点击正确走「重新创建」分支。
 *         ★ 不置 NULL 的后果：第二次点击时指针非空但内存已释放，
 *           判空判断失效 → 直接把悬空指针交给 lv_screen_load → 段错误。
 *
 * ★ 返回桌面是反向操作，见各模块里的 to_select_app_screen_cb()：
 *   那边的顺序是「先删定时器 → 建/切桌面 → 最后删自己」。
 *   两条路径都必须遵守同一个原则：删除任何控件前，先确保没有定时器
 *   还在引用它。
 * ========================================================================== */

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

/* 新增：切换到网络聊天室的连接页
 * 注意目标是「连接页（chat_conn_screen）」而不是聊天页 ——
 * 因为进入聊天室必须先填 IP/端口/昵称并连上服务器，
 * 聊天页（chat_room_screen）是在连接成功后才由 conn_btn_cb() 创建的。 */
static void to_chat_cb(lv_event_t *e)
{
    if(chat_conn_screen == NULL)
        chat_conn_screen = ui_chat_conn_init();
    lv_screen_load(chat_conn_screen);
    if(select_app_screen != NULL)
    {
        lv_obj_delete(select_app_screen);
        select_app_screen = NULL;
    }
}


/**
 * 创建单个 App 图标卡片（图标 + App 名 + 描述）。
 *
 * 【卡片内部结构（自下而上三层）】
 *   tile（按钮，176x230，圆角深色底）
 *     ├── img      图标 96x96，贴顶居中、下移 18px
 *     ├── name_lab App 名，居中、下移 140px
 *     └── desc_lab 描述文字，居中、下移 175px
 *
 * 【为什么用 lv_button_create 而不是 lv_obj_create】
 *   lv_button 自带 CLICKABLE、可聚焦等「像按钮」的默认行为，
 *   而且默认样式就是给点击用的。用 lv_obj 做卡片的话要自己加 flag，
 *   漏了就会出现「卡片点了没反应」。
 *
 * @param parent    父对象（本文件传的是全屏 win）
 * @param icon_path 图标 BMP 路径（"A:/work_space/..." 形式）
 * @param name      App 名
 * @param desc      描述文字
 * @param f_name    名字字号
 * @param f_desc    描述字号
 * @param cb        点击回调（LV_EVENT_CLICKED 时触发）
 * @param x         卡片左上角 x 坐标（由调用方按 TILE_X0 + i*(W+GAP) 算出）
 */
static void app_tile_create(lv_obj_t *parent, const char *icon_path,
                            const char *name, const char *desc,
                            lv_font_t *f_name, lv_font_t *f_desc,
                            lv_event_cb_t cb, int x)
{
    /* 卡片背景：深色圆角卡片 + 1px 描边，y 固定 215（在标题栏下方） */
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_pos(tile, x, 215);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_set_style_border_color(tile, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(tile, 16, 0);
    /* 内边距置 0：让下面用 align 定位的子控件坐标不受 padding 影响，
     * 否则图标/文字的实际位置会整体偏移，对不上设计稿。 */
    lv_obj_set_style_pad_all(tile, 0, 0);

    /* 图标（已按 96x96 尺寸生成，直接显示，不需要缩放）
     * 路径前缀 "A:/" 是 LVGL 的虚拟盘符，映射关系在 lv_conf.h 里配：
     *   LV_FS_POSIX_LETTER 'A'  +  LV_FS_POSIX_PATH ""
     * 所以 "A:/work_space/xxx" 实际读取板子的 /work_space/xxx。
     * ★ 注意：只有 LVGL 的 API（lv_image_set_src 等）认这个 A: 前缀；
     *   标准 C 库的 fopen 必须写真实路径 /work_space/xxx（不带 A:）。 */
    lv_obj_t *img = lv_image_create(tile);
    lv_image_set_src(img, icon_path);
    lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 18);

    /* App 名 */
    lv_obj_t *name_lab = lv_label_create(tile);
    lv_label_set_text(name_lab, name);
    lv_obj_set_style_text_font(name_lab, f_name, 0);
    lv_obj_set_style_text_color(name_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_align(name_lab, LV_ALIGN_TOP_MID, 0, 140);

    /* 描述（灰一档，形成层次感） */
    lv_obj_t *desc_lab = lv_label_create(tile);
    lv_label_set_text(desc_lab, desc);
    lv_obj_set_style_text_font(desc_lab, f_desc, 0);
    lv_obj_set_style_text_color(desc_lab, lv_color_hex(0x6A6A7A), 0);
    lv_obj_align(desc_lab, LV_ALIGN_TOP_MID, 0, 175);

    /* 事件注册在 tile 上（不是子控件上）。
     * 点图标/文字时，事件会从子控件冒泡到 tile，所以整张卡都可点。
     * 注意 lv_image 默认不可点击、lv_label 默认也不接收点击，
     * 正因如此冒泡才顺畅 —— 如果子控件吃掉了事件，卡片点击会时灵时不灵。 */
    lv_obj_add_event_cb(tile, cb, LV_EVENT_CLICKED, NULL);
}


/**
 * 桌面界面初始化入口。
 *
 * ★ 副作用提醒：本函数内部已经调用了 lv_screen_load(scr)，
 *   也就是说「创建完就直接切过去了」。
 *   所以调用方（login.c）只需要 `select_app_screen = ui_select_app_screen();`
 *   记录指针即可，不需要再手动切屏。
 *
 * ★ 同时它会把全局 select_app_screen 赋值为自己，
 *   这是「屏的所有权登记」——返回桌面时其它模块靠这个全局变量判断
 *   「桌面屏还在不在，要不要重新创建」。
 *
 * @return 创建的屏幕对象（同时已切换到该屏）
 */
lv_obj_t * ui_select_app_screen(void)
{
    /* 三种字号各取一次；cn_font 内部有缓存，重复调用不会重复加载字体 */
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_name  = cn_font(20);   // App 名字号
    lv_font_t *font_desc  = cn_font(14);   // 描述字号

    /* 创建屏幕和全屏窗口。
     * 分两层的原因：scr 是「屏」（容器，不可见），
     * win 是铺满整屏的「可见面板」，背景图、标题、卡片都挂在 win 上。
     * 这样以后想给整屏加深色蒙层之类，操作 win 一处即可。 */
    lv_obj_t *scr = lv_obj_create(NULL);   // 父对象传 NULL = 创建新的屏
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图（铺满全屏）。
     * 用 bg_image_src 而不是放一个 lv_image 控件，好处是自动平铺/拉伸到
     * 容器大小，不用手工算尺寸。图是 1024x600，正好一屏。 */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_home.bmp", 0);
    /* 去掉 win 的默认描边、圆角和内边距 ——
     * lv_obj_create 默认带 1px 边框和小圆角，不清掉会在屏幕边缘露出深色线。 */
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题「桌面」（左上角） */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "桌面");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 30, 15);

    /* 右上角「玩家小明」状态胶囊。
     * 用 lv_label 加背景 + 圆角 + 内边距来做「胶囊」形状，
     * 比专门建一个容器再塞 label 少一层对象。
     * 圆角半径 20 大于高度的一半，就能得到完全圆头的效果。 */
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

    /* 五个 App 图标卡片（横向排列，x = 32 + i*(176+20)）
     * 顺序 = 用户看到的从左到右的顺序，和下方回调一一对应。
     * 每张卡的参数含义见 app_tile_create 的注释。 */
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_sensor.bmp",
                    "传感器实验室", "姿态 · 照度", font_name, font_desc,
                    to_sensor_lab_cb, TILE_X0 + 0 * (TILE_W + TILE_GAP));
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_game.bmp",
                    "体感游戏", "陀螺仪操控", font_name, font_desc,
                    to_game_screen_cb, TILE_X0 + 1 * (TILE_W + TILE_GAP));
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_album.bmp",
                    "电子相册", "自动播放", font_name, font_desc,
                    to_album_screen_cb, TILE_X0 + 2 * (TILE_W + TILE_GAP));
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_settings.bmp",
                    "设置", "亮度 · 体感", font_name, font_desc,
                    to_settings_cb, TILE_X0 + 3 * (TILE_W + TILE_GAP));
    app_tile_create(win, "A:/work_space/bmp_pic/icon/icon_chat.bmp",
                    "网络聊天室", "局域网互通", font_name, font_desc,
                    to_chat_cb, TILE_X0 + 4 * (TILE_W + TILE_GAP));

    lv_screen_load(scr);       /* 创建完立刻切过去（见函数头部的副作用提醒） */
    select_app_screen = scr;   // 记录全局，供返回时复用
    return scr;
}
