/**
 * @file    chat_ui.c
 * @brief   网络聊天室 —— 界面层实现（连接页 + 聊天页）
 *
 * =====================================================================
 * 一、这个文件在整个模块里的位置
 * =====================================================================
 *   chat.c      —— 纯网络层：socket、协议编解码、收包线程、事件队列
 *   chat_ui.c   —— 纯界面层（本文件）：LVGL 控件、布局、事件处理
 *   两者之间只通过 chat.h 暴露的接口通信：
 *
 *     界面线程（LVGL）                网络线程（recv_thread）
 *     ─────────────────               ──────────────────────
 *     chat_connect()  ──发起连接──▶
 *     chat_send_public() ─发数据─▶
 *                                    收到数据 → 组包 → 解析
 *                                    塞进事件环形队列
 *     poll_cb（每 100ms）  ◀─取事件── chat_poll_event()
 *
 *   铁律：网络线程里绝对不能调用任何 lv_* 函数！
 *   LVGL 不是线程安全的，跨线程直接操作控件会随机崩溃。
 *   所以网络线程只往队列里塞数据，界面线程自己定时去"取快递"。
 *
 * =====================================================================
 * 二、两个screen的职责
 * =====================================================================
 *   1) 连接页 ui_chat_conn_init()
 *      填 IP / 端口 / 昵称 → 点"连接"
 *      连接成功后创建聊天页并切过去，连接页**先留着不删**
 *      （chat_exit_cb 里一起删），这样返回时不用重建。
 *
 *   2) 聊天页 room_create()
 *      三栏布局：左=在线用户列表，中=消息区，下=快捷短语+输入行+软键盘。
 *
 * =====================================================================
 * 三、几个必须记住的设计决策
 * =====================================================================
 *   1. 界面层不碰 socket，只通过 chat.h 的接口收发；
 *      网络事件在 100ms 定时器里用 chat_poll_event() 取出来处理。
 *   2. 气泡宽度自己按 UTF-8 逐字符估算，不依赖 LVGL 的
 *      SIZE_CONTENT + max_width 组合（那个在 v9 里行为不够确定）。
 *   3. 消息条数上限 80，超出删最旧，避免长时间聊天内存一直涨。
 *   4. 在线列表比对 chat_online_version()，变了才重建控件，
 *      否则每 100ms 重建一次会把 LVGL 拖垮。
 *   5. LV_USE_FLOAT=0，全程不用 %f。
 *   6. 中文输入走 LVGL 自带的 lv_ime_pinyin（拼音输入法），
 *      它本身不是控件，而是"挂在键盘上的一个附加组件"，
 *      所以要手动把候选条样式/位置改成符合本工程暗色主题的样子。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "chat.h"
#include "chat_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

/* 中文字体：板上绝对路径（LV_FS_POSIX_LETTER 'A' + LV_FS_POSIX_PATH "" 的规则）。
 * 注意不要写 /mnt/hgfs/... 那种开发机路径，板子上不存在。 */
#define CN_FONT_PATH  "/work_space/font/msyh.ttc"
/* 聊天页背景图：'A:' 是 LVGL 虚拟盘符，实际指向板上根目录 */
#define BG_CHAT_PATH  "A:/work_space/bmp_pic/bg/bg_chat.bmp"

/* ---- 配色（和工程其它模块统一，底深、卡片略浅、绿作强调色） ---- */
#define C_BG      0x111119      /* 页面最深底色 */
#define C_CARD    0x1B1B26      /* 卡片/气泡底色 */
#define C_LINE    0x2B2B3A      /* 分隔线、边框 */
#define C_TEXT    0xECEAF2      /* 主文字（近白） */
#define C_DIM     0x6A6A7A      /* 次要文字（系统提示、占位符） */
#define C_ACCENT  0x2FD3A0      /* 强调绿：在线状态、昵称 */
#define C_MY_BG   0x1D9E75      /* 自己气泡的底色（深绿） */
#define C_MY_TX   0x06130E      /* 自己气泡的文字色（近黑，绿底上好读） */
#define C_ERR     0xE24B4A      /* 错误红 */

#define MSG_MAX         80      /* 消息区最多保留的气泡行数 */
#define BUBBLE_MAX_W    470     /* 气泡文本区最大像素宽 */
#define POLL_PERIOD_MS  100     /* 轮询网络事件的周期 */

/* ------------------------------------------------------------------ */
/* 全局状态                                                           */
/*                                                                    */
/* 这些都是 static：本文件专用，别的模块访问不到。                    */
/* 好处是命名可以短，坏处是必须在 chat_exit_cb 里手动清干净（见下）。  */
/* ------------------------------------------------------------------ */

lv_obj_t *chat_conn_screen = NULL;      /* 连接页 screen（非 static，chat.c 侧可能引用） */
lv_obj_t *chat_room_screen = NULL;      /* 聊天页 screen */

static lv_timer_t *poll_timer = NULL;   /* 100ms 事件轮询定时器 */

static int  target_id  = 0;             /* 0 = 公共大厅，其它 = 私聊对象 ID */

/* 三个输入框的"上次值"：点返回桌面再进来时还保留着，体验更好。
 * 这里给的是默认值，第一次进来直接能点连接（连本机常用服务端）。 */
static char my_nick[CHAT_NAME_LEN]     = "玩家小明";
static char conn_ip[32]                = "172.100.1.126";
static char conn_port[8]               = "8888";

static int  connecting = 0;             /* 正在连接：期间屏蔽返回键 */
static int  online_ver_seen = -1;       /* 已处理过的在线列表版本号 */
static int  last_conn_state = -1;       /* 上次看到的连接状态，-1 = 未知 */

/* 当前会话里用到的字体。
 * 字号越大越占内存，所以只备 5 档，用完缓存在 cn_font 里不释放。 */
static lv_font_t *g_f_tiny    = NULL;   /* 12 */
static lv_font_t *g_f_desc    = NULL;   /* 14 */
static lv_font_t *g_f_norm    = NULL;   /* 18 */
static lv_font_t *g_f_bubble  = NULL;   /* 18 */
static lv_font_t *g_f_title   = NULL;   /* 24 */

/* ---- 连接页控件 ---- */
static lv_obj_t *conn_panel     = NULL;     /* 中央面板（键盘弹起时整体上移） */
static lv_obj_t *conn_ip_ta     = NULL;     /* IP 输入框 */
static lv_obj_t *conn_port_ta   = NULL;     /* 端口输入框 */
static lv_obj_t *conn_nick_ta   = NULL;     /* 昵称输入框 */
static lv_obj_t *conn_status    = NULL;     /* 状态提示标签（"未连接"/"正在连接…"） */
static lv_obj_t *conn_btn_lab   = NULL;     /* "连接"按钮上的文字（改成"连接中…"用） */
static lv_obj_t *conn_kb        = NULL;     /* 连接页软键盘 */

/* ---- 聊天页控件 ---- */
static lv_obj_t *room_msg_area  = NULL;     /* 中间消息区（气泡都挂这里） */
static lv_obj_t *room_user_list = NULL;     /* 左侧在线用户列表 */
static lv_obj_t *room_peer_pill = NULL;     /* 顶部胶囊：当前聊天对象 */
static lv_obj_t *room_status    = NULL;     /* 右上角连接状态 */
static lv_obj_t *room_quick_row = NULL;     /* 快捷短语行 */
static lv_obj_t *room_input_row = NULL;     /* 输入行容器（输入框+发送+键盘按钮） */
static lv_obj_t *room_ta        = NULL;     /* 消息输入框 */
static lv_obj_t *room_kb        = NULL;     /* 聊天页软键盘 */
static lv_obj_t *room_ime       = NULL;     /* 拼音输入法（LVGL lv_ime_pinyin） */
static int       room_msg_count = 0;        /* 当前消息区里的行数（含系统提示） */

/* ---- 布局尺寸（屏幕 1024x600） ---- */
#define CONN_PANEL_W 520
#define CONN_PANEL_H 320   /* 内容 260 + 上下各 24 内边距 + 余量 */
#define CONN_PANEL_Y 100

#define MSG_AREA_X   228    /* 让开左侧 200 宽的在线列表（16+200+12 间距）*/
#define MSG_AREA_Y   70
#define MSG_AREA_W   780    /* 228 + 780 = 1008，右边留 16 */
#define MSG_AREA_H   424    /* 70 + 424 = 494，下面给快捷短语留位置 */

#define QUICK_ROW_Y  500
#define INPUT_ROW_Y  546    /* 546 + 44 = 590，底部留 10 */
#define INPUT_ROW_H  44

/* 软键盘高度：LVGL 默认是屏高 50% = 300，
 * 缩到 250 是为了在键盘上方腾出候选条的 42px。 */
#define KB_HEIGHT    250
#define CAND_H        42    /* 拼音候选条高度 */
#define CAND_GAP       4    /* 候选条与输入行的间距 */

/* 快捷短语：拼音输入法要一个个字选，短句一键发送更省事。
 * 注意 user_data 传的是字符串常量地址，生命周期 = 整个程序，安全。 */
static const char *QUICK_PHRASES[] = {
    "你好", "收到", "我在", "一起玩游戏？", "拜拜"
};
/* 编译期算数组长度，加短语不用改这里的数字 */
#define QUICK_COUNT ((int)(sizeof(QUICK_PHRASES) / sizeof(QUICK_PHRASES[0])))

/* ------------------------------------------------------------------ */
/* 工具                                                               */
/* ------------------------------------------------------------------ */

/**
 * 创建指定字号的中文字体。
 *
 * freetype 字体是"按字号实例化"的，同一个 ttc 每要一个字号就得 create 一次，
 * 而且很占内存（几十 KB 到几百 KB），所以必须缓存：
 *   cache[i]      缓存的字体句柄
 *   cache_size[i] 它对应的字号
 * 命中就复用，miss 就找空槽新建。
 *
 * 注意 lv_freetype_font_create 这个函数在 LVGL v9 里是**永久占用**的，
 * 没有对应的销毁调用（freetype 模块自己管生命周期），所以不用担心泄漏。
 *
 * @param size 需要的字号（像素高）
 * @return 字体句柄；失败返回 NULL（调用方需要容忍 NULL，LVGL 会退回默认字体）
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[5] = {NULL};    /* 最多缓存 5 档字号 */
    static int cache_size[5] = {0};
    int empty = -1;                         /* 第一个空槽下标 */

    /* 先查有没有同字号的缓存；顺便记下第一个空槽 */
    for(int i = 0; i < 5; i++) {
        if(cache[i] != NULL && cache_size[i] == size) return cache[i];
        if(cache[i] == NULL && empty < 0) empty = i;
    }
    /* 5 档全满且都不匹配 —— 这种情况本工程不会发生（只用了 12/14/18/24 四档），
     * 但显式判掉，避免数组越界写 cache[5]。 */
    if(empty < 0) return NULL;

    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    /* 【潜在地雷】这里其实少了一行 if(!f) return NULL;（见下方说明） */
    if(!f) {
        LV_LOG_ERROR("freetype font create failed: %s", CN_FONT_PATH);
        return NULL;
    }
    cache[empty] = f;
    cache_size[empty] = size;
    return f;
}

/**
 * 按 UTF-8 逐字符估算文本渲染宽度（LV_USE_FLOAT=0，不能用浮点）。
 *
 * 为什么不用 lv_obj_set_width(lab, LV_SIZE_CONTENT) + max_width？
 *   1) 在 LVGL v9 里 max_width 与 SIZE_CONTENT 的组合行为不够确定，
 *      出现过宽度算错、文字被截断的情况；
 *   2) 气泡宽度需要在**创建控件之前**就知道（先算宽度再造对象），
 *      SIZE_CONTENT 是布局阶段才回填的，拿不到。
 * 所以干脆自己数：全角（中文/emoji）按 font_size，半角按 0.55*font_size。
 *
 * 全角判定靠 UTF-8 首字节的前缀位：
 *   0xxxxxxx            → 1 字节，ASCII
 *   110xxxxx            → 2 字节
 *   1110xxxx            → 3 字节（中文常用字都是这种）
 *   11110xxx            → 4 字节（emoji、生僻字）
 *
 * 遇到 '\n' 换行，宽度从 0 重新累计（多行取最宽的一行）。
 *
 * @param text        文本（UTF-8）
 * @param font_size   字号，用来估算每个字符的像素宽
 * @param max_line_px 单行最大像素宽，超过就算作换行
 * @param out_max_w   输出：最长一行的像素宽
 */
static void text_measure(const char *text, int font_size, int max_line_px, int *out_max_w)
{
    const unsigned char *p = (const unsigned char *)text;
    int line_w = 0, max_w = 0;

    /* 空文本给个最小宽度，免得算出 0 让气泡塌成一条线 */
    if(text == NULL || text[0] == '\0') { *out_max_w = font_size * 2; return; }

    while(*p) {
        int bytes, cw;      /* bytes = 该字符占几个字节，cw = 估算像素宽 */

        if(*p == '\n') { p++; line_w = 0; continue; }          /* 换行：本行宽度归零 */
        if(*p < 0x80)                 { bytes = 1; cw = font_size * 55 / 100; }  /* ASCII 半角 */
        else if((*p & 0xE0) == 0xC0)  { bytes = 2; cw = font_size; }
        else if((*p & 0xF0) == 0xE0)  { bytes = 3; cw = font_size; }             /* 中文 */
        else                          { bytes = 4; cw = font_size; }
        /* 注：这里用 55/100 而不是 0.55，因为 LV_USE_FLOAT=0 时浮点会被裁掉 */

        if(line_w + cw > max_line_px) {     /* 这一行放不下了 → 记下最宽值重新开始 */
            if(line_w > max_w) max_w = line_w;
            line_w = 0;
        }
        line_w += cw;
        p += bytes;                          /* 按字节长跳，不按字符数跳 */
    }
    if(line_w > max_w) max_w = line_w;
    if(max_w <= 0) max_w = font_size * 2;    /* 兜底，避免返回 0 */
    *out_max_w = max_w;
}

/** 建一个文字标签：设文本 + 字体 + 颜色，返回标签对象 */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, lv_font_t *font, uint32_t color)
{
    lv_obj_t *lab = lv_label_create(parent);
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, font, 0);           /* part 0 = 主体 */
    lv_obj_set_style_text_color(lab, lv_color_hex(color), 0);
    return lab;
}

/**
 * 建一个按钮 + 居中文字，返回按钮对象。
 *
 * 注意：文字标签是按钮的第 0 号子对象，
 * 所以外部想改按钮文字可以 lv_obj_get_child(btn, 0) 拿到（conn_btn_lab 就是这么取的）。
 *
 * @param bg 按钮底色；@param fg 文字颜色
 * @param cb 点击回调，传 NULL 表示纯展示
 * @param user_data 透传给回调的自定义数据
 */
static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, lv_font_t *font,
                           int x, int y, int w, int h, uint32_t bg, uint32_t fg,
                           lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);      /* 必须设不透明度，否则底色看不见 */
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);                /* 去掉内边距，靠 lv_obj_center 定位 */

    lv_obj_t *lab = mk_label(btn, text, font, fg);
    lv_obj_center(lab);                                 /* 文字在按钮里居中 */

    /* cb 为空时不挂事件，省一次回调开销 */
    if(cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/** 建一个单行输入框（统一圆角/边框/内边距样式，免得每处重复写） */
static lv_obj_t *mk_textarea(lv_obj_t *parent, const char *placeholder, lv_font_t *font,
                             int x, int y, int w, int h)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, h);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);                     /* 单行：回车即"完成" */
    lv_textarea_set_placeholder_text(ta, placeholder);      /* 空的时候显示的灰字 */
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(ta, 10, 0);
    /* 文字字体/颜色设在 part 0；光标和占位符会自动继承 */
    lv_obj_set_style_text_font(ta, font, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_pad_left(ta, 12, 0);
    lv_obj_set_style_pad_right(ta, 12, 0);
    return ta;
}

/*
 * 取键盘高度。
 * 刚创建 / 刚从 hidden 恢复时，对象还没走过布局流程，
 * lv_obj_get_height() 可能返回 0 或旧值，所以先强制 update_layout。
 * 万一还是不合理（< 100），给个 240 的兜底值，避免布局算出负数。
 */
static int kb_height(lv_obj_t *kb)
{
    int h;
    lv_obj_update_layout(kb);       /* 立刻重算布局，不等下一帧 */
    h = lv_obj_get_height(kb);
    if(h < 100) h = 240;        /* 兜底值，和 LVGL 默认键盘高度接近 */
    return h;
}

/* ------------------------------------------------------------------ */
/* 拼音输入法辅助（lv_ime_pinyin）                                    */
/*                                                                    */
/* 【背景知识】                                                       */
/*   lv_ime_pinyin 不是一个独立控件，而是"挂在某个 lv_keyboard 上的    */
/*   输入法插件"。用法三步：                                          */
/*     room_ime = lv_ime_pinyin_create(scr);                          */
/*     lv_ime_pinyin_set_keyboard(room_ime, room_kb);                 */
/*   之后键盘每敲一个字母，插件就在内部拼拼音，                   */
/*   并在"候选条"里列出候选汉字；点候选字才真正写进 textarea。        */
/*                                                                    */
/*   候选条也是个 lv_obj（内部是 buttonmatrix），可以用                */
/*   lv_ime_pinyin_get_cand_panel() 拿到，然后自己改样式/位置。        */
/*                                                                    */
/* 【为什么要自己改样式】                                             */
/*   LVGL 默认给候选条的是白底 + montserrat_14（只有 ASCII），         */
/*   直接用在暗色主题上会：① 白块很刺眼 ② 候选汉字全是"方框"乱码。    */
/*                                                                    */
/* 【为什么这段代码放在这里】                                         */
/*   下面 send_btn_cb / ta_focus_cb / kb_toggle_cb 都要调用           */
/*   ime_reset / ime_drop_composing，所以这几个静态函数必须先定义，    */
/*   否则 C 语言只见到隐式声明（返回 int），链接时类型不匹配会警告。   */
/* ------------------------------------------------------------------ */

/** 取候选条对象；输入法没创建（连接页方向调用）时返回 NULL，调用方判空 */
static lv_obj_t *ime_cand(void)
{
    return room_ime ? lv_ime_pinyin_get_cand_panel(room_ime) : NULL;
}

/* 候选条配色：LVGL 给的是白底，和本工程暗色主题对不上，重刷一遍 */
static void ime_cand_style(void)
{
    lv_obj_t *cand = ime_cand();
    if(cand == NULL) return;

    /* --- part 0：候选条这个容器本身 --- */
    lv_obj_set_style_bg_color(cand, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cand, 1, 0);
    lv_obj_set_style_border_color(cand, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(cand, 10, 0);
    lv_obj_set_style_pad_all(cand, 2, 0);
    lv_obj_set_style_pad_column(cand, 4, 0);        /* 候选字之间留 4px */

    /* 关键：必须换成带汉字字形的字体，否则候选字全是方框
     *（LVGL 默认字体是 montserrat_14，只有 ASCII） */
    lv_obj_set_style_text_font(cand, g_f_norm, 0);
    lv_obj_set_style_text_color(cand, lv_color_hex(C_TEXT), 0);

    /* buttonmatrix 的按钮文字走 LV_PART_ITEMS，两个 part 都设上最稳 */
    lv_obj_set_style_text_font(cand, g_f_norm, LV_PART_ITEMS);
    lv_obj_set_style_text_color(cand, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_radius(cand, 8, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(cand, LV_OPA_TRANSP, LV_PART_ITEMS);   /* 平时按钮透明 */
    /* 按下时用强调绿高亮，让用户看到手指点到了哪个候选字 */
    lv_obj_set_style_bg_color(cand, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(cand, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(cand, lv_color_hex(C_MY_TX), LV_PART_ITEMS | LV_STATE_PRESSED);
}

/*
 * 候选条位置。
 * LVGL 默认把候选条贴在键盘顶部 —— 而键盘顶部正好是输入行所在高度，
 * 候选字会把输入框盖住。改成"贴着输入行正上方"：
 * 用 lv_obj_align_to(cand, room_input_row, LV_ALIGN_OUT_TOP_LEFT, 0, -CAND_GAP)
 * 意思是：把 cand 的左上角对齐到 room_input_row 的左上角外侧上方，再往上挪 4px。
 *
 * 所以本文件里输入行（room_input_row）位置一变，就必须重新调一次本函数
 * —— 见 room_layout_with_kb() 末尾。
 */
static void ime_cand_layout(void)
{
    lv_obj_t *cand = ime_cand();
    /* room_input_row 还没建出来时（room_create 早期）直接返回，调用方不必判空 */
    if(cand == NULL || room_input_row == NULL) return;

    lv_obj_set_size(cand, MSG_AREA_W, CAND_H);
    lv_obj_align_to(cand, room_input_row, LV_ALIGN_OUT_TOP_LEFT, 0, -CAND_GAP);
}

/*
 * 收起键盘时清掉没上屏的拼音状态。
 * 不清的话 ta_count 会留着旧值，下次弹键盘点候选字会按旧数量
 * 去回删输入框里的字，把内容搞乱。
 *
 * 【ta_count 是什么】
 *   用户敲 "nihao" 时，插件把 n-i-h-a-o 这 5 个字母**真的写进了 textarea**，
 *   同时用 ta_count = 5 记住"这 5 个字符是还没确定的拼音"。
 *   点候选字时，插件先 lv_textarea_delete_char() 回删 ta_count 个字符，
 *   再把汉字写进去 —— 这样 "nihao" 才会变成 "你好" 而不是 "nihao你好"。
 *   所以 ta_count 是个必须和输入框内容对齐的状态，收起键盘时必须归零。
 *
 * 【为什么要 cast 成 lv_ime_pinyin_t*】
 *   lv_ime_pinyin.h 里结构体定义是公开的（不像很多 LVGL 对象用私有结构体），
 *   所以可以直接访问内部字段，不用专门去做状态备份。
 */
static void ime_reset(void)
{
    lv_ime_pinyin_t *ime;

    if(room_ime == NULL) return;
    ime = (lv_ime_pinyin_t *)room_ime;   /* 结构体在 lv_ime_pinyin.h 里是公开的 */

    ime->ta_count = 0;                   /* 拼音待回删字符数归零 */
    ime->input_char[0] = '\0';           /* 清空当前组字缓冲 */
#if LV_IME_PINYIN_USE_K9_MODE
    /* K9（九宫格）模式额外一套状态；本工程用全键盘，这段不编译 */
    ime->k9_input_str[0] = '\0';
    ime->k9_input_str_len = 0;
#endif
    if(ime_cand()) lv_obj_add_flag(ime_cand(), LV_OBJ_FLAG_HIDDEN);   /* 顺手藏掉候选条 */
}

/*
 * 组字中途点「发送」：先把没上屏的字母删掉，只发已经确定的文字。
 *
 * 场景：用户敲了 "nihao" 但还没点候选字，这时直接点"发送"。
 * 如果不管，发出去的会是 "nihao" 这种拼音串。
 * 这里按 ta_count 把字母一个个删掉，效果等于"放弃这次组字"。
 *
 * 注意 while 里每次 delete 后 ta_count 也在减，
 * 循环条件用的是同一个变量，所以不会多删或少删。
 */
static void ime_drop_composing(void)
{
    lv_ime_pinyin_t *ime;

    if(room_ime == NULL) return;
    ime = (lv_ime_pinyin_t *)room_ime;

    while(ime->ta_count > 0) {
        if(room_ta) lv_textarea_delete_char(room_ta);
        ime->ta_count--;
    }
    ime_reset();        /* 顺带清输入法内部缓冲和候选条 */
}

/**
 * 连接页的一个字段：左侧标签 + 右侧输入框
 * （一行一个，紧凑到键盘弹起也放得下）
 *
 * 布局算式：
 *   标签的 y = row_y + 13   → 输入框高 44、标签高 18，13 = (44-18)/2，垂直居中
 *   输入框 x = 96，宽 376   → 面板内容宽 520-48(内边距) = 472，96+376 = 472，正好顶到右边
 *
 * @param value 预填值（上次输入的内容），NULL 或空串表示不填
 * @return 输入框对象，调用方通常存起来读文本 / 加事件
 */
static lv_obj_t *mk_field(lv_obj_t *panel, const char *label_text, const char *placeholder,
                          const char *value, int row_y)
{
    lv_obj_t *lab = mk_label(panel, label_text, g_f_desc, 0x9A9AAC);
    lv_obj_t *ta;

    lv_obj_set_pos(lab, 0, row_y + 13);     /* (44-18)/2：标签与输入框垂直居中 */
    ta = mk_textarea(panel, placeholder, g_f_norm, 96, row_y, 376, 44);
    if(value != NULL && value[0] != '\0')
        lv_textarea_set_text(ta, value);
    return ta;
}

/* ------------------------------------------------------------------ */
/* 聊天页：消息区                                                     */
/* ------------------------------------------------------------------ */

/*
 * 删掉超出上限的最旧气泡行。
 * MSG_MAX = 80：聊天久了内存会一直涨（一条气泡包含 row + 气泡 + 标签 3 个对象），
 * 所以超过 80 行就把最旧的删掉（child 0 就是最旧的那个，因为新消息都往末尾加）。
 *
 * 双条件 while：既看计数也看实际子对象数，
 * 防止 room_msg_count 和真实控件数量不同步时死循环 / 越界。
 */
static void msg_trim(void)
{
    if(room_msg_area == NULL) return;
    while(room_msg_count > MSG_MAX && lv_obj_get_child_count(room_msg_area) > 0) {
        lv_obj_delete(lv_obj_get_child(room_msg_area, 0));
        room_msg_count--;
    }
}

/** 滚到最底部：取最后一个子对象滚进可视区，带动画更自然 */
static void msg_scroll_bottom(void)
{
    if(room_msg_area == NULL) return;
    uint32_t n = lv_obj_get_child_count(room_msg_area);
    if(n > 0)
        lv_obj_scroll_to_view(lv_obj_get_child(room_msg_area, n - 1), LV_ANIM_ON);
}

/**
 * 一条居中的系统提示（灰色小字，占满整行居中）。
 *
 * 用途很广：连接成功提示、发送失败提示、有人上下线消息、
 * 以及**协议不认识的消息**（chat.c 的 dispatch 白名单之外的都走这里）。
 * 如果发现"别人发的话变成了中间小灰字"，就是这个函数在接 —— 说明
 * 那条消息的 status/message 字段没被白名单识别（见 chat.c dispatch）。
 */
static void sys_line_add(const char *text)
{
    lv_obj_t *lab;

    if(room_msg_area == NULL || text == NULL) return;

    lab = mk_label(room_msg_area, text, g_f_desc, C_DIM);
    lv_obj_set_width(lab, LV_PCT(100));                 /* 占满整行才能居中 */
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);    /* 太长自动换行 */
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);

    room_msg_count++;
    msg_trim();
    msg_scroll_bottom();
}

/**
 * 加一条消息气泡。
 *
 * 【为什么需要"行"这一层】
 *   气泡要左对齐或右对齐。用 flex 做主对齐的话，
 *   最直接的做法是：外层 row 撑满宽度（LV_PCT(100)），
 *   row 自己是 flex 容器，气泡是唯一的子对象，
 *   然后靠 row 的 main-axis 对齐（START=左 / END=右）决定气泡靠哪边。
 *   如果直接把气泡挂在消息区，就只能整体左对齐，做不出右侧绿泡。
 *
 * 【自己发的用 ROW_REVERSE 而不是 END 对齐】
 *   两种都能往右靠，但 ROW_REVERSE 连"子对象顺序"也反了，
 *   后面如果要加头像/时间戳，位置会自动跟着镜像，更省事。
 *
 * @param name 发送者昵称
 * @param text 正文
 * @param mine 1 = 自己发的（绿底右对齐），0 = 别人发的（灰底左对齐）
 */
static void bubble_add(const char *name, const char *text, int mine)
{
    lv_obj_t *row, *bub, *lab;
    int text_w = 0;             /* text_measure 量出来的文本宽 */
    int bubble_w;               /* 气泡总宽 = 文本宽 + 两侧内边距 */

    if(room_msg_area == NULL || text == NULL) return;

    /* 1. 外层行：撑满宽度，用来做左右对齐 */
    row = lv_obj_create(room_msg_area);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);            /* 高度跟着气泡走 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);     /* 行本身透明，只要布局功能 */
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);    /* 不要滚动条 */
    lv_obj_set_flex_flow(row, mine ? LV_FLEX_FLOW_ROW_REVERSE : LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 2. 气泡本体，宽度按文本量出来 */
    text_measure(text, 18, BUBBLE_MAX_W, &text_w);
    bubble_w = text_w + 24;                    /* 左右各 12 内边距 */
    if(bubble_w > BUBBLE_MAX_W + 24) bubble_w = BUBBLE_MAX_W + 24;   /* 封顶 */
    if(bubble_w < 60) bubble_w = 60;                                 /* 太短也撑到 60，好看些 */

    bub = lv_obj_create(row);
    lv_obj_set_width(bub, bubble_w);
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bub, lv_color_hex(mine ? C_MY_BG : C_CARD), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bub, mine ? 0 : 1, 0);    /* 自己的气泡无边框，别人的有 */
    lv_obj_set_style_border_color(bub, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(bub, 12, 0);
    lv_obj_set_style_pad_all(bub, 12, 0);
    lv_obj_remove_flag(bub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bub, LV_FLEX_FLOW_COLUMN);         /* 昵称在上、正文在下 */
    lv_obj_set_style_pad_row(bub, 2, 0);

    /* 3. 昵称（自己的不显示） */
    if(!mine && name != NULL && name[0] != '\0') {
        lv_obj_t *who = mk_label(bub, name, g_f_tiny, C_ACCENT);
        lv_obj_set_width(who, LV_PCT(100));
    }

    /* 4. 正文：宽度锁定为气泡宽减去内边距，配合 WRAP 让长文本正确折行 */
    lab = mk_label(bub, text, g_f_bubble, mine ? C_MY_TX : C_TEXT);
    lv_obj_set_width(lab, bubble_w - 24);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);

    room_msg_count++;
    msg_trim();
    msg_scroll_bottom();
}

/* ------------------------------------------------------------------ */
/* 聊天页：在线列表                                                   */
/* ------------------------------------------------------------------ */

/** 顶部胶囊：显示当前聊天对象（绿字=公共大厅，紫字=私聊某号） */
static void peer_pill_update(void)
{
    char buf[64];
    if(room_peer_pill == NULL) return;

    if(target_id == 0)
        snprintf(buf, sizeof(buf), "公共大厅");
    else
        snprintf(buf, sizeof(buf), "私聊：%d 号", target_id);

    lv_label_set_text(room_peer_pill, buf);
    lv_obj_set_style_text_color(room_peer_pill,
        lv_color_hex(target_id == 0 ? C_ACCENT : 0x8B7CF6), 0);
}

/*
 * 点某一项 -> 切换聊天对象。
 * user_data 里塞的是用户 ID，用 (void *)(long)id 传递
 * —— 不要用 (void *)&id 传局部变量地址，那是悬垂指针。
 */
static void user_item_cb(lv_event_t *e)
{
    target_id = (int)(long)lv_event_get_user_data(e);
    peer_pill_update();

    /* 版本置 -1，让轮询定时器下一次重建列表，从而刷新选中项的高亮。
     * 不在这里直接重建，是因为回调里删掉自己所在的对象会崩
     * —— 本回调是列表项触发的，重建会把当前项 delete 掉，
     *    LVGL 事件系统还在用它，立刻崩溃。 */
    online_ver_seen = -1;
}

/**
 * 建一条在线用户项。
 * @param id 用户 ID，0 表示"公共大厅"这个虚拟项
 *
 * 选中态视觉：底色用 C_MY_BG、边框用强调绿、文字用 C_MY_TX，
 * 和普通项的 C_CARD/C_LINE/C_TEXT 拉开对比。
 */
static void user_item_create(const char *name, int id)
{
    int selected = (id == target_id);
    lv_obj_t *item = lv_button_create(room_user_list);

    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_height(item, 42);                        /* 固定高，列表整齐 */
    lv_obj_set_style_bg_color(item, lv_color_hex(selected ? C_MY_BG : C_CARD), 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(item, 1, 0);
    lv_obj_set_style_border_color(item, lv_color_hex(selected ? C_ACCENT : C_LINE), 0);
    lv_obj_set_style_radius(item, 8, 0);
    lv_obj_set_style_pad_left(item, 10, 0);
    lv_obj_set_style_pad_right(item, 8, 0);

    /* 注意：名字用 desc 字号(14)，长昵称可能超出 200 宽，
     * 这里没做截断/省略号，极端昵称会溢出列表边界。 */
    lv_obj_t *lab = mk_label(item, name, g_f_desc,
                             selected ? C_MY_TX : C_TEXT);
    lv_obj_align(lab, LV_ALIGN_LEFT_MID, 0, 0);         /* 左对齐垂直居中 */

    /* ID 塞进 user_data。用户 ID 从 1 开始，0 是公共大厅。 */
    lv_obj_add_event_cb(item, user_item_cb, LV_EVENT_CLICKED, (void *)(long)id);
}

/*
 * 重建在线列表（只在版本号变化时调用）。
 *
 * 【为什么用"版本号 + 重建"而不是"增量更新"】
 *   增量更新要自己算谁上线谁下线、谁改名，逻辑复杂容易出错；
 *   直接把列表清空重建最简单。但重建有开销（每项 2 个控件），
 *   所以必须由 chat_online_version() 的变化来触发，
 *   绝不能每 100ms 无条件重建 —— 那样 LVGL 会被拖垮。
 *
 * 顺序上的讲究：
 *   先 lv_obj_clean 清掉旧项，再建新区块。
 *   第一项永远固定是"公共大厅"（id = 0），保证用户总能切回公共频道。
 */
static void user_list_rebuild(void)
{
    int  ids[CHAT_MAX_USERS];
    char names[CHAT_MAX_USERS][CHAT_NAME_LEN];      /* 注意：这是 20*32 ≈ 640 字节的栈数组，
                                                     * CHAT_MAX_USERS 别调太大，会爆栈 */
    int  n, i, shown = 0;

    if(room_user_list == NULL) return;

    lv_obj_clean(room_user_list);       /* 清空所有子对象（会比 delete 慢一点，但一次性安全） */

    /* 第一项固定是公共大厅 */
    user_item_create("公共大厅", 0);
    shown++;

    n = chat_get_online(ids, names, CHAT_MAX_USERS);
    for(i = 0; i < n; i++) {
        if(ids[i] == 0 || ids[i] == chat_my_id()) continue;    /* 不列自己 */
        user_item_create(names[i], ids[i]);
        shown++;
    }

    /* 除了自己没别人时给个提示 */
    if(shown == 1) {
        lv_obj_t *tip = mk_label(room_user_list, "暂无其他用户", g_f_tiny, C_DIM);
        lv_obj_set_width(tip, LV_PCT(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* 自己的目标若已下线，退回公共大厅
     * （否则会一直对着一个不存在的 ID 发私聊，服务器会报错） */
    if(target_id != 0) {
        int found = 0;
        for(i = 0; i < n; i++)
            if(ids[i] == target_id) { found = 1; break; }
        if(!found) { target_id = 0; peer_pill_update(); }
    }
}

/* ------------------------------------------------------------------ */
/* 聊天页：底部输入区                                                 */
/* ------------------------------------------------------------------ */

static void room_status_set(const char *text, uint32_t color)
{
    if(room_status == NULL) return;
    lv_label_set_text(room_status, text);
    lv_obj_set_style_text_color(room_status, lv_color_hex(color), 0);
}

/**
 * 真正发出一条消息：按 target_id 决定走公共还是私聊，本地立即回显。
 *
 * 【为什么本地要自己回显】
 *   服务器广播回来需要时间（几十毫秒），如果等广播再显示，
 *   用户每发一句话都要顿一下。所以本地先画出来（"乐观显示"），
 *   之后收到服务端广播时 chat.c 侧会按消息 ID 过滤掉自己的，
 *   不会出现一条消息显示两遍。
 *
 * @param text 要发的正文（调用方保证非空且有效）
 */
static void do_send(const char *text)
{
    int r;

    if(text == NULL || text[0] == '\0') return;

    /* 连接断了就别发了，直接提示，避免走一遍无用的失败流程 */
    if(!chat_is_connected()) {
        room_status_set("● 未连接", C_ERR);
        sys_line_add("发送失败：与服务器连接已断开");
        return;
    }

    if(target_id == 0)
        r = chat_send_public(text);
    else
        r = chat_send_private(target_id, text);

    if(r != 0) {
        sys_line_add("发送失败，请检查网络");
        return;
    }
    bubble_add(my_nick, text, 1);      /* 本地回显，不等服务端广播 */
}

/**
 * 发送按钮回调。
 *
 * 【顺序很关键，不能调换】
 *   1. ime_drop_composing() —— 先删掉没上屏的拼音（会调用 lv_textarea_delete_char）
 *   2. lv_textarea_get_text() —— 之后才取指针
 *   3. 拷到本地 copy 数组 —— 因为下一步 lv_textarea_set_text(room_ta, "")
 *      会释放内部缓冲区，早先拿到的指针立刻失效（悬垂指针）。
 *      所以必须先拷贝再清空。
 */
static void send_btn_cb(lv_event_t *e)
{
    const char *txt;
    char copy[CHAT_TEXT_LEN];

    (void)e;

    ime_drop_composing();      /* 组字中途点发送：先丢掉没上屏的拼音 */

    /* 必须在上面那步之后取文本 —— delete_char 会让早先拿到的指针失效 */
    txt = lv_textarea_get_text(room_ta);
    if(txt == NULL || txt[0] == '\0') return;       /* 空文本不发 */

    snprintf(copy, sizeof(copy), "%s", txt);        /* 拷贝一份（同时做了长度截断保护）*/
    lv_textarea_set_text(room_ta, "");              /* 清空输入框 */
    do_send(copy);
}

/*
 * 快捷短语按钮。
 * user_data 是 QUICK_PHRASES[i] 这个字符串常量的地址（静态存储期，不会失效），
 * 所以这里可以直接用，不用拷贝。
 * 和 send_btn_cb 不同：快捷短语不经输入框，所以也不需要处理拼音状态。
 */
static void quick_btn_cb(lv_event_t *e)
{
    const char *phrase = (const char *)lv_event_get_user_data(e);
    do_send(phrase);
}

/* ------------------------------------------------------------------ */
/* 聊天页：软键盘                                                     */
/* ------------------------------------------------------------------ */

/**
 * 键盘弹出 / 收起时重新排布底部。
 *
 * 【布局算式（按 1024x600，键盘高 250）】
 *   键盘弹出时，键盘占据底部 250px，输入行必须搬到键盘正上方：
 *       row_y = 600 - kh - INPUT_ROW_H - 4 = 600 - 250 - 44 - 4 = 302
 *   然后消息区缩小，把候选条那 46px 也让出去：
 *       msg_area_h = row_y - CAND_H - CAND_GAP - MSG_AREA_Y - 10
 *                  = 302 - 42 - 4 - 70 - 10 = 176
 *   最后 ime_cand_layout() 把候选条贴到输入行上方（y = 302 - 4 - 42 = 256）。
 *
 * 【下限那个 clamp 为什么不是 150】
 *   老代码把 row_y 下限写成 150，但键盘 250 + 输入行 44 + 间距就已经 298 了，
 *   clamp 到 150 会让输入行被键盘压住。现在下限算的是"候选条 + 输入行 + 一点消息区"
 *   都显示得下的最小值，保证控件不重叠。
 *
 * @param kb_on 1 = 键盘弹出（压缩消息区、藏快捷短语），0 = 收起（恢复原布局）
 */
static void room_layout_with_kb(int kb_on)
{
    if(room_kb == NULL || room_input_row == NULL) return;

    if(kb_on) {
        int kh = kb_height(room_kb);
        int row_y = 600 - kh - INPUT_ROW_H - 4;     /* 输入行贴在键盘上方 */

        /* 下限要留出「候选条 + 输入行 + 一点消息区」，不能按老值 clamp 到 150 */
        if(row_y < MSG_AREA_Y + CAND_H + CAND_GAP + 120)
            row_y = MSG_AREA_Y + CAND_H + CAND_GAP + 120;
        lv_obj_set_y(room_input_row, row_y);
        /* 消息区再让出候选条那一条，免得候选字盖住最后一条消息 */
        lv_obj_set_height(room_msg_area, row_y - CAND_H - CAND_GAP - MSG_AREA_Y - 10);
        if(room_quick_row) lv_obj_add_flag(room_quick_row, LV_OBJ_FLAG_HIDDEN);
    }
    else {
        /* 收起键盘：一切恢复设计值 */
        lv_obj_set_y(room_input_row, INPUT_ROW_Y);
        lv_obj_set_height(room_msg_area, MSG_AREA_H);
        if(room_quick_row) lv_obj_remove_flag(room_quick_row, LV_OBJ_FLAG_HIDDEN);
    }
    ime_cand_layout();          /* 候选条跟着输入行走 */
    msg_scroll_bottom();        /* 消息区高度变了，重新滚到底 */
}

/**
 * 输入框焦点事件：显隐键盘（聊天页和连接页共用这一个回调）。
 *
 * 靠 user_data 区分哪个键盘：聊天页传 room_kb，连接页传 conn_kb，
 * 里面用 `kb == room_kb` 判断是哪一页，再走各自的布局逻辑。
 *
 * 监听的是 LV_EVENT_ALL，四种事件：
 *   FOCUSED    —— 输入框被选中（点击/程序设为焦点）→ 弹键盘
 *   DEFOCUSED  —— 失去焦点 → 收键盘
 *   READY      —— 单行输入框按回车 → 收键盘（视为输入完成）
 *   CANCEL     —— 键盘上的取消键 → 收键盘（丢弃输入）
 */
static void ta_focus_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);

    if(kb == NULL) return;

    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(kb, ta);               /* 告诉键盘往哪个输入框写字 */
        lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);     /* 显示键盘 */

        if(kb == room_kb) room_layout_with_kb(1);       /* 聊天页：压缩消息区 */
        else if(conn_panel) {                           /* 连接页：面板整体上移 */
            int kh = kb_height(kb);
            int py = 600 - kh - CONN_PANEL_H - 8;       /* 面板放到键盘正上方 */
            lv_obj_set_y(conn_panel, py < 6 ? 6 : py);  /* 顶到屏幕最上边就不许再往上 */
        }
    }
    else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ime_reset();                                    /* 清掉没上屏的拼音状态 */
        lv_keyboard_set_textarea(kb, NULL);             /* 解绑，避免键盘操作已失效的输入框 */
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);        /* 隐藏键盘 */

        if(kb == room_kb) room_layout_with_kb(0);       /* 聊天页：恢复布局 */
        else if(conn_panel) lv_obj_set_y(conn_panel, CONN_PANEL_Y);
    }
}

/*
 * 「键盘」按钮：手动切换软键盘显隐。
 *
 * 和 ta_focus_cb 的区别：这个按钮是"强制显示"键盘，
 * 即使用户没点输入框（比如想先看看键盘长什么样）也能弹出来。
 *
 * 注意两处手动同步焦点状态：
 *   弹出时 lv_obj_add_state(room_ta, LV_STATE_FOCUSED)  —— 让输入框显示光标
 *   收起时 lv_obj_remove_state(room_ta, LV_STATE_FOCUSED)
 * 这样按钮操作和焦点操作走的是同一套视觉状态，不会出现"键盘在但光标没了"。
 */
static void kb_toggle_cb(lv_event_t *e)
{
    (void)e;
    if(room_kb == NULL) return;

    if(lv_obj_has_flag(room_kb, LV_OBJ_FLAG_HIDDEN)) {
        lv_keyboard_set_textarea(room_kb, room_ta);
        lv_obj_remove_flag(room_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_state(room_ta, LV_STATE_FOCUSED);
        room_layout_with_kb(1);
    }
    else {
        lv_keyboard_set_textarea(room_kb, NULL);
        lv_obj_add_flag(room_kb, LV_OBJ_FLAG_HIDDEN);
        ime_reset();                                    /* 清掉没上屏的拼音状态 */
        lv_obj_remove_state(room_ta, LV_STATE_FOCUSED);
        room_layout_with_kb(0);
    }
}

/* ------------------------------------------------------------------ */
/* 退出：回桌面                                                       */
/* ------------------------------------------------------------------ */

/**
 * 返回桌面。
 *
 * 顺序不能改：
 *   1. 先删定时器      —— 否则它可能在屏被删后还去访问已释放的控件
 *   2. 再 chat_disconnect() —— shutdown 唤醒接收线程并 join
 *   3. 切回桌面屏
 *   4. 最后删本模块的两个屏
 *
 * 【为什么顺序这么重要】
 *   定时器 poll_cb 和网络线程都会碰界面状态：
 *   - 定时器不先删：删完屏之后定时器还可能在下一拍访问 room_msg_area 等已释放对象 → 段错误
 *   - 网络线程不 join：它还在往事件队列里塞数据，而队列/界面已经没人消费了
 *   第 3 步先切屏再删屏，是 LVGL 的要求：不能删除当前正在显示的 screen。
 *
 * 【最后清指针的原因】
 *   这些 static 指针不清就是悬垂指针。
 *   下次进来若代码里某处判断 `if(room_msg_area != NULL)`，
 *   就会拿着已释放的地址去操作 → 崩溃。全部置 NULL 最省心。
 */
static void chat_exit_cb(lv_event_t *e)
{
    (void)e;

    if(connecting) return;                 /* 连接中不许退出，避免状态错乱 */

    /* 1. 删定时器 */
    if(poll_timer != NULL) {
        lv_timer_delete(poll_timer);
        poll_timer = NULL;
    }
    /* 2. 断开网络（内部会 shutdown + join 收包线程） */
    chat_disconnect();

    /* 3. 切回桌面 */
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);

    /* 4. 删本模块的屏 */
    if(chat_room_screen != NULL) {
        lv_obj_delete(chat_room_screen);
        chat_room_screen = NULL;
    }
    if(chat_conn_screen != NULL) {
        lv_obj_delete(chat_conn_screen);
        chat_conn_screen = NULL;
    }

    /* 清掉界面缓存指针，防止下次进入时复用已释放的对象 */
    room_msg_area = room_user_list = room_peer_pill = NULL;
    room_status = room_quick_row = room_input_row = room_ta = room_kb = NULL;
    room_ime = NULL;
    conn_panel = conn_ip_ta = conn_port_ta = conn_nick_ta = NULL;
    conn_status = conn_kb = NULL;
    room_msg_count = 0;
    target_id = 0;                 /* 回到公共大厅 */
    online_ver_seen = -1;          /* 强制下次刷新列表 */
}

/* ------------------------------------------------------------------ */
/* 聊天页：创建                                                       */
/* ------------------------------------------------------------------ */

/**
 * 创建聊天页（1024x600）。
 *
 * 【整体布局】
 *   y=0   ┌────────────────────────────────────────────────┐
 *         │ [返回] 网络聊天室      (当前对象胶囊)   ● 已连接│  ← 顶部栏
 *   y=100 │ 在线   ┌──────────────────────────────────────┐ │
 *         │ 用户   │                                      │ │
 *         │ 列表   │        消息区（气泡往这里加）        │ │
 *         │ 200宽  │        228,70  780x424               │ │
 *   y=494 │        └──────────────────────────────────────┘ │
 *   y=500 │        [你好][收到][我在]…  ← 快捷短语（键盘弹出时隐藏）
 *   y=546 │        [输入框 590宽      ][发送][键盘]          │
 *   y=600 └────────────────────────────────────────────────┘
 *         键盘弹出时：输入行上移到键盘顶上，候选条出现在输入行上方
 *
 * 注意所有控件都挂在 `scr` 或 `win` 上，而软键盘挂在 `scr` 上
 * —— 这样键盘始终在窗口之上（后创建的兄弟对象在最上层）。
 *
 * @return 新建的 screen（调用方负责保存和后续删除）
 */
static lv_obj_t *room_create(void)
{
    lv_obj_t *scr, *win, *title;

    scr = lv_obj_create(NULL);          /* 独立 screen，不带默认样式 */
    win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_image_src(win, BG_CHAT_PATH, 0);    /* 背景图（BMP，注意字节序约定） */
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);                    /* 内边距清零，坐标才好算 */

    /* ---- 顶部栏 ---- */
    /* 返回按钮：点它走 chat_exit_cb，直接回桌面 */
    mk_button(win, "返回", g_f_norm, 20, 12, 90, 40, C_BG, C_TEXT, chat_exit_cb, NULL);

    title = mk_label(win, "网络聊天室", g_f_title, 0xFFFFFF);
    lv_obj_set_pos(title, 130, 18);

    /* 当前对象胶囊：用 lv_label 直接加背景/边框/圆角实现（不用容器，省一个对象） */
    room_peer_pill = lv_label_create(win);
    lv_obj_set_style_text_font(room_peer_pill, g_f_norm, 0);
    lv_obj_set_style_bg_color(room_peer_pill, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(room_peer_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(room_peer_pill, 1, 0);
    lv_obj_set_style_border_color(room_peer_pill, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(room_peer_pill, 20, 0);         /* 20 = 圆角胶囊 */
    lv_obj_set_style_pad_hor(room_peer_pill, 18, 0);        /* 左右留白，圆角不切字 */
    lv_obj_set_style_pad_ver(room_peer_pill, 8, 0);
    lv_obj_set_pos(room_peer_pill, 420, 16);

    /* 连接状态 */
    room_status = mk_label(win, "● 已连接", g_f_desc, C_ACCENT);
    lv_obj_set_pos(room_status, 900, 24);

    /* ---- 左侧：在线用户列表 ---- */
    {
        lv_obj_t *col_title = mk_label(win, "在线用户", g_f_desc, C_DIM);
        lv_obj_set_pos(col_title, 20, 74);

        room_user_list = lv_obj_create(win);
        lv_obj_set_size(room_user_list, 200, 424);          /* 和消息区同高 */
        lv_obj_set_pos(room_user_list, 16, 100);
        lv_obj_set_style_bg_color(room_user_list, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_bg_opa(room_user_list, LV_OPA_70, 0);   /* 半透，露出背景图 */
        lv_obj_set_style_border_width(room_user_list, 1, 0);
        lv_obj_set_style_border_color(room_user_list, lv_color_hex(C_LINE), 0);
        lv_obj_set_style_radius(room_user_list, 12, 0);
        lv_obj_set_style_pad_all(room_user_list, 8, 0);
        lv_obj_set_flex_flow(room_user_list, LV_FLEX_FLOW_COLUMN);  /* 纵向排列用户项 */
        lv_obj_set_style_pad_row(room_user_list, 6, 0);
        lv_obj_set_scroll_dir(room_user_list, LV_DIR_VER);          /* 只允许竖向滚动 */
        lv_obj_set_scrollbar_mode(room_user_list, LV_SCROLLBAR_MODE_AUTO);  /* 装不下才显示滚动条 */
    }

    /* ---- 中间：消息区 ---- */
    room_msg_area = lv_obj_create(win);
    lv_obj_set_size(room_msg_area, MSG_AREA_W, MSG_AREA_H);
    lv_obj_set_pos(room_msg_area, MSG_AREA_X, MSG_AREA_Y);
    lv_obj_set_style_bg_color(room_msg_area, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(room_msg_area, LV_OPA_60, 0);
    lv_obj_set_style_border_width(room_msg_area, 1, 0);
    lv_obj_set_style_border_color(room_msg_area, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(room_msg_area, 12, 0);
    lv_obj_set_style_pad_all(room_msg_area, 12, 0);
    lv_obj_set_flex_flow(room_msg_area, LV_FLEX_FLOW_COLUMN);       /* 气泡依次往下排 */
    lv_obj_set_style_pad_row(room_msg_area, 8, 0);
    lv_obj_set_scroll_dir(room_msg_area, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(room_msg_area, LV_SCROLLBAR_MODE_AUTO);

    /* ---- 底部：快捷短语行 ---- */
    room_quick_row = lv_obj_create(win);
    lv_obj_set_size(room_quick_row, MSG_AREA_W, 40);
    lv_obj_set_pos(room_quick_row, MSG_AREA_X, QUICK_ROW_Y);
    lv_obj_set_style_bg_opa(room_quick_row, LV_OPA_TRANSP, 0);      /* 纯布局容器，透明 */
    lv_obj_set_style_border_width(room_quick_row, 0, 0);
    lv_obj_set_style_pad_all(room_quick_row, 0, 0);
    lv_obj_remove_flag(room_quick_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(room_quick_row, LV_FLEX_FLOW_ROW);         /* 短语横向一排 */
    lv_obj_set_style_pad_column(room_quick_row, 8, 0);
    lv_obj_set_flex_align(room_quick_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for(int i = 0; i < QUICK_COUNT; i++) {
        /* 按钮宽度按短语实际长度算，中文短语才不会被截断 */
        int w = 0;
        text_measure(QUICK_PHRASES[i], 14, 300, &w);
        mk_button(room_quick_row, QUICK_PHRASES[i], g_f_desc,
                  0, 0, w + 28, 34, C_CARD, C_TEXT,              /* +28 = 左右内边距 */
                  quick_btn_cb, (void *)QUICK_PHRASES[i]);       /* user_data 传短语本身 */
    }

    /* ---- 底部：输入行 ---- */
    room_input_row = lv_obj_create(win);
    lv_obj_set_size(room_input_row, MSG_AREA_W, INPUT_ROW_H);
    lv_obj_set_pos(room_input_row, MSG_AREA_X, INPUT_ROW_Y);
    lv_obj_set_style_bg_opa(room_input_row, LV_OPA_TRANSP, 0);      /* 纯布局容器 */
    lv_obj_set_style_border_width(room_input_row, 0, 0);
    lv_obj_set_style_pad_all(room_input_row, 0, 0);
    lv_obj_remove_flag(room_input_row, LV_OBJ_FLAG_SCROLLABLE);

    /* 三个控件横向摆：输入框 590 宽，发送 90，键盘 76，加起来 830 < 780? 不，
     * 这里用的是绝对坐标：输入框 0..590，发送 602..692，键盘 702..778，
     * 总宽 778 略小于容器 780 —— 用的是 set_pos 绝对定位，不是 flex。 */
    room_ta = mk_textarea(room_input_row, "说点什么…", g_f_norm, 0, 0, 590, INPUT_ROW_H);
    mk_button(room_input_row, "发送", g_f_norm, 602, 2, 90, 40,
              C_MY_BG, C_MY_TX, send_btn_cb, NULL);
    mk_button(room_input_row, "键盘", g_f_desc, 702, 2, 76, 40,
              C_CARD, C_TEXT, kb_toggle_cb, NULL);

    /* ---- 软键盘（默认隐藏，高度缩到 250 给候选条让位） ---- */
    /* 键盘挂在 scr 上而不是 win 上：兄弟对象后创建的在上层，保证键盘不被遮挡 */
    room_kb = lv_keyboard_create(scr);
    lv_obj_add_flag(room_kb, LV_OBJ_FLAG_HIDDEN);                   /* 默认不显示 */
    lv_obj_set_size(room_kb, 1024, KB_HEIGHT);                      /* 高度 250 而非默认 300 */
    lv_obj_align(room_kb, LV_ALIGN_BOTTOM_MID, 0, 0);               /* 贴屏幕底部 */
    /* LV_EVENT_ALL：焦点/失焦/回车/取消 四种事件都走同一个回调 */
    lv_obj_add_event_cb(room_ta, ta_focus_cb, LV_EVENT_ALL, room_kb);

    /* ---- 拼音输入法：给键盘挂上中文候选条（默认隐藏） ---- */
    room_ime = lv_ime_pinyin_create(scr);
    lv_ime_pinyin_set_keyboard(room_ime, room_kb);                  /* 绑定到聊天键盘 */
    lv_obj_set_style_text_font(room_ime, g_f_norm, 0);   /* 必须：驱动候选条同步中文字体 */
    ime_cand_style();       /* 候选条改成暗色主题 */
    ime_cand_layout();      /* 候选条贴到输入行上方 */

    /* ---- 初始化状态 ---- */
    peer_pill_update();     /* 显示"公共大厅" */

    sys_line_add("欢迎使用网络聊天室，左侧点选用户可切私聊");
    user_list_rebuild();    /* 首次填充在线列表（至少要显示出"公共大厅"） */

    lv_screen_load(scr);    /* 创建完立刻切换过去（和桌面 Launcher 的"一次性"约定不同） */
    return scr;
}

/* ------------------------------------------------------------------ */
/* 事件轮询定时器                                                     */
/* ------------------------------------------------------------------ */

/**
 * 每 100ms 被 LVGL 调用一次：把网络线程塞进队列的事件取出来处理。
 *
 * 【为什么要有它】
 *   网络线程不能碰 lv_*，只能往环形队列塞数据；
 *   这个定时器就是"收件人"，在自己的线程（LVGL 线程）里读队列并操作控件。
 *
 * 【为什么要 while 循环】
 *   100ms 内可能收到好几条事件（比如一批用户上下线），
 *   一次 tick 要把队列里现有的全处理完，只取一条会积压。
 *   循环靠 chat_poll_event() 返回 0（队列空）退出。
 *
 * 【两个"兜底"块是干什么的】
 *   环形队列深度有限（64），极端情况下事件会被挤掉（丢最新的）。
 *   如果被挤掉的恰好是"在线列表变了"或"连接断开"这种状态类事件，
 *   界面就会一直显示旧状态。所以每拍都主动比对一次底层状态，
 *   状态对不上就补一次刷新 —— 幂等操作，重复执行没有副作用。
 */
static void poll_cb(lv_timer_t *t)
{
    chat_event_t ev;

    (void)t;

    /* 把队列里积压的事件全部消费掉 */
    while(chat_poll_event(&ev)) {
        switch(ev.type) {
        case CHAT_EV_CONNECTED:
            sys_line_add(ev.text);
            room_status_set("● 已连接", C_ACCENT);
            break;

        case CHAT_EV_PUBLIC:
            /* 公共消息：灰色气泡靠左 */
            bubble_add(ev.from_name, ev.text, 0);
            break;

        case CHAT_EV_PRIVATE:
            /* 私聊消息加个前缀，和公共消息区分开 */
            {
                /* +16 给 "[私聊] " 这 6 个字节（UTF-8 下 [ 1 + 私聊 6 + ] 1 + 空格 1 ≈ 9），
                 * 留 16 字节余量足够 */
                char buf[CHAT_TEXT_LEN + 16];
                snprintf(buf, sizeof(buf), "[私聊] %s", ev.text);
                bubble_add(ev.from_name, buf, 0);
            }
            break;

        case CHAT_EV_SYS:
            /* 系统消息（上线/下线通知等）：居中灰字 */
            sys_line_add(ev.text);
            break;

        case CHAT_EV_USERS:
            /* 在线列表变了：记下最新版本号再重建列表 */
            online_ver_seen = chat_online_version();
            user_list_rebuild();
            break;

        case CHAT_EV_ERROR:
            /* 出错一般是连接断了：红灯 + 提示 */
            room_status_set("● 连接已断开", C_ERR);
            sys_line_add(ev.text);
            break;

        default:
            break;
        }
    }

    /* 兜底：在线列表版本变了但事件被挤掉了，也补一次刷新 */
    if(chat_online_version() != online_ver_seen) {
        online_ver_seen = chat_online_version();
        user_list_rebuild();
    }

    /* 兜底：连接状态变化时刷新状态灯（正常由事件驱动，这里防事件被挤掉） */
    {
        int cs = chat_is_connected();
        if(cs != last_conn_state) {
            last_conn_state = cs;
            room_status_set(cs ? "● 已连接" : "● 未连接", cs ? C_ACCENT : C_ERR);
            if(!cs) sys_line_add("与服务器的连接已断开");
        }
    }
}

/* ------------------------------------------------------------------ */
/* 连接页：连接按钮                                                   */
/* ------------------------------------------------------------------ */

static void conn_status_set(const char *text, uint32_t color)
{
    if(conn_status == NULL) return;
    lv_label_set_text(conn_status, text);
    lv_obj_set_style_text_color(conn_status, lv_color_hex(color), 0);
}

/**
 * "连接"按钮回调 —— 本文件里最"重"的一个回调。
 *
 * 【流程】
 *   1. 防重入：connecting 非 0 就直接 return（连点会创建多个连接）
 *   2. 取三个输入框的值 + 校验（空、端口范围）
 *   3. 把值存进 static 全局，下次进来还记得
 *   4. 收起键盘、恢复面板位置（不然用户看不到状态提示）
 *   5. lv_refr_now(NULL) 手动刷一次屏 —— 关键，原因见下
 *   6. chat_connect()（阻塞，最长 3 秒）
 *   7. 成功 → 建聊天页 + 切屏 + 启动轮询定时器；失败 → 显示错误
 *
 * 【lv_refr_now 为什么是关键的】
 *   chat_connect() 是阻塞调用，最长会占住 LVGL 线程 3 秒。
 *   这 3 秒里 lv_timer_handler() 一直没返回，屏幕完全不会刷新
 *   —— 上面刚设的"正在连接，请稍候…"根本画不出来，用户看到的是卡死画面。
 *   所以先手动 lv_refr_now(NULL) 把当前这一帧强制刷出去，再发起连接。
 *
 * 【为什么 chat_connect 一定要带超时】
 *   不带超时的 connect 在内网不通时会卡 75 秒以上（TCP 重传超时），
 *   界面就真死了。传 3000ms 让它最多占 3 秒，用户还能接受。
 */
static void conn_btn_cb(lv_event_t *e)
{
    const char *ip, *port, *nick;
    int p;

    (void)e;
    if(connecting) return;      /* 防重入：连接中不响应二次点击 */

    ip   = lv_textarea_get_text(conn_ip_ta);
    port = lv_textarea_get_text(conn_port_ta);
    nick = lv_textarea_get_text(conn_nick_ta);

    /* ---- 输入校验：任一为空就提示并中断 ---- */
    if(ip   == NULL || ip[0]   == '\0') { conn_status_set("请填写服务器 IP", C_ERR);   return; }
    if(port == NULL || port[0] == '\0') { conn_status_set("请填写端口", C_ERR);        return; }
    if(nick == NULL || nick[0] == '\0') { conn_status_set("请填写昵称", C_ERR);        return; }

    p = atoi(port);             /* 端口范围检查：atoi 失败返回 0，会被这个判断拦下 */
    if(p <= 0 || p > 65535) { conn_status_set("端口不合法（1~65535）", C_ERR); return; }

    /* 记下来，下次进来还是这些值 */
    /* 用 snprintf 而不是 strcpy：自带长度保护，不会溢出 */
    snprintf(conn_ip,   sizeof(conn_ip),   "%s", ip);
    snprintf(conn_port, sizeof(conn_port), "%s", port);
    snprintf(my_nick,   sizeof(my_nick),   "%s", nick);

    /* 收起键盘，让用户看到状态提示 */
    if(conn_kb) {
        lv_keyboard_set_textarea(conn_kb, NULL);        /* 先解绑再隐藏 */
        lv_obj_add_flag(conn_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if(conn_panel) lv_obj_set_y(conn_panel, CONN_PANEL_Y);   /* 面板复位 */

    connecting = 1;
    if(conn_btn_lab) lv_label_set_text(conn_btn_lab, "连接中…");    /* 按钮文字换成连接中 */
    conn_status_set("正在连接，请稍候…", C_ACCENT);

    /* 关键：chat_connect 是阻塞调用（最长 3 秒），期间 lv_timer_handler 不会返回，
     * 界面根本不刷新 —— 上面那两句提示用户看不到，屏幕会像卡死一样。
     * 手动刷一次，把「连接中…」先画出来再发起连接。 */
    lv_refr_now(NULL);

    /* chat_connect 内部带超时（3 秒内一定返回），不会把界面卡死太久 */
    if(chat_connect(conn_ip, p, my_nick, 3000) == 0) {
        /* 成功：建聊天页并切过去，连接页留着（返回时统一删） */
        if(chat_room_screen == NULL)
            chat_room_screen = room_create();       /* 首次：创建（内部已 lv_screen_load） */
        else
            lv_screen_load(chat_room_screen);       /* 二次进入：复用已有 screen */

        /* 轮询定时器也只建一次；退出时在 chat_exit_cb 里删掉 */
        if(poll_timer == NULL)
            poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);

        /* 版本置 -1 强制下一次重建在线列表（把最新在线用户显示出来） */
        online_ver_seen = -1;
        room_status_set("● 已连接", C_ACCENT);
        user_list_rebuild();
    }
    else {
        /* chat_last_error() 里是 chat.c 记下的具体原因（超时/被拒绝/名失败等） */
        conn_status_set(chat_last_error(), C_ERR);
    }

    /* 无论成败都恢复按钮和标志位 */
    if(conn_btn_lab) lv_label_set_text(conn_btn_lab, "连接");
    connecting = 0;
}

/* 连接页返回（未连接状态下直接回桌面） */
static void conn_back_cb(lv_event_t *e)
{
    (void)e;
    if(connecting) return;      /* 连接中不许退，和 chat_exit_cb 保持一致 */
    chat_exit_cb(NULL);         /* 复用同一套清理逻辑（此时没有聊天页，删的是空指针，安全） */
}

/* ------------------------------------------------------------------ */
/* 连接页：创建                                                       */
/* ------------------------------------------------------------------ */

/**
 * 创建连接页（本模块的对外入口，从桌面 Launcher 调进来）。
 *
 * 【为什么先建字体】
 * 后面所有控件都要用这些字体指针，必须先准备好，
 * 否则控件会退回 LVGL 默认字体（中文全变方框）。
 *
 * 【为什么每次进来都重置这几个全局】
 *   虽然 chat_exit_cb 已经清过一次，但这里是"防御性重置"：
 *   万一某条路径没走 exit（比如直接关机重启？不可能），
 *   或者以后有人改了退出流程，这里也不会带着脏状态进来。
 *
 * 【三个输入框的初始值来自 static 全局】
 *   conn_ip/conn_port/my_nick 存的是上次输入的（或默认值），
 *   mk_field 的 value 参数会预填进去，用户体验上"记住上次连的服务器"。
 *
 * @return 新建的 screen（保存到 chat_conn_screen，chat_exit_cb 里删）
 */
lv_obj_t *ui_chat_conn_init(void)
{
    lv_obj_t *scr, *win, *title, *lab;
    int y;

    /* 建 5 档字体，指针存全局供各控件使用 */
    g_f_tiny   = cn_font(12);
    g_f_desc   = cn_font(14);
    g_f_norm   = cn_font(18);
    g_f_bubble = cn_font(18);       /* 和 norm 同字号，但语义分开（气泡专用） */
    g_f_title  = cn_font(24);

    /* 每次进入重置会话状态 */
    connecting      = 0;
    target_id       = 0;
    online_ver_seen = -1;
    room_msg_count  = 0;

    scr = lv_obj_create(NULL);
    win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_image_src(win, BG_CHAT_PATH, 0);    /* 和聊天页共用背景图 */
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 顶部：返回 + 标题 */
    mk_button(win, "返回", g_f_norm, 20, 12, 90, 40, C_BG, C_TEXT, conn_back_cb, NULL);
    title = mk_label(win, "网络聊天室", g_f_title, 0xFFFFFF);
    lv_obj_set_pos(title, 130, 18);

    lab = mk_label(win, "局域网聊天 · 公共大厅与私聊", g_f_desc, C_DIM);
    lv_obj_set_pos(lab, 132, 50);

    /* 中央面板 */
    conn_panel = lv_obj_create(win);
    lv_obj_set_size(conn_panel, CONN_PANEL_W, CONN_PANEL_H);        /* 520x320 */
    lv_obj_set_pos(conn_panel, (1024 - CONN_PANEL_W) / 2, CONN_PANEL_Y);   /* 水平居中 */
    lv_obj_set_style_bg_color(conn_panel, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(conn_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(conn_panel, 1, 0);
    lv_obj_set_style_border_color(conn_panel, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_radius(conn_panel, 16, 0);
    lv_obj_set_style_pad_all(conn_panel, 24, 0);                    /* 内容宽 = 520-48 = 472 */
    lv_obj_remove_flag(conn_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 三个字段：一行一个（标签在左、输入框在右）
     * 行距 56 = 输入框高 44 + 间距 12 */
    conn_ip_ta   = mk_field(conn_panel, "服务器 IP", "例如 172.100.1.126", conn_ip,   0);
    conn_port_ta = mk_field(conn_panel, "端口",      "例如 8888",          conn_port, 56);
    conn_nick_ta = mk_field(conn_panel, "昵称",      "例如 玩家小明",       my_nick,  112);

    /* IP：只允许数字和点，最多 15 字符（255.255.255.255） */
    lv_textarea_set_accepted_chars(conn_ip_ta, "0123456789.");
    lv_textarea_set_max_length(conn_ip_ta, 15);

    /* 端口：只允许数字，最多 5 位 */
    lv_textarea_set_accepted_chars(conn_port_ta, "0123456789");
    lv_textarea_set_max_length(conn_port_ta, 5);

    /* 昵称：不限字符，但要给 '\0' 留一个字节，所以是 CHAT_NAME_LEN - 1 */
    lv_textarea_set_max_length(conn_nick_ta, CHAT_NAME_LEN - 1);

    /* 连接按钮 */
    {
        /* 宽度 472 = 面板内容宽，横贯整行；y=176 在三行字段（0/56/112，各高44）下方 */
        lv_obj_t *btn = mk_button(conn_panel, "连接", g_f_norm, 0, 176, 472, 52,
                                  C_MY_BG, C_MY_TX, conn_btn_cb, NULL);
        /* 取出按钮里的文字标签，后续要改成"连接中…" */
        conn_btn_lab = lv_obj_get_child(btn, 0);
    }

    /* 状态行 */
    conn_status = mk_label(conn_panel, "未连接", g_f_desc, C_DIM);
    lv_obj_set_pos(conn_status, 0, 240);    /* 176+52+12 = 240，按钮下方 */

    /* 软键盘：默认隐藏，输入框获得焦点时显示 */
    /* 三个输入框共用同一个键盘和同一个回调，回调靠 user_data(=conn_kb) 知道操作哪个键盘 */
    conn_kb = lv_keyboard_create(scr);
    lv_obj_add_flag(conn_kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(conn_ip_ta,   ta_focus_cb, LV_EVENT_ALL, conn_kb);
    lv_obj_add_event_cb(conn_port_ta, ta_focus_cb, LV_EVENT_ALL, conn_kb);
    lv_obj_add_event_cb(conn_nick_ta, ta_focus_cb, LV_EVENT_ALL, conn_kb);

    lv_screen_load(scr);
    return scr;
}
