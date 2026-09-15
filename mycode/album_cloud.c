/* ============================================================================
 * @file    album_cloud.c
 * @brief   云相册弹层实现
 *
 * 【它在整个相册功能里的位置】
 *
 *   album.c（相册页）
 *     └── 点「云相册」按钮 → album_cloud_open(album_screen)
 *                              本文件：盖一层弹层
 *                              ├── 填 IP/端口 → 连接 → ftp_connect()
 *                              ├── 刷新列表   → ftp_list_begin()
 *                              ├── 点某行下载 → ftp_get_begin() ×2（大图 + 缩略图）
 *                              └── 下完 → album_rescan() 通知相册刷新
 *                                          （回调进 album.c，实现解耦）
 *
 *   ★ 本文件是「界面层」，但它是唯一同时碰 LVGL 和 ftp.c 的模块 ——
 *     这没问题，因为 ftp.c 内部自己起了任务线程，
 *     本文件所有 lv_* 调用都在主线程（含定时器回调），没有跨线程碰界面。
 *
 * 【关于弹层的对象树（关闭时靠它一次性清干净）】
 *
 *   cloud_root（全屏半透明遮罩，本文件所有控件的共同祖先）
 *     ├── cloud_panel（中央面板）
 *     │     ├── 标题 / 说明 / 关闭按钮
 *     │     ├── ip_ta / port_ta / 连接 / 刷新列表 / status_lab
 *     │     ├── file_list（flex 竖向列表，每个文件一行）
 *     │     ├── bar（进度条）
 *     │     └── 底部提示
 *     └── cloud_kb（软键盘，挂在 cloud_root 上而不是 panel 上，避免被裁切）
 *
 *   因为键盘是 root 的子对象，album_cloud_close() 只要
 *   lv_obj_delete(cloud_root) 就能把整棵树（含键盘）递归删掉。
 *
 * 【三个设计决定（都是为了不破坏已验收的相册模块）】
 *   1. 只新增编号，绝不覆盖已有文件。本工程 LV_CACHE_DEF_SIZE = 0（无图片缓存），
 *      同名覆盖后 LVGL 会不会重读盘不确定，走新编号行为是确定的。
 *   2. 大图与缩略图必须成对下齐才算成功，否则相册里出现的照片没有缩略图。
 *   3. 下载由 ftp.c 落到 .tmp 再 rename，断线只会留个被删掉的临时文件。
 * ========================================================================== */

#include "../lvgl/lvgl.h"
#include "album.h"
#include "album_cloud.h"
#include "ftp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

/* ------------------------------------------------------------------ */
/* 常量与状态                                                         */
/* ------------------------------------------------------------------ */

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* POSIX 路径（标准 fopen/opendir 用），不是 LVGL 的 A:/ 路径。
 * ★ 本文件两种路径都会用到：
 *   标「已在本地」用 fopen 查文件 → 走 POSIX 路径
 *   （本文件没有直接读图，读图在 album.c，那边才用 A:/） */
#define PHOTO_DIR_POSIX "/work_space/bmp_pic/photo"

#define C_BG      0x111119
#define C_CARD    0x1B1B26
#define C_LINE    0x2B2B3A
#define C_TEXT    0xECEAF2
#define C_DIM     0x6A6A7A
#define C_ACCENT  0x2FD3A0
#define C_MY_BG   0x1D9E75
#define C_MY_TX   0x06130E
#define C_ERR     0xE24B4A

/* 轮询间隔。200ms 的权衡：
 *   太密（如 50ms）会给 LVGL 主循环增加无谓负担；
 *   太疏（如 1 秒）进度条会一跳一跳的，感觉卡。
 *   200ms 下进度条 5 帧/秒，肉眼已经足够顺。 */
#define POLL_PERIOD_MS 200

/* 下载阶段（两阶段下载的状态标记，poll_cb 靠它决定「下一步做什么」） */
#define PH_IDLE    0
#define PH_BIG     1     /* 正在下大图 */
#define PH_THUMB   2     /* 正在下缩略图 */

static lv_obj_t   *cloud_root   = NULL;   /* 全屏遮罩（父对象） */
static lv_obj_t   *cloud_panel  = NULL;   /* 中央面板 */
static lv_obj_t   *cloud_kb     = NULL;
static lv_obj_t   *file_list    = NULL;   /* 文件列表容器 */
static lv_obj_t   *ip_ta        = NULL;
static lv_obj_t   *port_ta      = NULL;
static lv_obj_t   *conn_lab     = NULL;   /* 连接按钮上的文字 */
static lv_obj_t   *status_lab   = NULL;
static lv_obj_t   *bar          = NULL;   /* 进度条 */
static lv_timer_t *poll_timer   = NULL;

static lv_font_t  *f_desc  = NULL;
static lv_font_t  *f_title = NULL;

/* IP/端口存在静态缓冲里（不是指针指向 textarea 内部）。
 * ★ 为什么必须拷贝：textarea 的文字存在 LVGL 的内存池里，
 *   弹层一关就随控件一起释放了，而这两个值是下次打开时要回填到
 *   输入框里的「记忆」，必须自己留一份。 */
static char cloud_ip[32]   = "172.100.1.126";
static char cloud_port[8]  = "8889";

static int  phase        = PH_IDLE;
static int  local_index  = 0;      /* 本次下载要用的新编号 */
static char pending_remote[FTP_NAME_LEN];   /* 正在下的大图远程名 */

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * ★ 注意本函数与 album.c / main_interface.c 的同名函数相比，
 *   多了一行 `if(empty < 0) return NULL;` —— 这行是必需的：
 *   3 个缓存槽满后请求第 4 种字号时，empty 仍是 -1，
 *   少了这行就会执行 cache[-1] = f 越界写。
 *   本文件只用 2 种字号（14/24），所以其它文件即使漏了也没暴露出来。
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[3] = {NULL};
    static int cache_size[3] = {0};
    int empty = -1;

    for(int i = 0; i < 3; i++) {
        if(cache[i] != NULL && cache_size[i] == size) return cache[i];
        if(cache[i] == NULL && empty < 0) empty = i;
    }
    if(empty < 0) return NULL;        /* ★ 槽满时返回 NULL，绝不能让 empty 以 -1 下探 */

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

/** 造一个带字体的 label（本文件里出现频率太高，抽出来省重复代码） */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, lv_font_t *font, uint32_t color)
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    return lab;
}

/**
 * 造一个「按钮 + 居中文字」的组合（返回的是按钮，文字是它的子对象）。
 *
 * @param cb        点击回调，为 NULL 则不注册事件
 * @param user_data 透传给回调（lv_event_get_user_data 取回）
 *
 * ★ 用 lv_button_create 而不是 lv_obj_create：
 *   lv_button 自带 CLICKABLE，省得自己加 flag 漏了就点不动。
 */
static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, lv_font_t *font,
                           int x, int y, int w, int h, uint32_t bg, uint32_t fg,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);

    lv_obj_t *lab = mk_label(btn, text, font, fg);
    lv_obj_center(lab);

    if(cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/**
 * 把字节数格式化成人类可读的 KB / MB。
 *
 * 【为什么写得这么绕：LV_USE_FLOAT = 0】
 *   本工程关掉了 LVGL 的浮点支持（省空间、省 CPU），
 *   连带影响是不方便用 %f 做显示格式化。
 *   所以这里用「先乘 10 再除」的定点技巧手工拼小数：
 *     要显示 1.8 MB，就先算 mb10 = 18，再输出 mb10/10（=1）和 mb10%10（=8）。
 *   整数除法天然完成了取整，不需要任何浮点运算。
 *
 * @param bytes    字节数
 * @param out      输出缓冲
 * @param out_size 缓冲大小
 */
static void fmt_size(long bytes, char *out, int out_size)
{
    if(bytes >= 1024 * 1024) {
        long mb10 = bytes * 10 / (1024 * 1024);
        snprintf(out, out_size, "%ld.%ld MB", mb10 / 10, mb10 % 10);
    }
    else if(bytes >= 1024) {
        long kb10 = bytes * 10 / 1024;         /* 保留 1 位小数的 KB×10 */
        snprintf(out, out_size, "%ld.%ld KB", kb10 / 10, kb10 % 10);
    }
    else {
        snprintf(out, out_size, "%ld B", bytes);
    }
}

/**
 * 扫本地目录，找下一个可用的照片编号（photo_N.bmp 的 max N + 1）。
 *
 * 【为什么是「最大编号 + 1」而不是「第一个空缺」】
 *   取 max+1 保证新照片永远排在最后，相册的排序（按编号）看起来就是
 *   「新下载的加在末尾」，符合直觉。
 *   而且 max+1 不会复用已删除照片的编号，
 *   避免了「删掉第 3 张后又下载，新照片顶上编号 3」这种迷惑情况。
 *
 * 【命名格式约束】
 *   sscanf 的 "photo_%d.bmp" 要求文件名严格是 photo_数字.bmp，
 *   所以目录里像 photo_1_old.bmp / photo_1.bmp.bak 这类名字不会被算进去
 *   —— 这是好事，备份文件不会干扰编号计算。
 *
 * @return 可用的新编号；目录打不开时返回 7（兜底，避免返回 0 覆盖第一张）
 */
static int find_next_photo_index(void)
{
    DIR *d;
    struct dirent *e;
    int max_n = 0;

    d = opendir(PHOTO_DIR_POSIX);
    if(d == NULL) return 7;            /* 目录打不开就先从 7 开始试 */

    while((e = readdir(d)) != NULL) {
        int n = 0;
        /* sscanf 返回 1 表示成功匹配到一个整数，即文件名符合 photo_N.bmp */
        if(sscanf(e->d_name, "photo_%d.bmp", &n) == 1) {
            if(n > max_n) max_n = n;
        }
    }
    closedir(d);

    if(max_n < 0) max_n = 0;
    return max_n + 1;
}

/** 本地是否已有同名文件（用来在列表里标「已在本地」） */
static int local_has_file(const char *name)
{
    char path[352];
    FILE *fp;

    /* 拼完整路径后试着打开，能打开就算存在。
     * 用 fopen 试而不是 access()/stat()：不用引入额外头文件，
     * 而且「能读」这个条件比「存在」更贴近真实需求。 */
    snprintf(path, sizeof(path), "%s/%s", PHOTO_DIR_POSIX, name);
    fp = fopen(path, "rb");
    if(fp) { fclose(fp); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 界面刷新                                                           */
/* ------------------------------------------------------------------ */

/** 设置状态栏文字与颜色（本文件里所有用户可见的反馈都走这里） */
static void status_set(const char *text, uint32_t color)
{
    if(status_lab == NULL) return;
    lv_label_set_text(status_lab, text);
    lv_obj_set_style_text_color(status_lab, lv_color_hex(color), 0);
}

/* 前向声明 */
static void file_list_rebuild(void);
static void start_download(const char *remote_name);

/* 一个文件行的「下载」按钮 */
static void dl_btn_cb(lv_event_t *e)
{
    /* user_data 就是该行对应的远程文件名（指向 file_list_rebuild 里的静态池） */
    const char *name = (const char *)lv_event_get_user_data(e);
    start_download(name);
}

/**
 * 重建远程文件列表。
 *
 * 【user_data 的生命周期问题，以及 name_pool 的用意】
 *   每个「下载」按钮的回调需要知道「自己是哪个文件」，做法是把文件名
 *   当作 user_data 挂上去。但 LVGL 不会拷贝这个指针所指的内容 ——
 *   如果传的是栈上局部数组，函数返回后指针就悬空了，
 *   用户点按钮时读到的是垃圾数据甚至崩溃。
 *   所以这里用一个 static 的二维数组 name_pool 做「长期有效的字符串池」，
 *   按行索引取槽位写入。
 *
 * ★ 为什么不用 lv_malloc 动态分配：那样每次重建列表都要把上一次分配的
 *   全部 free 掉，漏一个就泄漏；而重建频率不低（每次下载完都刷），
 *   很容易积少成多。静态池容量固定（64 行 × 64 字节 = 4KB），
 *   既不会泄漏也不需要释放逻辑。
 *
 * ★ 重建时掉旧的 user_data 指针为什么安全：
 *   lv_obj_clean() 会把旧的行控件全部删掉，那些按钮也随之消失，
 *   所以不会再有「已删除的按钮」去解引用被覆盖的池子。
 *   池子内容和行控件是同步重建的。
 */
static void file_list_rebuild(void)
{
    /* 每个下载按钮的 user_data 需要长期有效，这里用静态池按索引取，
     * 不用 lv_malloc，省掉释放的麻烦也不会泄漏 */
    static char name_pool[FTP_MAX_FILES][FTP_NAME_LEN];
    int n, i;

    if(file_list == NULL) return;
    lv_obj_clean(file_list);        /* 清掉所有旧行（递归删除子对象） */

    n = ftp_file_count();
    if(n <= 0) {
        /* 空列表给一句可操作的提示，而不是干瘪的「列表为空」 */
        lv_obj_t *tip = mk_label(file_list, "列表为空，点「刷新列表」重试", f_desc, C_DIM);
        lv_obj_set_width(tip, LV_PCT(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    if(n > FTP_MAX_FILES) n = FTP_MAX_FILES;

    for(i = 0; i < n; i++) {
        const char *name = ftp_file_name(i);
        long size = ftp_file_size(i);
        char info[128];
        char sz[32];
        lv_obj_t *row, *lab, *btn;

        /* 只列大图（photo_ 开头），缩略图是跟着大图一起下的。
         * ★ 这是本模块的「约定优于配置」：用户看到的就是「一张照片」，
         *   不该看到 photo_1.bmp 和 thumb_1.bmp 两条并列，
         *   否则很容易只下其中一个，导致相册里出现缺缩略图的坏照片。
         *   两个过滤条件是互补的：前缀保证是大图，后缀保证是 BMP
         *   （服务器目录里可能有 .txt/.json 之类的东西）。 */
        if(strncmp(name, "photo_", 6) != 0) continue;
        if(strstr(name, ".bmp") == NULL) continue;

        /* 把文件名拷进静态池，供本行按钮的 user_data 长期引用 */
        snprintf(name_pool[i], FTP_NAME_LEN, "%s", name);

        /* 一行：深色半透明底 + 圆角 + 不可滚动 */
        row = lv_obj_create(file_list);
        lv_obj_set_width(row, LV_PCT(100));      /* 宽度撑满列表容器 */
        lv_obj_set_height(row, 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);   /* 行本身不滚动，滚动交给 file_list */

        /* 文件名 + 大小（+ 已在本地标记）。用空格对齐而不是表格控件，
         * 简单且不依赖等宽字体。 */
        fmt_size(size, sz, sizeof(sz));
        if(local_has_file(name))
            snprintf(info, sizeof(info), "%s      %s     (已在本地)", name, sz);
        else
            snprintf(info, sizeof(info), "%s      %s", name, sz);

        lab = mk_label(row, info, f_desc, C_TEXT);
        lv_obj_align(lab, LV_ALIGN_LEFT_MID, 12, 0);       /* 左对齐 */

        btn = mk_button(row, "下载", f_desc, 0, 0, 80, 32,
                        C_MY_BG, C_MY_TX, dl_btn_cb, name_pool[i]);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -8, 0);      /* 右对齐 */
    }
}

/* ------------------------------------------------------------------ */
/* 下载流程                                                           */
/* ------------------------------------------------------------------ */

/**
 * 把 photo_N.bmp 换成 thumb_N.bmp。
 * 例：photo_12.bmp → thumb_12.bmp（"thumb_" + 跳过 "photo_" 6 个字符后的部分）
 *
 * ★ `p == big` 这个条件不能省：它保证 "photo_" 出现在**开头**。
 *   如果只判 p != NULL，那 photo_1_photo_2.bmp 这种怪名字会被错误改写。
 *   不匹配时原样输出，让上层去报错（服务器缺缩略图）。
 */
static void remote_thumb_name(const char *big, char *out, int out_size)
{
    const char *p = strstr(big, "photo_");
    if(p != NULL && p == big)
        snprintf(out, out_size, "thumb_%s", big + 6);
    else
        snprintf(out, out_size, "%s", big);
}

/**
 * 开始下载一张照片的【第一阶段：大图】。
 *
 * 【为什么大图和缩略图要分两次 FTP 任务】
 *   ftp.c 的任务槽深度是 1，一次只能跑一个任务，
 *   所以只能「下完大图 → 在 poll_cb 里接着发起缩略图任务」这样串起来。
 *   这是状态机（phase 变量）存在的全部原因。
 *
 * 【为什么先定编号再下载】
 *   本地编号（local_index）在下载开始前就要算好并分配到两个目标路径上，
 *   这样两张图必定同号成对。如果等下载完再编号，
 *   中途可能有别的来源（比如另一个下载）插进来抢编号，导致不成对。
 *
 * @param remote_name 服务器上的大图文件名（形如 photo_N.bmp）
 */
static void start_download(const char *remote_name)
{
    char local_path[320];

    if(!ftp_is_connected()) {
        status_set("未连接 FTP 服务器", C_ERR);
        return;
    }
    if(phase != PH_IDLE) {
        status_set("上一个下载还没完成", C_ERR);
        return;
    }

    local_index = find_next_photo_index();       /* 一次性算好编号，两张图共用 */
    snprintf(pending_remote, sizeof(pending_remote), "%s", remote_name);

    snprintf(local_path, sizeof(local_path), "%s/photo_%d.bmp",
             PHOTO_DIR_POSIX, local_index);

    if(ftp_get_begin(remote_name, local_path) != 0) {
        status_set(ftp_message(), C_ERR);        /* 失败原因由 ftp.c 提供 */
        return;
    }

    /* 提交成功后才置 phase —— 顺序很重要：
     * 如果先置 PH_BIG 再提交，提交失败时 phase 会卡在 PH_BIG，
     * 之后所有下载请求都被上面的「上一个下载还没完成」挡住，模块僵死。 */
    phase = PH_BIG;
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    {
        char tip[128];
        snprintf(tip, sizeof(tip), "正在下载 %s → photo_%d.bmp", remote_name, local_index);
        status_set(tip, C_ACCENT);
    }
}

/**
 * 第二阶段：大图下完后自动接着下缩略图。
 * 由 poll_cb 在检测到「大图任务成功且 phase == PH_BIG」时调用。
 */
static void start_thumb_download(void)
{
    char thumb_remote[FTP_NAME_LEN];
    char local_path[320];

    /* photo_12.bmp → thumb_12.bmp，本地目标 thumb_12.bmp（编号与大图一致） */
    remote_thumb_name(pending_remote, thumb_remote, sizeof(thumb_remote));
    snprintf(local_path, sizeof(local_path), "%s/thumb_%d.bmp",
             PHOTO_DIR_POSIX, local_index);

    if(ftp_get_begin(thumb_remote, local_path) != 0) {
        /* 缩略图起不来：把 phase 复位回 IDLE，让用户可以重试。
         * ★ 注意此时大图已经落地了（photo_N.bmp 存在但 thumb_N.bmp 缺失），
         *   所以相册的扫描（成对匹配）不会把这张不完整的照片显示出来 ——
         *   这正是「成对校验」设计的价值：宁可不显示，也不显示坏图。 */
        status_set("缩略图下载启动失败", C_ERR);
        phase = PH_IDLE;
        return;
    }
    phase = PH_THUMB;
    status_set("正在下载缩略图…", C_ACCENT);
}

/** 两个阶段都成功，收尾 */
static void finish_ok(void)
{
    char tip[160];

    phase = PH_IDLE;
    lv_bar_set_value(bar, 100, LV_ANIM_OFF);

    snprintf(tip, sizeof(tip), "下载完成，相册已更新（第 %d 张）", local_index);
    status_set(tip, C_ACCENT);

    album_rescan();            /* 让相册重扫目录并跳到新照片 */
    file_list_rebuild();       /* 刷新列表里的「已在本地」标记 */
}

/* ------------------------------------------------------------------ */
/* 轮询定时器                                                         */
/* ------------------------------------------------------------------ */

/**
 * 200ms 轮询 ftp.c 的状态，驱动整个「连接 → 列表 → 大图 → 缩略图 → 完成」流程。
 *
 * 【为什么用轮询而不是回调】
 *   下载在 ftp.c 的独立线程里跑，它不能直接调 lv_*（线程安全问题）。
 *   最省事且安全的做法就是：主线程定时去看一眼状态位。
 *   代价是 200ms 的响应延迟，对人眼完全无感。
 *
 * 【进度映射：大图占 0~80，缩略图占 80~100】
 *   一次「下载一张照片」实际是两个任务，但用户看到的是「一个进度条」。
 *   所以要手动把两个任务的 0~100 拼成一条连续的 0~100：
 *     大图阶段  p(0~100) → p * 80 / 100          → 0 ~ 80
 *     缩略图阶段 p(0~100) → 80 + p / 5           → 80 ~ 100
 *   （p/5 就是把 0~100 压到 0~20）
 *   ★ 为什么给大图 80%：大图 1.8MB、缩略图才 34KB，
 *     按体积分配进度才符合用户对「还要等多久」的感觉。
 *
 * 【state 处理：DONE 要区分三种情况】
 *   本弹层里 ftp.c 的任务有三种来源，光看 FTP_ST_DONE 分不出来：
 *     ① 列表任务完成      → 刷列表
 *     ② 大图任务完成      → 发起缩略图任务
 *     ③ 缩略图任务完成    → 全流程成功，收尾
 *   靠 phase 变量区分：phase==PH_BIG 说明刚跑完的是大图，等等。
 *   phase==PH_IDLE 时出现的 DONE 就一定是列表任务（因为下载会先置 phase）。
 */
static void poll_cb(lv_timer_t *t)
{
    ftp_state_t st;

    (void)t;
    /* ★ 弹层已经关掉了就直接返回。
     *   理论上关闭时定时器已被 delete，轮不到这里；
     *   但这是「定时器回调访问已释放控件」的最后一道保险，
     *   加上一行判断的成本几乎为零。 */
    if(cloud_root == NULL) return;

    st = ftp_state();

    if(st == FTP_ST_RUNNING) {
        int p = ftp_progress();
        /* 大图占 0~80，缩略图占 80~100 */
        if(phase == PH_THUMB) p = 80 + p / 5;
        else                   p = p * 80 / 100;
        if(p > 100) p = 100;
        lv_bar_set_value(bar, p, LV_ANIM_OFF);
        return;                      /* 运行中只刷进度，不做状态迁移 */
    }

    if(st == FTP_ST_DONE) {
        if(phase == PH_BIG) {
            start_thumb_download();          /* 大图好了，接着下缩略图 */
        }
        else if(phase == PH_THUMB) {
            finish_ok();                     /* 两个都好了 */
        }
        else {
            /* 列表任务完成 */
            status_set(ftp_message(), C_ACCENT);
            file_list_rebuild();
        }
        /* 消费掉状态，避免下一轮重复处理
         *
         * ★★ 这里有一个很关键的互动，别小看这行的位置 ★★
         *   在上面 `phase == PH_BIG` 那条分支里，start_thumb_download()
         *   已经提交了新的 FTP 任务，于是 ftp.c 里 g_state 变成 RUNNING、
         *   g_busy 变成 1。
         *   此时再调 ftp_reset_state()，正是靠 ftp.c 里那行
         *   `if(g_busy) return;` 才会直接返回 ——
         *   否则它会把刚设好的 RUNNING 清成 IDLE，
         *   下一轮 poll_cb 就看不到任务在跑，
         *   缩略图进度条不动、流程也不会收尾，整个下载卡死在 80%。
         *   两个文件的这两行必须同时存在，改动任何一个都要检查另一个。 */
        ftp_reset_state();
        return;
    }

    if(st == FTP_ST_ERROR) {
        if(phase != PH_IDLE) {
            /* 下载任务失败：进度条归零，phase 复位，允许重试。
             * ★ 不复位 phase 的话，用户会一直看到「上一个下载还没完成」。 */
            phase = PH_IDLE;
            lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        }
        status_set(ftp_message(), C_ERR);      /* 具体原因由 ftp.c 给出 */
        ftp_reset_state();
    }
}

/* ------------------------------------------------------------------ */
/* 键盘                                                               */
/* ------------------------------------------------------------------ */

/**
 * IP / 端口输入框的焦点回调：弹出/收起软键盘。
 * 两个输入框共用同一个回调（同一把键盘轮流绑定），所以这里用
 * lv_event_get_target(e) 取「当前是谁获得焦点」再绑给它。
 */
static void cloud_ta_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if(cloud_kb == NULL) return;

    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(cloud_kb, ta);        /* 把键盘绑到这个输入框 */
        lv_obj_remove_flag(cloud_kb, LV_OBJ_FLAG_HIDDEN);
    }
    else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        /* 三种收起时机都处理：失去焦点、按键盘的「确定」、按「取消」 */
        lv_keyboard_set_textarea(cloud_kb, NULL);
        lv_obj_add_flag(cloud_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------ */
/* 按钮回调                                                           */
/* ------------------------------------------------------------------ */

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    album_cloud_close();
}

/**
 * 「刷新列表」按钮：重新拉一次服务器文件列表。
 * 两道前置检查（未连接 / 正在下载），任一不满足就只更新状态栏文字。
 */
static void refresh_btn_cb(lv_event_t *e)
{
    (void)e;
    if(!ftp_is_connected()) {
        status_set("请先点「连接」", C_ERR);
        return;
    }
    if(phase != PH_IDLE) {
        status_set("正在下载中，稍后再刷新", C_ERR);
        return;
    }
    if(ftp_list_begin() != 0) {
        status_set(ftp_message(), C_ERR);
        return;
    }
    status_set("正在获取文件列表…", C_ACCENT);
}

/**
 * 「连接」按钮：校验 IP/端口 → 连 FTP → 成功后自动拉一次列表。
 *
 * ★ 输入校验三连（IP 非空 → 端口非空 → 端口范围和数值合法）：
 *   端口用 atoi 转数字后再判范围，是因为 atoi("abc") 返回 0，
 *   只判字符串非空是挡不住 "abc" 的。
 *   范围 [1, 65535] 是 TCP 端口的合法区间（0 是保留值）。
 */
static void connect_btn_cb(lv_event_t *e)
{
    const char *ip, *port;
    int p;

    (void)e;
    if(phase != PH_IDLE) {
        status_set("正在下载中，请稍候", C_ERR);
        return;
    }

    ip   = lv_textarea_get_text(ip_ta);
    port = lv_textarea_get_text(port_ta);
    if(ip == NULL || ip[0] == '\0') { status_set("请填写服务器 IP", C_ERR); return; }
    if(port == NULL || port[0] == '\0') { status_set("请填写端口", C_ERR); return; }

    p = atoi(port);
    if(p <= 0 || p > 65535) { status_set("端口不合法", C_ERR); return; }

    /* 记住这次的地址，下次打开弹层时回填 */
    snprintf(cloud_ip, sizeof(cloud_ip), "%s", ip);
    snprintf(cloud_port, sizeof(cloud_port), "%s", port);

    /* 收键盘：要开始连接了，输入框不再需要焦点 */
    if(cloud_kb) {
        lv_keyboard_set_textarea(cloud_kb, NULL);
        lv_obj_add_flag(cloud_kb, LV_OBJ_FLAG_HIDDEN);
    }

    if(conn_lab) lv_label_set_text(conn_lab, "连接中…");
    status_set("正在连接 FTP 服务器…", C_ACCENT);

    /* 同上：ftp_connect 阻塞最长 3 秒，先手动刷一帧，否则看不到这句提示 */
    lv_refr_now(NULL);

    if(ftp_connect(cloud_ip, p, 3000) == 0) {
        if(conn_lab) lv_label_set_text(conn_lab, "已连接");
        status_set("连接成功，正在获取列表…", C_ACCENT);
        if(ftp_list_begin() != 0)
            status_set(ftp_message(), C_ERR);
    }
    else {
        if(conn_lab) lv_label_set_text(conn_lab, "连接");
        status_set(ftp_message(), C_ERR);       /* 失败原因来自 ftp.c */
    }
}

/* ------------------------------------------------------------------ */
/* 打开 / 关闭                                                        */
/* ------------------------------------------------------------------ */

/**
 * 关闭弹层。
 *
 * 【清理顺序（这个顺序是有讲究的）】
 *   ① lv_timer_delete(poll_timer) 并置 NULL
 *      —— ★ 必须第一个做。定时器回调会访问 file_list / bar / status_lab，
 *         如果先把控件删了，下一次定时器触发（最多 200ms 后）就会
 *         访问已释放的控件 → 花屏或崩溃。
 *         「定时器必须比它盯着的控件先销毁」是全工程的铁律。
 *
 *   ② ftp_disconnect()
 *      —— 断开 FTP，让可能正在跑的任务线程收尾。
 *         放在删控件之前做，是因为它内部会等最多 5 秒，
 *         先断开再删控件可以让界面"干净地"停在断开之后的状态，
 *         而不是删完控件后还在等后台线程。
 *
 *   ③ lv_obj_delete(cloud_root)
 *      —— 递归删掉整棵树。因为键盘也是 cloud_root 的子对象，
 *         所以键盘不需要单独处理。这一步之后 cloud_root 及其所有后代
 *         都成了悬空指针。
 *
 *   ④ 把所有缓存的控件指针置 NULL
 *      —— ★ 不置 NULL 的后果：album_cloud_is_open() 是靠
 *         `cloud_root != NULL` 判断的（见下面），不置 NULL 会让相册
 *         以为弹层还开着，于是再点「云相册」按钮什么都不发生。
 *         其它指针置 NULL 是给 status_set 之类的函数当保护用
 *         （它们开头都有 if(xxx == NULL) return;）。
 *
 *   ⑤ 复位状态变量（phase / local_index / pending_remote）
 *      —— 让下一次打开是干净的初始状态。
 *         不清理 pending_remote 的话，如果上次下载半途关掉，
 *         残留的文件名理论上会被下次的大图流程误用。
 */
void album_cloud_close(void)
{
    if(poll_timer != NULL) {
        lv_timer_delete(poll_timer);        /* ① 定时器最先死 */
        poll_timer = NULL;
    }
    ftp_disconnect();                        /* ② 断开网络 */

    if(cloud_root != NULL) {
        lv_obj_delete(cloud_root);      /* ③ 子对象（含键盘）一起销毁 */
        cloud_root = NULL;
    }

    /* ④ 清空所有控件指针 */
    cloud_panel = NULL;
    cloud_kb    = NULL;
    file_list   = NULL;
    ip_ta       = NULL;
    port_ta     = NULL;
    conn_lab    = NULL;
    status_lab  = NULL;
    bar         = NULL;

    /* ⑤ 复位业务状态 */
    phase       = PH_IDLE;
    local_index = 0;
    pending_remote[0] = '\0';
}

/** 弹层是否开着。相册页用它判断「云相册」按钮的行为（开着就不再重复创建） */
int album_cloud_is_open(void)
{
    return (cloud_root != NULL);
}

/**
 * 打开弹层（在 parent 上盖一层）。
 * @param parent 父对象，通常是相册屏（album.c 传 album_screen）
 *
 * 【幂等保护】cloud_root != NULL 时直接返回，
 *   防止连点两次「云相册」按钮创建出两层弹层
 *   （那样关掉一层后还留一层，且外层的指针已被覆盖，再也关不掉）。
 */
void album_cloud_open(lv_obj_t *parent)
{
    lv_obj_t *title, *lab, *btn;

    if(cloud_root != NULL || parent == NULL) return;

    /* 取两种字号。cn_font 内部有缓存，重复打开也不会重复加载字体。 */
    f_desc  = cn_font(14);
    f_title = cn_font(24);

    phase       = PH_IDLE;
    local_index = 0;

    /* ---- 全屏半透明遮罩（点它不关闭，避免误触） ----
     * 压暗背景，把注意力集中到中央面板上。
     * ★ 注释说「点它不关闭」——遮罩本身没有注册点击事件，
     *   所以要关只能点面板上的「关闭」按钮。
     *   这是有意的：弹层里有正在进行的下载和输入框，
     *   误触关闭会打断流程。 */
    cloud_root = lv_obj_create(parent);
    lv_obj_set_size(cloud_root, 1024, 600);
    lv_obj_set_pos(cloud_root, 0, 0);
    lv_obj_set_style_bg_color(cloud_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cloud_root, LV_OPA_70, 0);      /* 70% 黑，保留能看见底下的相册 */
    lv_obj_set_style_border_width(cloud_root, 0, 0);
    lv_obj_set_style_radius(cloud_root, 0, 0);
    lv_obj_set_style_pad_all(cloud_root, 0, 0);
    lv_obj_remove_flag(cloud_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 中央面板 860x480 @ (82,50) ----
     * 居中算法：(1024-860)/2 = 82，(600-480)/2 = 60 —— 
     * 纵向写成 50 是稍微偏上一点，留出底部软键盘的位置。 */
    cloud_panel = lv_obj_create(cloud_root);
    lv_obj_set_size(cloud_panel, 860, 480);
    lv_obj_set_pos(cloud_panel, 82, 50);
    lv_obj_set_style_bg_color(cloud_panel, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(cloud_panel, LV_OPA_COVER, 0);  /* 面板本身不透明 */
    lv_obj_set_style_border_width(cloud_panel, 1, 0);
    lv_obj_set_style_border_color(cloud_panel, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(cloud_panel, 16, 0);
    lv_obj_set_style_pad_all(cloud_panel, 20, 0);
    lv_obj_remove_flag(cloud_panel, LV_OBJ_FLAG_SCROLLABLE); /* 面板不滚，滚动交给列表 */
    /* ★ 注意：面板设了 pad_all(20)，但下面所有子控件都用 set_pos 绝对定位，
     *   而 set_pos 的坐标系是「内容区」原点（已含 padding 偏移），
     *   所以实际显示位置会比写的大 20px。这是有意的 —— 所有坐标
     *   都是按「内容区左上角为 0,0」设计的。 */

    /* 标题 */
    title = mk_label(cloud_panel, "云相册", f_title, 0xFFFFFF);
    lv_obj_set_pos(title, 0, 0);

    /* 副标题说明（放在标题右边，x=110 避开「云相册」三个字） */
    lab = mk_label(cloud_panel, "从 FTP 服务器下载照片到本机相册", f_desc, C_DIM);
    lv_obj_set_pos(lab, 110, 8);

    /* 关闭按钮（右上）。
     * 面板内容区宽 860-40 = 820，按钮宽 70，放 x=750 刚好贴合右边缘。 */
    mk_button(cloud_panel, "关闭", f_desc, 750, 0, 70, 34, C_BG, C_TEXT, close_btn_cb, NULL);

    /* ---- 连接行：IP 输入框 ---- */
    ip_ta = lv_textarea_create(cloud_panel);
    lv_obj_set_size(ip_ta, 240, 40);
    lv_obj_set_pos(ip_ta, 0, 52);
    lv_textarea_set_one_line(ip_ta, true);              /* 单行，回车即提交 */
    lv_textarea_set_placeholder_text(ip_ta, "服务器 IP");
    lv_textarea_set_text(ip_ta, cloud_ip);              /* 回填上次用过的地址 */
    /* accepted_chars 限制只能输数字和点 —— IP 里不可能有别的字符，
     * 从输入源头上就避免用户打出非法字符，比事后校验更友好。 */
    lv_textarea_set_accepted_chars(ip_ta, "0123456789.");
    lv_obj_set_style_bg_color(ip_ta, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(ip_ta, 1, 0);
    lv_obj_set_style_border_color(ip_ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(ip_ta, 8, 0);
    lv_obj_set_style_text_font(ip_ta, f_desc, 0);       /* ★ 必须设中文字体，
                                                         *   否则 placeholder 里的
                                                         *   中文会显示成方框 */
    lv_obj_set_style_text_color(ip_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_left(ip_ta, 10, 0);            /* 文字离左边框留点空 */

    /* ---- 端口输入框（宽 90，只收数字，最多 5 位 = 65535） ---- */
    port_ta = lv_textarea_create(cloud_panel);
    lv_obj_set_size(port_ta, 90, 40);
    lv_obj_set_pos(port_ta, 252, 52);
    lv_textarea_set_one_line(port_ta, true);
    lv_textarea_set_placeholder_text(port_ta, "端口");
    lv_textarea_set_text(port_ta, cloud_port);
    lv_textarea_set_accepted_chars(port_ta, "0123456789");
    lv_textarea_set_max_length(port_ta, 5);             /* 端口最大 65535，5 位足够 */
    lv_obj_set_style_bg_color(port_ta, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(port_ta, 1, 0);
    lv_obj_set_style_border_color(port_ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(port_ta, 8, 0);
    lv_obj_set_style_text_font(port_ta, f_desc, 0);
    lv_obj_set_style_text_color(port_ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_left(port_ta, 10, 0);

    /* 连接按钮。★ conn_lab 取的是按钮的第 0 个子对象，
     *   也就是 mk_button 内部造的那个居中 label ——
     *   这样后续就能直接改按钮文字（"连接" ↔ "连接中…" ↔ "已连接"），
     *   不用再自己找对象。 */
    btn = mk_button(cloud_panel, "连接", f_desc, 354, 52, 90, 40,
                    C_MY_BG, C_MY_TX, connect_btn_cb, NULL);
    conn_lab = lv_obj_get_child(btn, 0);

    mk_button(cloud_panel, "刷新列表", f_desc, 456, 52, 110, 40,
              C_BG, C_TEXT, refresh_btn_cb, NULL);

    /* 状态栏（连接/下载的反馈都显示在这里，x=580 起避开左边一排按钮） */
    status_lab = mk_label(cloud_panel, "未连接", f_desc, C_DIM);
    lv_obj_set_pos(status_lab, 580, 62);

    /* ---- 文件列表 ---- */
    file_list = lv_obj_create(cloud_panel);
    lv_obj_set_size(file_list, 820, 250);
    lv_obj_set_pos(file_list, 0, 104);
    lv_obj_set_style_bg_color(file_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(file_list, LV_OPA_40, 0);
    lv_obj_set_style_border_width(file_list, 1, 0);
    lv_obj_set_style_border_color(file_list, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(file_list, 10, 0);
    lv_obj_set_style_pad_all(file_list, 8, 0);
    /* flex 竖向流水布局：加进去的子对象自动竖排，不用手工算 y 坐标。
     * 这是列表类界面的标准做法 —— 行数变了布局自动重排。 */
    lv_obj_set_flex_flow(file_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(file_list, 6, 0);          /* 行间距 6px */
    lv_obj_set_scroll_dir(file_list, LV_DIR_VER);       /* 只允许纵向滚动 */
    lv_obj_set_scrollbar_mode(file_list, LV_SCROLLBAR_MODE_AUTO);  /* 需要时才出滚动条 */

    /* ---- 进度条 ---- */
    bar = lv_bar_create(cloud_panel);
    lv_obj_set_size(bar, 820, 10);
    lv_obj_set_pos(bar, 0, 366);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_BG), 0);              /* 槽（底色） */
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);  /* 已填充部分 */
    /* ★ 圆角要 main 和 indicator 各设一次：
     *   main 是槽、indicator 是填充条，是两个独立的 part，
     *   只设一个会出现「槽圆角、填充条方角」的难看效果。 */
    lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);

    /* 底部提示：给运维/同学一个明确的「为什么图显示不出来」的线索 */
    lab = mk_label(cloud_panel, "提示：下载的图片必须是 24 位 BMP，否则板子上显示不出来",
                   f_desc, C_DIM);
    lv_obj_set_pos(lab, 0, 384);

    /* ---- 软键盘（默认隐藏）----
     * ★ 父对象是 cloud_root（而不是 cloud_panel）：
     *   键盘高度 300，而面板高度只有 480 且带裁剪，
     *   挂在面板上会被裁掉一半。挂在全屏遮罩上才能完整显示。 */
    cloud_kb = lv_keyboard_create(cloud_root);
    lv_obj_add_flag(cloud_kb, LV_OBJ_FLAG_HIDDEN);       /* 默认不显示 */
    /* 两个输入框共用同一把键盘：谁获得焦点就绑给谁（见 cloud_ta_focus_cb） */
    lv_obj_add_event_cb(ip_ta,   cloud_ta_focus_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(port_ta, cloud_ta_focus_cb, LV_EVENT_ALL, NULL);

    /* 键盘要盖在遮罩之上。
     * 后创建的对象本来就在上层，但显式调一次更稳 ——
     * 万一以后调整了创建顺序，这行能兜住。 */
    lv_obj_move_foreground(cloud_kb);

    /* ---- 起轮询定时器 ----
     * ★ 最后一个动作。定时器一创建就开始跑（下个 LVGL 周期就会触发），
     *   所以必须等所有控件都建好、状态都初始化完再创建它，
     *   否则第一次回调会访问到还没建出来的 bar / status_lab。 */
    poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
}
