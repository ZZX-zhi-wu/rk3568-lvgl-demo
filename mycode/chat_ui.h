/**
 * @file    chat_ui.h
 * @brief   网络聊天室 —— 界面层接口
 *
 * 界面层只依赖 chat.h 暴露的纯数据接口，不碰 socket。
 *
 * 两个屏：
 *   chat_conn_screen  连接页（填 IP / 端口 / 昵称），同时是断线重连的入口
 *   chat_room_screen  聊天页（在线列表 + 消息气泡 + 输入区），连接成功后创建
 *
 * 返回桌面时的清理顺序（顺序错了会黑屏卡死）：
 *   删定时器 → chat_disconnect() → 切回桌面屏 → 删本模块的两个屏
 */
#ifndef __CHAT_UI_H__
#define __CHAT_UI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "../lvgl/lvgl.h"

extern lv_obj_t *chat_conn_screen;   /* 连接页屏 */
extern lv_obj_t *chat_room_screen;   /* 聊天页屏（未连接时为 NULL） */

/**
 * 创建连接页并返回屏幕对象，同时加载到当前显示。
 * @return 屏幕对象
 */
lv_obj_t *ui_chat_conn_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __CHAT_UI_H__ */
