/**
 * @file    login.c
 * @brief   登录界面
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 项目入口界面，左侧是 Logo + 项目名「智趣魔方」，右侧是登录面板。
 *   - 登录面板包含：账号输入框、密码输入框（密码显示为 •）、登录按钮、游客模式按钮。
 *   - 账号密码校验：admin / 123456 登录成功，跳转到桌面；游客模式跳过校验直接进入。
 *   - 点击输入框会弹出软键盘（LVGL 自带键盘）。
 *
 * =====================================================================
 * 二、界面布局（1024×600）
 * =====================================================================
 *   左侧                          右侧
 *   ┌──────────────────┐         ┌──────────────────┐
 *   │  Logo 图(162,150)│         │  登录面板(590,150)│
 *   │        ↓         │         │  360×300 圆角卡片 │
 *   │  智趣魔方 (44号) │         │  账号 / 密码 / 两个按钮
 *   │        ↓         │         └──────────────────┘
 *   │ 体感多媒体娱乐终端│
 *   └──────────────────┘
 *
 *   两个标题用 lv_obj_align_to() 挂在 Logo/上一个标题的**外侧下方**，
 *   所以改 Logo 位置时下面两个标题会自动跟着走，不用逐个改坐标。
 *
 * =====================================================================
 * 三、几个已知问题（知道就好，本次不修改）
 * =====================================================================
 *   1) 登录失败时**界面没有任何反馈**，只在串口 printf 一句。
 *      用户看到的是一点反应都没有，很容易以为按钮坏了。
 *      更好的做法是像 chat_ui 那样加一个状态标签，显示"账号或密码错误"。
 *
 *   2) 密码被明文打印到串口日志：printf("account:%s passwd:%s\n", ...)。
 *      这在正式产品里是不可接受的（日志会被记录、会被别人看到），
 *      演示项目里问题不大，但答辩时如果被问到安全性，这是要主动承认的点。
 *      同理，账号密码是硬编码在代码里的，没有真正的校验服务。
 *
 *   3) 登录屏创建后**没有全局指针持有**，也从不删除，
 *      所以它会在内存里一直留着（在本工程里不算问题，
 *      因为 login() 由 main.c 只在启动时调用一次）。
 *
 *   4) 键盘回调少了 LV_EVENT_CANCEL 分支：
 *      按软键盘上的"取消"键时键盘不会自动收起（chat_ui.c 的
 *      ta_focus_cb 是处理了 CANCEL 的，可以对照看）。
 */
#include "login.h"
#include <string.h>

/* 中文字体路径：微软雅黑 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/**
 * 创建指定字号的中文字体（freetype）。
 *
 * 【为什么不带缓存（和其他模块不一样）】
 *   其他模块（chat_ui / album / settings / game_center）的 cn_font
 *   都有一个 static 缓存数组，避免同字号重复创建。
 *   这里没有缓存 —— 因为登录界面全生命周期只构建一次、
 *   只用到 3 档字号，重复创建的风险为零，加了缓存反而多一层逻辑。
 *
 *   代价：如果以后 login 被反复调用（比如做成"退出登录"再回来），
 *   每调一次就会多创建 3 个字体对象，而这些对象不会释放（见下）。
 *
 * 【关于释放】
 *   lv_freetype_font_create 在 LVGL v9 里没有对应的销毁函数，
 *   freetype 模块自己管理字体内存池，所以"创建了不释放"是设计如此，
 *   不是本代码漏写。但反复创建同字号字体仍会白占内存池空间，
 *   这就是其他模块要加缓存的原因。
 *
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL
 */
static lv_font_t *cn_font(int size)
{
    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f)
        LV_LOG_ERROR("freetype font create failed: %s", CN_FONT_PATH);
    return f;
}

/**
 * 存放两个输入框指针，打包后作为 user_data 传给登录按钮回调。
 *
 * 【为什么要定义一个结构体，而不是把两个指针都塞进 user_data】
 *   user_data 只有一个 void*，装不下两个指针。
 *   所以把两个输入框打包成一个结构体，把结构体地址传过去。
 *
 * 【为什么在 login_init 里声明成 static】
 *   如果用局部变量 `login_text_t inputs;`，函数返回后这块栈内存就失效了，
 *   而回调是**之后**才被触发的，那时读到的就是被覆盖过的垃圾值 → 崩溃。
 *   加 static 让它变成静态存储期，生命周期贯穿整个程序，就安全了。
 *   这是"用 user_data 传结构体"时的标准做法。
 */
typedef struct {
    lv_obj_t *account_text;   // 账号输入框
    lv_obj_t *passwd_text;    // 密码输入框
} login_text_t;

/*
 * 输入框键盘事件回调：输入框获得焦点时显示软键盘并绑定，
 * 失焦/按下回车时隐藏键盘。
 *
 * 一个回调同时服务两个输入框（账号和密码），靠两个手段区分：
 *   - lv_event_get_target(e)    拿到"是哪个输入框触发的"，绑定给键盘
 *   - lv_event_get_user_data(e) 拿到键盘对象（注册时用 user_data 传进来）
 *
 * 监听 LV_EVENT_ALL（所有事件），但只处理其中两种：
 *   FOCUSED   获得焦点 → 弹键盘（点输入框时触发）
 *   DEFOCUSED 失去焦点 → 收键盘（点了别处）
 *   READY     单行输入框按回车 → 收键盘（视为输入完成）
 *
 * 【已知缺口】没有处理 LV_EVENT_CANCEL（软键盘上的取消键），
 * 按取消键时键盘不会收起。chat_ui.c 的同名回调处理了这一种，
 * 要做一致的话这里补一个 || code == LV_EVENT_CANCEL 即可。
 */
static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *key = lv_event_get_user_data(e);   // 软键盘对象（通过 user_data 传入）
    lv_obj_t *ta  = lv_event_get_target(e);      // 触发事件的输入框

    if(code == LV_EVENT_FOCUSED)
    {
        /* 先把键盘"接到"这个输入框上，之后键盘敲的字符才会进这个框 */
        lv_keyboard_set_textarea(key, ta);
        lv_obj_remove_flag(key, LV_OBJ_FLAG_HIDDEN);   /* 去掉隐藏标志 = 显示 */
    }
    else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY)
    {
        /* 先解绑再隐藏：解绑能防止键盘后续操作一个已经不可见的输入框 */
        lv_keyboard_set_textarea(key, NULL);
        lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN);      /* 加上隐藏标志 = 隐藏 */
    }
}

/**
 * 登录按钮回调：读取账号密码，校验 admin/123456，成功则跳转桌面。
 *
 * 【校验方式说明】
 *   管理员账号密码是**硬编码**在代码里的（明文），没有后端服务、
 *   没有加密、没有盐值。这是演示项目的常见做法 ——
 *   目的是让助教/评委能"一键进去看功能"，而不是做一个真的认证系统。
 *   答辩被问到安全性时，正确的回答是承认这一点，
 *   并说明要改成什么样（服务端校验 / 密码哈希存储 / 不打印日志）。
 *
 * 【失败时的问题】
 *   只 printf，界面无任何反馈。见文件头的"已知问题 1"。
 *
 * 【成功时的跳转】
 *   ui_select_app_screen() 会创建桌面屏并 lv_screen_load 它，
 *   同时返回屏幕指针存进全局 select_app_screen（供各子界面返回时复用）。
 *   注意登录屏没有被删除 —— 它仍在内存里，只是不再是当前显示屏。
 */
static void login_btn_click_cb(lv_event_t *e)
{
    login_text_t *t = lv_event_get_user_data(e);
    const char *account = lv_textarea_get_text(t->account_text);
    const char *passwd  = lv_textarea_get_text(t->passwd_text);
    printf("account:%s passwd:%s\n", account, passwd);   /* ⚠ 明文打印密码，仅演示用 */

    if(strcmp(account, "admin") == 0 && strcmp(passwd, "123456") == 0)
    {
        printf("登录成功\n");
        select_app_screen = ui_select_app_screen();   // 跳转到桌面
    }
    else
    {
        printf("登录失败\n");                          // ⚠ 界面上看不到任何提示
    }
}

/**
 * 游客模式按钮回调：跳过校验，直接进入桌面。
 *
 * 存在的意义：演示/验收时不用输账号密码，一点就进，
 * 避免现场因为输错密码而尴尬。
 * 注意它和登录按钮走的是**同一个**进入桌面的动作，
 * 区别只是跳过 strcmp 校验。
 */
static void guest_btn_click_cb(lv_event_t *e)
{
    (void)e;                                  /* 参数没用，显式 void 掉，避免编译器告警 */
    printf("以游客身份进入\n");
    select_app_screen = ui_select_app_screen();
}

/**
 * 登录界面初始化（构建所有控件并加载屏幕）。
 *
 * 函数末尾会 lv_screen_load(login_screen)，属于"创建即显示"风格。
 *
 * 注意 login_screen 是局部变量、没有存进全局 ——
 * 也就是说函数返回后就再也没人持有这个指针了，
 * 这一屏从此刻起永远不会被删除（本工程里无所谓，见文件头说明）。
 */
static void login_init(void)
{
    // 三种字号的中文字体
    /* 44 号用于"智趣魔方"大标题；18 号用于副标题；20 号用于面板内的正文和按钮 */
    lv_font_t *font_title = cn_font(44);   // 项目名大标题
    lv_font_t *font_sub   = cn_font(18);   // 副标题
    lv_font_t *font_norm  = cn_font(20);   // 面板正文

    /* 创建屏幕和全屏窗口
     * scr：LVGL 的"屏对象"，不带默认样式，作为最底层容器
     * win：铺满 1024×600 的窗口，所有元素都挂在它上面 */
    lv_obj_t *login_screen = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(login_screen);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图（铺满全屏，去边框圆角内边距）
     * 四行样式是固定套路：LVGL 的容器默认有边框、圆角和内边距，
     * 不清掉的话背景图四周会露出一圈底色、坐标也会整体偏移。 */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_login.bmp", 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 左侧：Logo 图（'A:' 是 LVGL 虚拟盘符，映射到板子根目录） */
    lv_obj_t *logo = lv_image_create(win);
    lv_image_set_src(logo, "A:/work_space/bmp_pic/logo/logo.bmp");
    lv_obj_set_pos(logo, 162, 150);

    /* 左侧：项目名（对齐到 Logo 下方）
     * lv_obj_align_to(自己, 参照物, 对齐方式, x偏移, y偏移)
     * LV_ALIGN_OUT_BOTTOM_MID = 贴到参照物底部中间的外侧，再往下 16px。
     * 好处：Logo 挪位置时标题自动跟随，不用手动同步坐标。 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "智趣魔方");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_align_to(title, logo, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    /* 左侧：副标题（同理，挂在项目名下方 10px） */
    lv_obj_t *sub = lv_label_create(win);
    lv_label_set_text(sub, "体感多媒体娱乐终端");
    lv_obj_set_style_text_font(sub, font_sub, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* 右侧：登录面板（深色圆角卡片）
     * 360×300，内边距 20 → 内容区 320×260
     * 位置 (590,150)：右侧留 1024-590-360 = 74px 边距，和左侧 Logo 形成左右分栏 */
    lv_obj_t *panel = lv_obj_create(win);
    lv_obj_set_size(panel, 360, 300);
    lv_obj_set_pos(panel, 590, 150);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);

    /* ---- 以下坐标都是相对 panel 内容区的 ----
     * 内容区高 260，各行排布：
     *   y=10   账号标签（高约 20）
     *   y=42   账号输入框（高 48）→ 结束于 90
     *   y=104  密码标签 → 中间留 14px 呼吸空隙
     *   y=136  密码输入框（高 48）→ 结束于 184
     *   y=202  两个按钮（高 50）→ 结束于 252，内容区 260，正好留 8px 底部余量
     * x 一律用 4（轻微内缩，视觉上不贴边） */

    /* 账号标签 */
    lv_obj_t *account_lab = lv_label_create(panel);
    lv_label_set_text(account_lab, "账号");
    lv_obj_set_style_text_font(account_lab, font_norm, 0);
    lv_obj_set_style_text_color(account_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(account_lab, 4, 10);

    /* 账号输入框（宽 320 = 内容区宽度，正好占满） */
    lv_obj_t *account_text = lv_textarea_create(panel);
    lv_obj_set_size(account_text, 320, 48);
    lv_obj_set_pos(account_text, 4, 42);
    lv_textarea_set_one_line(account_text, true);                  /* 单行：回车即"完成"，换行键变成确定键 */
    lv_textarea_set_placeholder_text(account_text, "请输入账号");   /* 空时的灰色提示文字 */
    lv_obj_set_style_bg_color(account_text, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(account_text, LV_OPA_COVER, 0);        /* 必须设，否则底色不显示 */
    lv_obj_set_style_border_width(account_text, 1, 0);
    lv_obj_set_style_border_color(account_text, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(account_text, 9, 0);
    lv_obj_set_style_text_color(account_text, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_style_text_font(account_text, font_norm, 0);        /* 必须设中文字体，否则中文输入显示方框 */

    /* 密码标签 */
    lv_obj_t *passwd_lab = lv_label_create(panel);
    lv_label_set_text(passwd_lab, "密码");
    lv_obj_set_style_text_font(passwd_lab, font_norm, 0);
    lv_obj_set_style_text_color(passwd_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(passwd_lab, 4, 104);

    /* 密码输入框（密码模式，输入显示成 •）
     * lv_textarea_set_password_mode(ta, true) 会让输入内容以圆点回显，
     * 但**内部真实文本仍然是明文**，lv_textarea_get_text 拿到的就是原文
     * —— 校验能用，只是显示被遮住。所以这只是"看得见的防护"，不是加密。
     * 圆点符号可以用 lv_textarea_set_password_bullet() 换成别的字符。 */
    lv_obj_t *passwd_text = lv_textarea_create(panel);
    lv_obj_set_size(passwd_text, 320, 48);
    lv_obj_set_pos(passwd_text, 4, 136);
    lv_textarea_set_one_line(passwd_text, true);
    lv_textarea_set_password_mode(passwd_text, true);
    lv_textarea_set_placeholder_text(passwd_text, "请输入密码");
    lv_obj_set_style_bg_color(passwd_text, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(passwd_text, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(passwd_text, 1, 0);
    lv_obj_set_style_border_color(passwd_text, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(passwd_text, 9, 0);
    lv_obj_set_style_text_color(passwd_text, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_style_text_font(passwd_text, font_norm, 0);

    /* 登录按钮（绿色）
     * 位置 x=4 宽 150；游客按钮 x=170 宽 150 → 4+150=154，间隔 16，170+150=320，
     * 正好铺满 320 的内容区宽度，两个按钮等宽对称。 */
    lv_obj_t *login_btn = lv_button_create(panel);
    lv_obj_set_size(login_btn, 150, 50);
    lv_obj_set_pos(login_btn, 4, 202);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(0x1D9E75), 0);   /* 主色绿，突出主操作 */
    lv_obj_set_style_radius(login_btn, 12, 0);
    lv_obj_t *login_lab = lv_label_create(login_btn);
    lv_label_set_text(login_lab, "登录");
    lv_obj_set_style_text_font(login_lab, font_norm, 0);
    lv_obj_set_style_text_color(login_lab, lv_color_hex(0x06130E), 0); /* 深色字配亮绿底，对比度高 */
    lv_obj_center(login_lab);

    /* 游客模式按钮（描边样式：深底 + 细边框，视觉上是"次要操作"）
     * 主按钮用实心绿、次按钮用描边，这是很常见的层级区分手法。 */
    lv_obj_t *guest_btn = lv_button_create(panel);
    lv_obj_set_size(guest_btn, 150, 50);
    lv_obj_set_pos(guest_btn, 170, 202);
    lv_obj_set_style_bg_color(guest_btn, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(guest_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(guest_btn, 1, 0);
    lv_obj_set_style_border_color(guest_btn, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(guest_btn, 12, 0);
    lv_obj_t *guest_lab = lv_label_create(guest_btn);
    lv_label_set_text(guest_lab, "游客模式");
    lv_obj_set_style_text_font(guest_lab, font_norm, 0);
    lv_obj_set_style_text_color(guest_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_center(guest_lab);

    /* 把两个输入框打包成结构体，作为 user_data 传给登录按钮回调
     * ★ static 是必须的：局部变量的地址在函数返回后就失效了，
     *   而回调是稍后才触发的，那时会读到垃圾数据。
     *   静态存储期保证这块内存在程序整个运行期间都有效。 */
    static login_text_t inputs;
    inputs.account_text = account_text;
    inputs.passwd_text  = passwd_text;
    lv_obj_add_event_cb(login_btn, login_btn_click_cb, LV_EVENT_CLICKED, &inputs);
    lv_obj_add_event_cb(guest_btn, guest_btn_click_cb, LV_EVENT_CLICKED, NULL);   /* 游客按钮不需要输入框 */

    /* 软键盘：默认隐藏，输入框获得焦点时由事件回调显示
     * 挂在 login_screen 上而不是 panel 上 —— 键盘在 panel 之外也能显示，
     * 挂 panel 上会被 panel 的裁剪区域(360×300)切掉。 */
    lv_obj_t *key = lv_keyboard_create(login_screen);
    lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN);
    /* 两个输入框共用同一个键盘，回调靠 target 区分操作哪个框 */
    lv_obj_add_event_cb(account_text, textarea_event_cb, LV_EVENT_ALL, key);
    lv_obj_add_event_cb(passwd_text,  textarea_event_cb, LV_EVENT_ALL, key);

    lv_screen_load(login_screen);   /* 创建完立刻显示 */
}

/* 登录界面入口函数（供 main.c 调用）：只是 login_init 的一层包装 */
void login(void)
{
    login_init();
}
