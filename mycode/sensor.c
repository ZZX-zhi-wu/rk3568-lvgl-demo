/**
 * @file    sensor.c
 * @brief   传感器数据层（MPU6050 六轴 + BH1750 照度）
 *
 * =====================================================================
 * 一、硬件怎么访问：IIO 子系统的两层结构（本文件的核心套路）
 * =====================================================================
 *   两个传感器都被内核挂到 IIO (Industrial I/O) 子系统下，
 *   在 /sys/bus/iio/devices/iio:deviceN/ 里暴露成普通文件。
 *   每个通道都有两个文件，必须配套使用：
 *
 *     in_accel_x_raw     ← 原始计数（LSB），有符号整数
 *     in_accel_scale     ← 每个 LSB 代表多少物理量
 *
 *     物理量 = raw × scale
 *
 *   为什么要分两步而不是直接读物理量？
 *     因为 raw 是 16 位整数，精度高、传输快；
 *     而 scale 随量程档位变化（±2g 和 ±16g 差 8 倍），
 *     内核把它单独放在一个文件里，改档位时不用改驱动代码。
 *
 * ★ 单位陷阱（最容易错的地方）：
 *     in_accel_scale   的单位是 **m/s² / LSB**  → 乘完还要 ÷9.81 才是 g
 *     in_anglvel_scale 的单位是 **rad/s / LSB** → 乘完还要 ×180/π 才是 °/s
 *   两个 scale 的单位不一样！陀螺仪那个如果忘了转角度，
 *   数值会差 57.3 倍，看着像"传感器坏了"，其实只是单位没换。
 *
 * =====================================================================
 * 二、功能说明
 * =====================================================================
 *   - 封装 MPU6050（加速度计+陀螺仪）和 BH1750（环境光）的读取。
 *   - 加速度计输出单位 g（重力加速度），陀螺仪输出单位 °/s。
 *   - 提供零点校准（把当前姿态设为 0）和低通滤波（去抖）。
 *   - 提供由加速度计解算姿态角（俯仰/横滚）的函数。
 *
 * =====================================================================
 * 三、硬件路径
 * =====================================================================
 *   - MPU6050 挂在 IIO 的 iio:device1。
 *   - BH1750 环境光传感器挂在 IIO 的 iio:device2。
 *   - 编号是内核枚举顺序，若日后板子加了别的 IIO 设备导致变化，
 *     用 `ls /sys/bus/iio/devices/` 查实际编号再改下面的宏。
 */
#include <stdio.h>
#include <math.h>
#include "sensor.h"

/* 数学常量。
 * 末尾的 f 很重要：不加 f 会按 double 运算，
 * ARM 上 double 是软件模拟的，比 float 慢很多，而且这里根本不需要双精度。 */
#define PI 3.14159265f   // 圆周率
#define G  9.81f         // 标准重力加速度（m/s²）
#define RAD_TO_DEG 57.2957778f   // 180/π，弧度转角度


/* MPU6050（iio:device1）的 sysfs 路径 */
#define ACCEL_X_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_x_raw"
#define ACCEL_Y_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_y_raw"
#define ACCEL_Z_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_z_raw"
#define ACCEL_SCALE    "/sys/bus/iio/devices/iio:device1/in_accel_scale"
/* 陀螺仪通道。
 * 名字里是 anglvel = angular velocity（角速度），不是加速度，别和 accel 混。 */
#define GYRO_X_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_x_raw"
#define GYRO_Y_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_y_raw"
#define GYRO_Z_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_z_raw"
#define GYRO_SCALE     "/sys/bus/iio/devices/iio:device1/in_anglvel_scale"

/* BH1750 环境光传感器（iio:device2）的 sysfs 路径
 * input 是内核算好的 lux 值；raw/scale 是后备方案（input 读不到时才用）。 */
#define ILLUM_INPUT    "/sys/bus/iio/devices/iio:device2/in_illuminance_input"
#define ILLUM_RAW      "/sys/bus/iio/devices/iio:device2/in_illuminance_raw"
#define ILLUM_SCALE    "/sys/bus/iio/devices/iio:device2/in_illuminance_scale"

/* =====================================================================
 * 模块级状态（static：只在本文件可见，但跨调用持续存在）
 * ===================================================================== */

/* 传感器量程比例因子（raw 值 × scale = 物理量）
 * 由 sensor_init() 从 sysfs 读出，之后一直复用。
 * 初值 1.0 表示"还没 init 过"，此时数值不准确但不会崩。 */
static float accel_scale = 1.0f;   // 加速度 scale（m/s² / LSB）
static float gyro_scale  = 1.0f;   // 陀螺仪 scale（rad/s / LSB，注意不是 °/s）

/* 零点偏移（校准后记录的水平姿态基准读数）
 * 校准做了两件事：消除"装配时芯片没完全水平"的固定倾角，
 * 以及消除陀螺仪静止时的输出偏置（理论应为 0，实际有零点漂移）。 */
static float off_ax = 0, off_ay = 0, off_az = 0;
static float off_gx = 0, off_gy = 0, off_gz = 0;

/* 低通滤波状态（对加速度做一阶低通，平滑噪声）
 * ★ 这是"记忆"，每调用一次取值函数它就往前走一步，
 *   所以取值函数不是纯函数，详见下面 sensor_accel_x 的说明。 */
static float filt_ax = 0, filt_ay = 0, filt_az = 0;

/**
 * 读取 sysfs 中的整数原始值。
 * @return 文件里的整数；打不开或格式不对时返回 0
 */
static int read_raw(const char *path)
{
    FILE *fp = fopen(path, "r");
    if(!fp) return 0;               /* 容错优先：读不到当 0，保证不崩 */
    int v = 0;
    fscanf(fp, "%d", &v);
    fclose(fp);                     /* 必须关：sysfs 也是文件描述符，泄漏会耗尽 fd */
    return v;
}

/**
 * 读取 sysfs 中的浮点值，读不到返回默认值 def。
 *
 * 和 read_raw 的区别是多了个 def 参数：
 * 因为 scale 这类参数一旦读成 0，后面所有数值都会变成 0（比读错更隐蔽），
 * 所以宁可退回一个"至少能用"的默认值 1.0。
 */
static float read_float(const char *path, float def)
{
    FILE *fp = fopen(path, "r");
    if(!fp) return def;
    float v = def;
    fscanf(fp, "%f", &v);
    fclose(fp);
    return v;
}

/**
 * 初始化：读量程比例因子（进入传感器实验室时调用一次）
 *
 * 读两个文件而已，开销极小，但必须调用 —— 不调用的话
 * accel_scale / gyro_scale 都是 1.0，算出来的物理量会差几倍到几十倍。
 */
void sensor_init(void)
{
    accel_scale = read_float(ACCEL_SCALE, 1.0f);
    gyro_scale  = read_float(GYRO_SCALE, 1.0f);
    printf("[sensor] accel_scale=%f gyro_scale=%f\n", accel_scale, gyro_scale);
}

/**
 * 零点校准：把当前姿态读数记为零点偏移，并重置低通滤波状态。
 *
 * ★ 三个要点：
 *
 * 1) 必须在【水平静止】时调用。
 *    本函数无条件把当前读数当"水平"，板子歪着校就会把歪当成正。
 *
 * 2) Z 轴故意不校准（off_az = 0）——这是有意的设计，不是漏写。
 *    加速度计水平放置时 Z 轴测到的就是重力本身（≈1g）。
 *    如果把 Z 也减掉零点，水平时 az 就变成 0，
 *    后面 sensor_pitch_roll 算姿态角时分母 √(ax²+az²) 会失效，
 *    角度就全错了。重力分量是姿态解算的基准，必须留着。
 *
 * 3) 顺便清空滤波状态。
 *    因为滤波器是"旧值×0.7 + 新值×0.3"的递归形式，
 *    里面存着校准时那批旧数据；不清零的话校准后前几百毫秒
 *    显示的还是校准前的值，看起来像"校准没生效"。
 *
 * 注意：这里每个轴只采 1 次样本，没有平均。
 *       偶发噪声会让零点偏一点，界面数值轻微漂移。
 *       对显示用途可以接受；要更稳可以多调几次本函数。
 */
void sensor_calibrate(void)
{
    off_ax = read_raw(ACCEL_X_PATH);
    off_ay = read_raw(ACCEL_Y_PATH);
    off_az = 0;   // ← 改这里：Z 轴不校准，保留 1g 重力分量
    off_gx = read_raw(GYRO_X_PATH);
    off_gy = read_raw(GYRO_Y_PATH);
    off_gz = read_raw(GYRO_Z_PATH);
    filt_ax = 0; filt_ay = 0; filt_az = 0;
    printf("[sensor] 零点校准完成\n");
}


/**
 * 加速度 X 轴（单位 g，减零点 + 低通滤波）
 *
 * 三步计算链，顺序不能变：
 *   raw - off_ax      ① 去掉零点偏移（此时水平读数为 0）
 *   × accel_scale     ② LSB 换成 m/s²（内核给的比例）
 *   ÷ G               ③ m/s² 换成 g，让显示更直观（水平时为 0 附近）
 *
 * 【一阶低通滤波：filt = 旧值×0.7 + 新值×0.3】
 *   这是一个简单的 IIR 滤波器，作用是压制传感器的随机抖动。
 *   0.7/0.3 的含义：新数据只占 30% 权重，所以突然的尖峰
 *   会被"稀释"掉，输出曲线变平滑。
 *   权重越大越平滑但响应越慢；0.7/0.3 是响应和稳定之间的折中。
 *
 *   ★ 因为 filt_ax 是 static，本函数有"记忆"：
 *     同一周期内调两次，等于让滤波器前进两步，
 *     等效截止频率就变了。所以调用方应该取一次存进变量复用。
 *
 *   还有一个副作用：滤波器从 0 开始（或校准时被清零），
 *   刚进入界面时前几百毫秒读数会从 0 慢慢爬到真实值。
 *   如果觉得"刚进去数值不对、过一会儿才好"，就是这个原因。
 */
float sensor_accel_x(void)
{
    float raw = (read_raw(ACCEL_X_PATH) - off_ax) * accel_scale / G;
    filt_ax = filt_ax * 0.7f + raw * 0.3f;   // 一阶低通：0.7 旧值 + 0.3 新值
    return filt_ax;
}

/* 加速度 Y 轴（单位 g）：处理链与 X 轴完全相同，只是换了零点与滤波状态 */
float sensor_accel_y(void)
{
    float raw = (read_raw(ACCEL_Y_PATH) - off_ay) * accel_scale / G;
    filt_ay = filt_ay * 0.7f + raw * 0.3f;
    return filt_ay;
}

/**
 * 加速度 Z 轴（单位 g，水平放置时约为 1g）
 *
 * 和 X/Y 的区别：off_az 恒为 0，所以这里减去 0 相当于没减。
 * 结果就是水平静置时 az ≈ +1.0 g —— 这正说明传感器工作正常。
 * 这一轴是姿态角解算的基准，所以不能校准掉（见 sensor_calibrate 说明）。
 */
float sensor_accel_z(void)
{
    float raw = (read_raw(ACCEL_Z_PATH) - off_az) * accel_scale / G;
    filt_az = filt_az * 0.7f + raw * 0.3f;
    return filt_az;
}

/**
 * 陀螺仪三轴（单位 °/s）
 *
 * 计算链：raw - 零点 → × gyro_scale（得到 rad/s）→ × 180/π（得到 °/s）
 *
 * ★ 最后那步乘 RAD_TO_DEG 是关键：
 *   内核给 gyro_scale 的单位是 rad/s（弧度每秒），
 *   而人看惯的是 °/s（度每秒），1 rad/s ≈ 57.3 °/s。
 *   漏掉这步，数值会小 57 倍，看起来像"转了但几乎没反应"。
 *
 * 和加速度计不同，陀螺仪三个轴**都**做了零点校准：
 * 因为静止时角速度理论上就是 0，任何非零读数都是误差，直接减掉最干净。
 *
 * 这里没有做低通滤波（加速度那三个才做）：
 * 陀螺仪用在需要快速响应的场合，滤波会带来延迟感。
 */
float sensor_gyro_x(void)
{
    int r = read_raw(GYRO_X_PATH) - off_gx;
    return r * gyro_scale * RAD_TO_DEG;     // LSB → rad/s → °/s
}
float sensor_gyro_y(void)
{
    int r = read_raw(GYRO_Y_PATH) - off_gy;
    return r * gyro_scale * RAD_TO_DEG;
}
float sensor_gyro_z(void)
{
    int r = read_raw(GYRO_Z_PATH) - off_gz;
    return r * gyro_scale * RAD_TO_DEG;
}


/**
 * 用加速度计解算姿态角（水平 = 0°）。
 *
 * 【原理】
 *   加速度计静止时测到的就是重力向量 g（大小恒为 1g，方向永远朝下）。
 *   板子倾斜时，g 会按角度分配到三个轴上。
 *   反过来，由各轴分量的比例就能算出板子的倾角 —— 这就是"静态姿态解算"。
 *
 * 【公式为什么这么写】
 *   pitch（俯仰，前后倾）= atan2(-ay, √(ax²+az²))
 *   roll （横滚，左右倾）= atan2( ax, √(ay²+az²))
 *     - 用 atan2 而不是 atan：atan 分母为 0 会出 inf，
 *       而且只能给出 -90°~+90°，atan2 能正确处理全象限且分母为 0 的情况。
 *     - 分母用"另外两轴的合成模长"而不是单轴：
 *       这样除法结果恒为 tan(倾斜角)，角度在全量程内单调，不会在某个角度跳变。
 *     - ay 前面的负号是符号约定（让"往前倾"对应正角度），
 *       想反过来就把负号去掉。ax 没有负号，是另一套约定。
 *
 * 【注意：这里会重复读文件】
 *   内部调用 3 次 sensor_accel_*，也就是 3 次 fopen/fscanf/fclose
 *   ＋ 3 次滤波推进。如果调用方已经读好了 ax/ay/az，
 *   建议照上面的公式自己算，能省掉这 3 次文件读。
 *   sensor_lab.c 就是这么做的 —— 可以去那里看优化写法。
 *
 * 【精度局限（答辩可能被问）】
 *   纯加速度计解算有两个天生缺陷：
 *     ① 板子运动时的加速度会和重力混在一起，导致角度抖动；
 *     ② 无法测出绕垂直轴的旋转（偏航），那一轴只有陀螺仪能测。
 *   要更准就要做互补滤波/卡尔曼滤波，把陀螺仪积分和加速度计融合。
 *   本项目的显示场景不需要那么高精度，所以只用加速度计。
 */
void sensor_pitch_roll(float *pitch, float *roll)
{
    float ax = sensor_accel_x();
    float ay = sensor_accel_y();
    float az = sensor_accel_z();
    *pitch = atan2f(-ay, sqrtf(ax * ax + az * az)) * 180.0f / PI;  // 俯仰（前后，ay）
    *roll  = atan2f( ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;  // 横滚（左右，ax）
}


/**
 * 读取环境光照度（单位 lux）。
 *
 * 两条路，优先走第一条：
 *   ① in_illuminance_input      —— 内核已经换算好的 lux，直接读走人
 *   ② raw × scale               —— 内核没提供 input 时的后备方案
 * 都失败返回 0（0 lux 在界面上表现为"很暗"，属于合理的降级）。
 *
 * 为什么不像加速度那样封装成 read_raw 一行搞定：
 * 因为这里需要区分"input 能打开"和"打不开需要回退"两种情况，
 * 直接写 fopen 判断更清楚。
 *
 * 相关用法：settings.c 的自动亮度会按 lux 值反推背光亮度
 * （30 + lux×70/990，最低 30%，最高 100%）。
 */
int sensor_illuminance(void)
{
    FILE *fp = fopen(ILLUM_INPUT, "r");
    if(fp) {
        int v = 0;
        fscanf(fp, "%d", &v);
        fclose(fp);
        return v;               /* 首选路径成功，直接返回 */
    }
    /* 回退路径：自己乘 scale */
    int raw = read_raw(ILLUM_RAW);
    float scale = read_float(ILLUM_SCALE, 1.0f);
    return (int)(raw * scale);
}
