/**
 * @file    chat.h
 * @brief   网络聊天室 —— 网络层接口
 *
 * ★★ 铁律：本文件与 chat.c 里绝对不能出现任何 lv_* 调用 ★★
 *    接收线程在独立线程里跑，LVGL 不是线程安全的，
 *    在线程里碰界面会随机崩溃 / 花屏。
 *    跨线程数据一律通过 chat_event_t 事件队列交给 UI 线程。
 *
 * 分层：
 *    chat.c（socket + 收线程 + 事件队列，只认 cJSON）
 *        ↑
 *    chat_ui.c（LVGL 界面，只认 chat_event_t 和 chat.h 的接口）
 *
 * 服务端协议（沿用课程资料的 command/data 风格）：
 *   发  {"command":"register","data":{"name":"小明"}}
 *   收  {"status":"success","message":"welcome","data":{"id":1,"name":"小明"}}
 *   发  {"command":"list"}                                  → 收在线列表
 *   发  {"command":"pm","data":{"receiver_id":2,"message":"hi"}}   → 私聊
 *   发  {"command":"broadcast","data":{"message":"hi"}}            → 公共大厅
 *   收  {"status":"message","message":"public_message",
 *        "data":{"sender_id":1,"sender_name":"小明","message":"hi"}}
 *   收  {"status":"success","message":"online_users",
 *        "data":[{"id":1,"name":"小明"},...]}
 */
#ifndef __CHAT_H__
#define __CHAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CHAT_NAME_LEN 32
#define CHAT_TEXT_LEN 512
#define CHAT_MAX_USERS 64

/* 事件类型 */
typedef enum {
    CHAT_EV_CONNECTED = 0,  /* 连接（含注册）成功      text = 服务器问候语 */
    CHAT_EV_PUBLIC,         /* 公共大厅消息            from_id/from_name/text */
    CHAT_EV_PRIVATE,        /* 私聊消息                from_id/from_name/text */
    CHAT_EV_SYS,            /* 系统提示（进/退/错误）  text */
    CHAT_EV_USERS,          /* 在线列表已更新，请用 chat_get_online() 拉取 */
    CHAT_EV_ERROR,          /* 连接出错 / 已断开        text = 原因 */
} chat_ev_type_t;

/* 交给 UI 线程的事件。只放纯数据，绝不放 lv_obj_t* */
typedef struct {
    chat_ev_type_t type;
    int  from_id;
    char from_name[CHAT_NAME_LEN];
    char text[CHAT_TEXT_LEN];
} chat_event_t;

/**
 * 连接服务器并完成注册（同步，带超时，失败会返回错误而不是卡住）。
 * 内部会自动开一个接收线程。
 *
 * @param ip         服务器 IP，例如 "192.168.1.100"
 * @param port       服务器端口，例如 8888
 * @param nick       昵称（服务端同名会顶号，演示时两个客户端别用同名）
 * @param timeout_ms 连接 + 注册的总超时，建议 3000
 * @return 0 成功；-1 失败（原因可用 chat_last_error() 取）
 */
int  chat_connect(const char *ip, int port, const char *nick, int timeout_ms);

/** 断开连接：shutdown 唤醒接收线程 → join → close。可重复调用。 */
void chat_disconnect(void);

/** 当前是否处于已连接状态 */
int  chat_is_connected(void);

/** 自己的用户 ID（未连接时为 0） */
int  chat_my_id(void);

/** 发送公共大厅消息。@return 0 成功，-1 失败 */
int  chat_send_public(const char *text);

/** 发送私聊消息。@return 0 成功，-1 失败 */
int  chat_send_private(int to_id, const char *text);

/**
 * 非阻塞取一条事件（在 LVGL 定时器里循环调用，直到返回 0）。
 * @return 1 取到；0 队列空
 */
int  chat_poll_event(chat_event_t *ev);

/** 在线列表版本号。UI 侧比对它，变了才重建列表（避免每 100ms 重建控件） */
int  chat_online_version(void);

/**
 * 拉取在线用户快照。
 * @param ids    输出：用户 ID 数组
 * @param names  输出：昵称数组，每个元素 CHAT_NAME_LEN 字节
 * @param max    数组容量
 * @return 实际个数
 */
int  chat_get_online(int *ids, char (*names)[CHAT_NAME_LEN], int max);

/** 最近一次错误描述（界面状态行用） */
const char *chat_last_error(void);

/** 清空事件队列与在线列表（重连前调用，避免看到上一轮的残留消息） */
void chat_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* __CHAT_H__ */
