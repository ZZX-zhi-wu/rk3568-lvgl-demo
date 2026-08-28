/**
 * @file    imu.c
 * @brief   IMU 数据层（体感游戏用）
 *
 * 功能说明：
 *   - 读取 MPU6050 加速度计 X/Y 轴原始值，供体感游戏（2048 倾斜、重力滚球）使用。
 *   - 提供零点校准（把当前水平姿态设为 0）。
 *   - 提供方向修正（INVERT_X/INVERT_Y，解决芯片安装朝向导致的方向相反）。
 *
 * 与 sensor.c 的区别：
 *   - sensor.c 输出带单位的物理量（g、°/s、lux），供传感器实验室显示；
 *   - imu.c 输出原始整数倾斜值，供体感游戏做方向判断（更简单、更快）。
 */
#include <stdio.h>
#include "imu.h"

/* MPU6050 加速度计 IIO 路径 */
#define ACCEL_X_PATH "/sys/bus/iio/devices/iio:device1/in_accel_x_raw"
#define ACCEL_Y_PATH "/sys/bus/iio/devices/iio:device1/in_accel_y_raw"

/* 方向修正：左右/上下反了就把对应项置 1 */
#define INVERT_X 1
#define INVERT_Y 0

/* 零点偏移（水平放置时的基准读数） */
static int offset_x = 0;
static int offset_y = 0;

/* 读取 sysfs 中的整数原始值 */
static int read_raw(const char *path)
{
    FILE *fp = fopen(path, "r");
    if(!fp) return 0;
    int v = 0;
    fscanf(fp, "%d", &v);
    fclose(fp);
    return v;
}

/* 校准：把当前水平姿态的读数作为零点（取 10 次平均降抖动） */
void imu_calibrate(void)
{
    long sx = 0, sy = 0;
    for(int i = 0; i < 10; i++) {
        sx += read_raw(ACCEL_X_PATH);
        sy += read_raw(ACCEL_Y_PATH);
    }
    offset_x = (int)(sx / 10);
    offset_y = (int)(sy / 10);
    printf("[imu] 校准完成 offset=(%d, %d)\n", offset_x, offset_y);
}

/* X 轴倾斜值：减零点 + 方向修正 */
int imu_tilt_x(void)
{
    int v = read_raw(ACCEL_X_PATH) - offset_x;
    return INVERT_X ? -v : v;
}

/* Y 轴倾斜值：减零点 + 方向修正 */
int imu_tilt_y(void)
{
    int v = read_raw(ACCEL_Y_PATH) - offset_y;
    return INVERT_Y ? -v : v;
}
