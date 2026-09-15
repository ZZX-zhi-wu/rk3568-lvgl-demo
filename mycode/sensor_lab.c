/**
 * @file    sensor_lab.c
 * @brief   传感器实验室界面（实时数据可视化）
 *
 * =====================================================================
 * 一、功能说明
 * =====================================================================
 *   - 实时显示 MPU6050 加速度/姿态角 和 BH1750 照度。
 *   - 左侧：俯仰角 Pitch、横滚角 Roll 两个对称仪表条（-90° ~ +90°）。
 *   - 右侧：加速度三轴实时滚动曲线（红 ax / 绿 ay / 蓝 az）。
 *   - 底部：照度仪表条 + 三轴加速度/陀螺仪数值。
 *   - 每 200ms 刷新一次（5Hz）。
 *
 * =====================================================================
 * 二、布局（1024×600，纯色底无背景图）
 * =====================================================================
 *   y=12   [返回]  传感器实验室                    (实时胶囊 880,15)
 *   y=84   俯仰角 Pitch                       加速度曲线 (g)  ax红 ay绿 az蓝
 *   y=116  ══仪表条 220×24══  +0.0°           ┌─────────────────────┐
 *   y=174  横滚角 Roll                        │  折线图 650×300      │
 *   y=206  ══仪表条══         +0.0°           │  (340,116)           │
 *   y=440  照度                                └─────────────────────┘
 *   y=445  加速度  ax … ay … az … g            （数值标签在图的左下方）
 *   y=470  ══照度条 220×20══   0 lux
 *   y=495  陀螺仪  gx … gy … gz … °/s
 *
 * =====================================================================
 * 三、两个实现要点
 * =====================================================================
 *   1) 姿态角**不调用** sensor_pitch_roll()，而是用已经读到的 ax/ay/az
 *      自己算。原因：sensor_pitch_roll 内部会再读 3 次加速度文件，
 *      而这里为了画曲线本来就已经读了，再读一遍纯属浪费。
 *      公式与 sensor.c 里完全一致（见 sensor.h 的说明）。
 *
 *   2) 浮点格式化用标准 snprintf，**不用** lv_label_set_text_fmt。
 *      因为 lv_conf.h 里 LV_USE_FLOAT=0，LVGL 自带的 lv_vsnprintf
 *      不支持 %f —— 传 %f 进去会输出乱码或什么都不输出。
 *      所以：整数用 lv_label_set_text_fmt（方便），
 *            浮点必须先 snprintf 到 char buf 再 set_text。
 *
 * =====================================================================
 * 四、注意
 * =====================================================================
 *   - 板上 MPU6050 的 X 轴对应"左右"、Y 轴对应"前后"，
 *     所以俯仰（前后）用 ay、横滚（左右）用 ax。
 *   - 进入本页会 sensor_calibrate()，**板子必须水平**，
 *     否则会把这个错误的姿态当成"水平"，出去玩游戏方向就偏了。
 */
#include "../lvgl/lvgl.h"
#include "main_interface.h"
#include "sensor_lab.h"
#include "sensor.h"
#include <stdio.h>
#include <math.h>

#define CN_FONT_PATH "/work_space/font/msyh.ttc"

/* 屏幕指针：进来创建、返回时删除并置 NULL */
lv_obj_t * sensor_lab_screen = NULL;

/* ---------------------------------------------------------------------
 * 定时器回调要访问的控件，全部存放在模块级 static 全局里。
 * 为什么不能用局部变量：定时器回调的入参只有 lv_timer_t*，
 * 拿不到创建界面时的那些局部变量，所以必须"提"到文件作用域。
 * --------------------------------------------------------------------- */
static lv_obj_t *chart = NULL;               // 加速度曲线控件
static lv_chart_series_t *ser_ax, *ser_ay, *ser_az;  // 三条曲线（ax/ay/az）
static lv_obj_t *pitch_bar, *roll_bar, *lux_bar;     // 三个仪表条
static lv_obj_t *pitch_val, *roll_val, *lux_val;     // 三个数值标签
static lv_obj_t *accel_lab, *gyro_lab;               // 加速度/陀螺仪数值标签
static lv_timer_t *sensor_timer = NULL;              // 刷新定时器

/**
 * 创建指定字号的中文字体（带缓存）。
 *
 * 缓存 4 槽，本文件用 3 档（24 标题、18 正文、14 小字），够用。
 *
 * 【已知隐患】和 game_center.c / settings.c / album.c 一样，
 * 少了 if(empty < 0) return NULL; 这行保护 ——
 * 一旦字号种类超过 4 档，`cache[-1] = f;` 会数组越界写。
 * 现在固定 3 档，不会触发；加字号前先补上。
 *
 * @param size 字号（像素）
 * @return 字体指针，失败返回 NULL（此时 LVGL 会退回默认字体，中文会变方框）
 */
static lv_font_t *cn_font(int size)
{
    static lv_font_t *cache[4] = {NULL};
    static int cache_size[4] = {0};
    int empty = -1;
    for(int i = 0; i < 4; i++) {
        if(cache[i] != NULL && cache_size[i] == size) return cache[i];   /* 命中缓存 */
        if(cache[i] == NULL && empty < 0) empty = i;                     /* 记空槽 */
    }
    lv_font_t *f = lv_freetype_font_create(CN_FONT_PATH,
        LV_FREETYPE_FONT_RENDER_MODE_BITMAP, size, LV_FREETYPE_FONT_STYLE_NORMAL);
    if(!f) { LV_LOG_ERROR("freetype font create failed"); return NULL; }
    cache[empty] = f;
    cache_size[empty] = size;
    return f;
}

/**
 * 每 200ms 刷新一次：读传感器 -> 更新曲线、仪表条、数值标签。
 *
 * 一次 tick 里做 8 次 sysfs 文件读（加速度 3 + 陀螺仪 3 + 照度 1，
 * 另外 ax 的滤波状态被推进 3 次是因为后面算姿态角也用同一批变量）。
 * 200ms 一次完全跑得动，不会影响界面手感。
 *
 * 【为什么先存进局部变量再复用】
 *   sensor_accel_x() 每调一次就会推进一次低通滤波（内部有 static 状态）。
 *   如果"画曲线"和"算姿态角"各调一次，滤波就被推进两次，
 *   等效平滑程度和设计时不一样，而且白读两次文件。
 *   所以这里是标准做法：**读一次，存局部变量，后面全用局部变量**。
 */
static void sensor_timer_cb(lv_timer_t *t)
{
    char buf[64];
    /* 先把这一拍的所有传感器值读出来（各读一次，只读一次） */
    float ax = sensor_accel_x();
    float ay = sensor_accel_y();
    float az = sensor_accel_z();
    float gx = sensor_gyro_x();
    float gy = sensor_gyro_y();
    float gz = sensor_gyro_z();
    int lux = sensor_illuminance();

    // 姿态角：直接用已读的 ax/ay/az 算（板上 X=左右、Y=前后，所以 ay 对应俯仰、ax 对应横滚）
    /* 公式与 sensor.c 的 sensor_pitch_roll 完全相同，只是省掉了重复读文件。
     * 用 atan2f（不是 atan）避免分母为 0，且能覆盖全象限。
     * 乘 180/π 是为了把弧度换成度。 */
    float pitch = atan2f(-ay, sqrtf(ax*ax + az*az)) * 180.0f / 3.14159265f;  // 俯仰（前后）
    float roll  = atan2f( ax, sqrtf(ay*ay + az*az)) * 180.0f / 3.14159265f;  // 横滚（左右）

    // 曲线（加速度 g 值放大 100 存整数，±2g 对应 ±200）
    /* 【为什么要乘 100】
     *   lv_chart 的数据点是 int32_t，存不了小数。
     *   把 g 值乘 100 变成整数，就把 0.01g 的精度"搬"到了整数域。
     *   配合下面 lv_chart_set_range(-200, 200)，
     *   正好覆盖 -2g ~ +2g，而且刻度换算简单（图上 100 就是 1g）。
     *   LVGL v9 其实也支持浮点数据点（LV_USE_FLOAT 打开时），
     *   但本工程 LV_USE_FLOAT=0，所以用整数放大法。 */
    lv_chart_set_next_value(chart, ser_ax, (int)(ax * 100));
    lv_chart_set_next_value(chart, ser_ay, (int)(ay * 100));
    lv_chart_set_next_value(chart, ser_az, (int)(az * 100));

    // 姿态角：更新仪表条 + 数值（用标准 snprintf 格式化 float，因为 LVGL 的 set_text_fmt 不支持 %f）
    lv_bar_set_value(pitch_bar, (int)pitch, LV_ANIM_OFF);   /* 用 OFF：每 200ms 一次，加动画反而拖影 */
    lv_bar_set_value(roll_bar,  (int)roll,  LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%+.1f°", pitch);            /* %+.1f：带符号、1 位小数，如 +12.3° */
    lv_label_set_text(pitch_val, buf);
    snprintf(buf, sizeof(buf), "%+.1f°", roll);
    lv_label_set_text(roll_val, buf);

    // 照度
    lv_bar_set_value(lux_bar, lux, LV_ANIM_OFF);            /* 条的范围是 0~1000，超过就顶格 */
    lv_label_set_text_fmt(lux_val, "%d lux", lux);          /* 整数用 set_text_fmt 没问题 */

    // 加速度/陀螺仪数值
    /* 这两行是"一屏看全"的设计：把六轴数据拼成一行字符串，
     * 方便肉眼比对曲线和数值是否一致。 */
    snprintf(buf, sizeof(buf), "加速度  ax %+.2f  ay %+.2f  az %+.2f g", ax, ay, az);
    lv_label_set_text(accel_lab, buf);
    snprintf(buf, sizeof(buf), "陀螺仪  gx %+.1f  gy %+.1f  gz %+.1f °/s", gx, gy, gz);
    lv_label_set_text(gyro_lab, buf);
}

/**
 * 返回按钮回调：先删刷新定时器（避免悬空），再切回桌面。
 *
 * 顺序：① 删定时器 → ② 切屏 → ③ 删屏。
 * 如果先删屏，定时器下一拍（最多 200ms 后）就会访问已经释放的
 * chart / pitch_bar 等控件 → 段错误。所以删定时器必须排第一。
 */
static void to_select_app_screen_cb(lv_event_t *e)
{
    if(sensor_timer != NULL) { lv_timer_delete(sensor_timer); sensor_timer = NULL; }
    if(select_app_screen == NULL)
        select_app_screen = ui_select_app_screen();
    lv_screen_load(select_app_screen);
    if(sensor_lab_screen != NULL) { lv_obj_delete(sensor_lab_screen); sensor_lab_screen = NULL; }
}

/**
 * 传感器实验室界面初始化入口。
 *
 * ★ 注意第 117-118 行的两个调用：sensor_init() 和 sensor_calibrate()。
 *   也就是说"进入本页"这个动作会重置体感零点 —— 板子必须水平！
 *
 * @return 创建的屏幕对象
 */
lv_obj_t * ui_sensor_lab_init(void)
{
    lv_font_t *font_title = cn_font(24);   // 标题字号
    lv_font_t *font_norm  = cn_font(18);   // 正文字号
    lv_font_t *font_desc  = cn_font(14);   // 提示小字

    sensor_init();        // 读量程比例因子
    sensor_calibrate();   // 进入时校准零点（板子水平放）

    /* 创建屏幕和全屏窗口（本页无背景图，用纯色底） */
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(scr);
    lv_obj_set_size(win, 1024, 600);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x111119), 0);
    lv_obj_set_style_bg_opa(win, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(win, 0, 0);
    lv_obj_set_style_radius(win, 0, 0);
    lv_obj_set_style_pad_all(win, 0, 0);

    /* 标题栏：返回按钮（90×40 放在 20,12，全工程统一的位置和样式） */
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

    /* 标题（x=130 让开返回按钮：20+90+20 = 130） */
    lv_obj_t *title = lv_label_create(win);
    lv_label_set_text(title, "传感器实验室");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(title, 130, 18);

    /* 右上角「实时」状态胶囊
     * 和游戏中心的"陀螺仪已就绪"一样，是固定文案，
     * 表示"这个页面的数据是持续刷新的"，不是真的做了状态检测。 */
    lv_obj_t *pill = lv_label_create(win);
    lv_label_set_text(pill, "实时");
    lv_obj_set_style_text_font(pill, font_desc, 0);
    lv_obj_set_style_text_color(pill, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x0F2A21), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 20, 0);
    lv_obj_set_style_pad_all(pill, 10, 0);
    lv_obj_set_pos(pill, 880, 15);

    /* ---- 左侧：俯仰角 ---- */
    lv_obj_t *lab_pitch = lv_label_create(win);
    lv_label_set_text(lab_pitch, "俯仰角 Pitch");
    lv_obj_set_style_text_font(lab_pitch, font_norm, 0);
    lv_obj_set_style_text_color(lab_pitch, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_pitch, 40, 84);

    /* 俯仰角仪表条（对称模式，中间为 0°，绿色）
     * 【LV_BAR_MODE_SYMMETRICAL 是什么】
     *   普通模式下指示条从最左往右长（0 → 最大值）。
     *   对称模式让"零点"位于正中间，指示条从中间往两边长 ——
     *   正负都有意义的角度值（-90~+90）必须用这个模式，
     *   否则 -90° 会显示成一条都看不见。 */
    pitch_bar = lv_bar_create(win);
    lv_obj_set_size(pitch_bar, 220, 24);
    lv_obj_set_pos(pitch_bar, 40, 116);
    lv_bar_set_range(pitch_bar, -90, 90);
    lv_bar_set_mode(pitch_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_obj_set_style_bg_color(pitch_bar, lv_color_hex(0x1B1B26), 0);            /* 槽底色 */
    lv_obj_set_style_bg_color(pitch_bar, lv_color_hex(0x2FD3A0), LV_PART_INDICATOR);  /* 指示条颜色 */

    /* 俯仰角数值（用 %+.1f 格式，带正负号，方便判断方向） */
    pitch_val = lv_label_create(win);
    lv_label_set_text(pitch_val, "+0.0°");
    lv_obj_set_style_text_font(pitch_val, font_norm, 0);
    lv_obj_set_style_text_color(pitch_val, lv_color_hex(0x2FD3A0), 0);
    lv_obj_set_pos(pitch_val, 270, 108);      /* 270 = 40 + 220 + 10，紧贴仪表条右侧 */

    /* ---- 左侧：横滚角（结构与俯仰角完全对称，颜色换成橙色便于区分） ---- */
    lv_obj_t *lab_roll = lv_label_create(win);
    lv_label_set_text(lab_roll, "横滚角 Roll");
    lv_obj_set_style_text_font(lab_roll, font_norm, 0);
    lv_obj_set_style_text_color(lab_roll, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_roll, 40, 174);

    /* 横滚角仪表条（橙色） */
    roll_bar = lv_bar_create(win);
    lv_obj_set_size(roll_bar, 220, 24);
    lv_obj_set_pos(roll_bar, 40, 206);
    lv_bar_set_range(roll_bar, -90, 90);
    lv_bar_set_mode(roll_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_obj_set_style_bg_color(roll_bar, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_color(roll_bar, lv_color_hex(0xF5A623), LV_PART_INDICATOR);

    /* 横滚角数值 */
    roll_val = lv_label_create(win);
    lv_label_set_text(roll_val, "+0.0°");
    lv_obj_set_style_text_font(roll_val, font_norm, 0);
    lv_obj_set_style_text_color(roll_val, lv_color_hex(0xF5A623), 0);
    lv_obj_set_pos(roll_val, 270, 198);

    /* ---- 右侧：加速度曲线 ---- */
    lv_obj_t *lab_chart = lv_label_create(win);
    lv_label_set_text(lab_chart, "加速度曲线 (g)  ax红 ay绿 az蓝");
    lv_obj_set_style_text_font(lab_chart, font_desc, 0);
    lv_obj_set_style_text_color(lab_chart, lv_color_hex(0x9A9AAC), 0);
    lv_obj_set_pos(lab_chart, 340, 84);       /* 图例直接写在标题里，省一个图例控件 */

    /* 加速度曲线（折线图，滚动更新，±2g 范围）
     *
     * 【三个关键设置，缺一个图就不对】
     *   LV_CHART_TYPE_LINE            画折线而不是柱状
     *   set_point_count(50)           每 200ms 来一个点 → 50 点 = 10 秒历史
     *   LV_CHART_UPDATE_MODE_SHIFT    新点从右边挤进来、旧点从左边挤出（"示波器"效果）。
     *                                 另一个可选值是 CIRCULAR（绕着圈覆盖），
     *                                 适合长时间观察，但看不出时间顺序。
     *   set_range(-200, 200)          Y 轴量程，对应 -2g ~ +2g（数据是 g×100） */
    chart = lv_chart_create(win);
    lv_obj_set_size(chart, 650, 300);
    lv_obj_set_pos(chart, 340, 116);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 50);                 // 显示最近 50 个点
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);  // 滚动模式
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, -200, 200); // ±2g
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_border_width(chart, 1, 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x2B2B3A), 0);

    /* 三条曲线：红 ax、绿 ay、蓝 az
     * lv_chart_add_series 返回的 series 句柄要存起来，
     * 之后每拍用 lv_chart_set_next_value(chart, ser, v) 往里塞数据。
     * 三个 series 挂在同一个 Y 轴（PRIMARY_Y）上，所以量程共享。 */
    ser_ax = lv_chart_add_series(chart, lv_color_hex(0xE74C3C), LV_CHART_AXIS_PRIMARY_Y);
    ser_ay = lv_chart_add_series(chart, lv_color_hex(0x2FD3A0), LV_CHART_AXIS_PRIMARY_Y);
    ser_az = lv_chart_add_series(chart, lv_color_hex(0x4DA6FF), LV_CHART_AXIS_PRIMARY_Y);

    /* ---- 底部：照度 ---- */
    lv_obj_t *lab_lux = lv_label_create(win);
    lv_label_set_text(lab_lux, "照度");
    lv_obj_set_style_text_font(lab_lux, font_norm, 0);
    lv_obj_set_style_text_color(lab_lux, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(lab_lux, 40, 440);

    /* 照度仪表条（0~1000 lux，蓝色）
     * 注意：量程上限写死 1000，用强光手电照会一直顶格；
     *       要能显示更大值就把上限调大，但那样弱光的分辨率就差了。 */
    lux_bar = lv_bar_create(win);
    lv_obj_set_size(lux_bar, 220, 20);
    lv_obj_set_pos(lux_bar, 40, 470);
    lv_bar_set_range(lux_bar, 0, 1000);
    lv_obj_set_style_bg_color(lux_bar, lv_color_hex(0x1B1B26), 0);
    lv_obj_set_style_bg_color(lux_bar, lv_color_hex(0x4DA6FF), LV_PART_INDICATOR);

    /* 照度数值 */
    lux_val = lv_label_create(win);
    lv_label_set_text(lux_val, "0 lux");
    lv_obj_set_style_text_font(lux_val, font_norm, 0);
    lv_obj_set_style_text_color(lux_val, lv_color_hex(0x4DA6FF), 0);
    lv_obj_set_pos(lux_val, 270, 465);

    /* ---- 底部：加速度数值标签（x=340 让开左侧仪表条区域） ---- */
    accel_lab = lv_label_create(win);
    lv_label_set_text(accel_lab, "加速度  ax 0.00  ay 0.00  az 0.00 g");
    lv_obj_set_style_text_font(accel_lab, font_norm, 0);
    lv_obj_set_style_text_color(accel_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(accel_lab, 340, 445);

    /* ---- 底部：陀螺仪数值标签 ---- */
    gyro_lab = lv_label_create(win);
    lv_label_set_text(gyro_lab, "陀螺仪  gx 0.0  gy 0.0  gz 0.0 °/s");
    lv_obj_set_style_text_font(gyro_lab, font_norm, 0);
    lv_obj_set_style_text_color(gyro_lab, lv_color_hex(0xECEAF2), 0);
    lv_obj_set_pos(gyro_lab, 340, 495);

    /* 启动刷新定时器（200ms 周期，约 5Hz，足够且不卡顿）
     * 【为什么是 200ms】
     *   加速度曲线要看清"倾斜动作"的变化，5Hz 已经能看出趋势；
     *   而每次 tick 都要读 7~8 个 sysfs 文件 + 重绘图表，
     *   频率再高（比如 50ms）会明显增加 CPU 占用但肉眼看不出区别。 */
    sensor_timer = lv_timer_create(sensor_timer_cb, 200, NULL);

    lv_screen_load(scr);        /* 创建完立刻显示 */
    sensor_lab_screen = scr;    /* 记录全局，供返回时删除 */
    return scr;
}
