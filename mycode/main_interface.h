/**
 * @file    main_interface.h
 * @brief   桌面 Launcher（App 选择界面）接口声明
 *
 * ── 关于两个全局屏幕指针的约定（改界面时最容易踩的地方）──────────────
 *   select_app_screen  桌面屏。它是「各子界面返回时的公共落脚点」：
 *                      子界面自己的屏可以删掉重建，但桌面屏一直复用一个实例，
 *                      这样返回桌面不用每次重建整个桌面，速度更快。
 *   album_screen       电子相册屏。相册模块内部复用，不在这里管理生命周期。
 *
 * ★ 从任意子界面返回桌面的顺序（顺序颠倒会黑屏或卡死）：
 *       1) 先 lv_timer_delete 删掉该界面自己的定时器
 *       2) 再切屏（lv_screen_load(select_app_screen) 或调 ui_select_app_screen()）
 *       3) 最后 lv_obj_delete 删掉子界面自己的屏
 *   原因：LVGL 不允许删除「当前正在显示的屏」，必须先切换走；
 *   而定时器回调里会访问本屏控件，所以要赶在删屏之前先停掉。
 *   另外子界面的屏指针删完要记得置 NULL，避免下次误当有效指针复用。
 */
#ifndef __MAIN_INTERFACE_H__
#define __MAIN_INTERFACE_H__

extern lv_obj_t * album_screen;        // 电子相册屏（album.c 使用，相册模块内部复用）
extern lv_obj_t * select_app_screen;   // 桌面屏（全局，各子界面返回时复用）

/**
 * 创建并返回桌面屏幕对象（桌面 Launcher）。
 * @return 桌面屏对象；调用方一般把它存进 select_app_screen 以便复用
 */
lv_obj_t * ui_select_app_screen(void);

#endif
