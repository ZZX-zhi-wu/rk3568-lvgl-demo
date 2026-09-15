<p align="center">
  <img src="https://cdn.jsdelivr.net/gh/ZZX-zhi-wu/rk3568-lvgl-demo@main/docs/images/logo.png" width="128" alt="logo" />
</p>

<h1 align="center">智趣魔方</h1>

<p align="center">
  基于 <b>RK3568</b> 开发板的<strong>体感多媒体娱乐终端</strong>（v2.0），集成登录、桌面、体感游戏、电子相册、<strong>网络聊天室</strong>、传感器实验与系统设置，搭配 MPU6050 陀螺仪实现真正的"动手玩"。
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-2.0-blue" alt="version" />
  <img src="https://img.shields.io/badge/platform-RK3568%20(aarch64)-blue" alt="platform" />
  <img src="https://img.shields.io/badge/gui-LVGL-orange" alt="lvgl" />
  <img src="https://img.shields.io/badge/font-FreeType-green" alt="freetype" />
  <img src="https://img.shields.io/badge/language-C99-lightgrey" alt="language" />
  <img src="https://img.shields.io/badge/build-CMake%20%2F%20Makefile-blueviolet" alt="build" />
  <img src="https://img.shields.io/badge/network-TCP%20%2B%20cJSON-yellow" alt="network" />
</p>

---

## 项目简介

智趣魔方是一款运行在 RK3568 开发板上的体感多媒体娱乐终端，使用 C 语言 + [LVGL](https://lvgl.io/) 图形库开发。系统启动后进入登录界面（支持账号密码登录 + 游客模式），登录后进入桌面 Launcher，可进入 **5 个应用**：传感器实验室、体感游戏中心、电子相册、系统设置和网络聊天室。

项目有两个技术亮点：

- **体感交互** —— 借助板载 MPU6050 六轴传感器，通过倾斜/摇动开发板即可控制游戏（2048 甩牌、重力滚球、贪吃蛇转向），无需外接手柄或键盘。首次进入游戏区会提示"将开发板水平放置 2 秒完成校准"，游戏区顶部还会实时显示陀螺仪就绪状态。
- **局域网互联** —— 自研 TCP 长连接聊天室，多块开发板之间可实时收发消息（公共大厅 + 私聊）；另有一套基于 JSON 命令协议的 FTP 文件传输，让「云相册」能从服务器拉取照片到本地。

## 功能特性

### 本地功能

| 模块 | 文件 | 说明 |
|------|------|------|
| 登录界面 | `mycode/login.c` | 系统启动入口，登录后跳转桌面 |
| 桌面 Launcher | `mycode/main_interface.c` | 应用选择主界面，5 个入口卡片横排 |
| 游戏中心 | `mycode/game_center.c` | 体感游戏选择界面 |
| 2048 | `mycode/2048.c` | 体感版 2048，倾斜开发板移动数字块 |
| 贪吃蛇 | `mycode/snake.c` | 体感贪吃蛇，摇动控制方向 |
| 重力滚球 | `mycode/ball.c` | 重力感应滚球游戏 |
| 电子相册 | `mycode/album.c` | 动态张数扫描 + 缩略图条 + 点击切换 + 照片删除 |
| 传感器实验室 | `mycode/sensor_lab.c` | 实时显示传感器数据与曲线 |
| 设置 | `mycode/settings.c` | 背光亮度、自动亮度、体感/触摸模式 |
| IMU 数据层 | `mycode/imu.c` | 加速度计倾斜读取（体感游戏用） |
| 传感器数据层 | `mycode/sensor.c` | MPU6050 + BH1750 数据读取 |

### 网络功能

| 模块 | 文件 | 说明 |
|------|------|------|
| 网络聊天室 | `mycode/chat.c` + `mycode/chat_ui.c` | TCP 长连接 + JSON 协议，公共大厅 / 私聊 / 在线列表，内置中文拼音输入法 |
| 云相册 | `mycode/album_cloud.c` | 相册页内的弹层，连接服务器浏览并下载照片（大图+缩略图成对落盘） |
| FTP 文件传输 | `mycode/ftp.c` | 按 JSON 命令协议实现列表 / 下载 / 上传，带进度与状态上报 |
| 网络封包层 | `mycode/net_pkt.c` | TCP 粘包拆包处理（4 字节长度头），超时与错误三分语义 |
| JSON 解析 | `mycode/cJSON.c` | 第三方库，负责协议的编解码 |

### 相册功能升级（v2.0）

`album.c` 相比 v1.0 有 5 处实质改动：

1. **照片张数由编译期常量改为运行时扫描** —— 原版 `PHOTO_COUNT` 写死 6 张，放到第 7 张就会重叠；现在 `scan_photos()` 扫描 `photo_N.bmp` + `thumb_N.bmp` 成对存在的照片，并按编号升序排列，上限 12 张（受 `LV_MEM_SIZE` 限制）。
2. **缩略图条改为 flex 横排 + 可横向滚动** —— 不再手算 6 个固定 x 坐标。
3. **缩略图可点击直接切换照片** —— 原版只能靠上一张/下一张按钮。
4. **顶部新增「云相册」入口** —— 点开进入 `album_cloud.c` 的弹层。
5. **顶部新增「删除」按钮** —— 删除当前照片及其配套缩略图；做成"点两下"确认（第一下进入待确认状态，3 秒不点自动复位），防止误触删盘。

## 硬件平台

- **主控**：Rockchip RK3568 开发板（aarch64，交叉编译）
- **显示**：1024×600 LCD 触摸屏（framebuffer `/dev/fb0` + evdev 触摸输入）
- **传感器**：
  - MPU6050 —— 三轴加速度计 + 三轴陀螺仪（体感游戏、姿态检测）
  - BH1750 —— 光照强度传感器（照度测量、自动亮度）

## 技术栈

- **语言**：C99
- **图形库**：[LVGL](https://lvgl.io/)（轻量嵌入式 GUI 库）
- **字体渲染**：[FreeType 2.13.3](https://freetype.org/) + 系统中文字体 `msyh.ttc`
- **网络**：POSIX socket（TCP）+ pthread 多线程 + [cJSON](https://github.com/DaveGamble/cJSON) 协议编解码
- **构建工具**：CMake / Makefile
- **交叉编译**：aarch64-linux-gnu 工具链

## 网络通信架构

聊天室与 FTP 两套业务共用同一个封包层 `net_pkt.c`，两者都是"连上服务器，收发一段 JSON"。

### 报文格式

TCP 是字节流，没有消息边界 —— `send` 两次 100 字节，对端可能一次收到 200 字节（粘包），也可能分三次才收齐（拆包）。所以必须自己划边界：

```
┌──────────────────┬────────────────────────────┐
│ 4 字节网络序长度  │  length 字节的载荷（JSON）  │
└──────────────────┴────────────────────────────┘
```

发送端先用 `htonl()` 把长度转成网络序（大端）写出去，接收端先用 `read_full` 精确读 4 字节拿到长度，再精确读 `length` 字节，才能保证一次调用正好拿到一个完整报文。

### 线程模型

`net_pkt.c` 的返回值严格三分（`NET_PKT_OK` / `NET_PKT_TIMEOUT` / `NET_PKT_ERR`），这是后台线程能够优雅退出的前提：

- **聊天室**（`chat.c`）需要一个常驻接收线程，因为消息随时可能被推过来（被动收）。套接字设 1 秒 `SO_RCVTIMEO`，超时返回 `EAGAIN` → 转成 `NET_PKT_TIMEOUT` → 线程每秒醒一次检查退出标志。断开连接时先 `shutdown(SHUT_RDWR)` 唤醒阻塞的 `recv`，再 `pthread_join` 回收。
- **FTP**（`ftp.c`）是严格"一问一答"，收数据的时机完全由本端掌握，所以在任务线程里顺序收发即可，**不需要额外接收线程**。

**LVGL 不是线程安全的**，所以有一条贯穿全工程的红线：`chat.c` / `ftp.c` / `net_pkt.c` 里**不出现任何 `lv_*` 调用**。网络线程只把事件塞进环形队列，界面层在自己的定时器回调（也就是 LVGL 线程）里取出来渲染。

### 聊天室协议

| 发送命令 | 期望回包 `message` | 说明 |
|---|---|---|
| `register` | `success` + `welcome` | 注册昵称，拿到本端 id |
| `list` | `online_users` | 请求在线用户列表 |
| `broadcast` | `public_message` | 公共大厅发言（发送者自己不收） |
| `pm` | `private_message` | 私聊指定 id 的用户 |

### 中文输入

聊天输入框挂的是 LVGL 内置的 `lv_ime_pinyin` 拼音输入法：按键先把原始拼音插进输入框，选中候选汉字时输入法再按字符数回删并写入汉字。候选条是 `lv_buttonmatrix`，它的 label 描述符初始化在 `LV_PART_ITEMS` 分区，而输入法的样式同步事件只覆盖 `part 0` —— 所以两个分区的字体都要显式设置，否则候选字全是方框。

## 目录结构

```
rk3568_demo/
├── main.c                  # 程序入口
├── mouse_cursor_icon.c     # 鼠标光标图标
├── lv_conf.h               # LVGL 配置
├── STLITI.TTF              # 早期使用的字体（当前代码已统一改走 /work_space/font/msyh.ttc）
├── CMakeLists.txt          # CMake 构建脚本（交叉编译）
├── Makefile                # Make 构建脚本
├── mycode/                 # 业务代码（18 个模块）
│   ├── login.c/h           # 登录
│   ├── main_interface.c/h  # 桌面 Launcher（5 个应用入口）
│   ├── game_center.c/h     # 游戏中心
│   ├── 2048.c/h            # 2048
│   ├── snake.c/h           # 贪吃蛇
│   ├── ball.c/h            # 重力滚球
│   ├── album.c/h           # 电子相册
│   ├── album_cloud.c/h     # 云相册（弹层，走 FTP 下载）
│   ├── chat.c/h            # 聊天室网络层（socket + 接收线程 + 事件队列）
│   ├── chat_ui.c/h         # 聊天室界面层（连接页 + 聊天页 + 拼音输入法）
│   ├── ftp.c/h             # FTP 文件传输
│   ├── net_pkt.c/h         # 网络封包 / 拆包公共层
│   ├── cJSON.c/h           # JSON 解析（第三方库）
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

`CMakeLists.txt` 用 `aux_source_directory(mycode SRC_FILE)` 收集源码，所以 `mycode/` 下新增 `.c` 文件会自动参与编译，不需要改构建脚本。编译产物输出到 `bin/main`，拷贝到开发板后运行：

```bash
./bin/main
```

> 带独立 `main()` 的程序（如终端版聊天客户端）不能放在 `mycode/` 下，否则会和 `main.c` 的 `main` 重复定义；需要单独放到 `tools/` 并另开一个 `add_executable`。

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
- 代码中通过绝对路径 `A:/work_space/...` 引用这些图片，部署时请将图片目录放到对应路径，或按需修改源码中的路径。相册要求 `photo_N.bmp`（大图 1024×600）与 `thumb_N.bmp`（缩略图 140×82）**成对存在**，否则该张照片会被跳过。
- 聊天室图标为 `bmp_pic/icon/icon_chat.bmp`。

## 部署说明

开发板上的资源需要放在 `/work_space/` 下：

| 板端路径 | 内容 |
|---|---|
| `/work_space/bmp_pic/` | 界面图片资源（背景、图标、Logo、相册照片） |
| `/work_space/2048pic/` | 2048 数字块图片 |
| `/work_space/font/msyh.ttc` | 中文字体（微软雅黑）。工程中各模块统一用 `#define CN_FONT_PATH` 指向它 |

`lv_conf.h` 里配置的是 `LV_FS_POSIX_LETTER 'A'` + `LV_FS_POSIX_PATH ""`，所以代码里写的 `A:/work_space/bmp_pic/...` 就是板上 `/work_space/bmp_pic/...` 的绝对路径。

## 已知问题

- 登录失败时界面没有反馈，只在串口打印日志；更好的做法是像聊天室连接页那样加一个状态标签。
- `album.c` 的字体缓存函数缺少 `if(empty < 0) return NULL;` 边界保护，缓存满 4 种字号后再请求第 5 种会有越界写风险（`album_cloud.c` / `chat_ui.c` 的同名函数都有这层保护）。
- `ftp.c` 的 `do_put()` 在上传超时后 `continue` 会重发整块数据，服务器端文件在断点处会多出重复内容，而客户端仍会报告上传成功。

## 依赖与致谢

- [LVGL](https://github.com/lvgl/lvgl) — 轻量级嵌入式图形库
- [FreeType](https://gitlab.freedesktop.org/freetype/freetype) — 字体渲染引擎
- [cJSON](https://github.com/DaveGamble/cJSON) — 超轻量 JSON 解析器

## 许可证

本项目仅供学习与演示使用。
