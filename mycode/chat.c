/* ============================================================================
 * @file    chat.c
 * @brief   网络聊天室 —— 网络层实现（socket + 接收线程 + 事件队列）
 *
 * ★★ 本文件不含任何 lv_* 调用，只依赖 socket / pthread / cJSON ★★
 *
 * 【为什么这条铁律必须遵守】
 *   LVGL 不是线程安全的：它内部有大量全局状态（当前活动屏、绘制缓冲、
 *   样式缓存、控件树）。如果在接收线程里直接调 lv_label_set_text()，
 *   恰好主线程也在跑 lv_timer_handler → 绘制，两边同时改同一块数据，
 *   结果就是随机崩溃、花屏、或控件树被写坏。
 *   所以本文件把所有「想告诉界面的东西」都转成 chat_event_t 塞进队列，
 *   由界面层在自己的定时器回调（也就是 LVGL 线程）里取出来处理。
 *   ★ 唯一安全的跨线程通信方式就是队列 + 锁，没有例外。
 *
 * 【线程模型】
 *
 *   主线程（LVGL）  ──chat_send_public()──→  send()   [g_send_lock 保护]
 *   接收线程        ──recv_packet()───────→  解析 JSON → 入环形队列 [g_q_lock]
 *   主线程（定时器）──chat_poll_event()───→  出队 → 更新界面
 *
 *   两个线程都会碰套接字：主线程发、接收线程收。
 *   TCP 套接字本身支持「一个方向读、另一个方向写」并发，
 *   所以不需要为 send/recv 上同一把锁；
 *   但「多个地方调 send」之间必须互斥（g_send_lock），否则两条
 *   JSON 报文会交错写进同一个流，对端解析必然错乱。
 *
 * 【三个必须遵守的细节（否则会踩坑）】
 *   1. SO_RCVTIMEO = 1000ms。recv 超时返回 EAGAIN，线程借机检查 g_running，
 *      否则 shutdown 之后线程仍可能卡在 recv 里退不出去。
 *   2. 断开必须 shutdown(SHUT_RDWR) 唤醒阻塞的 recv，然后才 pthread_join。
 *   3. 在线用户列表单独存快照 + 版本号，不塞进消息队列。
 *      否则一次列表刷新会挤掉 64 条消息里的一大半。
 *
 * 【协议速查（配合本文件看）】
 *   发送命令              期望回包 message 取值        本文件处理位置
 *   ──────────────────────────────────────────────────────────────────
 *   register              success + welcome            chat_connect 内联处理
 *   list                  online_users                 dispatch → users_update
 *   broadcast             自己收不到；别人收 public_message   dispatch
 *   pm                    对方收 private_message        dispatch
 * ========================================================================== */

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "net_pkt.h"
#include "chat.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                           */
/* ------------------------------------------------------------------ */

#define CHAT_Q_SIZE 64          /* 事件队列深度，满则丢最旧一条 */

/* 接收超时：1000ms 醒一次，用于检查退出标志。
 * ★ 这个值和 CHAT_Q_SIZE 是本文件两个「魔法数字」，改动前先读上面铁律第 1 条。 */
#define CHAT_RECV_TIMEOUT_MS 1000

/* 连接 + 注册阶段的同步等待上限。
 * 注册是一次性同步操作，用较长的窗口（3 秒）等服务器应答；
 * 注册完就收窄到 1 秒轮询。 */
#define CHAT_CONNECT_DEFAULT_MS 3000

static int              g_sock          = -1;   /* 套接字，-1 表示未连接 */
static int              g_my_id         = 0;    /* 服务端分配的 ID */
static volatile int     g_running       = 0;    /* 接收线程运行标志 */
static volatile int     g_connected     = 0;    /* 连接状态（UI 可读） */
static pthread_t        g_recv_tid;
static int              g_recv_tid_valid = 0;   /* 线程句柄是否有效（能否 join） */

/* 三把锁各管一块数据，粒度分开是为了减少互相等待：
 *   g_send_lock  保护「发送」这个动作（防两条报文交错）
 *   g_q_lock     保护环形事件队列
 *   g_user_lock  保护在线用户快照
 * ★ volatile 用在 g_running / g_connected / g_user_ver 上是必须的：
 *   它们会被另一个线程改动，不加 volatile 编译器可能把读操作优化成
 *   「只读一次寄存器」，导致循环里永远看到旧值（经典的多线程坑）。 */
static pthread_mutex_t  g_send_lock  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  g_q_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  g_user_lock  = PTHREAD_MUTEX_INITIALIZER;

/* 环形事件队列：head = 下一个写入位置，count = 当前元素数。
 *
 * 为什么用「环形 + count」而不是「head/tail 双指针」？
 *   双指针要额外区分「空」和「满」两种状态（两者 head==tail），
 *   要么浪费一个槽，要么加标志位。用 count 计数最直观，也不需要浪费槽位。
 *
 * 元素位置计算（本文件最容易看错的地方）：
 *   最旧元素（队头）= (head - count + SIZE) % SIZE
 *   最新元素（队尾）= (head - 1 + SIZE) % SIZE */
static chat_event_t     g_q[CHAT_Q_SIZE];
static int              g_q_head  = 0;
static int              g_q_count = 0;

/* 在线用户快照。
 * ★ 为什么不塞进事件队列：一次 list 刷新就携带几十个用户，
 *   如果每个用户压一条事件，64 条队列立刻被挤爆，聊天消息全丢了。
 *   所以列表用「整体快照 + 版本号」的方式传：
 *   接收线程写好快照并 g_user_ver++，界面线程靠比对版本号决定要不要重绘。 */
static struct { int id; char name[CHAT_NAME_LEN]; } g_users[CHAT_MAX_USERS];
static int              g_user_count = 0;
static volatile int     g_user_ver   = 0;       /* 版本号，UI 比对用 */

static char             g_last_error[128] = "未连接";

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static void set_error(const char *msg)
{
    /* 注意 msg 为空时兜底成「未知错误」，避免界面显示空白 */
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg ? msg : "未知错误");
}

/**
 * 安全字符串拷贝（等价 strncpy + 保证结尾 '\0'）。
 * ★ 工程里统一用它而不是 strcpy / strncpy：
 *   - strcpy 不看目标大小，溢出；
 *   - strncpy 在源串过长时不补 '\0'，后续当字符串用就越界读。
 *   snprintf 两者都避开了。
 */
static void copy_str(char *dst, const char *src, int dst_size)
{
    if(dst_size <= 0) return;
    if(src == NULL) { dst[0] = '\0'; return; }
    snprintf(dst, dst_size, "%s", src);
}

/**
 * 安全取字符串字段。
 * ★ 原版直接写 `cJSON_GetObjectItem(root,"x")->valuestring`，
 *   一旦字段不存在（返回 NULL）或类型不是字符串，立刻段错误。
 *   网络来的数据不能信，所以统一走这个包装：
 *   字段不存在、类型不对，一律返回 NULL，由调用方决定兜底值。
 */
static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *it;
    if(obj == NULL) return NULL;
    it = cJSON_GetObjectItem(obj, key);
    if(it == NULL || !cJSON_IsString(it)) return NULL;
    return it->valuestring;
}

/** 安全取整数字段（同上，取不到就用调用方给的默认值 def） */
static int json_int(cJSON *obj, const char *key, int def)
{
    cJSON *it;
    if(obj == NULL) return def;
    it = cJSON_GetObjectItem(obj, key);
    if(it == NULL || !cJSON_IsNumber(it)) return def;
    return it->valueint;
}

/* ------------------------------------------------------------------ */
/* 事件队列                                                           */
/* ------------------------------------------------------------------ */

/**
 * 入队（生产者 = 接收线程）。
 *
 * 【队列满时的策略：丢最旧，保最新】
 *   聊天场景下，最新的消息比历史消息重要，所以「满了丢最旧」比「丢最新」合理。
 *
 * 【怎么实现丢最旧 —— 这段是全文最绕的 3 行】
 *   队列满意味着 count == CHAT_Q_SIZE，此时 head 正好指向最旧元素
 *   （因为绕了一整圈，写指针追上了读指针）。
 *   把 count 减 1 再照常写入，效果就是：
 *     ① 声明「现在只剩 SIZE-1 个元素」（等于宣布最旧的那个作废）
 *     ② g_q[head] = *ev 覆盖掉最旧元素的位置
 *     ③ head++ 且 count++，队列重新变成满
 *   净效果 = 丢掉最旧的一条、写入最新的一条。
 *   ★ 不需要真的移动任何内存，这就是环形的价值。
 */
static void q_push(const chat_event_t *ev)
{
    pthread_mutex_lock(&g_q_lock);
    if(g_q_count >= CHAT_Q_SIZE) {
        /* 满：丢最旧一条。最旧元素是 (head - count) 位置，
         * 少算一个 count 就等于让它被下一次写入覆盖 */
        g_q_count = CHAT_Q_SIZE - 1;
    }
    g_q[g_q_head] = *ev;
    g_q_head = (g_q_head + 1) % CHAT_Q_SIZE;   /* 环绕 */
    g_q_count++;
    pthread_mutex_unlock(&g_q_lock);
}

/** 入队「纯文本事件」（无发送者信息）：系统提示、错误、状态通知用 */
static void q_push_simple(chat_ev_type_t type, const char *text)
{
    chat_event_t ev;
    memset(&ev, 0, sizeof(ev));      /* ★ 必须清零：结构体里有多个字段，
                                      *   只赋一部分会让未赋值的字段带垃圾值，
                                      *   界面层读到乱码或随机 ID */
    ev.type = type;
    copy_str(ev.text, text, CHAT_TEXT_LEN);
    q_push(&ev);
}

/** 入队「带发送者的消息事件」：公共消息与私聊用 */
static void q_push_msg(chat_ev_type_t type, int from_id,
                       const char *from_name, const char *text)
{
    chat_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type    = type;
    ev.from_id = from_id;
    copy_str(ev.from_name, from_name, CHAT_NAME_LEN);
    copy_str(ev.text,      text,      CHAT_TEXT_LEN);
    q_push(&ev);
}

/**
 * 出队（消费者 = LVGL 定时器回调，运行在主线程）。
 *
 * @param ev 输出参数，取出的元素拷贝到这里
 * @return 1 取到一条；0 队列为空
 *
 * 【为什么是这个下标表达式】
 *   最旧元素 = head - count。但 head - count 可能为负
 *   （例如 head=3, count=64 → -61）。
 *   C 语言的 % 对负数会返回负值，直接用它当下标就越界了。
 *   所以先加 SIZE*2 把它抬成正数再取模 ——
 *   加 2 倍而不是 1 倍，是因为 count 最大为 SIZE，
 *   head - count 最小是 -(SIZE-1)，加 SIZE 后只到 1，加 SIZE*2 更保险。
 *
 * 【为什么出队后不把该槽清零】
 *   没必要：count 和 head 已经决定了哪些槽是有效的，
 *   下次写入会整块覆盖。多一次 memset 只是浪费 CPU。
 */
int chat_poll_event(chat_event_t *ev)
{
    int got = 0;
    if(ev == NULL) return 0;

    pthread_mutex_lock(&g_q_lock);
    if(g_q_count > 0) {
        int idx = (g_q_head - g_q_count + CHAT_Q_SIZE * 2) % CHAT_Q_SIZE;
        *ev = g_q[idx];              /* 结构体整体拷贝（值语义，出队后互不影响） */
        g_q_count--;
        got = 1;
    }
    pthread_mutex_unlock(&g_q_lock);
    return got;
}

/**
 * 清空队列和用户快照。
 * 调用时机：开始新连接前、断开后。
 * 不清理的话，上一次的连接残留事件（比如旧的欢迎消息）会在新会话里
 * 被重新读出来，出现「刚连上就看到历史消息」的错觉。
 *
 * 注意：用户快照清空时也 g_user_ver++，
 * 这样界面能通过版本号变化感知到「列表变空了」，主动重绘。
 */
void chat_clear(void)
{
    pthread_mutex_lock(&g_q_lock);
    g_q_head  = 0;
    g_q_count = 0;
    pthread_mutex_unlock(&g_q_lock);

    pthread_mutex_lock(&g_user_lock);
    g_user_count = 0;
    g_user_ver++;
    pthread_mutex_unlock(&g_user_lock);
}

/* ------------------------------------------------------------------ */
/* 在线用户快照                                                       */
/* ------------------------------------------------------------------ */

/**
 * 用服务端推来的用户数组整体刷新快照（接收线程调用）。
 *
 * @param arr cJSON 数组，每项形如 {"id":3,"name":"小明"}
 *
 * 【为什么整块替换而不是增量更新】
 *   增量更新要处理「谁走了谁来了」的差集，逻辑复杂且容易和服务器不一致。
 *   整块替换的语义是「这就是当前全部在线用户」，
 *   天然不会残留已经下线的用户。
 *
 * 【越界防护】
 *   服务端数组长度不可信（可能超过本机 CHAT_MAX_USERS），
 *   先 clamp 到 CHAT_MAX_USERS 再写，避免写爆 g_users 数组。
 */
static void users_update(cJSON *arr)
{
    int n = 0;

    if(arr != NULL && cJSON_IsArray(arr))
        n = cJSON_GetArraySize(arr);
    if(n > CHAT_MAX_USERS) n = CHAT_MAX_USERS;    /* ★ 关键防线：限制写入条数 */

    pthread_mutex_lock(&g_user_lock);
    g_user_count = n;
    for(int i = 0; i < n; i++) {
        cJSON *o = cJSON_GetArrayItem(arr, i);
        g_users[i].id = json_int(o, "id", 0);
        copy_str(g_users[i].name, json_str(o, "name"), CHAT_NAME_LEN);
    }
    g_user_ver++;                    /* 版本号自增 = 通知界面「列表变了」 */
    pthread_mutex_unlock(&g_user_lock);
}

/** 取当前用户列表版本号。界面层比对两次结果，不同就重建列表 */
int chat_online_version(void)
{
    return g_user_ver;
}

/**
 * 拷出在线用户列表（界面线程调用，用锁保护）。
 *
 * @param ids   输出：用户 ID 数组（可为 NULL，表示不要 ID）
 * @param names 输出：用户昵称二维数组（可为 NULL，表示不要名字）
 * @param max   这两个数组的容量
 * @return 实际拷出的条数
 *
 * 返回的是「快照的副本」，不是内部数组的指针 ——
 * 这样界面拿去慢慢绘制时，接收线程可以继续刷新内部快照，互不干扰。
 * 如果直接返回指针，绘制到一半列表被服务器更新，就会读到写了一半的数组。
 */
int chat_get_online(int *ids, char (*names)[CHAT_NAME_LEN], int max)
{
    int n;
    pthread_mutex_lock(&g_user_lock);
    n = g_user_count;
    if(n > max) n = max;             /* 不超过调用方给的容量 */
    for(int i = 0; i < n; i++) {
        if(ids)   ids[i] = g_users[i].id;
        if(names) copy_str(names[i], g_users[i].name, CHAT_NAME_LEN);
    }
    pthread_mutex_unlock(&g_user_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* 收包分发                                                           */
/* ------------------------------------------------------------------ */

/**
 * 把一条 JSON 报文翻译成界面事件（接收线程调用）。
 *
 * ★★★ 这里是整个聊天室最需要理解的一个函数 ★★★
 *
 * 【它是「白名单」机制，不是「黑名单」】
 *   只有明确列举的报文类型会被识别成对应事件，其余全部走 else 兜底。
 *   这种设计的直接后果（也是跨服务器聊天出现「消息变成居中小字」的根因）：
 *   别人服务器发来的报文如果 message 取值不在下面这套白名单里
 *   （例如对方用 "chat" / "msg" / "broadcast" 等自定义名字），
 *   就会被归入「status==message 但不是 public/private」那个 else 分支，
 *   变成一条 CHAT_EV_SYS 系统提示。
 *   而界面层对 SYS 事件的渲染方式是「居中 + 灰暗 + 小字号」（sys_line_add），
 *   看起来就完全不像聊天气泡了。
 *
 *   所以：**气泡样式由本地客户端 100% 决定，服务器管不到外观，
 *   但服务器用哪个 message 取值，决定了本地走哪个渲染分支。**
 *
 * 【分支一览（顺序即优先级）】
 *   ① status=="message" && message=="public_message"  → CHAT_EV_PUBLIC  （左灰气泡）
 *   ② status=="message" && message=="private_message" → CHAT_EV_PRIVATE （右绿气泡）
 *   ③ status=="message" && message==其它               → CHAT_EV_SYS     （居中小字）
 *   ④ message=="online_users"（不看 status）           → 刷快照 + CHAT_EV_USERS
 *   ⑤ status=="sys"                                    → CHAT_EV_SYS
 *   ⑥ status=="error"                                  → CHAT_EV_ERROR
 *   ⑦ 以上都不匹配                                     → 只打串口，不上界面
 *
 * 【为什么 ⑦ 不往界面塞消息】
 *   聊天区空间有限。如果每个未知报文都显示一行，
 *   遇到一个会周期性发心跳/公告的服务器，屏幕会被垃圾信息刷满。
 *   只在串口打印，既不影响观感，排查时又能在串口日志里看到原始报文。
 *
 * @param json 一条完整的 JSON 文本（已保证以 '\0' 结尾）
 */
static void dispatch(const char *json)
{
    cJSON *root, *data;
    const char *status, *message;

    root = cJSON_Parse(json);
    if(root == NULL) {
        /* 解析失败也要告诉用户，否则会以为「服务器没回消息」 */
        q_push_simple(CHAT_EV_SYS, "[收到无法解析的数据]");
        return;
    }

    /* 三个字段一次取出。注意 status / message / sender_name 这类
     * 都可能为 NULL（字段缺失），下面每处使用前都必须判空，
     * 这也是为什么判断条件写成 `status != NULL && message != NULL && ...`。 */
    status  = json_str(root, "status");
    message = json_str(root, "message");
    data    = cJSON_GetObjectItem(root, "data");

    if(status != NULL && message != NULL && strcmp(status, "message") == 0) {
        if(strcmp(message, "public_message") == 0) {
            /* 公共大厅消息：带发送者昵称，界面渲染成左侧灰气泡 */
            q_push_msg(CHAT_EV_PUBLIC,
                       json_int(data, "sender_id", 0),
                       json_str(data, "sender_name"),
                       json_str(data, "message"));
        }
        else if(strcmp(message, "private_message") == 0) {
            /* 私聊消息：界面渲染成右侧绿气泡 */
            q_push_msg(CHAT_EV_PRIVATE,
                       json_int(data, "sender_id", 0),
                       json_str(data, "sender_name"),
                       json_str(data, "message"));
        }
        else {
            /* ★ 兜底：status 认对了，但 message 取值不认识。
             * 跨服务器场景下别人的消息就落在这里 → 居中小字。 */
            q_push_simple(CHAT_EV_SYS, json_str(data, "message"));
        }
    }
    else if(status != NULL && message != NULL && strcmp(message, "online_users") == 0) {
        /* 在线列表：先刷快照，再压一条「只为通知」的事件。
         * 事件本身不带数据（text 传 NULL），界面收到后自己去取快照 —— 
         * 这就是「列表不进队列」的具体做法。 */
        users_update(data);
        q_push_simple(CHAT_EV_USERS, NULL);      /* 事件只为通知，数据走快照 */
    }
    else if(status != NULL && strcmp(status, "sys") == 0) {
        /* 服务器主动广播的系统通知（有人进出等） */
        q_push_simple(CHAT_EV_SYS, json_str(data, "message"));
    }
    else if(status != NULL && strcmp(status, "error") == 0) {
        /* message 本身就是原因文本，直接用它 */
        q_push_simple(CHAT_EV_ERROR, message);
    }
    else {
        /* 未知报文：只在串口打一行，不往界面塞消息，免得污染聊天区 */
        printf("[chat] 未知报文: %s\n", json);
    }

    /* ★ cJSON 是手动内存管理：cJSON_Parse 出来的对象必须 Delete，
     *   否则每收一条报文泄漏一块内存，聊几分钟就吃光内存。
     *   注意 root 被 Delete 之后，上面取出的 status / message 指针
     *   以及 data 里的字符串也全部失效 —— 所以必须先 q_push（内部是
     *   深拷贝到事件结构体）再用，顺序不能反。 */
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* 接收线程                                                           */
/* ------------------------------------------------------------------ */

/**
 * 接收线程主体：不断收报文 → 分发到队列。
 *
 * 【循环结构的关键：三态返回各走哪条路】
 *   NET_PKT_TIMEOUT  超时（1 秒到点）→ continue，回到循环头检查 g_running。
 *                    ★ 这一条是线程能「响应退出」的全部机制所在。
 *   r == 0           空包 → 忽略，继续收。
 *   r < 0            对端关闭 / socket 错误 → break，跳出循环收摊。
 *   r > 0            正常报文 → dispatch() 翻译成事件。
 *
 * 【为什么 break 之后还要判断 g_running】
 *   跳出循环有两种原因，必须区分开：
 *     - 被动断开：对端关了连接、网络断了 → 此时 g_running 还是 1，
 *       说明用户没打算退出，所以要主动通知界面「连接已断开」并弹提示。
 *     - 主动断开：用户点了返回/断开，chat_disconnect() 会把 g_running
 *       清 0 再 shutdown。此时 g_running 已经是 0，
 *       就不该再压一条「连接断开」的错误事件（用户自己关的，提示很莫名其妙）。
 *   判空一次就分清了两种情况，这是本函数最巧妙的一行。
 */
static void *recv_thread(void *arg)
{
    char buf[NET_PKT_MAX];
    int  r;

    (void)arg;                       /* 线程参数没用上，显式忽略避免编译告警 */
    while(g_running) {
        r = net_recv_packet(g_sock, buf, (int)sizeof(buf));
        if(r == NET_PKT_TIMEOUT)
            continue;                    /* 超时：回到循环头检查 g_running */
        if(r == 0)
            continue;                    /* 空包，忽略 */
        if(r < 0)
            break;                       /* 对端关闭或出错，退出线程 */

        dispatch(buf);
    }

    /* 走到这里说明连接断了（或主动断开） */
    if(g_running) {                      /* 非主动断开 → 通知界面 */
        g_running   = 0;
        g_connected = 0;
        set_error("与服务器的连接已断开");
        q_push_simple(CHAT_EV_ERROR, g_last_error);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 发送                                                               */
/* ------------------------------------------------------------------ */

/**
 * 发送一条请求（组装 {"command":xxx,"data":{...}} 并发出）。
 *
 * @param command 命令名，如 "broadcast" / "pm" / "list"
 * @param data    数据对象。★ 所有权移交 ★ 本函数负责释放它，
 *                调用方创建后不能再碰（也不能自己 Delete，会双重释放）。
 *                传 NULL 表示这条命令没有 data 字段。
 * @return 0 成功；-1 失败（未连接 / 内存分配失败 / 发送失败）
 *
 * 【为什么要有 g_send_lock】
 *   两条线程都可能发送（主线程发消息、以及某些控制命令）。
 *   如果两次 net_send_str 交错执行，会变成：
 *     报文A的长度头 + 报文B的长度头 + 报文A的载荷 + 报文B的载荷
 *   对端按长度头解析，收到的就是一堆乱码。
 *   加锁保证「一条完整报文」的发送过程是原子的。
 *
 * 【所有失败路径都必须 Delete data】
 *   这是所有权移交函数的必备纪律：既然约定「进来就归我管」，
 *   那么任何一条 return 之前都要把它释放掉，漏一条就泄漏。
 */
static int send_request(const char *command, cJSON *data /* 所有权移交 */)
{
    cJSON *req;
    char  *str;
    int    ret;

    /* 未连接时也不能直接返回 —— 得先把移交过来的 data 释放掉 */
    if(!g_connected || g_sock < 0) {
        if(data) cJSON_Delete(data);
        set_error("未连接服务器");
        return -1;
    }

    req = cJSON_CreateObject();
    if(req == NULL) {
        if(data) cJSON_Delete(data);
        return -1;
    }
    cJSON_AddStringToObject(req, "command", command);
    if(data != NULL)
        cJSON_AddItemToObject(req, "data", data);   /* data 所有权归 req */

    /* PrintUnformatted = 紧凑 JSON（无缩进换行），省带宽且避免多余空白 */
    str = cJSON_PrintUnformatted(req);
    ret = -1;
    if(str != NULL) {
        pthread_mutex_lock(&g_send_lock);
        ret = net_send_str(g_sock, str);
        pthread_mutex_unlock(&g_send_lock);
        free(str);                   /* PrintUnformatted 返回的是 malloc 的串，
                                      * 必须 free，和 cJSON_Delete(req) 是两回事 */
    }
    cJSON_Delete(req);               /* 删 req 会连带删掉挂上去的 data */
    return ret;
}

/**
 * 发送公共大厅消息（界面点「发送」且当前选中的是「公共大厅」时调用）。
 * 组包 {"command":"broadcast","data":{"message":"..."}}
 *
 * ★ 注意服务端行为：broadcast 是「群发但不回给自己」，
 *   所以自己发出去的消息要靠界面层手动插一条本地气泡，
 *   不能等服务端回显（否则自己永远看不到自己说的话）。
 */
int chat_send_public(const char *text)
{
    cJSON *data;
    if(text == NULL || text[0] == '\0') return -1;   /* 空消息不发送 */

    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "message", text);
    return send_request("broadcast", data);
}

/**
 * 发送私聊消息。组包 {"command":"pm","data":{"receiver_id":N,"message":"..."}}
 *
 * @param to_id 目标用户 ID（来自在线列表）
 *
 * 字段名是 receiver_id，和收包时的 sender_id 对称 ——
 * 这与课程材料里的原版协议逐字一致，所以连别人的服务器时私聊能通。
 */
int chat_send_private(int to_id, const char *text)
{
    cJSON *data;
    if(text == NULL || text[0] == '\0') return -1;

    data = cJSON_CreateObject();
    cJSON_AddNumberToObject(data, "receiver_id", to_id);
    cJSON_AddStringToObject(data, "message", text);
    return send_request("pm", data);
}

/* ------------------------------------------------------------------ */
/* 连接 / 断开                                                        */
/* ------------------------------------------------------------------ */

/* 带超时的连接实现在 net_pkt.c 的 net_connect_timeout() 里，
 * FTP 模块也用同一份，避免两处重复维护。 */

/**
 * 连接服务器并完成注册（同步阻塞，界面会卡住，所以调用前要 lv_refr_now）。
 *
 * @param ip         服务器 IP
 * @param port       端口
 * @param nick       昵称
 * @param timeout_ms 超时毫秒；<=0 时用默认值 CHAT_CONNECT_DEFAULT_MS(3000)
 * @return 0 成功（接收线程已启动）；-1 失败（原因见 chat_last_error()）
 *
 * 【五步握手流程】
 *   ① 带超时 connect（服务器没开也是 3 秒返回，不会僵住 20 秒）
 *   ② 注册阶段临时用「整个超时窗口」，注册完再收窄到 1 秒
 *   ③ 同步等注册结果（循环最多 4 次，兼容先收到 online_users 的情况）
 *   ④ 转常规模式：1 秒轮询超时 + 启动接收线程
 *   ⑤ 顺手拉一次完整在线列表
 */
int chat_connect(const char *ip, int port, const char *nick, int timeout_ms)
{
    char   buf[NET_PKT_MAX];
    cJSON *data, *req;
    char  *str;
    int    fd, r, i;
    int    got_welcome = 0;

    if(ip == NULL || nick == NULL || nick[0] == '\0') {
        set_error("参数不完整");
        return -1;
    }
    if(timeout_ms <= 0) timeout_ms = CHAT_CONNECT_DEFAULT_MS;

    /* 已连接则先干净地断开。
     * ★ 不能直接覆盖 g_sock —— 那样旧线程还持着旧 fd，
     *   而旧 fd 的编号可能立刻被新 socket 复用，出现「两个线程操作同一个 fd」
     *   的灾难。必须先真正 join 掉旧线程。 */
    if(g_sock >= 0) chat_disconnect();
    chat_clear();
    set_error("连接中...");

    /* 1. 带超时地连接（内部非阻塞 connect + select，服务器没开也是 3 秒返回） */
    {
        char errbuf[128] = {0};
        fd = net_connect_timeout(ip, port, timeout_ms, errbuf, sizeof(errbuf));
        if(fd < 0) {
            /* 用 errbuf 里 net_connect_timeout 填好的具体原因
             *（「连接超时」/「服务器 IP 格式不对」/「无法连接到服务器」）。
             * errbuf 为空时兜底一句通用文案。 */
            set_error(errbuf[0] ? errbuf : "连接失败");
            return -1;
        }
    }

    /* 2. 注册阶段用「整个超时窗口」，之后收窄到 1 秒轮询。
     *    原因：注册是一次性同步操作，服务器可能稍慢才应答，
     *    用 1 秒会导致正常服务器被误判为「无响应」。 */
    net_set_recv_timeout(fd, timeout_ms);
    net_set_send_timeout(fd, timeout_ms);

    /* 手工组装 register 报文，没有走 send_request()。
     * ★ 为什么不用 send_request：那个函数开头会检查 g_connected，
     *   而此刻连接刚建立、g_connected 还是 0，会被它直接拒绝。
     *   所以这一步必须自己组包发送。 */
    data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "name", nick);
    req  = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "command", "register");
    cJSON_AddItemToObject(req, "data", data);
    str = cJSON_PrintUnformatted(req);
    r = str ? net_send_str(fd, str) : -1;
    if(str) free(str);
    cJSON_Delete(req);

    if(r != 0) {
        set_error("注册请求发送失败");
        close(fd);                    /* 失败路径必须关 fd，否则泄漏 */
        return -1;
    }

    /* 3. 同步等注册结果。
     *    这里可能先收到一条 online_users（服务端注册成功后会主动推列表），
     *    所以循环几次，直到拿到 welcome / welcome_back 或明确报错。
     *    ★ 最多 4 次：正常情况第 1、2 条就是结果；
     *      给 4 次是为了容忍「先来几条 online_users」的服务器实现。 */
    for(i = 0; i < 4 && !got_welcome; i++) {
        cJSON *root;
        const char *status, *message;

        r = net_recv_packet(fd, buf, (int)sizeof(buf));
        if(r <= 0) {
            /* r == NET_PKT_TIMEOUT(-2) 是超时，r == NET_PKT_ERR(-1) 是被拒绝 */
            set_error(r == NET_PKT_TIMEOUT ? "服务器无响应（超时）" : "服务器拒绝连接");
            close(fd);
            return -1;
        }

        root = cJSON_Parse(buf);
        if(root == NULL) continue;    /* 解析不了就跳过这一条，继续等 */

        status  = json_str(root, "status");
        message = json_str(root, "message");

        if(status != NULL && strcmp(status, "success") == 0 &&
           message != NULL &&
           (strcmp(message, "welcome") == 0 || strcmp(message, "welcome_back") == 0)) {
            /* 两种欢迎语都认：welcome = 首次注册，welcome_back = 重连/重名 */
            cJSON *d = cJSON_GetObjectItem(root, "data");
            g_my_id = json_int(d, "id", 0);      /* 记下服务端分配的 ID，私聊要用 */
            got_welcome = 1;
        }
        else if(message != NULL && strcmp(message, "online_users") == 0) {
            /* 注册过程中先到的列表，顺手收下 */
            users_update(cJSON_GetObjectItem(root, "data"));
        }
        else if(status != NULL && strcmp(status, "error") == 0) {
            set_error(message && strcmp(message, "server_full") == 0
                      ? "服务器已满" : "注册被拒绝");
            cJSON_Delete(root);       /* ★ 这条失败路径要记得 Delete */
            close(fd);
            return -1;
        }
        cJSON_Delete(root);
    }

    if(!got_welcome) {
        /* 循环 4 次都没拿到 welcome：服务器可能协议不同（比如别人的服务器） */
        set_error("注册未成功");
        close(fd);
        return -1;
    }

    /* 4. 转入常规模式：1 秒超时轮询，起接收线程 */
    net_set_recv_timeout(fd, CHAT_RECV_TIMEOUT_MS);
    net_set_send_timeout(fd, CHAT_RECV_TIMEOUT_MS);

    /* ★ 先把全局状态置好，再创建线程 ——
     *   接收线程一启动就要读 g_sock，如果此时 g_sock 还是 -1，
     *   线程会立刻因为操作无效 fd 而退出，表现为「刚连上就断开」。 */
    g_sock      = fd;
    g_running   = 1;
    g_connected = 1;
    set_error("已连接");

    if(pthread_create(&g_recv_tid, NULL, recv_thread, NULL) != 0) {
        /* 线程创建失败：必须把刚才置好的状态全部回滚，
         * 否则界面以为已连接，实际没有任何人在收数据 */
        g_running = 0;
        g_connected = 0;
        close(fd);
        g_sock = -1;
        set_error("创建接收线程失败");
        return -1;
    }
    g_recv_tid_valid = 1;

    /* 5. 顺手拉一次完整在线列表（服务端若已主动推过，这里也不会出错） */
    send_request("list", NULL);

    /* 最后压一条「已进入聊天室」的提示，界面会显示成系统行 */
    {
        char tip[96];
        snprintf(tip, sizeof(tip), "已进入聊天室，我的 ID 是 %d", g_my_id);
        q_push_simple(CHAT_EV_CONNECTED, tip);
    }
    return 0;
}

/**
 * 主动断开连接（界面返回/退出时调用）。
 *
 * 【三步顺序不能换】
 *   ① 先清标志（g_running = 0）
 *      —— 告诉接收线程「该退了」，这样它下一轮循环就会自己结束，
 *         而不会把这次断开误判成「被动掉线」去弹错误提示。
 *
 *   ② shutdown(g_sock, SHUT_RDWR)
 *      —— ★ 这一步是必需的，很容易被漏掉。
 *         接收线程此刻大概率正阻塞在 recv 里等着数据（最多要等满
 *         1 秒的 SO_RCVTIMEO）。shutdown 会让 recv 立刻返回，
 *         线程马上就能醒过来检查 g_running 并退出。
 *         不调 shutdown 的话，「断开」操作会硬等最多 1 秒才返回，
 *         界面表现为点了返回卡一下。
 *         注意用 shutdown 而不是 close —— close 不一定会唤醒
 *         正在阻塞的 recv，而且在多线程里语义不安全。
 *
 *   ③ pthread_join
 *      —— 等线程真正结束。必须在 close(fd) 之前做，
 *         否则线程可能正在用这个 fd 时被主线程关掉，
 *         出现「fd 编号被复用给别的文件」导致的诡异 bug。
 *
 * 【为什么 g_sock < 0 时要提前返回】
 *   从未连接过（或已断开）时调用本函数是正常情况
 *   （比如退出界面时无脑调一次），此时只需把状态清零，
 *   不能去 shutdown/close 一个无效 fd。
 */
void chat_disconnect(void)
{
    if(g_sock < 0) {
        g_connected = 0;
        g_running   = 0;
        return;
    }

    g_running   = 0;
    g_connected = 0;

    /* shutdown 会唤醒阻塞在 recv 的接收线程（否则要等满 1 秒超时） */
    shutdown(g_sock, SHUT_RDWR);

    if(g_recv_tid_valid) {
        pthread_join(g_recv_tid, NULL);   /* 等线程收摊，之后再关 fd 才安全 */
        g_recv_tid_valid = 0;
    }

    close(g_sock);
    g_sock  = -1;
    g_my_id = 0;

    /* 清掉队列与用户列表，避免下次连接看到上一次的残留 */
    chat_clear();
}

/* ---------- 下面三个是给界面层查询状态的只读接口 ---------- */

int  chat_is_connected(void) { return g_connected; }
int  chat_my_id(void)        { return g_my_id; }

/**
 * 取最近一次错误/状态的描述文本（"连接中..." / "已连接" / "连接超时" 等）。
 * ★ 返回的是内部静态缓冲的指针，不要去 free 它；
 *   也别指望它长期有效 —— 下一次 set_error 就会覆盖内容。
 */
const char *chat_last_error(void)
{
    return g_last_error;
}
