#include "lvgl/lvgl.h"
#include "lvgl/demos/lv_demos.h"
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "lvgl/examples/lv_examples.h"
#include <stdio.h>
#include "main_interface.h"
#include "2048.h"
#include "album.h"

static void test01()
{
    lv_obj_t *src=lv_screen_active();
    lv_obj_t *win=lv_obj_create(src);
    lv_obj_set_size(win,250,250);
    lv_obj_set_align(win,7);
}

static void test02()
{
    lv_obj_t *src=lv_screen_active();
    lv_obj_t *win=lv_obj_create(src);
    lv_obj_set_size(win,250,250);
    lv_obj_set_align(win,9);

    static lv_style_t style;
    lv_style_init(&style);

    lv_style_set_bg_color(&style,lv_color_hex(0xf17628));
    lv_style_set_bg_opa(&style,50);
    lv_style_set_border_color(&style,lv_color_hex(0x00ff00));
    lv_style_set_border_opa(&style,200);
    lv_style_set_radius(&style,50);
    lv_style_set_border_side(&style,0x0f);

    lv_obj_add_style(win,&style,2);
}

static void test03()
{
    lv_obj_t *image=lv_gif_create(lv_screen_active());
    lv_obj_set_size(image,1024,600);

    lv_gif_set_src(image,"A:/mnt/hgfs/share/2048pic/3.gif");
    
}

static void test04()
{
    lv_obj_t *image=lv_image_create(lv_screen_active());
    lv_obj_set_size(image,1024,600);
    lv_image_set_src(image,"A:/mnt/hgfs/share/2048pic/2048.bmp");
    
}

static void test05(void)
{
    lv_obj_t * win[9];
    //创建一个活动屏幕
    lv_obj_t * scr = lv_screen_active();
    //创建9个窗口

    const lv_font_t *font = &lv_font_montserrat_30;
    for(int i = 0 ; i < 9 ;i++)
    {
        win[i] = lv_obj_create(scr);

        //设置大小
        lv_obj_set_size(win[i],200,100);

        //设置位置
        lv_obj_set_align(win[i],i+1);

        //为每个窗口创建子对象(标签)
        lv_obj_t * lab = lv_label_create(win[i]);

        //在标签上显示数字
        lv_label_set_text_fmt(lab,"%d",i+1);

        lv_obj_set_style_text_color(lab,lv_color_hex(0xff0000),0);

        lv_obj_set_style_text_font(lab,font,0);
    }
}

static void test06(void)
{
    /*Create a font*/
    //如果字体传输到开发板 记得使用开发板路径
    lv_font_t * font = lv_freetype_font_create("/mnt/hgfs/share/STXINGKA.TTF",
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               40,
                                               LV_FREETYPE_FONT_STYLE_NORMAL);

    if(!font) {
        LV_LOG_ERROR("freetype font create failed.");
        return;
    }

    /*Create style with the new font*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, font);
    lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

    /*Create a label with the new style*/
    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_obj_add_style(label, &style, 0);
    lv_label_set_text(label, "你好世界");
    lv_obj_center(label);
}
    
static void test07(void)
{
    /*Create a font*/
    //如果字体传输到开发板 记得使用开发板路径
    lv_font_t * font = lv_freetype_font_create("/mnt/hgfs/share/ubuntu_demo/lvgl/examples/libs/freetype/FZSTK.TTF",
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               40,
                                               LV_FREETYPE_FONT_STYLE_NORMAL);
    
    if(!font) {
        LV_LOG_ERROR("freetype font create failed.");
        return;
    }

    /*Create style with the new font*/
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, font);
    lv_style_set_text_align(&style, LV_TEXT_ALIGN_CENTER);

    /*Create a label with the new style*/
    lv_obj_t * label = lv_label_create(lv_screen_active());
    lv_obj_add_style(label, &style, 0);
    lv_label_set_text(label, "你好世界");
    lv_obj_center(label);
}

static int click_count = 0;
void test09_cb(lv_event_t * e)
{
    //获取注册事件发生的对象 事件是由按钮点击产生的 所以我获取到的对象是 按钮
    lv_obj_t * btn = lv_event_get_target(e);
    //获取 btn 上的第0的子对象
    lv_obj_t * lab = lv_obj_get_child(btn,0);
    click_count++;
    lv_label_set_text_fmt(lab,"prv_1:%d",click_count);
}

//在活动屏上 创建一个按钮 大小为 100 50
//在按钮上显示 prv:数字 随着你每次按下一次 数字 +1
static void test09(void)
{
    //创建按钮
    lv_obj_t * btn = lv_button_create(lv_screen_active());
    //设置大小
    lv_obj_set_size(btn,100,50);
    
    //创建lab标签
    lv_obj_t * lab = lv_label_create(btn);
    //在标签上显示内容
    lv_label_set_text_fmt(lab,"prv_1:%d",click_count);
    lv_obj_add_event_cb(btn,test09_cb,LV_EVENT_CLICKED,NULL);
}



void test10_cb(lv_event_t * e)
{
    #if 1
    lv_obj_t *lab = lv_event_get_user_data(e);
    #else
    //获取注册事件发生的对象

    //获取子对象
    #endif
    //获取事件发生的编码
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_PRESSED)
    {
        lv_label_set_text(lab,"pressed");
    }
    else if(code == LV_EVENT_RELEASED)
    {
        lv_label_set_text(lab,"released");
    }

}

static void test10(void)
{
    //创建按钮
    lv_obj_t * btn = lv_button_create(lv_screen_active());
    //设置大小
    lv_obj_set_size(btn,100,100);
    
    //设置按钮位置居中
    lv_obj_set_align(btn,9);

    //为按钮创建标签
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab,"released");
    lv_obj_add_event_cb(btn,test10_cb,LV_EVENT_ALL,lab);
}

static lv_obj_t *win1;
static lv_obj_t *win2;

static void display_page(lv_obj_t *screen)
{
    //所有的界面全部设置隐藏
    lv_obj_add_flag(win1,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(win2,LV_OBJ_FLAG_HIDDEN);

    //显示screen
    lv_obj_remove_flag(screen,LV_OBJ_FLAG_HIDDEN);
}

void test13_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_user_data(e);
    char *str = lv_label_get_text(label);
    if(strcmp(str,"button") == 0)
    {
        //显示button2 显示界面2
        lv_label_set_text(label,"button2");
        display_page(win2);
    }
    else if(strcmp(str,"button2") == 0)
    {
        //显示button 显示界面1
        lv_label_set_text(label,"button");
        display_page(win1);        
    }
}

static void test13(void)
{
    //设置窗口
    win1 = lv_obj_create(lv_screen_active());
    //设置窗口大小
    lv_obj_set_size(win1,100,100);
    //设置窗口位置
    lv_obj_set_align(win1,LV_ALIGN_BOTTOM_RIGHT);
    //设置窗口的背景颜色
    lv_obj_set_style_bg_color(win1,lv_color_hex(0xFF00),0);

    //设置窗口
    win2 = lv_obj_create(lv_screen_active());
    //设置窗口大小
    lv_obj_set_size(win2,100,100);
    //设置窗口位置
    lv_obj_set_align(win2,LV_ALIGN_BOTTOM_RIGHT);
    //设置窗口的背景颜色
    lv_obj_set_style_bg_color(win2,lv_color_hex(0xFF),0);

    //显示窗口1
    display_page(win1);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    //设置按钮大小
    lv_obj_set_size(btn,100,50);
    lv_obj_set_align(btn,9);

    //在按钮上创建一个标签
    lv_obj_t * label = lv_label_create(btn);
    //显示文字
    lv_label_set_text(label,"button");
    lv_obj_set_align(label,9);

    //给按钮添加回调事件
    lv_obj_add_event_cb(btn,test13_cb,LV_EVENT_CLICKED,label);
}


static lv_obj_t *scr1 = NULL;
static lv_obj_t *scr2 = NULL;
static void init_scr1(void);
static void init_scr2(void);

void to_scr2_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED)
    {
        if(scr2 == NULL)
        {
            init_scr2();
        }
        //加载屏幕2 删除屏幕1
        lv_screen_load(scr2);
        if(scr1 != NULL)
        {
            lv_obj_delete(scr1);
            scr1 = NULL;
        }
    }

}

void to_scr1_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED)
    {
        if(scr1 == NULL)
        {
            init_scr1();
        }
        //加载屏幕1 删除屏幕2
        lv_screen_load(scr1);
        if(scr2 != NULL)
        {
            lv_obj_delete(scr2);
            scr2 = NULL;
        }
    }
}

static void init_scr2(void)
{
    //初始化界面2 对象 scr2 存在 但是不在活动屏幕 所以看不到 需要进行加载
    scr2 = lv_obj_create(NULL);
    lv_obj_t *label = lv_label_create(scr2);
    //设置标签 显示 this is screen2
    lv_label_set_text(label,"this is screen2");
    //居中显示
    lv_obj_set_align(label,9);

    //创建按键
    lv_obj_t *button = lv_button_create(scr2);
    lv_obj_set_align(button,LV_ALIGN_BOTTOM_MID);

    //在按钮上创建一个标签
    lv_obj_t *btn_label = lv_label_create(button);
    lv_label_set_text(btn_label,"change screen1");
    lv_obj_set_align(btn_label,9);

    //给按钮添加一个事件
    lv_obj_add_event_cb(button,to_scr1_cb,LV_EVENT_CLICKED,NULL);

    //加载屏幕
    lv_screen_load(scr2);
}

static void init_scr1(void)
{
    //初始化界面1 对象 scr1 存在 但是不在活动屏幕 所以看不到 需要进行加载
    scr1 = lv_obj_create(NULL);
    lv_obj_t *label = lv_label_create(scr1);
    //设置标签 显示 this is screen1
    lv_label_set_text(label,"this is screen1");
    //居中显示
    lv_obj_set_align(label,9);

    //创建按键
    lv_obj_t *button = lv_button_create(scr1);
    lv_obj_set_align(button,LV_ALIGN_BOTTOM_MID);

    //在按钮上创建一个标签
    lv_obj_t *btn_label = lv_label_create(button);
    lv_label_set_text(btn_label,"change screen2");
    lv_obj_set_align(btn_label,9);

    //给按钮添加一个事件
    lv_obj_add_event_cb(button,to_scr2_cb,LV_EVENT_CLICKED,NULL);

    //加载屏幕
    lv_screen_load(scr1);
}


//键盘事件回调
static void textarea_event_cb(lv_event_t *e)
{
    //获取是什么事件
    lv_event_code_t code = lv_event_get_code(e);
    //获取传入的参数  软键盘
    lv_obj_t *key = lv_event_get_user_data(e);
    //获取点击的对象  文本框
    lv_obj_t *ta = lv_event_get_target(e);

    if(code == LV_EVENT_FOCUSED)
    {
        //让你的键盘会文本框建立关系
        lv_keyboard_set_textarea(key,ta);
        //显示键盘
        lv_obj_remove_flag(key,LV_OBJ_FLAG_HIDDEN);
    }
    else if(code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY)
    {
        //让你的键盘会文本框断开关系
        lv_keyboard_set_textarea(key,NULL);
        //隐藏键盘
        lv_obj_add_flag(key,LV_OBJ_FLAG_HIDDEN);        
    }
}



static void login_btn_click_cb(lv_event_t *e)
{
    //获取传入的参数
    lv_obj_t * win = lv_event_get_user_data(e);
    //获取win的第2个子对象
    lv_obj_t *username_text = lv_obj_get_child(win,2);
    //获取win的第3个子对象
    lv_obj_t *passwd_text = lv_obj_get_child(win,3);

    const char *usrname_str = lv_textarea_get_text(username_text);
    const char *passwd_str = lv_textarea_get_text(passwd_text);
    printf("usr:%s passwd:%s\n",usrname_str,passwd_str);

    if(strcmp(usrname_str,"admin")==0 && strcmp(passwd_str,"123456")==0)
    {
        //登录成功
        printf("登录成功\n");
        //跳转到选择app界面
        select_app_screen = ui_select_app_screen();
    }
    else
    {
        printf("登录失败\n");
    }
    /*
        FILE*fp = fopen("user.txt","r");

        char usrname[50] = {0};
        char passwd[50] = {0};

        int flag = 0;
        while(!feof(fp))
        {
            fscanf(fp,"%s %s\n",usrname,passwd);
            if(strcmp(usrname_str,usrname)==0 && strcmp(passwd_str,passwd)==0)
            {
                flag = 1;
                break;
            }         
        }

        if(flag == 1)
        {
            //登录成功
            printf("登录成功\n");
            //跳转到选择app界面
            select_app_screen = ui_select_app_screen();
        }
        else
        {
            printf("登录失败\n");
        }
        fclose(fp);
    */
}

static void test14()
{
    lv_obj_t *login_screen = lv_obj_create(NULL);
    lv_obj_t *win = lv_obj_create(login_screen);
    lv_obj_set_size(win,1024,600);

    //设计窗口的样式

    //创建用户名的标签
    lv_obj_t *username_lab = lv_label_create(win);
    //设置标签大小
    lv_obj_set_size(username_lab,100,50);
    //设置标签相对于父对象的位置
    lv_obj_set_pos(username_lab,250,100);
    lv_label_set_text(username_lab,"username");

    //创建密码的标签
    lv_obj_t *passwd_lab = lv_label_create(win);
    //设置标签大小
    lv_obj_set_size(passwd_lab,100,50);
    //设置标签相对于父对象的位置
    lv_obj_set_pos(passwd_lab,250,200);
    lv_label_set_text(passwd_lab,"password");

    //创建用户名的输入框
    lv_obj_t *username_text = lv_textarea_create(win);
    lv_obj_set_size(username_text,200,60);
    lv_obj_set_pos(username_text,350,100);
    //设置单行输入模式
    lv_textarea_set_one_line(username_text,true);

    //创建密码的输入框
    lv_obj_t *passwd_text = lv_textarea_create(win);
    lv_obj_set_size(passwd_text,200,60);
    lv_obj_set_pos(passwd_text,350,200);
    //设置单行输入模式
    lv_textarea_set_one_line(passwd_text,true);   
    //设置为密码输入模式
    lv_textarea_set_password_mode(passwd_text, true);

    //创建登录按钮
    lv_obj_t*login_btn = lv_button_create(win);
    lv_obj_set_pos(login_btn,300,300);
    lv_obj_t*login_btn_lab = lv_label_create(login_btn);
    lv_label_set_text(login_btn_lab,"login");
    lv_obj_add_event_cb(login_btn,login_btn_click_cb,LV_EVENT_CLICKED,win); 


    //创建软键盘
    lv_obj_t*key = lv_keyboard_create(login_screen);
    //一开始软键盘应该是隐藏的
    lv_obj_add_flag(key,LV_OBJ_FLAG_HIDDEN);
    //用户文本框
    lv_obj_add_event_cb(username_text,textarea_event_cb,LV_EVENT_ALL,key);   
    //密码文本框
    lv_obj_add_event_cb(passwd_text,textarea_event_cb,LV_EVENT_ALL,key);   

    //加载活动屏幕
    lv_screen_load(login_screen);
}

void test(void)
{
    test14();
}






