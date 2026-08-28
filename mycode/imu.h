/**
 * @file    imu.h
 * @brief   IMU 数据层接口声明（体感游戏用）
 */
#ifndef __IMU_H__
#define __IMU_H__

/* 读取加速度计倾斜值（已减零点偏移 + 方向修正） */
int imu_tilt_x(void);
int imu_tilt_y(void);

/* 校准：把当前姿态作为零点 */
void imu_calibrate(void);

#endif
