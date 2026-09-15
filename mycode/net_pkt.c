/* ============================================================================
 * @file    net_pkt.c
 * @brief   网络封包 / 拆包公共层实现
 *
 * 【这个文件在整个工程里的位置】
 *
 *   chat.c（聊天室网络层）──┐
 *                          ├──► net_pkt.c（本文件）──► socket API
 *   ftp.c（FTP 传输层）  ───┘
 *
 *   上层只关心「我要发一段 JSON」和「我要收一段 JSON」，
 *   不关心 TCP 的粘包/拆包、字节序、部分读写、超时，全部由本文件包掉。
 *
 * 【本文件的三条铁律】
 *   1. 不依赖 LVGL，也不依赖 cJSON —— 可单独编译、单独测试。
 *   2. 所有函数都是阻塞语义，但阻塞时长受 SO_RCVTIMEO / SO_SNDTIMEO 控制。
 *   3. 返回值严格三分：NET_PKT_OK / NET_PKT_TIMEOUT / NET_PKT_ERR，
 *      上层靠 NET_PKT_TIMEOUT 来「醒来检查退出标志」，这是线程能正常退出的前提。
 *
 * 【报文格式（聊天室与 FTP 两套协议共用）】
 *
 *   ┌──────────────────┬────────────────────────────┐
 *   │ 4 字节网络序长度  │  length 字节的载荷（JSON）  │
 *   └──────────────────┴────────────────────────────┘
 *
 *   为什么需要这个长度头？
 *     TCP 是字节流，没有消息边界。你 send 两次 100 字节，
 *     对端可能一次 recv 到 200 字节（粘包），也可能分 3 次才收齐（拆包）。
 *     所以必须自己划边界：先声明「接下来有 N 字节」，对端先读 4 字节拿到 N，
 *     再精确读 N 字节，才能保证一次调用正好拿到一个完整报文。
 *
 *   为什么长度要用 htonl 转网络序？
 *     不同 CPU 的多字节整数在内存里排列顺序不同（大端/小端）。
 *     直接发 4 字节内存原文，两端字节序不同就会读出荒谬的长度值。
 *     htonl = host to network long，统一成网络序（大端）后再发。
 *
 * 【相比课程资料的原版，这里补了 3 处健壮性处理（原版会踩的坑）】
 *   1. recv 被信号打断（EINTR）时重试，而不是直接当失败返回；
 *      —— 原版把 EINTR 当错误，导致偶发「莫名其妙的断线」。
 *   2. 区分「对端关闭」和「接收超时」：超时返回 NET_PKT_TIMEOUT，
 *      让上层线程可以「醒来看一眼退出标志再继续等」，否则线程永远退不出去；
 *      —— 这是 chat.c 里 recv_thread 能优雅退出的关键。
 *   3. 长度头读取也走循环读（原版 FTP 的 recv_json 是单次 recv，
 *      网络抖动时只收到 2 字节长度头就会错位，后续全部解析失败）。
 * ========================================================================== */

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "net_pkt.h"


/* ==========================================================================
 * 1. 底层收发
 * ========================================================================== */

/**
 * 循环发送，保证 len 字节全部写完。
 *
 * 【为什么不能只调一次 send？】
 *   send() 的返回值是「本次实际写出去的字节数」，可能小于 len。
 *   当发送缓冲区快满时，内核只收走一部分就返回，剩下的要你自己再发。
 *   比如 send 1000 字节返回 300，那还剩 700 字节没发出去 ——
 *   如果不检查返回值就当成「发完了」，对端就会一直等不到完整报文而卡死。
 *   所以这里用 while 循环，把「已发指针 p 往后挪、剩余 left 减少」做到 left == 0。
 *
 * @param sockfd 已连接的套接字
 * @param buf    待发送数据首地址
 * @param len    待发送字节数
 * @return NET_PKT_OK 全部写完；NET_PKT_ERR 出错（含发送超时）
 *
 * ★ 注意（潜在坑）：本函数把 EAGAIN 也归为 NET_PKT_ERR。
 *   套接字设了 SO_SNDTIMEO 之后，缓冲区满时 send 会返回 -1 / EAGAIN。
 *   此时「已经发出去的那部分字节」已经到达对端，无法撤回。
 *   调用方（见 ftp.c 的 do_put）如果拿到 ERR 后重发整个缓冲区，
 *   对端就会收到一段重复数据。这里不改语义，只在此备案。
 */
int net_write_full(int sockfd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int left = len;

    while(left > 0) {
        ssize_t w = send(sockfd, p, left, 0);
        if(w > 0) {                      /* 写出去一部分，挪指针继续 */
            p += w;
            left -= (int)w;
            continue;
        }
        if(w < 0 && errno == EINTR)      /* 被信号打断，重来 */
            continue;
        return NET_PKT_ERR;              /* 含 EAGAIN：发送超时 */
    }
    return NET_PKT_OK;
}

/**
 * 循环接收，保证 len 字节全部读齐。
 *
 * 【三种返回情况必须分清楚，这是上层逻辑的基础】
 *   r > 0           读到数据，继续读剩余部分
 *   r == 0          对端「有序关闭」（发了 FIN）。这是正常的连接结束信号，
 *                   不是错误 —— 但对本层而言要报给上层，让上层收摊。
 *   r == -1 + EINTR     被信号打断，重试（什么都不做，循环下一轮）
 *   r == -1 + EAGAIN    接收超时。★关键★ 这正是 SO_RCVTIMEO 到点后的表现。
 *                       返回 NET_PKT_TIMEOUT 而不是 ERR，
 *                       让上层可以「醒一下 → 查退出标志 → 没退出就继续等」。
 *   r == -1 + 其他      真正的错误（连接被重置等），返回 ERR。
 *
 * @return NET_PKT_OK 读齐；NET_PKT_TIMEOUT 超时；NET_PKT_ERR 对端关闭或错误
 */
int net_read_full(int sockfd, void *buf, int len)
{
    char *p = (char *)buf;
    int left = len;

    while(left > 0) {
        ssize_t r = recv(sockfd, p, left, 0);
        if(r > 0) {                      /* 读到一部分，挪指针继续 */
            p += r;
            left -= (int)r;
            continue;
        }
        if(r == 0)                       /* 对端有序关闭（FIN），非错误 */
            return NET_PKT_ERR;
        if(errno == EINTR)               /* 被信号打断，重来 */
            continue;
        if(errno == EAGAIN || errno == EWOULDBLOCK)
            return NET_PKT_TIMEOUT;      /* 接收超时：让上层有机会查退出标志 */
        return NET_PKT_ERR;              /* 其它错误（ECONNRESET 等） */
    }
    return NET_PKT_OK;
}


/* ==========================================================================
 * 2. 封包 / 拆包
 * ========================================================================== */

/**
 * 封包发送：先发 4 字节网络序长度头，再发载荷。
 *
 * 对应报文格式的前半部分。注意两个细节：
 *   - 长度头本身也要用 net_write_full（它只有 4 字节，但依然可能只发出去 2 字节）。
 *   - 载荷长度为 0 时跳过第二次发送（len > 0 判断），
 *     因为空载荷发出去对端读 0 字节也算成功，没必要多一次系统调用。
 *
 * @return NET_PKT_OK 成功；NET_PKT_ERR 失败（长度头和载荷任一环节失败都算）
 */
int net_send_packet(int sockfd, const void *buf, int len)
{
    /* htonl：把本机字节序的 32 位整数转成网络字节序（大端）。
     * 必须在「放到 socket 上」之前转，收到的一端再用 ntohl 转回来。 */
    int net_len = htonl(len);

    /* 第一步：发 4 字节长度头 */
    if(net_write_full(sockfd, &net_len, (int)sizeof(net_len)) != NET_PKT_OK)
        return NET_PKT_ERR;

    /* 第二步：发载荷（空载荷跳过） */
    if(len > 0 && net_write_full(sockfd, buf, len) != NET_PKT_OK)
        return NET_PKT_ERR;

    return NET_PKT_OK;
}

/**
 * 拆包接收：先读 4 字节长度头，再按长度读载荷。
 *
 * 对应报文格式的后半部分。返回值的「原样透传」是关键设计：
 *   读长度头失败时直接 `return r`，把 -1 / -2 两种语义原封不动交给上层。
 *   如果这里统一改写成 -1，上层就分不清「服务器挂了」和「暂时没数据」，
 *   要么误判断线、要么无法轮询，两种都是灾难。
 *
 * @param buf    接收缓冲区。函数会在载荷末尾补 '\0'，
 *               所以缓冲区必须至少 maxlen 字节、且 maxlen >= len + 1。
 * @param maxlen 缓冲区大小（含结尾 '\0' 的位置）
 *
 * @return >0           实际载荷长度（不含 '\0'）
 *         0            收到空包（对端故意发了长度 0）
 *         NET_PKT_TIMEOUT(-2)  超时，上层可继续轮询
 *         NET_PKT_ERR(-1)      对端关闭 / 长度非法 / socket 错误
 */
int net_recv_packet(int sockfd, char *buf, int maxlen)
{
    int net_len = 0;
    int r;

    /* 1. 读长度头（网络字节序 4 字节） */
    r = net_read_full(sockfd, &net_len, (int)sizeof(net_len));
    if(r != NET_PKT_OK)
        return r;                        /* -1 或 -2 原样透传，不吞掉超时语义 */

    /* 转回本机字节序，才是一个有意义的长度值 */
    int len = ntohl(net_len);

    /* 长度合法性检查 —— 这行是防止被恶意/错乱数据打爆缓冲区的唯一屏障：
     *   len < 0        长度头被解释成负数（对方字节序搞错或数据错位）
     *   len > maxlen-1 载荷放不进调用方的缓冲区
     * 为什么是 maxlen - 1 而不是 maxlen？因为下面要写 buf[len] = '\0'，
     * 必须给结尾符留出 1 字节，否则就是典型的缓冲区溢出。 */
    if(len < 0 || len > maxlen - 1) {    /* 留 1 字节给 '\0' */
        return NET_PKT_ERR;
    }
    if(len == 0) {                       /* 空包：只把缓冲区置空，直接返回 */
        if(buf && maxlen > 0) buf[0] = '\0';
        return 0;
    }

    /* 2. 读载荷（精确读 len 字节，同样是循环读） */
    r = net_read_full(sockfd, buf, len);
    if(r != NET_PKT_OK)
        return r;

    /* 3. 补字符串结尾符，让上层可以直接当 C 字符串用（配 cJSON_Parse 等）。
     *    上一条长度检查已经保证了这里不越界。 */
    buf[len] = '\0';
    return len;
}

/**
 * 便捷封装：把 C 字符串作为载荷发出（长度取 strlen）。
 *
 * 只用于「载荷本身就是文本」的场景（本工程全部报文都是 JSON 文本）。
 * 二进制数据必须用 net_send_packet 显式给长度 ——
 * 因为二进制里可能含 '\0'，strlen 会提前截断。
 */
int net_send_str(int sockfd, const char *str)
{
    return net_send_packet(sockfd, str, (int)strlen(str));
}


/* ==========================================================================
 * 3. 超时设置
 * ========================================================================== */

/**
 * 设置套接字接收超时（毫秒）。0 表示不超时（恢复阻塞）。
 *
 * 【为什么板端必须要这个】
 *   聊天室有一个独立接收线程 recv_thread，它的循环结构是：
 *       while(不退出) { 收报文; 检查退出标志; }
 *   如果套接字是纯阻塞的，「收报文」会一直挂着不动，主线程就算把退出标志置 1，
 *   这个线程也永远醒不过来 —— lv_obj_delete 之后的 join 会永久卡住界面。
 *   设了 1 秒超时之后，recv 最多等 1 秒就返回 EAGAIN，
 *   net_read_full 转成 NET_PKT_TIMEOUT，线程就能「每秒醒一次」去查退出标志。
 *   ★ 缺了它线程退不出去，是必须存在的设置。
 *
 * 【tv_usec 的换算】
 *   struct timeval 是「秒 + 微秒」两个字段，而毫秒需要拆开填：
 *     tv_sec  = ms / 1000           取整得秒数，如 1500ms → 1 秒
 *     tv_usec = (ms % 1000) * 1000  余数转微秒，如 1500ms → 500 * 1000 = 500000us
 *   写错成 ms * 1000 会让超时变成 1000 倍。
 */
void net_set_recv_timeout(int sockfd, int ms)
{
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/**
 * 设置套接字发送超时（毫秒）。
 *
 * 作用：防止对端不读数据导致本端 send 永远阻塞（界面卡死）。
 * 副作用：发大文件（相册大图 1.8MB）时若链路过慢，send 会超时返回 EAGAIN，
 *        被 net_write_full 归为 NET_PKT_ERR —— 见该函数头部的「潜在坑」说明。
 */
void net_set_send_timeout(int sockfd, int ms)
{
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}


/* ==========================================================================
 * 4. 带超时的 TCP 连接
 * ========================================================================== */

/**
 * 带超时的 TCP 连接（内部用非阻塞 connect + select 实现）。
 *
 * 【为什么必须自己写，不能用现成的 connect】
 *   connect() 是阻塞的，且内核默认重试 SYN 约 20 秒以上。
 *   本工程是在 LVGL 主线程里点「连接」按钮的 —— 直接调 connect，
 *   服务器没开时整个界面会僵死 20 秒，期间按钮不响应、画面不刷新，
 *   用户会以为程序崩了。所以必须能自己指定超时时间。
 *
 * 【非阻塞 connect + select 的三步套路】
 *   ① 把 fd 设成 O_NONBLOCK，再调 connect。
 *      此时 connect 立刻返回 -1 且 errno == EINPROGRESS
 *      —— 这不表示失败，而是「三次握手正在进行中，别急」。
 *   ② 用 select 监听这个 fd 的「可写」事件，带超时。
 *      连接成功或失败，套接字都会变成可写（这是判断连接完成的惯用手法）。
 *      select 返回 0 就是真超时，返回正数是「可写了，去看结果」。
 *   ③ 可写不代表成功！必须用 getsockopt(SO_ERROR) 取回真正的结果：
 *      SO_ERROR == 0 才是连上了；非 0 是 ECONNREFUSED 之类的具体错误。
 *      ★ 很多人漏掉这一步，导致「服务器拒绝连接」被误判成连接成功。
 *   最后把 fd 恢复成阻塞模式，交给上层按普通套接字使用。
 *
 * @param ip         目标 IP（点分十进制字符串，如 "192.168.1.100"）
 * @param port       目标端口
 * @param timeout_ms 连接超时毫秒数
 * @param err        失败原因输出缓冲，可直接显示到界面上（可为 NULL）
 * @param err_size   err 缓冲区大小
 * @return >=0 已连接的套接字 fd（调用方负责 close）；-1 失败（原因已写入 err）
 */
int net_connect_timeout(const char *ip, int port, int timeout_ms,
                        char *err, int err_size)
{
    int fd, flags, ret;
    struct sockaddr_in addr;
    fd_set wf;
    struct timeval tv;
    int sock_err = 0;
    socklen_t err_len = sizeof(sock_err);

    /* 小工具宏：把错误原因写进调用方的缓冲区。
     * 用 do{}while(0) 包起来是为了让它看起来像一条语句，
     * 这样在 if 后面不用加大括号也不会出语法事故。
     * 注意 err_size 判了 > 0，防止调用方传 0 时 snprintf 越界写。
     * 本宏只在本函数内有效，函数末尾用 #undef 撤掉，避免污染全局。 */
#define SET_ERR(msg) do { if(err && err_size > 0) snprintf(err, err_size, "%s", msg); } while(0)

    /* 入参防线：IP 为空直接失败，省得后面 inet_pton 报个含糊的错 */
    if(ip == NULL || ip[0] == '\0') { SET_ERR("服务器 IP 为空"); return -1; }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) { SET_ERR("创建套接字失败"); return -1; }

    /* 填写目标地址结构。sin_port 必须 htons 转网络序
     * （sin_addr 由 inet_pton 直接写入网络序，不需要再转） */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    if(inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        SET_ERR("服务器 IP 格式不对");   /* 返回值 != 1 说明不是合法 IPv4 */
        close(fd);                       /* ★ 失败路径必须 close，否则泄漏 fd */
        return -1;
    }

    /* ---- ① 非阻塞发起连接 ---- */
    flags = fcntl(fd, F_GETFL, 0);       /* 先存下原有标志，最后要还原 */
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0 && errno != EINPROGRESS) {
        /* 立刻失败（比如地址不可达），不是「正在连接」 */
        SET_ERR("无法连接到服务器");
        close(fd);
        return -1;
    }

    if(ret < 0) {                        /* 正在连接中（EINPROGRESS），等它可写 */
        /* ---- ② select 等待，只关心「可写」，超时用传入的毫秒数 ---- */
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        ret = select(fd + 1, NULL, &wf, NULL, &tv);
        if(ret == 0) {                   /* 超时：一个字都没等到 */
            SET_ERR("连接超时");
            close(fd);
            return -1;
        }
        if(ret < 0) {                    /* select 本身出错（被信号打断等） */
            SET_ERR("连接被中断");
            close(fd);
            return -1;
        }
        /* ---- ③ 可写 ≠ 成功，必须取 SO_ERROR 确认 ---- */
        if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &err_len) < 0 || sock_err != 0) {
            SET_ERR("无法连接到服务器");   /* sock_err 里是 ECONNREFUSED 等具体原因 */
            close(fd);
            return -1;
        }
    }

    /* 恢复成阻塞模式：上层后续按普通阻塞套接字使用，
     * 阻塞时长由 net_set_recv_timeout / net_set_send_timeout 控制。 */
    fcntl(fd, F_SETFL, flags);

    /* 走到这里说明成功了，把 err 缓冲置空，
     * 免得调用方看到上次遗留的旧错误信息。 */
    if(err && err_size > 0) err[0] = '\0';

#undef SET_ERR
    return fd;
}
