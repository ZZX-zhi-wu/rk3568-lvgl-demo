/**
 * @file    sensor.c
 * @brief   传感器数据层（MPU6050 六轴 + BH1750 照度）
 *
 * 功能说明：
 *   - 封装 MPU6050（加速度计+陀螺仪）和 BH1750（环境光）的读取。
 *   - 加速度计输出单位 g（重力加速度），陀螺仪输出单位 °/s。
 *   - 提供零点校准（把当前姿态设为 0）和低通滤波（去抖）。
 *   - 提供由加速度计解算姿态角（俯仰/横滚）的函数。
 *
 * 硬件路径：
 *   - MPU6050 挂在 IIO 的 iio:device1。
 *   - BH1750 环境光传感器挂在 IIO 的 iio:device2。
 */
#include <stdio.h>
#include <math.h>
#include "sensor.h"

#define PI 3.14159265f   // 圆周率
#define G  9.81f         // 标准重力加速度（m/s²）
#define RAD_TO_DEG 57.2957778f   // 180/π，弧度转角度


/* MPU6050（iio:device1）的 sysfs 路径 */
#define ACCEL_X_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_x_raw"
#define ACCEL_Y_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_y_raw"
#define ACCEL_Z_PATH   "/sys/bus/iio/devices/iio:device1/in_accel_z_raw"
#define ACCEL_SCALE    "/sys/bus/iio/devices/iio:device1/in_accel_scale"
/* 陀螺仪通道 */
#define GYRO_X_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_x_raw"
#define GYRO_Y_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_y_raw"
#define GYRO_Z_PATH    "/sys/bus/iio/devices/iio:device1/in_anglvel_z_raw"
#define GYRO_SCALE     "/sys/bus/iio/devices/iio:device1/in_anglvel_scale"

/* BH1750 环境光传感器（iio:device2）的 sysfs 路径 */
#define ILLUM_INPUT    "/sys/bus/iio/devices/iio:device2/in_illuminance_input"
#define ILLUM_RAW      "/sys/bus/iio/devices/iio:device2/in_illuminance_raw"
#define ILLUM_SCALE    "/sys/bus/iio/devices/iio:device2/in_illuminance_scale"

/* 传感器量程比例因子（raw 值 × scale = 物理量） */
static float accel_scale = 1.0f;   // 加速度 scale（m/s² / LSB）
static float gyro_scale  = 1.0f;   // 陀螺仪 scale（°/s / LSB）

/* 零点偏移（校准后记录的水平姿态基准读数） */
static float off_ax = 0, off_ay = 0, off_az = 0;
static float off_gx = 0, off_gy = 0, off_gz = 0;

/* 低通滤波状态（对加速度做一阶低通，平滑噪声） */
static float filt_ax = 0, filt_ay = 0, filt_az = 0;

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

/* 读取 sysfs 中的浮点值，读不到返回默认值 def */
static float read_float(const char *path, float def)
{
    FILE *fp = fopen(path, "r");
    if(!fp) return def;
    float v = def;
    fscanf(fp, "%f", &v);
    fclose(fp);
    return v;
}

/* 初始化：读量程比例因子（进入传感器实验室时调用一次） */
void sensor_init(void)
{
    accel_scale = read_float(ACCEL_SCALE, 1.0f);
    gyro_scale  = read_float(GYRO_SCALE, 1.0f);
    printf("[sensor] accel_scale=%f gyro_scale=%f\n", accel_scale, gyro_scale);
}

/* 零点校准：把当前姿态读数记为零点偏移，并重置低通滤波状态 */
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


/* 加速度 X 轴（单位 g，减零点 + 低通滤波） */
float sensor_accel_x(void)
{
    float raw = (read_raw(ACCEL_X_PATH) - off_ax) * accel_scale / G;
    filt_ax = filt_ax * 0.7f + raw * 0.3f;   // 一阶低通：0.7 旧值 + 0.3 新值
    return filt_ax;
}

/* 加速度 Y 轴（单位 g） */
float sensor_accel_y(void)
{
    float raw = (read_raw(ACCEL_Y_PATH) - off_ay) * accel_scale / G;
    filt_ay = filt_ay * 0.7f + raw * 0.3f;
    return filt_ay;
}

/* 加速度 Z 轴（单位 g，水平放置时约为 1g） */
float sensor_accel_z(void)
{
    float raw = (read_raw(ACCEL_Z_PATH) - off_az) * accel_scale / G;
    filt_az = filt_az * 0.7f + raw * 0.3f;
    return filt_az;
}

/* 陀螺仪三轴（单位 °/s） */
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


/*
 * 用加速度计解算姿态角（水平 = 0°）。
 * 注意：此函数内部会再读三次加速度，若调用方已经读好 ax/ay/az，
 *       建议直接按同样的公式算，避免重复读文件（见 sensor_lab.c）。
 */
void sensor_pitch_roll(float *pitch, float *roll)
{
    float ax = sensor_accel_x();
    float ay = sensor_accel_y();
    float az = sensor_accel_z();
    *pitch = atan2f(-ay, sqrtf(ax * ax + az * az)) * 180.0f / PI;  // 俯仰（前后，ay）
    *roll  = atan2f( ax, sqrtf(ay * ay + az * az)) * 180.0f / PI;  // 横滚（左右，ax）
}


/* 读取环境光照度（单位 lux） */
int sensor_illuminance(void)
{
    FILE *fp = fopen(ILLUM_INPUT, "r");
    if(fp) {
        int v = 0;
        fscanf(fp, "%d", &v);
        fclose(fp);
        return v;
    }
    int raw = read_raw(ILLUM_RAW);
    float scale = read_float(ILLUM_SCALE, 1.0f);
    return (int)(raw * scale);
}
