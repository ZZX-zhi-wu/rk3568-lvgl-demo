/**
 * @file    login.c
 * @brief   登录界面
 *
 * 功能说明：
 *   - 项目入口界面，左侧是 Logo + 项目名「智趣魔方」，右侧是登录面板。
 *   - 登录面板包含：账号输入框、密码输入框（密码显示为 •）、登录按钮、游客模式按钮。
 *   - 账号密码校验：admin / 123456 登录成功，跳转到桌面；游客模式跳过校验直接进入。
 *   - 点击输入框会弹出软键盘（LVGL 自带键盘）。
 *
 * 全局变量说明：
 *   - select_app_screen：登录成功/游客模式后赋值为桌面屏（定义在 main_interface.c）。
 */
#include "login.h"
#include <string.h>

/* 中文字体路径：微软雅黑 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/**
 * 创建指定字号的中文字体（freetype）。
 * 注意：此函数没有缓存，每次调用都会新建字体，login 界面只创建一次所以可接受。
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

/* 存放两个输入框指针，打包后作为 user_data 传给登录按钮回调 */
typedef struct {
    lv_obj_t *account_text;   // 账号输入框
    lv_obj_t *passwd_text;    // 密码输入框
} login_text_t;

/*
 * 输入框键盘事件回调：
 * 输入框获得焦点时显示软键盘并绑定，失焦/按下回车时隐藏键盘。
 */
static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *key = lv_event_get_user_data(e);   // 软键盘对象（通过 user_data 传入）
    lv_obj_t *ta  = lv_event_get_target(e);      // 触发事件的输入框

    if(code == LV_EVENT_FOCUSED)
    {
        lv_keyboard_set_textarea(key, ta);
        lv_obj_remove_flag(key, LV_OBJ_FLAG_HIDDEN);
    }
    else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY)
    {
        lv_keyboard_set_textarea(key, NULL);
        lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 登录按钮回调：读取账号密码，校验 admin/123456，成功则跳转桌面 */
static void login_btn_click_cb(lv_event_t *e)
{
    login_text_t *t = lv_event_get_user_data(e);
    const char *account = lv_textarea_get_text(t->account_text);
    const char *passwd  = lv_textarea_get_text(t->passwd_text);
    printf("account:%s passwd:%s\n", account, passwd);

    if(strcmp(account, "admin") == 0 && strcmp(passwd, "123456") == 0)
    {
        printf("登录成功\n");
        select_app_screen = ui_select_app_screen();   // 跳转到桌面
    }
    else
    {
        printf("登录失败\n");
    }
}

/* 游客模式按钮回调：跳过校验，直接进入桌面 */
static void guest_btn_click_cb(lv_event_t *e)
{
    (void)e;
    printf("以游客身份进入\n");
    select_app_screen = ui_select_app_screen();
}

/* 登录界面初始化（构建所有控件并加载屏幕） */
static void login_init(void)
{
    // 三种字号的中文字体
    lv_font_t *font_title = cn_font(44);   // 项目名大标题
    lv_font_t *font_sub   = cn_font(18);   // 副标题
    lv_font_t *font_norm  = cn_font(20);   // 面板正文

    /* 创建屏幕和全屏窗口 */
    lv_obj_t *login_screen = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(login_screen);
    lv_obj_set_size(win, 1024, 600);

    /* 背景图（铺满全屏，去边框圆角内边距） */
    lv_obj_set_style_bg_image_src(win, "A:/work_space/bmp_pic/bg/bg_login.bmp", 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 左侧：Logo 图 */
    lv_obj_t *logo = lv_image_create(win);
    lv_image_set_src(logo, "A:/work_space/bmp_pic/logo/logo.bmp");
    lv_obj_set_pos(logo, 162, 150);

    /* 左侧：项目名（对齐到 Logo 下方） */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "智趣魔方");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_align_to(title, logo, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

    /* 左侧：副标题 */
    lv_obj_t *sub = lv_label_create(win);
    lv_label_set_text(sub, "体感多媒体娱乐终端");
    lv_obj_set_style_text_font(sub, font_sub, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9A9AAC), 0);
    lv_obj_align_to(sub, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);

    /* 右侧：登录面板（深色圆角卡片） */
    lv_obj_t *panel = lv_obj_create(win);
    lv_obj_set_size(panel, 360, 300);
    lv_obj_set_pos(panel, 590, 150);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);

    /* 账号标签 */
    lv_obj_t *account_lab = lv_label_create(panel);
    lv_label_set_text(account_lab, "账号");
    lv_obj_set_style_text_font(account_lab, font_norm, 0);
    lv_obj_set_style_text_color(account_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(account_lab, 4, 10);

    /* 账号输入框 */
    lv_obj_t *account_text = lv_textarea_create(panel);
    lv_obj_set_size(account_text, 320, 48);
    lv_obj_set_pos(account_text, 4, 42);
    lv_textarea_set_one_line(account_text, true);
    lv_textarea_set_placeholder_text(account_text, "请输入账号");
    lv_obj_set_style_bg_color(account_text, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(account_text, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(account_text, 1, 0);
    lv_obj_set_style_border_color(account_text, lv_color_hex(0x2B2B3A), 0);
    lv_obj_set_style_radius(account_text, 9, 0);
    lv_obj_set_style_text_color(account_text, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_style_text_font(account_text, font_norm, 0);

    /* 密码标签 */
    lv_obj_t *passwd_lab = lv_label_create(panel);
    lv_label_set_text(passwd_lab, "密码");
    lv_obj_set_style_text_font(passwd_lab, font_norm, 0);
    lv_obj_set_style_text_color(passwd_lab, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(passwd_lab, 4, 104);

    /* 密码输入框（密码模式，输入显示成 •） */
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

    /* 登录按钮（绿色） */
    lv_obj_t *login_btn = lv_button_create(panel);
    lv_obj_set_size(login_btn, 150, 50);
    lv_obj_set_pos(login_btn, 4, 202);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(login_btn, 12, 0);
    lv_obj_t *login_lab = lv_label_create(login_btn);
    lv_label_set_text(login_lab, "登录");
    lv_obj_set_style_text_font(login_lab, font_norm, 0);
    lv_obj_set_style_text_color(login_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(login_lab);

    /* 游客模式按钮（描边） */
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

    /* 把两个输入框打包成结构体，作为 user_data 传给登录按钮回调 */
    static login_text_t inputs;
    inputs.account_text = account_text;
    inputs.passwd_text  = passwd_text;
    lv_obj_add_event_cb(login_btn, login_btn_click_cb, LV_EVENT_CLICKED, &inputs);
    lv_obj_add_event_cb(guest_btn, guest_btn_click_cb, LV_EVENT_CLICKED, NULL);

    /* 软键盘：默认隐藏，输入框获得焦点时由事件回调显示 */
    lv_obj_t *key = lv_keyboard_create(login_screen);
    lv_obj_add_flag(key, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(account_text, textarea_event_cb, LV_EVENT_ALL, key);
    lv_obj_add_event_cb(passwd_text,  textarea_event_cb, LV_EVENT_ALL, key);

    lv_screen_load(login_screen);
}

/* 登录界面入口函数（供 main.c 调用） */
void login(void)
{
    login_init();
}
