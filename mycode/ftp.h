/**
 * @file    ftp.h
 * @brief   FTP 文件传输 —— 网络层接口（云相册用）
 *
 * ★★ 本文件与 ftp.c 不含任何 lv_* 调用 ★★
 *
 * 与聊天室的模型差异（很重要，别照抄 chat.c）：
 *   聊天室：长连接 + 常驻接收线程 + 事件队列（消息随时可能来）
 *   FTP   ：请求-响应同步模型，不需要常驻线程
 *           → 用「任务线程 + 深度 1 的任务槽」：
 *             UI 提交任务 → 后台线程执行 → 写进度变量 → LVGL 定时器读进度
 *
 * 协议沿用课程资料 2-FTP文件传输，注意它和聊天室的 command/data 风格不同，
 * 报文要单独组装，不能复用 chat 的封装：
 *   发  {"cmd":"ls"}
 *   收  {"status":"ok","files":["a.bmp",...],"sizes":[123,...]}
 *   发  {"cmd":"get","filename":"a.bmp"}
 *   收  {"status":"ok","filesize":123}  紧接 filesize 字节裸数据
 *   发  {"cmd":"put","filename":"a.bmp","filesize":123}  紧接 filesize 字节裸数据
 *   收  {"status":"ok"}
 *   出错收 {"status":"error","message":"file not found"}
 */
#ifndef __FTP_H__
#define __FTP_H__

#ifdef __cplusplus
extern "C" {
#endif

#define FTP_MAX_FILES 64
#define FTP_NAME_LEN  64

/* 任务状态机：UI 定时器轮询它来决定界面显示 */
typedef enum {
    FTP_ST_IDLE = 0,    /* 空闲，没有任务 */
    FTP_ST_RUNNING,     /* 任务进行中，可读 ftp_progress() */
    FTP_ST_DONE,        /* 上一次任务成功 */
    FTP_ST_ERROR,       /* 上一次任务失败，原因见 ftp_message() */
} ftp_state_t;

/**
 * 连接 FTP 服务器（带超时）。
 * @return 0 成功；-1 失败（原因见 ftp_message()）
 */
int  ftp_connect(const char *ip, int port, int timeout_ms);

/**
 * 断开。若此刻有任务在跑，会先 shutdown 打断它再关闭。
 */
void ftp_disconnect(void);

int  ftp_is_connected(void);

/**
 * 提交「列目录」任务。结果用 ftp_file_count() / ftp_file_name() / ftp_file_size() 取。
 * @return 0 已提交；-1 失败（未连接 / 上一个任务还没跑完）
 */
int  ftp_list_begin(void);

/**
 * 提交「下载」任务。
 * 实现细节：先写到 local_path + ".tmp"，全部收完才 rename 成正式名，
 * 中途断线只会留下一个被删掉的临时文件，不会出现半张坏图。
 *
 * @param remote_name 服务端文件名，例如 "photo_7.bmp"
 * @param local_path  本地完整路径，例如 "/work_space/bmp_pic/photo/photo_7.bmp"
 */
int  ftp_get_begin(const char *remote_name, const char *local_path);

/**
 * 提交「上传」任务（资料里没有，这是为板子新增的）。
 * @param local_path  本地完整路径
 * @param remote_name 服务端保存的文件名
 */
int  ftp_put_begin(const char *local_path, const char *remote_name);

/** 当前任务状态 */
ftp_state_t ftp_state(void);

/** 当前任务进度 0~100 */
int  ftp_progress(void);

/** 最近一次提示 / 错误信息（界面状态行用） */
const char *ftp_message(void);

/** 把状态复位成 IDLE（界面关掉弹层时调用） */
void ftp_reset_state(void);

/* ---- 列目录结果 ---- */
int  ftp_file_count(void);
const char *ftp_file_name(int idx);
long ftp_file_size(int idx);

#ifdef __cplusplus
}
#endif

#endif /* __FTP_H__ */
