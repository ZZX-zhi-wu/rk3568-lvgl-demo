/* ============================================================================
 * @file    album.c
 * @brief   电子相册界面（动态张数 + 云相册入口 + 删除当前照片）
 *
 * 【它在工程里的位置】
 *
 *   main_interface.c（桌面）→ ui_album_init() → 本文件
 *        本文件 ──► album_cloud.c（云相册弹层，下载图片）
 *              ◄── album_rescan()（下载完回调进来刷新）
 *
 *   这是全局唯一的「本地照片浏览」界面：大图 + 缩略图条 + 自动播放 + 删除。
 *
 * 【照片的存放约定（理解本文件的前提）】
 *   目录：/work_space/bmp_pic/photo/
 *   每张照片由**两个文件**组成，编号相同：
 *     photo_N.bmp   大图 1024x600（约 1.8MB）
 *     thumb_N.bmp   缩略图 140x82
 *   ★ 必须成对存在才算「一张有效照片」。扫描时两个都要存在才收录，
 *     因为大图存在但缩略图缺失时，缩略图条上会出现一个空白格，
 *     点它才能显示 —— 体验很差，不如干脆不显示这张。
 *
 * 【相比 8-28 版改了 5 处（其余逻辑一字未动）】
 *   1. 照片张数由编译期 PHOTO_COUNT 6 改成运行时扫描：
 *      scan_photos() 扫 /work_space/bmp_pic/photo/ 里成对的
 *      photo_N.bmp + thumb_N.bmp，按编号升序；一张都扫不到就回退到原来 6 张。
 *      ——原版写出第 7 张会重叠，就是因为 PHOTO_COUNT 是编译期定死的。
 *   2. 缩略图条改成 flex 横排 + 可横向滚动，不再手算 x 坐标
 *      （原版 62/214/366/518/670/822 写死 6 个位置）。
 *   3. 缩略图现在可点击切换照片（原来只能靠上一张/下一张按钮）。
 *   4. 顶部新增「云相册」入口，点开 album_cloud_open()。
 *   5. 【本次新增】顶部新增「删除」按钮：删掉当前照片 + 配套缩略图。
 *      做成「点两下」确认：第一下按钮变实心红进入待确认，第二下才真删，
 *      3 秒不点自动复位，防止误触把磁盘上的照片删掉。
 *
 * 【顶部一行不重叠的坐标账（都按 font_norm=18 / font_title=24 估）】
 *   返回 20~110 | 标题 130~226 | 提示 240~510 | 删除 610~730 | 云相册 750~870 | 计数 890~1010
 *   改动任何一个元素的宽度/位置时，照这张账重新核对，别让它们叠在一起。
 *
 * 【全局变量说明】
 *   - album_screen：相册屏（定义在 main_interface.c）。
 *     本文件里多处用它判断「相册是否还活着」（如 album_rescan）。
 * ========================================================================== */

#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "album.h"
#include "album_cloud.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 为什么上限是 12：LV_MEM_SIZE 只有 256KB，大图虽然不走 LVGL 内存
 * （BMP 是直接读盘绘制），但每个缩略图控件、每张图的解码缓冲都要占。
 * 12 张是实测下来既够用又不至于把内存吃紧的值。 */
#define PHOTO_MAX 12            /* 相册最多装 12 张（LV_MEM_SIZE 只有 256KB，别贪） */

/* 扫描阶段的暂存上限。为什么比 PHOTO_MAX 大这么多：
 * 策略是「先收齐 → 排序 → 再截断」，而不是边扫边截。
 * 如果边扫边截，留下哪 12 张就取决于 readdir 的返回顺序，
 * 而文件系统的目录顺序是不保证的（甚至同一目录两次结果都可能不同），
 * 会造成「相册内容随重启而变」的诡异现象。
 * 先全收再按编号排序截断，结果就完全确定了。 */
#define SCAN_MAX  128           /* 扫描阶段暂存上限：先收齐再排序再截断，避免丢哪张由 readdir 顺序决定 */

/* POSIX 路径：opendir/stat/remove 等标准接口用这个；LVGL 读图用 A:/ 前缀那个
 * ★ 这是本工程最容易混的一点：
 *   fopen/opendir/remove  → "/work_space/..."（真实路径，不带前缀）
 *   lv_image_set_src      → "A:/work_space/..."（LVGL 虚拟盘符）
 *   两者指向同一个文件，只是接口不同。用错的表现：
 *   标准库带 A: → 找不到文件；LVGL 不带 A: → 图片显示不出来。 */
#define PHOTO_DIR_POSIX "/work_space/bmp_pic/photo"

/* 底部栏（缩略图+按键容器）的两个 y 坐标 */
#define BAR_VISIBLE_Y 440   // 显示时的 y
#define BAR_HIDDEN_Y  610   // 隐藏时的 y（滑到屏幕外）
/* ★ BAR_HIDDEN_Y = 610 比屏高 600 还大 10，
 *   确保隐藏时整个容器完全在屏幕外，一点边都不露。 */

/* 新增 5：删除按钮「待确认」状态的自动取消时间（毫秒）
 * 3 秒的依据：足够让人完成「看清提示 → 确认要删」的思考，
 * 又不至于长到让人以为按钮坏了。 */
#define DEL_CONFIRM_MS 3000

/* 照片与缩略图路径（运行时填充，不再写死）
 * 存的是 LVGL 用的 "A:/..." 形式（给 lv_image_set_src） */
static char photo_path[PHOTO_MAX][96];
static char thumb_path[PHOTO_MAX][96];
static int  photo_num[PHOTO_MAX];            // 新增 5：每张对应的文件编号 N（删文件要用）
static int  photo_count = 0;

static int current_index = 0;                // 当前显示的照片索引
static lv_obj_t *big_image = NULL;           // 大图控件
static lv_obj_t *counter_label = NULL;       // 计数标签（x / 总数）
static lv_obj_t *play_lab = NULL;            // 播放/暂停按钮文字
static lv_obj_t *thumb_objs[PHOTO_MAX];      // 缩略图控件数组
static lv_obj_t *thumb_row = NULL;           // 缩略图横向滚动容器
static lv_obj_t *ctrl_bar = NULL;            // 底部栏容器（缩略图 + 按键）
static lv_timer_t *play_timer = NULL;        // 自动播放定时器
static int is_playing = 0;                   // 是否正在自动播放
static int bar_visible = 1;                  // 底部栏是否可见

/* 新增 5：删除照片相关状态 */
static lv_obj_t *del_btn = NULL;             // 删除按钮
static lv_obj_t *del_btn_lab = NULL;         // 删除按钮文字（「删除」<->「确认删除」）
static lv_obj_t *del_msg = NULL;             // 顶部状态提示行
static lv_timer_t *del_timer = NULL;         // 待确认状态的自动取消定时器
static int del_armed = 0;                    // 1 = 已点第一下，等第二下确认

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * ★★ 已知隐患（P0，改动触发型）★★
 *   本函数**缺少** `if(empty < 0) return NULL;` 这一行保护。
 *   当 4 个缓存槽全部占满、又来请求第 5 种字号时：
 *     循环结束后 empty 仍然是 -1
 *     → 执行 cache[-1] = f，即向数组前一个元素写指针 → 越界写内存
 *   album_cloud.c 里同名的 cn_font() 有这行保护，本文件漏了。
 *
 *   当前为什么没暴露：本文件只用 2 种字号（24 / 18），离 4 个槽还有余量。
 *   什么时候会踩到：以后新增控件时多调一次 cn_font(其它字号)，
 *   累计超过 4 种字号就触发 —— 表现为随机崩溃或字体错乱，
 *   而且很难定位（因为崩溃点离出错点很远）。
 *
 *   一行修复：在 for 循环之后、调用 freetype 之前补上
 *     if(empty < 0) return NULL;
 *
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[4] = {NULL};
    static int cache_size[4] = {0};
    int empty = -1;

    /* 线性查缓存：命中同字号直接返回；顺便记下第一个空槽 */
    for(int i = 0; i < 4; i++) {
        if(cache[i] != NULL && cache_size[i] == size)
            return cache[i];
        if(cache[i] == NULL && empty < 0)
            empty = i;
    }
    /* ★ 这里本该有一行 if(empty < 0) return NULL; —— 见函数头部说明 */

    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f) {
        LV_LOG_ERROR("freetype font create failed: %s", CN_FONT_PATH);
        return NULL;
    }
    cache[empty] = f;                /* ★ 槽满时 empty == -1 → 越界写 */
    cache_size[empty] = size;
    return f;
}

/* 前向声明 */
static void show_photo(int index);

/* ------------------------------------------------------------------ */
/* 新增 1：扫描照片目录                                               */
/* ------------------------------------------------------------------ */

/* 指定编号的缩略图是否存在（必须成对，否则相册里会出现没有缩略图的照片） */
static int thumb_exists(int n)
{
    char p[128];
    FILE *fp;

    snprintf(p, sizeof(p), "%s/thumb_%d.bmp", PHOTO_DIR_POSIX, n);
    fp = fopen(p, "rb");
    if(fp) { fclose(fp); return 1; }
    return 0;
}

/*
 * 扫描照片目录。
 * 只收「photo_N.bmp 存在且 thumb_N.bmp 也存在」的编号，按 N 升序填进路径数组。
 * 一个都扫不到（目录不存在 / 全被删了）就回退到原来那 6 张硬编码路径，
 * 保证相册永远是能看的，不会因为扫描失败变成空白。
 *
 * 【为什么需要「回退到 6 张」这条分支】
 *   如果扫描结果为空就 photo_count = 0，界面会变成全黑（大图没有 src、
 *   缩略图条也是空的），用户看到的就是「相册坏了」。
 *   这是当初验收时的兜底要求：任何情况下相册都要有内容可看。
 *   副作用：这也是「删除时至少留一张」这条规则的原因（见 delete_current_photo）。
 *
 * 【去重为什么必要】
 *   虽然目录里理论上不会有重复编号（文件名唯一），
 *   但万一存在 photo_3.bmp 和 photo_03.bmp，
 *   sscanf 的 %d 会把两者都解析成 3，于是 nums 里出现两个 3，
 *   后面会试图为同一编号建两个缩略图控件（其中一个指向不存在的文件）。
 *   加个查重既省事又稳妥。
 */
static void scan_photos(void)
{
    DIR *d;
    struct dirent *e;
    int nums[SCAN_MAX];          /* 暂存阶段：只收集编号，路径最后统一拼 */
    int n = 0, i, j;

    photo_count = 0;             /* ★ 先清零：扫描中途失败也不能留着上次的计数 */

    d = opendir(PHOTO_DIR_POSIX);
    if(d != NULL) {
        /* 注意 && n < SCAN_MAX 的位置 —— 它保证写入 nums[n++] 不会越过 SCAN_MAX */
        while((e = readdir(d)) != NULL && n < SCAN_MAX) {
            int idx = 0;
            int dup = 0;

            /* 文件名不符合 photo_N.bmp 的（含 . / .. / thumb_* / 备份文件）一律跳过 */
            if(sscanf(e->d_name, "photo_%d.bmp", &idx) != 1) continue;
            if(idx <= 0) continue;                      /* 过滤 photo_0 / 负数 */
            if(!thumb_exists(idx)) continue;            /* ★ 成对校验：缺缩略图就不收 */

            /* 编号查重 */
            for(i = 0; i < n; i++)
                if(nums[i] == idx) { dup = 1; break; }
            if(dup) continue;

            nums[n++] = idx;
        }
        closedir(d);
    }

    /* 升序排序（数量少，冒泡足够）。
     * 这里用最朴素的选择：最多 12~128 个元素，冒泡的 O(n²) 完全够快，
     * 而且代码短、没有引入 qsort 的比较函数。 */
    for(i = 0; i < n - 1; i++)
        for(j = 0; j < n - 1 - i; j++)
            if(nums[j] > nums[j + 1]) {
                int t = nums[j]; nums[j] = nums[j + 1]; nums[j + 1] = t;
            }

    /* 排完序再截断：保证留下的一定是编号最小的那几张（= 最早放进来的），
     * 不依赖 readdir 的返回顺序（文件系统不同顺序会不一样）。 */
    if(n > PHOTO_MAX) {
        printf("[album] 目录里有 %d 张，超过上限 %d，只取编号最小的 %d 张\n",
               n, PHOTO_MAX, PHOTO_MAX);
        n = PHOTO_MAX;
    }

    if(n > 0) {
        for(i = 0; i < n; i++) {
            photo_num[i] = nums[i];      /* 新增 5：记住编号，删除时按编号定位文件 */
            /* ★ 这里拼的是 LVGL 用的 "A:/" 路径（供 lv_image_set_src）。
             *   删除时要用 POSIX 路径，所以那边重新拼一次，不共用这里。 */
            snprintf(photo_path[i], sizeof(photo_path[i]),
                     "A:/work_space/bmp_pic/photo/photo_%d.bmp", nums[i]);
            snprintf(thumb_path[i], sizeof(thumb_path[i]),
                     "A:/work_space/bmp_pic/photo/thumb_%d.bmp", nums[i]);
        }
        photo_count = n;
        printf("[album] 扫描到 %d 张照片\n", n);
    }
    else {
        /* 兜底：回退到固定 1~6 号（假设它们一定存在） */
        for(i = 0; i < 6; i++) {
            photo_num[i] = i + 1;        /* 新增 5：回退分支同样要填编号 */
            snprintf(photo_path[i], sizeof(photo_path[i]),
                     "A:/work_space/bmp_pic/photo/photo_%d.bmp", i + 1);
            snprintf(thumb_path[i], sizeof(thumb_path[i]),
                     "A:/work_space/bmp_pic/photo/thumb_%d.bmp", i + 1);
        }
        photo_count = 6;
        printf("[album] 目录扫描失败（或为空），回退到默认 6 张\n");
    }
}

/* ------------------------------------------------------------------ */
/* 显示与切换                                                         */
/* ------------------------------------------------------------------ */

/**
 * 点缩略图切换照片。
 * ★ user_data 传的是「下标」，用 (void *)(long)i 强转 ——
 *   为什么绕一层 long 而不是直接 (void *)i：
 *   int 是 32 位、指针在 64 位平台上也是 32 位？不，指针是 64 位。
 *   直接把 int 当指针传会触发 -Wint-to-pointer-cast 告警，
 *   先转 long（64 位）再转指针就干净了，取回时反过来。
 *   ★ 不要传「下标变量的地址」—— 那种指针指向局部变量，会悬空。
 */
static void thumb_click_cb(lv_event_t *e)
{
    int idx = (int)(long)lv_event_get_user_data(e);

    if(idx < 0 || idx >= photo_count) return;    /* 防越界（缩略图条可能刚被重建） */
    current_index = idx;
    show_photo(current_index);
}

/**
 * 显示第 index 张照片，并更新计数标签和缩略图高亮。
 *
 * 【一次调用做四件事】
 *   ① 换大图          lv_image_set_src
 *   ② 更新计数标签    "3 / 7"
 *   ③ 高亮当前缩略图  绿色 3px 边框；其余恢复灰色 2px
 *   ④ 把当前缩略图滚进可视区（照片多时才有必要）
 *
 * 【为什么入参要 clamp 而不是直接判非法返回】
 *   本函数被多处调用（点缩略图、上一张/下一张、自动播放、album_rescan），
 *   调用方传进来的 index 有时是「算出来的」（比如删掉最后一张后
 *   索引会临时越界）。与其在每个调用点做检查，
 *   不如在本函数入口统一收拢到合法区间，这样所有调用点都安全。
 *
 * @param index 目标索引（会被自动钳到 [0, photo_count-1]）
 */
static void show_photo(int index)
{
    if(photo_count <= 0) return;         /* 一张都没有：什么都不做，避免索引 -1 */
    if(index < 0) index = 0;
    if(index >= photo_count) index = photo_count - 1;

    lv_image_set_src(big_image, photo_path[index]);
    /* 注意用 set_text_fmt 是安全的：%d 不涉及浮点，
     * 在 LV_USE_FLOAT=0 的配置下 %d / %s 照常可用，
     * 只有 %f 会被禁掉。 */
    lv_label_set_text_fmt(counter_label, "%d / %d", index + 1, photo_count);

    for(int i = 0; i < photo_count; i++) {
        if(thumb_objs[i] == NULL) continue;     /* 保险：重建期间可能为 NULL */
        if(i == index) {
            // 当前照片的缩略图加粗绿色边框
            lv_obj_set_style_border_width(thumb_objs[i], 3, 0);
            lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2FD3A0), 0);
        } else {
            lv_obj_set_style_border_width(thumb_objs[i], 2, 0);
            lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2B2B3A), 0);
        }
    }

    /* 新增：让当前缩略图自己滚进可视区（照片多时才看得出效果）。
     * 用 LV_ANIM_ON 让滚动有过渡，比瞬移更符合「相册」的观感。 */
    if(thumb_row != NULL && thumb_objs[index] != NULL)
        lv_obj_scroll_to_view(thumb_objs[index], LV_ANIM_ON);
}

/**
 * 上一张按钮回调。
 * ★ `(current_index - 1 + photo_count) % photo_count` 里的 + photo_count
 *   不能省：C 语言的取模对负数返回负值，
 *   当 current_index == 0 时 0-1 = -1，-1 % 7 在 C 里是 -1 而不是 6，
 *   直接用它当索引就越界了。加上一个 photo_count 把它抬成正数即可。
 */
static void prev_cb(lv_event_t *e)
{
    if(photo_count <= 0) return;
    current_index = (current_index - 1 + photo_count) % photo_count;
    show_photo(current_index);
}

/* 下一张按钮回调（这里 current_index+1 不会为负，所以不需要 +photo_count） */
static void next_cb(lv_event_t *e)
{
    if(photo_count <= 0) return;
    current_index = (current_index + 1) % photo_count;
    show_photo(current_index);
}

/* 自动播放定时器回调：每 3 秒切下一张（取模实现循环播放） */
static void play_timer_cb(lv_timer_t *t)
{
    if(photo_count <= 0) return;
    current_index = (current_index + 1) % photo_count;
    show_photo(current_index);
}

/**
 * 播放/暂停按钮回调：切换定时器暂停/恢复，并更新按钮文字。
 *
 * 【为什么是 pause/resume 而不是 delete/create】
 *   用 lv_timer_pause/resume 保留定时器对象，切换时不用重新分配内存，
 *   也不需要重置计时 —— 恢复播放时延续之前的相位，观感更连续。
 *   但首次点「播放」时定时器还不存在，所以要 NULL 判断后创建。
 *
 * 【为什么定时器是懒创建的】
 *   进相册默认不自动播放（避免一进来照片就自己翻），
 *   所以进界面时 play_timer 是 NULL，只有用户主动点了才建。
 *   好处是省一个定时器的开销，也避免用户没打算播放却在后台翻页。
 */
static void play_pause_cb(lv_event_t *e)
{
    if(is_playing) {
        lv_timer_pause(play_timer);
        is_playing = 0;
        lv_label_set_text(play_lab, "播放");
    } else {
        if(play_timer == NULL)
            play_timer = lv_timer_create(play_timer_cb, 3000, NULL);
        lv_timer_resume(play_timer);
        is_playing = 1;
        lv_label_set_text(play_lab, "暂停");
    }
}

/* 升降动画的执行回调：每帧把底部栏的 y 设为 v。
 * 动画系统只负责算 v（从 from 插值到 to），
 * 具体「怎么把 v 应用到对象上」由这个回调决定 —— 这里就是设 y 坐标。 */
static void anim_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

/**
 * 切换底部栏升降：显示<->隐藏之间做 250ms 缓动动画。
 *
 * 【为什么要 lv_anim_delete(ctrl_bar, anim_y_cb)】
 *   用户可能连续快速点击（显示→隐藏→显示…），
 *   如果不取消上一个动画，两个动画会同时往 ctrl_bar 上写 y，
 *   表现为抖动、或者停在中间某个奇怪的位置。
 *   按 (目标对象, 执行回调) 取消掉同目标的旧动画，保证同一时刻只有一个在跑。
 *
 * 【ease_out 的选择】
 *   先快后慢，停下时有个自然的减速感。
 *   如果用线性会显得机械，用 ease_in 则起步太慢、像卡住。
 */
static void toggle_bar(void)
{
    int32_t from = bar_visible ? BAR_VISIBLE_Y : BAR_HIDDEN_Y;
    int32_t to   = bar_visible ? BAR_HIDDEN_Y : BAR_VISIBLE_Y;
    bar_visible = !bar_visible;             /* ★ 先翻转状态：动画是异步的，
                                             *   靠 bar_visible 记录目标状态，
                                             *   动画完成后不需要再改它 */

    lv_anim_delete(ctrl_bar, anim_y_cb);   // 先取消上一次动画，避免冲突
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctrl_bar);          /* 动画作用对象 */
    lv_anim_set_exec_cb(&a, anim_y_cb);     /* 每帧怎么应用 */
    lv_anim_set_values(&a, from, to);       /* 起止值 */
    lv_anim_set_time(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* 点击大图 -> 升降底部栏（沉浸看照片时把底部栏收起来） */
static void photo_click_cb(lv_event_t *e)
{
    toggle_bar();
}

/* 新增 4：点「云相册」打开远程文件弹层。
 * ★ 父对象用 lv_screen_active()（当前活动屏 = 相册屏）而不是 win，
 *   这样弹层挂在「屏」上，不受 win 的裁剪和布局影响，
 *   尺寸按全屏 1024x600 算才是对的。 */
static void cloud_btn_cb(lv_event_t *e)
{
    (void)e;
    album_cloud_open(lv_screen_active());
}

/* ------------------------------------------------------------------ */
/* 新增 5：删除当前照片（photo_N.bmp + 配套 thumb_N.bmp）              */
/* ------------------------------------------------------------------ */

/* 顶部提示行 */
static void del_msg_set(const char *text, uint32_t color)
{
    if(del_msg == NULL) return;
    lv_label_set_text(del_msg, text);
    lv_obj_set_style_text_color(del_msg, lv_color_hex(color), 0);
}

/** 退出「待确认」状态：按钮外观恢复成普通「删除」（红色描边 + 深色底） */
static void del_disarm(void)
{
    del_armed = 0;
    if(del_btn_lab != NULL) {
        lv_label_set_text(del_btn_lab, "删除");
        lv_obj_set_style_text_color(del_btn_lab, lv_color_hex(0xE24B4A), 0);
    }
    if(del_btn != NULL) {
        /* 恢复成「深底 + 红边」的普通态（与 del_btn_cb 里的实心红相反） */
        lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x111119), 0);
        lv_obj_set_style_bg_opa(del_btn, LV_OPA_90, 0);
    }
}

/**
 * 到点自动取消（防止点了一下就没再管，按钮一直红着）。
 *
 * 【为什么第一件事是 lv_timer_pause(t) 而不是 delete】
 *   这个定时器是复用的（见 del_msg_flash）：第一次用时创建，
 *   之后靠 reset + resume 重复使用。
 *   所以超时后不能 delete，而是 pause 让它停住 ——
 *   下次 del_msg_flash 会 reset 它重新计时再 resume。
 *   如果这里 delete 了，del_timer 就成了悬空指针，
 *   而 del_msg_flash 里的 `if(del_timer == NULL)` 判断会失效，
 *   去 reset 一个已释放的定时器 → 崩溃。
 *   ★ 这是「复用定时器」模式必须遵守的配对关系：
 *     create 一次，之后只有 pause/resume/reset，delete 只在退出界面时做一次。
 */
static void del_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(t);
    del_disarm();
    del_msg_set("", 0x9A9AAC);       /* 清空提示（恢复成暗淡的中性色） */
}

/**
 * 显示提示 + 保证 3 秒后自动清掉。
 *
 * 【三种调用场景共用这一个函数】
 *   ① 进入待确认（"再点一次确认删除，3 秒后取消"）
 *   ② 删除成功（"已删除 photo_N，还剩 M 张"）
 *   ③ 删除失败（"至少保留一张照片" / "删除失败：..."）
 *   都要「显示 3 秒后自动消失」，所以统一走这里，用同一个定时器。
 *
 * 【reset 与 resume 的分工】
 *   lv_timer_reset  把计时归零（重新数 3 秒）—— 用于「已经在跑但想延长」
 *   lv_timer_resume 从暂停态恢复运行 —— 因为超时后是 pause 状态
 *   两个都需要：一个管计时起点，一个管运行状态。
 */
static void del_msg_flash(const char *text, uint32_t color)
{
    del_msg_set(text, color);

    if(del_timer == NULL)
        del_timer = lv_timer_create(del_timer_cb, DEL_CONFIRM_MS, NULL);
    else {
        lv_timer_reset(del_timer);
        lv_timer_resume(del_timer);
    }
}

/*
 * 真正执行删除：按当前索引取出编号 N，删掉
 * photo_N.bmp 和配套的 thumb_N.bmp（POSIX 路径，不能带 A:/ 前缀）。
 *
 * 【为什么「至少留一张」】
 *   如果允许删到 0 张，scan_photos() 会发现目录为空，
 *   然后走那条兜底分支，把 photo_count 设成 6 并指向 1~6 号文件 ——
 *   而那些文件其实已经被删了。结果是相册里出现 6 张打不开的空白照片，
 *   看起来像程序坏了。所以卡在「至少 1 张」这个源头。
 *
 * 【删除顺序：先大图，后缩略图】
 *   大图删失败就直接返回（照片还在，状态一致）。
 *   大图删成功后缩略图删失败**不算致命** —— 照片本体已经没了，
 *   剩一个孤立的 thumb_N.bmp 不影响相册显示
 *   （扫描时要求成对，只有缩略图没有大图，这张不会被收录）。
 *   所以这里故意不检查 remove(small) 的返回值。
 *
 * 【删完为什么要调 album_rescan()】
 *   不重扫的话，内存里的 photo_path[] / photo_count 还是旧的，
 *   大图会继续显示一张已经从磁盘上删掉的文件（LVGL 读不到就是空白）。
 *   重扫 + 重建 + 刷新一步到位（见 album_rescan 的注释）。
 */
static void delete_current_photo(void)
{
    int num, idx;
    char big[192];
    char small[192];
    char tip[80];

    /* 至少留一张：删光会让 scan_photos() 回退到 6 张硬编码路径，相册变全黑 */
    if(photo_count <= 1) {
        del_msg_flash("至少保留一张照片", 0xE24B4A);
        return;
    }

    idx = current_index;
    if(idx < 0 || idx >= photo_count) {
        del_msg_flash("删除失败：索引异常", 0xE24B4A);
        return;
    }
    num = photo_num[idx];        /* ★ 用「文件编号」而不是「数组下标」定位文件 ——
                                  *   编号和下标在删除后就不一致了，
                                  *   按下标删会删错文件 */

    /* ★ 这里必须是 POSIX 真实路径：remove() 是标准库函数，
     *   不认 LVGL 的 "A:/" 前缀。 */
    snprintf(big,   sizeof(big),   "%s/photo_%d.bmp", PHOTO_DIR_POSIX, num);
    snprintf(small, sizeof(small), "%s/thumb_%d.bmp", PHOTO_DIR_POSIX, num);

    if(remove(big) != 0) {
        del_msg_flash("删除失败：文件删不掉", 0xE24B4A);
        return;
    }
    remove(small);          /* 缩略图删不掉不算致命，照片本体已经没了 */

    /*
     * 重扫目录 + 重建缩略图条 + 刷新大图。
     * album_rescan() 内部会把越界的 current_index 收回来，正好实现：
     *   删中间某张 -> 索引不动，显示顶上来的那张
     *   删最后一张 -> 索引越界，自动回到新的最后一张
     * 另外它内部还调了 show_photo()，所以计数标签和缩略图高亮会一起更新。
     */
    album_rescan();

    /* ★ 注意顺序：这一行必须在 album_rescan() 之后 ——
     *   因为提示里要报「还剩 %d 张」，而 photo_count 是 rescan 里才更新的。
     *   写到 rescan 前面就会报错数字（少算 1）。 */
    snprintf(tip, sizeof(tip), "已删除 photo_%d，还剩 %d 张", num, photo_count);
    del_msg_flash(tip, 0x2FD3A0);
}

/**
 * 删除按钮回调：第一下进入待确认，第二下才真删。
 *
 * 【为什么要做两点确认】
 *   这是不可逆操作（直接 remove 磁盘文件，没有回收站）。
 *   误触一下就把照片删掉了，代价太大。
 *   两点确认的成本很低（多点一下），但能挡住绝大多数误触。
 *
 * 【「待确认」状态的视觉表达（必须足够醒目）】
 *   文字：删除 → 确认删除
 *   文字色：红字 → 白字
 *   背景：深底+红边 → 实心红（LV_OPA_COVER）
 *   三处一起变，确保用户一眼看出「按钮现在处于不同状态」，
 *   而不是以为第一次没点中又点一下 —— 那就真删了。
 *
 * 【auto-disarm 的兜底】
 *   进入待确认时调 del_msg_flash，它保证 3 秒后 del_timer_cb
 *   会把状态复位。所以用户点了一下就走开，按钮也不会一直红着。
 */
static void del_btn_cb(lv_event_t *e)
{
    (void)e;

    if(!del_armed) {
        /* ---- 第一下：进入待确认 ---- */
        del_armed = 1;
        if(del_btn_lab != NULL) {
            lv_label_set_text(del_btn_lab, "确认删除");
            lv_obj_set_style_text_color(del_btn_lab, lv_color_hex(0xFFFFFF), 0);
        }
        if(del_btn != NULL) {
            lv_obj_set_style_bg_color(del_btn, lv_color_hex(0xE24B4A), 0);
            lv_obj_set_style_bg_opa(del_btn, LV_OPA_COVER, 0);
        }
        del_msg_flash("再点一次确认删除，3 秒后取消", 0xE24B4A);
        return;
    }

    /* ---- 第二下：确认删除 ---- */
    /* 先暂停自动取消定时器：接下来要执行删除 + 显示结果，
     * 如果不暂停，3 秒的计时器可能中途触发把状态清掉，
     * 导致「已删除」的提示被空字符串覆盖（用户看不到操作结果）。 */
    if(del_timer != NULL)
        lv_timer_pause(del_timer);
    del_disarm();                       /* 按钮外观先复位，避免删除耗时中按钮还是红的 */
    delete_current_photo();             /* 里面会再 flash 一条结果提示 */
}

/**
 * 返回按钮回调：先停掉自动播放定时器（避免悬空），再切回桌面。
 *
 * ★★★ 本函数是全工程「清理顺序铁律」的典型样本，顺序绝对不能调换 ★★★
 *
 *   ① 关掉云相册弹层（如果开着）
 *      —— 弹层内部有自己的轮询定时器，必须让它先把自己收干净。
 *         如果直接删相册屏，弹层的定时器还在跑，下一次触发就会
 *         访问已被释放的控件 → 花屏 / 崩溃。
 *
 *   ② lv_timer_delete(play_timer) 并置 NULL
 *      —— 自动播放定时器的回调会调 show_photo() → 访问 big_image /
 *         counter_label / thumb_objs[]。这些控件马上就要被删掉，
 *         所以定时器必须死在这些控件之前。
 *         ★ 这是「定时器必须比它盯着的控件先销毁」铁律的核心原因：
 *           定时器是异步触发的，你无法预知它在哪一刻会去访问控件。
 *
 *   ③ lv_timer_delete(del_timer) 并置 NULL
 *      —— 同上，删除确认定时器也会访问 del_btn_lab 等控件。
 *         虽然它此刻可能是 pause 状态（不会触发），
 *         但 delete 掉更彻底 —— 而且下次进相册时 ui_album_init 会
 *         把 del_timer 置 NULL 让 del_msg_flash 重新创建，
 *         所以这里 delete 不会造成悬空。
 *
 *   ④ 切到桌面屏（不存在则创建）
 *      —— 先切后删，避免「没有任何屏」的瞬间闪黑。
 *
 *   ⑤ lv_obj_delete(album_screen)
 *      —— 最后才删自己。删完置 NULL，让下次点桌面上的相册卡片时
 *         to_album_screen_cb 里 `if(album_screen == NULL)` 判断生效，
 *         重新创建一块干净的相册屏。
 *
 * ★ 注意本函数**没有**顺手清掉 big_image / thumb_objs[] 这些控件指针。
 *   它们此刻全是悬空指针，靠的是 album_rescan() 里的
 *   `if(album_screen == NULL) return;` 挡住「在屏已删的情况下仍被调用」。
 *   这是个脆弱的设计 —— 详见 album_rescan 的注释。
 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    /* 新增：云相册弹层开着的话必须先收掉。
     * 它内部有定时器，屏删了定时器还在跑就会去访问已释放的控件。 */
    if(album_cloud_is_open())
        album_cloud_close();

    if(play_timer != NULL) {
        lv_timer_delete(play_timer);        /* ② 定时器先死 */
        play_timer = NULL;
    }
    is_playing = 0;

    /* 新增 5：删除确认定时器也要收掉，否则屏删了它还在跑 */
    if(del_timer != NULL) {
        lv_timer_delete(del_timer);         /* ③ 同上 */
        del_timer = NULL;
    }
    del_armed = 0;

    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();    /* ④ 先建/切目标屏 */
    lv_screen_load(select_app_screen);
    if(album_screen != NULL) {
        lv_obj_delete(album_screen);                   /* ⑤ 最后删自己 */
        album_screen = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* 新增 2：缩略图条重建                                               */
/* ------------------------------------------------------------------ */

/**
 * 按当前 photo_count 重建缩略图条（flex 横排，自动排布）。
 *
 * 【为什么重建而不做增量更新】
 *   删除/下载都会改变「照片集合」，增删单个缩略图要处理插入位置、
 *   flex 重排、下标变动 —— 复杂度高且容易和 photo_path[] 数组不同步。
 *   整个清掉重建最简单可靠，而且 12 个控件的创建开销可以忽略。
 *
 * 【两个循环的顺序不能反】
 *   先 lv_obj_clean() 把所有旧缩略图控件删掉，再把 thumb_objs[] 全部置 NULL，
 *   最后才按 photo_count 建新的。
 *   ★ 如果把「置 NULL」漏了：photo_count 变小（删了照片）时，
 *     新建的循环只覆盖前 photo_count 个元素，
 *     后面那些 thumb_objs[i] 还是指向**已删除控件**的悬空指针，
 *     而 show_photo() 里的高亮循环是按 i < photo_count 走的 ——
 *     这次不会碰到它们，但下次 photo_count 又变大时就会去改已释放的控件。
 *   这个「先清空指针数组」的动作是整套重建逻辑的安全前提。
 *
 * 【缩略图必须显式加 CLICKABLE】
 *   lv_image 控件默认**不可点击**（它被当作纯展示元素，不吞触摸事件）。
 *   不加这个 flag 的话，点缩略图完全没反应 ——
 *   这是本工程「lv_obj 默认行为」相关坑里的一个反向案例：
 *   大多数容器是「默认可点、做展示要去掉」，
 *   而 lv_image 恰恰相反，是「默认不可点、要点击得加上」。
 */
static void thumb_bar_rebuild(void)
{
    if(thumb_row == NULL) return;

    lv_obj_clean(thumb_row);        /* 删掉所有旧缩略图控件 */

    /* 指针数组整体清空（见函数头部说明：这是安全前提） */
    for(int i = 0; i < PHOTO_MAX; i++)
        thumb_objs[i] = NULL;

    for(int i = 0; i < photo_count; i++) {
        thumb_objs[i] = lv_image_create(thumb_row);
        lv_image_set_src(thumb_objs[i], thumb_path[i]);
        lv_obj_set_style_border_width(thumb_objs[i], 2, 0);
        lv_obj_set_style_border_color(thumb_objs[i], lv_color_hex(0x2B2B3A), 0);
        lv_obj_set_style_radius(thumb_objs[i], 6, 0);
        /* 缩略图要能点（lv_image 默认不可点） */
        lv_obj_add_flag(thumb_objs[i], LV_OBJ_FLAG_CLICKABLE);
        /* user_data 传下标 i，转 long 再转指针避免告警（见 thumb_click_cb） */
        lv_obj_add_event_cb(thumb_objs[i], thumb_click_cb,
                            LV_EVENT_CLICKED, (void *)(long)i);
    }
}

/*
 * 新增 3：外部调用入口 —— 重扫目录 + 重建缩略图条 + 跳到新照片。
 * 由 album_cloud.c 下载完成后、以及本文件的删除操作调用。
 *
 * 【为什么需要这个入口】
 *   云相册下载完图片后，相册屏（本模块）的数据是旧的。
 *   让 album_cloud.c 直接去改本文件的 static 数组是不行的（跨模块访问私有状态）。
 *   所以提供一个单一入口，由下载方调用，本模块自己完成「重扫 → 重建 → 刷新」。
 *   这是模块间解耦的标准做法：调用方只表达「环境变了，你更新一下」，
 *   具体怎么更新是模块内部的事。
 *
 * ★★ 这个 NULL 判断是唯一的防线，不要删 ★★
 *   场景：用户点了下载 → 立刻按返回键回桌面 → 下载线程继续跑完 →
 *         下载完成回调 album_rescan()。
 *   此时相册屏已经被 to_select_app_screen_cb() 删掉了，
 *   而 big_image / thumb_row / counter_label 这些静态指针
 *   在那个回调里**并没有被置 NULL**（只清了定时器和 album_screen）。
 *   所以如果少了这行判断，上面几个悬空指针会被直接送进
 *   lv_image_set_src / lv_obj_clean → 崩溃。
 *
 *   ★ 隐患（值得修）：目前全靠这一行挡着。
 *     更稳妥的做法是在 to_select_app_screen_cb() 里把
 *     big_image / thumb_row / counter_label / thumb_objs[] / play_lab
 *     也一并置 NULL，让本函数的判断可以升级成
 *     `if(thumb_row == NULL) return;` 这样的多重防线。
 *     现在这种「只有一道防线」的设计，
 *     以后如果新增了别的「不检查 album_screen 就调用」的入口，就会踩坑。
 */
void album_rescan(void)
{
    int old_count;

    /* 相册没打开就什么都不做（下载完时用户可能已经切走了） */
    if(album_screen == NULL)
        return;

    /* ★ 必须在 scan_photos() 之前存旧张数 ——
     *   scan_photos() 会改 photo_count，之后就取不到旧值了。 */
    old_count = photo_count;

    scan_photos();
    thumb_bar_rebuild();

    /* 有新照片就跳到第一张新增的，否则保持当前位置。
     *
     * 三个分支的完整性（每种「张数变化」都有对应处理）：
     *   张数变多（下载了）         → current_index = old_count（第一张新的）
     *   张数变少（删除了）且索引越界 → 钳到新的最后一张
     *   张数不变 / 索引仍有效       → 两个分支都不命中，保持不动
     *   极端：一张都没有            → current_index < 0 时回到 0
     *
     * 「跳到第一张新增的」的推导：
     *   新照片编号最大，排序后必然在数组末尾。
     *   原来有 old_count 张（下标 0..old_count-1），
     *   所以下标 old_count 正好是第一张新照片（0-based）。
     *   ★ 很容易写成 old_count - 1，那样会显示「旧的最后一张」。 */
    if(photo_count > old_count)
        current_index = old_count;
    else if(current_index >= photo_count)
        current_index = photo_count - 1;
    if(current_index < 0)
        current_index = 0;

    /* 一次调用完成「换大图 + 更新计数 + 缩略图高亮 + 滚动」 */
    show_photo(current_index);
}

/* ------------------------------------------------------------------ */
/* 界面入口                                                           */
/* ------------------------------------------------------------------ */

/*
 * 电子相册界面初始化入口。
 * @return 创建的屏幕对象
 *
 * ★ 副作用提醒：本函数末尾会调用 lv_screen_load(scr)，也就是
 *   「创建完就直接切过去了」（和 ui_select_app_screen 的行为一致）。
 *   所以调用方（main_interface.c 的 to_album_screen_cb）只需
 *   `album_screen = ui_album_init();` 记录指针，
 *   它后面那行 lv_screen_load(album_screen) 是重复调用，无副作用。
 *
 * ★ 本函数内部所有 static 状态都在开头重新初始化了一遍，
 *   这让「每次进相册」都是干净的初始状态，不依赖返回时的清理。
 *   这是有意为之的防御：返回时的清理（to_select_app_screen_cb）
 *   只清定时器和 album_screen，控件指针都留着，
 *   所以把状态复位放在入口处比放在出口处更可靠。
 */
lv_obj_t * ui_album_init(void)
{
    /* 两种字号：标题 24、正文 18。cn_font 内部有缓存。 */
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(18);   // 正文字号

    /* 每次进入重置状态。
     * ★ 这几个 static 变量在返回桌面时**没有**被清（to_select_app_screen_cb
     *   只清了定时器和 album_screen），所以必须在这里重新初始化，
     *   否则会出现「上次正在播放 → 这次进来照片自己翻」的怪现象。
     *   play_timer 置 NULL 尤其重要：上次的定时器对象已经被 delete 了，
     *   不置 NULL 会让 play_pause_cb 里的 `if(play_timer == NULL)` 判断失效，
     *   去 resume 一个已释放的定时器 → 崩溃。 */
    current_index = 0;
    is_playing = 0;
    play_timer = NULL;
    bar_visible = 1;
    del_armed = 0;                         /* 新增 5 */
    del_timer = NULL;                      /* 新增 5：同上，理由一致 */

    /* 先扫目录，确定这次有几张照片、都是哪些 */
    scan_photos();

    /* 创建屏幕和全屏窗口（和桌面同样的两层结构：scr 是屏，win 是可见面板） */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    /* 清掉 win 的默认描边/圆角/内边距，否则屏幕边缘会露出深色线 */
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 大图（全屏，可点击用于升降底部栏）。
     * 大图铺满整屏 1024x600，正好是照片尺寸，所以不需要缩放。 */
    big_image = lv_image_create(win);
    if(photo_count > 0)
        lv_image_set_src(big_image, photo_path[0]);   /* 有照片就先显示第一张 */
    /* ★ 必须显式加 CLICKABLE：lv_image 默认不可点击，
     *   不加就没法通过点大图来收起/展开底部栏。 */
    lv_obj_add_flag(big_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(big_image, photo_click_cb, LV_EVENT_CLICKED, NULL);

    /* 标题栏：返回按钮（左上，90x40 @ (20,12)） */
    lv_obj_t *back_btn = lv_button_create(win);
    lv_obj_set_size(back_btn, 90, 40);
    lv_obj_set_pos(back_btn, 20, 12);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(back_btn, 1, 0);
    lv_obj_set_style_border_color(back_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);
    lv_obj_t *back_lab = lv_label_create(back_btn);
    lv_label_set_text(back_lab, "返回");
    lv_obj_set_style_text_font(back_lab, font_norm, 0);
    lv_obj_set_style_text_color(back_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(back_lab);
    lv_obj_add_event_cb(back_btn, to_select_app_screen_cb, LV_EVENT_CLICKED, NULL);

    /* 标题（x=130 紧接返回按钮后面） */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "电子相册");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 新增 5：状态提示行，放在标题右边、删除按钮左边的空档里。
     * 初始为空字符串（不留占位文字），删除相关操作才往里写内容。 */
    del_msg = lv_label_create(win);
    lv_label_set_text(del_msg, "");
    lv_obj_set_style_text_font(del_msg, font_norm, 0);
    lv_obj_set_style_text_color(del_msg, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(del_msg, 240, 22);

    /* 新增 5：删除当前照片（红色调；第一下变实心红进入待确认，第二下才真删） */
    del_btn = lv_button_create(win);
    lv_obj_set_size(del_btn, 120, 40);
    lv_obj_set_pos(del_btn, 610, 12);
    lv_obj_set_style_bg_color(del_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(del_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(del_btn, 1, 0);
    lv_obj_set_style_border_color(del_btn, lv_color_hex(0xE24B4A), 0);  /* 红边表示危险操作 */
    lv_obj_set_style_radius(del_btn, 10, 0);
    del_btn_lab = lv_label_create(del_btn);
    lv_label_set_text(del_btn_lab, "删除");
    lv_obj_set_style_text_font(del_btn_lab, font_norm, 0);
    lv_obj_set_style_text_color(del_btn_lab, lv_color_hex(0xE24B4A), 0);
    lv_obj_center(del_btn_lab);
    lv_obj_add_event_cb(del_btn, del_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 新增：云相册入口（在计数标签左边，避免重叠）。
     * 绿色实心按钮，和删除按钮的红色调形成「安全/危险」的语义对比。 */
    lv_obj_t *cloud_btn = lv_button_create(win);
    lv_obj_set_size(cloud_btn, 120, 40);
    lv_obj_set_pos(cloud_btn, 750, 12);
    lv_obj_set_style_bg_color(cloud_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_border_width(cloud_btn, 1, 0);
    lv_obj_set_style_border_color(cloud_btn, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_style_radius(cloud_btn, 10, 0);
    lv_obj_t *cloud_lab = lv_label_create(cloud_btn);
    lv_label_set_text(cloud_lab, "云相册");
    lv_obj_set_style_text_font(cloud_lab, font_norm, 0);
    lv_obj_set_style_text_color(cloud_lab, lv_color_hex(0x06130E), 0);   /* 深绿字配亮绿底 */
    lv_obj_center(cloud_lab);
    lv_obj_add_event_cb(cloud_btn, cloud_btn_cb, LV_EVENT_CLICKED, NULL);

    /* 计数标签（右上角，"当前/总数"）。
     * 用 label + 背景 + 圆角做胶囊；圆角 20 大于高度一半 → 完全圆头。 */
    counter_label = lv_label_create(win);
    lv_label_set_text_fmt(counter_label, "1 / %d", photo_count);
    lv_obj_set_style_text_font(counter_label, font_norm, 0);
    lv_obj_set_style_text_color(counter_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(counter_label, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(counter_label, LV_OPA_90, 0);
    lv_obj_set_style_radius(counter_label, 20, 0);
    lv_obj_set_style_pad_all(counter_label, 10, 0);
    lv_obj_set_pos(counter_label, 890, 12);

    /* 底部栏容器（缩略图 + 按键），整体随点击升降。
     * 高度 160：上面 90 给缩略图条，下面留给三个按键（y=105 起，高 44）。
     * 位置初始在 BAR_VISIBLE_Y=440，即 440~600 正好占屏幕最下面一段。 */
    ctrl_bar = lv_obj_create(win);
    lv_obj_set_size(ctrl_bar, 1024, 160);
    lv_obj_set_pos(ctrl_bar, 0, BAR_VISIBLE_Y);
    lv_obj_set_style_bg_opa(ctrl_bar, LV_OPA_TRANSP, 0);   // 透明，不挡照片
    lv_obj_set_style_border_width(ctrl_bar, 0, 0);
    lv_obj_set_style_pad_all(ctrl_bar, 0, 0);

    /*
     * 新增 2：缩略图条改成 flex 横排 + 横向滚动。
     * 原版是 6 个写死的 x 坐标（62/214/366/518/670/822），
     * 第 7 张就会和第一张叠在一起；现在照片多少张都能排开、能滑动。
     */
    thumb_row = lv_obj_create(ctrl_bar);
    lv_obj_set_size(thumb_row, 1000, 90);
    lv_obj_set_pos(thumb_row, 12, 6);
    lv_obj_set_style_bg_opa(thumb_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(thumb_row, 0, 0);
    lv_obj_set_style_radius(thumb_row, 0, 0);
    lv_obj_set_style_pad_all(thumb_row, 2, 0);
    /* ROW 流水布局：子对象自动横排，间距由 pad_column 控制。
     * 这样照片数量变化时不需要任何手工坐标计算。 */
    lv_obj_set_flex_flow(thumb_row, LV_FLEX_FLOW_ROW);
    /* 三个对齐参数分别是：主轴（START=从左开始排）
     *                     交叉轴（CENTER=垂直居中）
     *                     轨道对齐（CENTER=单行时垂直居中） */
    lv_obj_set_flex_align(thumb_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(thumb_row, 12, 0);    /* 缩略图之间的水平间距 */
    lv_obj_set_scroll_dir(thumb_row, LV_DIR_HOR);     /* 只允许横向滚动 */
    lv_obj_set_scrollbar_mode(thumb_row, LV_SCROLLBAR_MODE_AUTO);

    /* 建缩略图（放在这里、三个按钮之前，
     * 这样父对象顺序是：缩略图条在最下，按钮压在上面） */
    thumb_bar_rebuild();

    /* 上一张按钮 */
    lv_obj_t *prev_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(prev_btn, 100, 44);
    lv_obj_set_pos(prev_btn, 342, 105);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(prev_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(prev_btn, 1, 0);
    lv_obj_set_style_border_color(prev_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(prev_btn, 10, 0);
    lv_obj_t *prev_lab = lv_label_create(prev_btn);
    lv_label_set_text(prev_lab, "上一张");
    lv_obj_set_style_text_font(prev_lab, font_norm, 0);
    lv_obj_set_style_text_color(prev_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(prev_lab);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);

    /* 播放/暂停按钮（绿色）。
     * 三个按钮 x 分别是 342 / 462 / 582，等间距 120，
     * 居中于 1024 宽：342 + 100 = 442，582 + 100 = 682，中点 512 = 1024/2 ✓ */
    lv_obj_t *play_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(play_btn, 100, 44);
    lv_obj_set_pos(play_btn, 462, 105);
    lv_obj_set_style_bg_color(play_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(play_btn, 10, 0);
    play_lab = lv_label_create(play_btn);
    lv_label_set_text(play_lab, "播放");
    lv_obj_set_style_text_font(play_lab, font_norm, 0);
    lv_obj_set_style_text_color(play_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(play_lab);
    lv_obj_add_event_cb(play_btn, play_pause_cb, LV_EVENT_CLICKED, NULL);

    /* 下一张按钮 */
    lv_obj_t *next_btn = lv_button_create(ctrl_bar);
    lv_obj_set_size(next_btn, 100, 44);
    lv_obj_set_pos(next_btn, 582, 105);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(next_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_width(next_btn, 1, 0);
    lv_obj_set_style_border_color(next_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(next_btn, 10, 0);
    lv_obj_t *next_lab = lv_label_create(next_btn);
    lv_label_set_text(next_lab, "下一张");
    lv_obj_set_style_text_font(next_lab, font_norm, 0);
    lv_obj_set_style_text_color(next_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(next_lab);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, NULL);

    /* 初始显示第一张（顺便高亮第一张缩略图）。
     * ★ 必须在所有控件都建好、缩略图条也 rebuild 完之后才调 ——
     *   show_photo 会访问 big_image / counter_label / thumb_objs[]，
     *   任何一个还没建出来都会出问题。 */
    show_photo(0);
    lv_screen_load(scr);
    return scr;
}
