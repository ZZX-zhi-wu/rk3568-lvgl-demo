/**
 * @file    net_pkt.h
 * @brief   网络封包 / 拆包公共层
 *
 * 把课程资料里「聊天室」和「FTP」两套代码中逐字重复的 4 个函数收成一份：
 *   write_full / read_full / send_packet / recv_packet
 *
 * 报文格式（两套协议共用）：
 *   ┌────────────────┬──────────────────────┐
 *   │ 4 字节网络序长度 │  length 字节的载荷   │
 *   └────────────────┴──────────────────────┘
 *   长度头用 htonl 转网络字节序，解决 TCP 粘包 / 拆包问题。
 *
 * 本文件不依赖 LVGL，也不依赖 cJSON，可单独编译。
 */
#ifndef __NET_PKT_H__
#define __NET_PKT_H__

#include <stddef.h>

/* 单个报文载荷的最大字节数（JSON 文本，不含 4 字节长度头） */
#define NET_PKT_MAX 4096

/* 返回值约定（三个接收类函数共用）
 *   0   成功
 *  -1   对端关闭 / 套接字错误 / 报文超长
 *  -2   接收超时（errno == EAGAIN），调用方可据此继续轮询
 */
#define NET_PKT_OK       0
#define NET_PKT_ERR     (-1)
#define NET_PKT_TIMEOUT (-2)

/**
 * 循环发送 len 字节，保证全部写完。
 * @return NET_PKT_OK 成功；NET_PKT_ERR 失败
 */
int net_write_full(int sockfd, const void *buf, int len);

/**
 * 循环接收 len 字节，保证全部读齐。
 * @return NET_PKT_OK 成功；NET_PKT_TIMEOUT 超时；NET_PKT_ERR 对端关闭或错误
 */
int net_read_full(int sockfd, void *buf, int len);

/**
 * 封包发送：先发 4 字节网络序长度，再发载荷。
 * @return NET_PKT_OK 成功；NET_PKT_ERR 失败
 */
int net_send_packet(int sockfd, const void *buf, int len);

/**
 * 拆包接收：先读 4 字节长度，再读对应长度的载荷。
 * @param buf    接收缓冲区（会自动补 '\0'，所以 maxlen 要留 1 字节余量）
 * @param maxlen 缓冲区大小
 * @return >0 实际载荷长度；0 空包；NET_PKT_TIMEOUT 超时；NET_PKT_ERR 失败
 */
int net_recv_packet(int sockfd, char *buf, int maxlen);

/**
 * 便捷封装：把 C 字符串作为载荷发出（长度取 strlen）。
 */
int net_send_str(int sockfd, const char *str);

/**
 * 设置套接字接收超时（毫秒）。0 表示不超时（恢复阻塞）。
 * 板端接收线程靠它做「1 秒轮询 + 检查退出标志」，缺了它线程退不出去。
 */
void net_set_recv_timeout(int sockfd, int ms);

/**
 * 设置套接字发送超时（毫秒）。
 */
void net_set_send_timeout(int sockfd, int ms);

/**
 * 带超时的 TCP 连接（内部用非阻塞 connect + select 实现）。
 * 课程资料里的 connect() 是阻塞的，服务器没开时要等 20 秒以上才返回，
 * 放在 LVGL 主线程里会让整个界面僵住，所以必须用这个版本。
 *
 * @param ip         目标 IP
 * @param port       目标端口
 * @param timeout_ms 超时毫秒数
 * @param err        失败原因输出缓冲（可为 NULL）
 * @param err_size   err 缓冲区大小
 * @return >=0 已连接的套接字 fd；-1 失败
 */
int net_connect_timeout(const char *ip, int port, int timeout_ms,
                        char *err, int err_size);

#endif /* __NET_PKT_H__ */
