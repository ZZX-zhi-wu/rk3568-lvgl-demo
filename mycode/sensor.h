/**
 * @file    sensor.h
 * @brief   传感器数据层接口声明（MPU6050 + BH1750）
 */
#ifndef __SENSOR_H__
#define __SENSOR_H__

void sensor_init(void);       // 读量程比例因子（进入实验室时调用一次）
void sensor_calibrate(void);  // 校准零点（水平放置时调用）

float sensor_accel_x(void);   // 加速度三轴，单位 g
float sensor_accel_y(void);
float sensor_accel_z(void);

float sensor_gyro_x(void);    // 陀螺仪三轴，单位 °/s
float sensor_gyro_y(void);
float sensor_gyro_z(void);

void sensor_pitch_roll(float *pitch, float *roll);  // 姿态角（度）
int  sensor_illuminance(void);                      // 照度（lux）

#endif
