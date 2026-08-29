<p align="center">
  <img src="https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/logo.png" width="128" alt="logo" />
</p>

<h1 align="center">智趣魔方</h1>

<p align="center">
  基于 <b>RK3568</b> 开发板的<strong>体感多媒体娱乐终端</strong>（v1.0），集成登录、桌面、体感游戏、电子相册、传感器实验与系统设置，搭配 MPU6050 陀螺仪实现真正的"动手玩"。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-1.0-blue" alt="version" />
  <img src="https://img.shields.io/badge/platform-RK3568%20(aarch64)-blue" alt="platform" />
  <img src="https://img.shields.io/badge/gui-LVGL-orange" alt="lvgl" />
  <img src="https://img.shields.io/badge/font-FreeType-green" alt="freetype" />
  <img src="https://img.shields.io/badge/language-C99-lightgrey" alt="language" />
  <img src="https://img.shields.io/badge/build-CMake%20%2F%20Makefile-blueviolet" alt="build" />
</p>

---

## 项目简介

智趣魔方是一款运行在 RK3568 开发板上的体感多媒体娱乐终端，使用 C 语言 + [LVGL](https://lvgl.io/) 图形库开发。系统启动后进入登录界面（支持账号密码登录 + 游客模式），登录后进入桌面 Launcher，可进入 4 个应用：体感游戏中心、电子相册、传感器实验室和系统设置。

项目最大的亮点是**体感交互** —— 借助板载 MPU6050 六轴传感器，通过倾斜/摇动开发板即可控制游戏（2048 甩牌、重力滚球、贪吃蛇转向），无需外接手柄或键盘。首次进入游戏区会提示"将开发板水平放置 2 秒完成校准"，游戏区顶部还会实时显示陀螺仪就绪状态。

## 功能特性

| 模块 | 文件 | 说明 |
|------|------|------|
| 登录界面 | `mycode/login.c` | 系统启动入口，登录后跳转桌面 |
| 桌面 Launcher | `mycode/main_interface.c` | 应用选择主界面，各子界面返回时复用 |
| 游戏中心 | `mycode/game_center.c` | 体感游戏选择界面 |
| 2048 | `mycode/2048.c` | 体感版 2048，倾斜开发板移动数字块 |
| 贪吃蛇 | `mycode/snake.c` | 体感贪吃蛇，摇动控制方向 |
| 重力滚球 | `mycode/ball.c` | 重力感应滚球游戏 |
| 电子相册 | `mycode/album.c` | 照片浏览与缩略图 |
| 传感器实验室 | `mycode/sensor_lab.c` | 实时显示传感器数据 |
| 设置 | `mycode/settings.c` | 系统设置 |
| IMU 数据层 | `mycode/imu.c` | 加速度计倾斜读取（体感游戏用） |
| 传感器数据层 | `mycode/sensor.c` | MPU6050 + BH1750 数据读取 |

## 硬件平台

- **主控**：Rockchip RK3568 开发板（aarch64，交叉编译）
- **显示**：1024×600 LCD 触摸屏（framebuffer `/dev/fb0` + evdev 触摸输入）
- **传感器**：
  - MPU6050 —— 三轴加速度计 + 三轴陀螺仪（体感游戏、姿态检测）
  - BH1750 —— 光照强度传感器（照度测量）

## 技术栈

- **语言**：C99
- **图形库**：[LVGL](https://lvgl.io/)（轻量嵌入式 GUI 库）
- **字体渲染**：[FreeType 2.13.3](https://freetype.org/) + 内置中文字体 `STLITI.TTF`
- **构建工具**：CMake / Makefile
- **交叉编译**：aarch64-linux-gnu 工具链

## 目录结构

```
rk3568_demo/
├── main.c                  # 程序入口
├── mouse_cursor_icon.c     # 鼠标光标图标
├── lv_conf.h               # LVGL 配置
├── STLITI.TTF              # 中文字体
├── CMakeLists.txt          # CMake 构建脚本（交叉编译）
├── Makefile                # Make 构建脚本
├── mycode/                 # 业务代码（12 个模块）
│   ├── login.c/h           # 登录
│   ├── main_interface.c/h  # 桌面 Launcher
│   ├── game_center.c/h     # 游戏中心
│   ├── 2048.c/h            # 2048
│   ├── snake.c/h           # 贪吃蛇
│   ├── ball.c/h            # 重力滚球
│   ├── album.c/h           # 电子相册
│   ├── sensor.c/h          # 传感器数据层
│   ├── sensor_lab.c/h      # 传感器实验室
│   ├── imu.c/h             # IMU 数据层
│   ├── settings.c/h        # 设置
│   └── test.c/h            # 测试
├── lvgl/                   # LVGL 图形库（完整源码）
├── freetype-2.13.3/        # FreeType 字体库（完整源码）
├── bmp_pic/                # 界面图片资源
│   ├── bg/                 # 各界面背景图
│   ├── icon/               # 应用图标
│   ├── logo/               # Logo
│   └── photo/              # 相册照片与缩略图
├── 2048pic/                # 2048 数字块图片
├── docs/images/            # README 预览图
├── build/                  # 编译产物（CMake 缓存、目标文件）
└── bin/main                # 编译生成的可执行文件
```

## 界面预览

| 登录界面 | 桌面主界面 |
|:---:|:---:|
| ![登录](https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/login.png) | ![桌面](https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/desktop.png) |

| 体感游戏中心 | 电子相册 | 设置 |
|:---:|:---:|:---:|
| ![游戏](https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/game-center.png) | ![相册](https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/album.jpg) | ![设置](https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/settings.png) |

## 编译与运行

### 1. 交叉编译（RK3568 开发板）

需要 aarch64-linux-gnu 交叉编译工具链，默认路径为 `/usr/local/arm-linux/bin/`，请根据实际环境修改 `CMakeLists.txt` 中的编译器路径：

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译产物输出到 `bin/main`，拷贝到开发板后运行：

```bash
./bin/main
```

### 2. 使用 Makefile 编译

```bash
make          # 编译，输出 build/bin/demo
make clean    # 清理
make install  # 安装到系统
```

### 运行环境变量

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `LV_LINUX_FBDEV_DEVICE` | `/dev/fb0` | framebuffer 设备 |
| `LV_LINUX_DRM_CARD` | `/dev/dri/card0` | DRM 显卡设备 |
| `LV_SDL_VIDEO_WIDTH` | `1024` | SDL 模拟窗口宽度 |
| `LV_SDL_VIDEO_HEIGHT` | `600` | SDL 模拟窗口高度 |

## 图片资源说明

- 界面背景、图标、Logo、相册照片等资源统一放在 `bmp_pic/` 目录下，按 `bg` / `icon` / `logo` / `photo` 分类。
- 2048 游戏的数字块图片放在 `2048pic/` 目录下（`0.bmp` ~ `2048.bmp`，对应不同数值）。
- 代码中通过绝对路径 `A:/work_space/...` 引用这些图片，部署时请将图片目录放到对应路径，或按需修改源码中的路径。

## 依赖与致谢

- [LVGL](https://github.com/lvgl/lvgl) — 轻量级嵌入式图形库
- [FreeType](https://gitlab.freedesktop.org/freetype/freetype) — 字体渲染引擎

## 许可证

本项目仅供学习与演示使用。
