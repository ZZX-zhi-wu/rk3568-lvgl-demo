/**
 * @file    settings.c
 * @brief   设置界面
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 屏幕亮度：滑条调节，写 /sys/class/backlight/backlight/brightness。
 *   - 自动亮度：开关，开启后每 1 秒读 BH1750 环境光传感器，自动调节背光。
 *   - 体感校准：按钮，调用 imu_calibrate() 把当前姿态设为体感零点。
 *   - 关于：版本信息。
 *
 * =====================================================================
 * 二、硬件说明
 * =====================================================================
 *   - 背光 PWM 是反极性的（写 255 最暗），所以 set_backlight 里做了反相。
 *   - 环境光传感器 BH1750 挂在 IIO 的 iio:device2。
 *
 * =====================================================================
 * 三、布局（1024×600，无背景图，纯深色底）
 * =====================================================================
 *   y=12    [返回]  设置
 *   y=100   屏幕亮度   [════滑条═══]  80%
 *   y=190   自动亮度   [开关]
 *   y=280   体感校准   [校准]
 *   y=370   智趣魔方 · 版本 1.0
 *
 *   四个设置项都是"左边标签 + 右边控件"的两列布局，
 *   右侧控件的 x 坐标统一在 700~870 之间，视觉上对齐成一列。
 *
 * =====================================================================
 * 四、几个值得注意的地方（本次只在注释里点出，未修改代码）
 * =====================================================================
 *   1) 自动亮度里 lv_slider_set_value() 会**再次触发滑条回调**。
 *      见 auto_bright_timer_cb 的说明 —— 结果是背光被重复写了一次。
 *
 *   2) 亮度设置不落盘。退出设置页再进来，滑条又是默认的 80%，
 *      但背光还是上一轮自动调好的值 —— 界面显示和实际亮度会不一致。
 *      要修的话：把亮度存进文件/环境变量，或在进入时读一次
 *      /sys/class/backlight/backlight/brightness 反推百分比。
 *
 *   3) read_illuminance_lux() 与 sensor.c 的 sensor_illuminance()
 *      功能完全重复。本文件没有 include sensor.h，而是自己写了一遍
 *      —— 属于代码重复。好处是设置模块不依赖 sensor 模块，
 *      坏处是三处（sensor.c / 本文件 / 以后可能还有）读同一份逻辑。
 *
 *   4) cn_font 的缓存数组缺 `if(empty < 0) return NULL;` 保护，
 *      和 game_center.c / album.c 一样（详见该函数注释）。
 *      当前只用 2 档字号，缓存 4 槽，安全。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "imu.h"


/* 中文字体路径 */
#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 背光 sysfs 目录
 * 下面两个文件在驱动注册背光设备时由内核生成：
 *   brightness      当前亮度（可读可写）
 *   max_brightness  最大亮度值（只读，通常 255，但不要写死） */
#define BACKLIGHT_DIR "/sys/class/backlight/backlight"
/* 背光极性：1=反相(写255最暗) 0=正向(写255最亮)。滑条往右反而变暗就置1
 *
 * 【为什么会反相】
 *   屏的背光是由 PWM 占空比控制的，而这个开发板上的 PWM 通过一个
 *   反相电路接到了背光使能脚上：占空比越大、实际电压越低、屏越暗。
 *   所以内核看到的 brightness 值和"人感觉到的亮度"是反的。
 *   把反相这件事**收敛到一个宏 + 一行代码**里，好处是全工程其他地方
 *   都只需要按"0=最暗、100=最亮"的直觉思考，不会被硬件怪癖污染。 */
#define BACKLIGHT_INVERTED 1

/* 环境光传感器 BH1750 的 IIO 路径
 * input 是内核算好的 lux；raw×scale 是后备方案。 */
#define ILLUM_INPUT_PATH "/sys/bus/iio/devices/iio:device2/in_illuminance_input"
#define ILLUM_RAW_PATH   "/sys/bus/iio/devices/iio:device2/in_illuminance_raw"
#define ILLUM_SCALE_PATH "/sys/bus/iio/devices/iio:device2/in_illuminance_scale"

/* 自动亮度采样周期（毫秒）
 * 1000ms 是个保守值：人眼对亮度变化的适应本来就慢，
 * 调太快反而显得屏幕在"闪"，而且每次都要读文件+写文件，没必要。 */
#define AUTO_BRIGHT_PERIOD 1000

/* 设置屏指针：进来创建、返回时删除并置 NULL */
lv_obj_t * settings_screen = NULL;

/* 这两个控件必须在模块级，因为自动亮度定时器回调需要操作它们，
 * 而定时器回调的参数只有 lv_timer_t*，拿不到创建时的局部变量。 */
static lv_timer_t *auto_bright_timer = NULL;   // 自动亮度定时器
static lv_obj_t *bright_slider = NULL;         // 亮度滑条（全局，供自动亮度回调访问）
static lv_obj_t *bright_val = NULL;            // 亮度百分比标签（全局）

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * 缓存 4 槽，本文件只用 2 档（24 标题、22 正文），够用。
 *
 * 【已知隐患】和 game_center.c / album.c 一样，缺一行
 *   if(empty < 0) return NULL;
 * 万一以后用到第 5 档字号，`cache[-1] = f;` 会越界写。
 * 现在字号种类固定，所以不会触发；加字号前请先把这行补上。
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[4] = {NULL};
    static int cache_size[4] = {0};
    int empty = -1;
    for(int i = 0; i < 4; i++) {
        if(cache[i] != NULL && cache_size[i] == size)
            return cache[i];               /* 命中缓存 */
        if(cache[i] == NULL && empty < 0)
            empty = i;                     /* 记下第一个空槽 */
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
 * 读取环境光照度（单位 lux）。
 * 优先读 in_illuminance_input（已经是 lux），否则读 in_illuminance_raw 乘 scale。
 *
 * 【为什么不直接用 sensor_illuminance()】
 *   本文件没有 include sensor.h，而是把同样的逻辑又写了一遍。
 *   好处：设置模块不依赖传感器模块，能独立编译测试。
 *   坏处：同一份逻辑在工程里存在两份，改了一处忘了另一处就会不一致。
 *   这是典型的"为了避免依赖而接受重复"的取舍，规模小的时候可以接受。
 *
 * @return lux 值；两条路都失败返回 0（表现为"环境很暗"→ 背光降到 30%）
 */
static int read_illuminance_lux(void)
{
    FILE *fp = fopen(ILLUM_INPUT_PATH, "r");
    if(fp) {
        int v = 0;
        fscanf(fp, "%d", &v);
        fclose(fp);
        return v;                       /* 首选路径：内核已换算好 */
    }
    /* 回退路径：自己乘 scale */
    int raw = 0;
    float scale = 1.0f;
    fp = fopen(ILLUM_RAW_PATH, "r");
    if(fp) { fscanf(fp, "%d", &raw); fclose(fp); }
    fp = fopen(ILLUM_SCALE_PATH, "r");
    if(fp) { fscanf(fp, "%f", &scale); fclose(fp); }
    return (int)(raw * scale);
}

/**
 * 照度(lux) -> 背光百分比：0~1000 lux 线性映射到 30%~100%（最低 30% 兜底）。
 *
 * 三段式，逐段解释：
 *   lux ≤ 10      → 30     下限保护。完全黑暗时如果还给 0%，
 *                          用户会以为设备关机了，什么都看不见。
 *                          留 30% 保证"再暗也能看清屏幕"。
 *   lux ≥ 1000    → 100    上限保护。强光下（比如窗边、户外）
 *                          再往上加也没有意义，直接给满。
 *   介于两者之间   → 30 + (lux-10) × 70 / 990
 *                          把 10~1000 这一段线性拉伸到 30~100。
 *                          分母 990 而不是 1000，是因为起点是 10：
 *                          (1000-10)=990，这样 lux=1000 时正好得 100。
 *
 * ★ 全程用整数运算、没有任何浮点 —— 因为 lv_conf.h 里 LV_USE_FLOAT=0，
 *   浮点会被裁掉。整数除法 (lux-10)*70/990 的精度足够（1% 的粒度）。
 */
static int lux_to_percent(int lux)
{
    if(lux <= 10)    return 30;
    if(lux >= 1000)  return 100;
    return 30 + (lux - 10) * 70 / 990;
}


/**
 * 设置背光亮度（0~100），内部做反相并写入 brightness 文件。
 *
 * 三个步骤：
 *   ① 从 max_brightness 读上限（不写死 255 —— 不同板子/驱动可能不同）
 *   ② 百分比换成设备值：val = percent × max / 100
 *   ③ 如果背光是反相的，做一次 val = max - val，再写进 brightness
 *
 * 例：percent=80, max=255 → val=204 → 反相后写 51（数值小 = 更亮）。
 *
 * 注意本函数**每次都重新读 max_brightness**（多一次文件读），
 * 属于"用性能换正确性"：max 理论上不会变，读一次缓存起来更好，
 * 但每次读也就几十微秒，对本场景无所谓。
 *
 * @param percent 0~100，越大越亮（函数内部负责处理硬件极性）
 */
static void set_backlight(int percent)
{
    char path[128];
    int max = 255;
    snprintf(path, sizeof(path), BACKLIGHT_DIR "/max_brightness");
    FILE *fp = fopen(path, "r");
    if(fp) { fscanf(fp, "%d", &max); fclose(fp); }   /* 读不到就沿用默认 255 */

    int val = percent * max / 100;
#if BACKLIGHT_INVERTED
    val = max - val;   // 反相：percent 越大 -> 实际写入值越小（越亮）
#endif

    snprintf(path, sizeof(path), BACKLIGHT_DIR "/brightness");
    fp = fopen(path, "w");
    if(fp) { fprintf(fp, "%d", val); fclose(fp); }
    else   { printf("写背光失败: %s\n", path); }     /* 打开失败大多是权限问题 */
}

/**
 * 亮度滑条回调：更新百分比标签并写入背光。
 *
 * 监听 LV_EVENT_VALUE_CHANGED —— 滑条拖动过程中会连续触发，
 * 所以这里是"边拖边生效"的实时效果，不需要确认按钮。
 *
 * user_data 传的是百分比标签对象（注册时用 bright_val 传入），
 * 所以回调里不用去访问全局变量，更干净。
 */
static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *val_lab = lv_event_get_user_data(e);
    int v = lv_slider_get_value(slider);
    lv_label_set_text_fmt(val_lab, "%d%%", v);       /* 用 set_text_fmt 而不是自己 snprintf，省一步 */
    set_backlight(v);
}

/**
 * 自动亮度定时器回调：读照度 -> 映射百分比 -> 写背光并更新界面。
 *
 * 每一拍做四件事：
 *   ① 读 lux（可能失败返回 0，那就当"很暗"处理）
 *   ② 映射成 0~100 的百分比
 *   ③ 写背光硬件
 *   ④ 把界面上的滑条和百分比标签同步到新值
 *
 * ★ 一个值得注意的副作用：
 *   第 ④ 步的 lv_slider_set_value() 在**数值确实发生变化时**
 *   会发出 LV_EVENT_VALUE_CHANGED，于是 brightness_cb 又被回调一次，
 *   结果 set_backlight() 被**重复调用了一遍**（写同一个值）。
 *
 *   后果：多一次文件写，功能上无害，也不会无限递归
 *   （值相同时 lv_bar 不再发事件）。
 *
 *   但这是一个"隐患"：如果以后在 brightness_cb 里加入了别的动作
 *   （写日志、发网络请求、播放音效），这里的重复触发就会变成真问题。
 *   更干净的做法是用 lv_obj_remove_event_cb 在自动模式下临时摘掉回调，
 *   或者给 brightness_cb 加一个"正在自动更新"的标志位。
 *
 * 参数 t 没被用到 —— 因为需要的东西都在 static 全局里
 * （auto_bright_cb 里也手动传 NULL 调过一次本函数，所以不能假设 t 非空）。
 */
static void auto_bright_timer_cb(lv_timer_t *t)
{
    int lux = read_illuminance_lux();
    int percent = lux_to_percent(lux);
    set_backlight(percent);
    if(bright_val != NULL)
        lv_label_set_text_fmt(bright_val, "%d%%", percent);
    if(bright_slider != NULL)
        lv_slider_set_value(bright_slider, percent, LV_ANIM_OFF);   /* 用 OFF：跟随式更新，不要动画 */
    printf("[auto] lux=%d -> backlight %d%%\n", lux, percent);      /* 串口日志，方便观察效果 */
}

/**
 * 自动亮度开关回调：开启时启动定时器并禁用手动滑条，关闭时反之。
 *
 * 【为什么用 lv_obj_has_state(sw, LV_STATE_CHECKED) 判断开还是关】
 *   lv_switch 这类"有状态控件"把开关状态存在 LV_STATE_CHECKED 里，
 *   事件触发时状态**已经**更新过了，所以直接读它最准确。
 *   不要用"上次是开还是关"这种自己维护的变量，容易和实际状态不同步。
 *
 * 【为什么立即手动调一次 auto_bright_timer_cb(NULL)】
 *   定时器要过 1000ms 才第一次触发，用户点开开关后会有 1 秒
 *   "好像没反应"的空窗。手动立刻执行一次，效果立竿见影。
 *   传 NULL 是安全的 —— 那个函数没用参数（见它的说明）。
 *
 * 【为什么要 DISABLED 滑条】
 *   自动模式和手动滑条同时在改背光会互相打架：
 *   用户刚拖到 50%，下一秒定时器又按环境光覆盖掉，体验很差。
 *   禁用滑条让"当前谁在控制"这件事在界面上是明确的。
 */
static void auto_bright_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    if(on) {
        /* 懒创建：只有真开了才建定时器（然后一直复用到关闭） */
        if(auto_bright_timer == NULL)
            auto_bright_timer = lv_timer_create(auto_bright_timer_cb, AUTO_BRIGHT_PERIOD, NULL);
        auto_bright_timer_cb(NULL);                       // 立即执行一次，不用等 1 秒
        if(bright_slider != NULL)
            lv_obj_add_state(bright_slider, LV_STATE_DISABLED);   // 自动时禁用手动滑条
        printf("自动亮度：开\n");
    } else {
        if(auto_bright_timer != NULL) {
            lv_timer_delete(auto_bright_timer);
            auto_bright_timer = NULL;                     /* 必须置 NULL，否则下次开会拿到野指针 */
        }
        if(bright_slider != NULL)
            lv_obj_remove_state(bright_slider, LV_STATE_DISABLED); // 恢复手动滑条
        printf("自动亮度：关\n");
    }
}

/**
 * 体感校准按钮回调：把当前姿态设为体感零点。
 *
 * 只是 imu_calibrate() 的一层包装。
 * 注意：本回调**没有任何界面反馈**（不弹提示、不变文字），
 * 用户只能从"游戏方向变正常了"间接感知。
 * 想更友好可以把按钮文字临时改成"已校准 ✓"，1 秒后再改回来。
 *
 * 另外：校准应该在板子**水平静止**时点 —— 但界面上没有这句提示
 * （游戏中心底部有提示，设置页没有）。
 */
static void calibrate_cb(lv_event_t *e)
{
    imu_calibrate();
}


/**
 * 返回按钮回调：先删自动亮度定时器（避免悬空），再切回桌面。
 *
 * 【顺序为什么这么重要】
 *   定时器每 1000ms 会访问 bright_slider / bright_val / 写背光文件。
 *   如果先删屏，那些控件就没了，下一拍定时器回调会操作已释放对象 → 崩溃。
 *   所以：删定时器 → 切屏 → 删屏，一步都不能换位置。
 *   （这也是全工程统一的清理顺序，见 chat_ui.c 的 chat_exit_cb）
 *
 * 【一个遗留问题】
 *   自动亮度开着的时候返回，定时器被删了，但：
 *     - 背光停在最后一次自动计算的值；
 *     - 开关状态也跟着屏幕一起消失（下次进来又是"关"）。
 *   于是"界面显示 80%，实际亮度是自动算出来的值"这种不一致会出现。
 *   要修的话得把亮度持久化（写文件），或在 ui_settings_init 里
 *   先读一次当前 brightness 反推百分比。本次不改，只记录。
 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    /* ① 先删定时器 —— 它会访问本屏的控件，必须在删屏之前停掉 */
    if(auto_bright_timer != NULL) {
        lv_timer_delete(auto_bright_timer);
        auto_bright_timer = NULL;
    }
    /* ② 切回桌面（桌面屏已存在就复用，避免重复创建） */
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    /* ③ 再删自己 */
    if(settings_screen != NULL) {
        lv_obj_delete(settings_screen);
        settings_screen = NULL;
    }
    /* 注意：bright_slider / bright_val 没有置 NULL。
     * 但它们只被 auto_bright_timer_cb 使用，而定时器已经在①删掉了，
     * 所以不会有悬空访问。下次进来 ui_settings_init 会重新赋值，也是安全的。
     * （属于"靠调用时序保证安全"而不是"靠显式清理保证安全"，
     *   能工作，但不如显式置 NULL 稳。） */
}

/**
 * 设置界面初始化入口。
 *
 * 本函数内部已经 lv_screen_load()，调用方不需要再切屏。
 *
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_settings_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(22);   // 正文字号

    /* 创建屏幕和全屏窗口。
     * ★ 与其他界面不同：本页**没有背景图**，用的是纯色底 0x111119，
     *   所以要显式设 bg_color + bg_opa 才能看到底色。 */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 顶部：返回按钮 */
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
    lv_obj_add_event_cb(back_btn, to_select_app_screen_cb, LV_EVENT_CLICKED, NULL);

    /* 标题 */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* ---- 第 1 项：屏幕亮度 ---- */
    lv_obj_t *lab1 = lv_label_create(win);
    lv_label_set_text(lab1, "屏幕亮度");
    lv_obj_set_style_text_font(lab1, font_norm, 0);
    lv_obj_set_style_text_color(lab1, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab1, 60, 100);

    /* 亮度滑条。用模块级 static 变量保存指针，
     * 因为自动亮度回调（auto_bright_timer_cb）需要操作它。
     * 范围 0~100 直接对应"百分比"，不暴露底层 0~255，
     * 硬件细节全部由 set_backlight 封装。 */
    bright_slider = lv_slider_create(win);
    lv_obj_set_size(bright_slider, 450, 24);
    lv_obj_set_pos(bright_slider, 400, 96);
    lv_slider_set_range(bright_slider, 0, 100);
    lv_slider_set_value(bright_slider, 80, LV_ANIM_OFF);      /* 默认 80%，不播动画 */
    /* 注意：这里只是设了滑条显示值，**没有同步写背光**。
     * 所以刚进设置页时滑条显示 80%，实际亮度还是上一轮的值。 */

    /* 亮度百分比标签（同样存模块级 static，供自动亮度回调更新） */
    bright_val = lv_label_create(win);
    lv_label_set_text(bright_val, "80%");
    lv_obj_set_style_text_font(bright_val, font_norm, 0);
    lv_obj_set_style_text_color(bright_val, lv_color_hex(0x2FD3A0), 0);   /* 绿色，突出数值 */
    lv_obj_set_pos(bright_val, 870, 100);
    /* 把标签作为 user_data 传给滑条回调，回调里就能直接更新它 */
    lv_obj_add_event_cb(bright_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, bright_val);

    /* ---- 第 2 项：自动亮度 ---- */
    lv_obj_t *lab2 = lv_label_create(win);
    lv_label_set_text(lab2, "自动亮度");
    lv_obj_set_style_text_font(lab2, font_norm, 0);
    lv_obj_set_style_text_color(lab2, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab2, 60, 190);

    /* 自动亮度开关（lv_switch 自带滑动手势和高亮态，不用自己做） */
    lv_obj_t *auto_sw = lv_switch_create(win);
    lv_obj_set_pos(auto_sw, 800, 185);
    lv_obj_add_event_cb(auto_sw, auto_bright_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ---- 第 3 项：体感校准 ---- */
    lv_obj_t *lab4 = lv_label_create(win);
    lv_label_set_text(lab4, "体感校准");
    lv_obj_set_style_text_font(lab4, font_norm, 0);
    lv_obj_set_style_text_color(lab4, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab4, 60, 280);

    /* 体感校准按钮（绿色）：点了调 imu_calibrate()，无界面反馈 */
    lv_obj_t *cal_btn = lv_button_create(win);
    lv_obj_set_size(cal_btn, 140, 44);
    lv_obj_set_pos(cal_btn, 700, 270);
    lv_obj_set_style_bg_color(cal_btn, lv_color_hex(0x1D9E75), 0);
    lv_obj_set_style_radius(cal_btn, 10, 0);
    lv_obj_t *cal_lab = lv_label_create(cal_btn);
    lv_label_set_text(cal_lab, "校准");
    lv_obj_set_style_text_font(cal_lab, font_norm, 0);
    lv_obj_set_style_text_color(cal_lab, lv_color_hex(0x06130E), 0);
    lv_obj_center(cal_lab);
    lv_obj_add_event_cb(cal_btn, calibrate_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 第 4 项：关于（纯展示文字，无交互） ---- */
    lv_obj_t *lab5 = lv_label_create(win);
    lv_label_set_text(lab5, "智趣魔方 · 版本 1.0");
    lv_obj_set_style_text_font(lab5, font_norm, 0);
    lv_obj_set_style_text_color(lab5, lv_color_hex(0x9A9AAC), 0);   /* 次要信息，用灰字 */
    lv_obj_set_pos(lab5, 60, 370);

    lv_screen_load(scr);        /* 创建完立刻切过去 */
    settings_screen = scr;      /* 记录全局，供返回时删除 */
    return scr;
}
