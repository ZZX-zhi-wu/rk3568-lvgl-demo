/* ============================================================================
 * @file    ftp.c
 * @brief   FTP 文件传输 —— 网络层实现
 *
 * ★★ 本文件不含任何 lv_* 调用 ★★
 *
 * 【这个文件干什么】
 *   给「云相册」提供从服务器拉图片的能力。服务器的图片目录通过 JSON
 *   命令协议列出来，选定某个文件后按裸字节流下载到本地。
 *
 * 【为什么它和 chat.c 长得不一样：不需要常驻接收线程】
 *   chat.c 必须有一个后台线程一直收数据，因为别人随时可能发消息过来
 *   （被动收）。而 FTP 是严格「一问一答」：
 *     客户端发 ls  → 服务器回列表
 *     客户端发 get → 服务器回元信息 → 客户端收数据
 *   每一步都是本端主动发起的，收数据的位置和时机构成本端完全可控，
 *   所以在**任务线程里顺序地收发**就行，不需要额外的接收线程。
 *   少一个线程就少一堆同步问题（也少一处必须 shutdown 才能退出的地方）。
 *
 * 【线程模型】
 *   主线程（LVGL）──ftp_get_begin()──→ 置任务槽 + 起一个 detached 线程
 *   任务线程      ──socket 收发──────→ 更新 g_progress / g_state
 *   主线程（定时器）──ftp_state()/ftp_progress()──→ 刷界面
 *
 *   ★ 任务线程 detached（不 join）：界面只靠轮询 g_state 知道进度，
 *     不需要等线程结束，所以没必要保存句柄。代价是不能用 join 回收，
 *     因此线程里的所有资源（fd、FILE*）都必须自己关干净。
 *
 * 【两个关键设计】
 *   ① 深度 1 的任务槽（g_job）
 *      同一时刻只允许一个任务，用 g_busy 挡住并发提交。
 *      为什么不做多任务并发？因为 g_sock 只有一条，两个线程同时在上面
 *      收发会互相抢数据（A 的响应被 B 读走）。要并发就得多连接、
 *      多套状态机，收益不值这个复杂度 —— 相册下载本来也是串行的。
 *   ② g_io_lock 保护 g_sock
 *      防止「传输进行中」与「断开连接」打架（见 ftp_disconnect）。
 *      注意加锁范围是「整个任务」而不是「每次收发」——
 *      因为任务中间不能被断开逻辑插进来关掉 fd。
 *
 * 【协议（两层）】
 *   控制层：JSON 文本，走 net_pkt 的 4 字节长度头封包
 *     请求  {"cmd":"ls"}
 *     应答  {"status":"ok","files":["a.jpg",...],"sizes":[123,...]}
 *     请求  {"cmd":"get","filename":"a.jpg"}
 *     应答  {"status":"ok","filesize":1843254}
 *     请求  {"cmd":"put","filename":"a.jpg","filesize":1843254}
 *     应答  {"status":"ok"}
 *   数据层：★ 裸字节流，不再封包 ★
 *     接着控制应答之后，服务器直接按 filesize 个字节吐数据，
 *     客户端循环 recv 到收满为止。
 *     —— 这跟聊天室完全不同：聊天室每条都要长度头，
 *        FTP 的数据部分不带长度头（长度已在元信息里告知）。
 * ========================================================================== */

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "net_pkt.h"
#include "ftp.h"

/* ------------------------------------------------------------------ */
/* 内部状态                                                           */
/* ------------------------------------------------------------------ */

#define FTP_RECV_TIMEOUT_MS  5000   /* 单次接收超时 */
#define FTP_MAX_TIMEOUTS     3      /* 连续超时这么多次就放弃 */

/* 【为什么超时取 5 秒、还要允许连续 3 次】
 *   收发图片过程中，服务器可能正在读磁盘（大图 1.8MB），
 *   偶发一次 5 秒没数据是正常的。如果一次超时就判失败，
 *   弱网/慢盘下会频繁误报。
 *   允许连续 3 次（累计最长 15 秒无数据）才放弃，兼顾宽容与不卡死。
 *   注意超时计数在「成功收到数据」时会清零（见 do_get），
 *   所以只要数据在持续流动，就不会累积到 3。 */

typedef enum {
    JOB_NONE = 0,
    JOB_LS,      /* 列目录 */
    JOB_GET,     /* 下载 */
    JOB_PUT,     /* 上传（本工程未使用，保留完整实现） */
} ftp_job_type_t;

/* 任务描述。local 给 320 字节：板子上路径是 /work_space/bmp_pic/... 这种长路径，
 * 留足余量免得被截断（截断后会写到错误的文件上，且不会报错）。 */
typedef struct {
    ftp_job_type_t type;
    char remote[FTP_NAME_LEN];   /* 服务器端文件名 */
    char local[320];             /* 本地完整路径 */
} ftp_job_t;

static int              g_sock    = -1;
static volatile int     g_busy    = 0;      /* 1 = 有任务在跑 */
static volatile int     g_state   = FTP_ST_IDLE;
static volatile int     g_progress = 0;     /* 0~100 */

static pthread_mutex_t  g_io_lock   = PTHREAD_MUTEX_INITIALIZER;  /* 保护 g_sock */
static pthread_mutex_t  g_file_lock = PTHREAD_MUTEX_INITIALIZER;  /* 保护 g_files */

static ftp_job_t        g_job;              /* 深度 1 的任务槽 */

static char             g_message[160] = "未连接";

/* 目录列表快照（和 chat.c 的在线用户快照同一个套路：
 * 后台线程写、界面线程读，用锁 + 独立缓冲避免读到写了一半的数据） */
static struct {
    char name[FTP_NAME_LEN];
    long size;
} g_files[FTP_MAX_FILES];
static int              g_file_count = 0;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

static void set_msg(const char *msg)
{
    snprintf(g_message, sizeof(g_message), "%s", msg ? msg : "");
}

static void copy_str(char *dst, const char *src, int size)
{
    if(size <= 0) return;
    if(src == NULL) { dst[0] = '\0'; return; }
    snprintf(dst, size, "%s", src);
}

static const char *json_str(cJSON *o, const char *k)
{
    cJSON *it = (o == NULL) ? NULL : cJSON_GetObjectItem(o, k);
    if(it == NULL || !cJSON_IsString(it)) return NULL;
    return it->valuestring;
}

/* 判断响应是不是 ok。
 * ★ 本协议用 status 字段（"ok" / "error"），
 *   和聊天室的 status+message 双字段不同 —— 这是两套独立协议，别混。 */
static int json_is_ok(cJSON *o)
{
    const char *s = json_str(o, "status");
    return (s != NULL && strcmp(s, "ok") == 0);
}

/* 取失败原因，取不到就给个默认值（保证界面上永远有文字可显示） */
static const char *json_err_msg(cJSON *o)
{
    const char *m = json_str(o, "message");
    return (m != NULL) ? m : "服务器返回错误";
}

/* 发一个 JSON 请求。root 的所有权在这里被释放
 * （和 chat.c 的 send_request 一样的「所有权移交」约定：
 *  调用方创建完就不管了，本函数任何路径都会负责 Delete） */
static int send_json(int sockfd, cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    int   ret = -1;

    if(str != NULL) {
        ret = net_send_str(sockfd, str);
        free(str);                   /* Print 出来的串要单独 free */
    }
    cJSON_Delete(root);              /* 这里统一释放，调用方不要再碰 root */
    return ret;
}

/* ------------------------------------------------------------------ */
/* 各任务实现（都在任务线程里跑，g_sock 已被 g_io_lock 持住）          */
/* ------------------------------------------------------------------ */

/**
 * 列目录：发 ls，收列表，填进 g_files 快照。
 * @return 1 成功；0 失败（失败原因已通过 set_msg 写入）
 *
 * 【为什么返回值有讲究】
 *   1/0 会被 ftp_thread 用来决定 g_state 是 DONE 还是 ERROR，
 *   界面就靠这个把进度条收掉、把弹层关掉。所以任何失败路径
 *   都必须 return 0 并先 set_msg 写清原因，不能静默失败。
 */
static int do_ls(void)
{
    char   buf[NET_PKT_MAX];
    cJSON *req, *resp, *files, *sizes;
    int    r, n = 0, i;

    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "ls");
    if(send_json(g_sock, req) != 0) {
        set_msg("发送 ls 请求失败");
        return 0;
    }

    /* 一问一答：发完就地等应答。因为整个任务持有 g_io_lock，
     * 不会有别的线程在同一个 socket 上抢数据，所以这里同步 recv 是安全的。 */
    r = net_recv_packet(g_sock, buf, (int)sizeof(buf));
    if(r <= 0) {
        /* -2 是超时（服务器没应答），-1 是断开 */
        set_msg(r == NET_PKT_TIMEOUT ? "服务器无响应" : "连接已断开");
        return 0;
    }

    resp = cJSON_Parse(buf);
    if(resp == NULL) {
        set_msg("响应不是合法 JSON");
        return 0;
    }

    if(!json_is_ok(resp)) {
        set_msg(json_err_msg(resp));
        cJSON_Delete(resp);
        return 0;
    }

    /* files 和 sizes 是两个「平行数组」：
     * files[i] 是文件名，sizes[i] 是对应大小，靠下标对应。
     * 所以下面取大小时也用同一个下标 i（sizes 缺失时就给 0）。 */
    files = cJSON_GetObjectItem(resp, "files");
    sizes = cJSON_GetObjectItem(resp, "sizes");
    if(files != NULL && cJSON_IsArray(files))
        n = cJSON_GetArraySize(files);
    if(n > FTP_MAX_FILES) n = FTP_MAX_FILES;   /* ★ 防越界：不信任服务器给的数量 */

    pthread_mutex_lock(&g_file_lock);
    g_file_count = n;
    for(i = 0; i < n; i++) {
        cJSON *f = cJSON_GetArrayItem(files, i);
        cJSON *s = (sizes && cJSON_IsArray(sizes)) ? cJSON_GetArrayItem(sizes, i) : NULL;

        /* 每一项都做类型检查（cJSON_IsString / cJSON_IsNumber），
         * 类型不对就当空/0 处理，绝不直接取 ->valuestring 后盲目使用。 */
        copy_str(g_files[i].name, (f && cJSON_IsString(f)) ? f->valuestring : NULL,
                 FTP_NAME_LEN);
        g_files[i].size = (s && cJSON_IsNumber(s)) ? (long)s->valuedouble : 0;
    }
    pthread_mutex_unlock(&g_file_lock);

    cJSON_Delete(resp);
    {
        char tip[64];
        snprintf(tip, sizeof(tip), "共 %d 个文件", n);
        set_msg(tip);
    }
    return 1;
}

/**
 * 下载文件。
 * @param remote 服务器端文件名
 * @param local  本地目标完整路径
 * @return 1 成功；0 失败
 *
 * 【核心机制：先写 .tmp，收完再 rename —— 保证原子性】
 *   如果直接往最终路径写，中途断线/超时/磁盘满，就会在相册目录里留下
 *   一个「看起来存在、其实是半张图」的坏文件。
 *   相册扫描是按文件名成对匹配的（photo_N + thumb_N），
 *   留下半张坏图会导致这张图打不开甚至显示花屏。
 *   所以：
 *     ① 全程写入 <local>.tmp
 *     ② 只有字节数完全对得上（got == filesize）才 rename 成正式名
 *     ③ 任何异常都 unlink 掉 .tmp，让这个文件「从未存在过」
 *   ★ rename 在同一文件系统内是原子的，所以相册那边永远只可能看到
 *     「完整文件」或「没有文件」两种状态，不存在中间态。
 */
static int do_get(const char *remote, const char *local)
{
    char   buf[NET_PKT_MAX];
    char   tmp[352];
    cJSON *req, *resp, *fs;
    long   filesize = 0, got = 0;
    int    r, ok = 0, timeouts = 0;
    FILE  *fp;

    /* 1. 请求文件 */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "get");
    cJSON_AddStringToObject(req, "filename", remote);
    if(send_json(g_sock, req) != 0) {
        set_msg("发送 get 请求失败");
        return 0;
    }

    /* 2. 收元信息（只含 filesize，数据在后面以裸字节流形式跟过来） */
    r = net_recv_packet(g_sock, buf, (int)sizeof(buf));
    if(r <= 0) {
        set_msg(r == NET_PKT_TIMEOUT ? "服务器无响应" : "连接已断开");
        return 0;
    }

    resp = cJSON_Parse(buf);
    if(resp == NULL) {
        set_msg("响应不是合法 JSON");
        return 0;
    }
    if(!json_is_ok(resp)) {
        set_msg(json_err_msg(resp));
        cJSON_Delete(resp);
        return 0;
    }

    fs = cJSON_GetObjectItem(resp, "filesize");
    filesize = (fs && cJSON_IsNumber(fs)) ? (long)fs->valuedouble : 0;
    cJSON_Delete(resp);

    /* filesize 为 0 就没什么可收的，直接退出。
     * ★ 这个判断也是后面 while 循环的「终止条件正确性」前提：
     *   如果 filesize 是 0，got(0) == filesize(0) 会立刻成立，
     *   后面的 rename 会把一个空 .tmp 文件变成正式文件 —— 所以必须提前拦掉。 */
    if(filesize <= 0) {
        set_msg("文件大小为 0，跳过");
        return 0;
    }

    /* 3. 先落临时文件，收完再 rename —— 中途断线不会留半张坏图 */
    snprintf(tmp, sizeof(tmp), "%s.tmp", local);
    fp = fopen(tmp, "wb");
    if(fp == NULL) {
        set_msg("本地文件创建失败");
        return 0;
    }

    /* 4. 裸字节流接收：循环 recv 直到收满 filesize。
     *    ★ 这里不能用 net_recv_packet —— 数据部分没有长度头，
     *      是纯字节流，必须用最原始的 recv。 */
    while(got < filesize) {
        int want = (int)(filesize - got);
        if(want > (int)sizeof(buf)) want = (int)sizeof(buf);

        r = (int)recv(g_sock, buf, want, 0);
        if(r > 0) {
            if(fwrite(buf, 1, r, fp) != (size_t)r) {
                set_msg("本地写入失败（磁盘满？）");
                break;                        /* 写盘失败没什么可补救的，退出 */
            }
            got += r;
            timeouts = 0;                     /* ★ 收到数据就清超时计数：
                                               *   连续 3 次才放弃，不是累计 3 次 */
            if(filesize > 0)
                g_progress = (int)(got * 100 / filesize);   /* 界面直接读它画进度条 */
        }
        else if(r == 0) {
            /* recv 返回 0 = 对端有序关闭，说明服务器提前断了 */
            set_msg("传输中断：服务器关闭了连接");
            break;
        }
        else if(errno == EINTR) {
            continue;                         /* 被信号打断，重来 */
        }
        else if(errno == EAGAIN || errno == EWOULDBLOCK) {
            /* 5 秒没收到数据。不立刻放弃，最多容忍连续 3 次 */
            timeouts++;
            if(timeouts > FTP_MAX_TIMEOUTS) {
                set_msg("传输超时，已中断");
                break;
            }
        }
        else {
            set_msg("接收出错");
            break;
        }
    }

    fclose(fp);

    /* 5. 收满了才算成功。★ 用 got == filesize 而不是循环正常退出判断 ——
     *    因为 break 出来时 got 通常也小于 filesize，
     *    但「循环因收满而结束」和「循环因出错而 break」这两种情况，
     *    只有比字节数才能可靠区分。 */
    if(got == filesize) {
        if(rename(tmp, local) == 0) {
            ok = 1;
            g_progress = 100;
            {
                char tip[128];
                snprintf(tip, sizeof(tip), "已下载 %s（%ld 字节）", remote, filesize);
                set_msg(tip);
            }
        }
        else {
            unlink(tmp);                      /* 改名失败，临时文件也得清掉 */
            set_msg("文件改名失败");
        }
    }
    else {
        unlink(tmp);                    /* 半途而废的文件直接丢掉 */
        /* 如果前面各分支已经写过具体原因，就不要再覆盖成笼统的「下载失败」；
         * 只有消息为空（理论上不会发生）时才兜底。 */
        if(g_message[0] == '\0') set_msg("下载失败");
    }
    return ok;
}

/**
 * 上传文件。
 * @param local  本地源文件完整路径
 * @param remote 服务器端目标文件名
 * @return 1 成功；0 失败
 *
 * 本工程未使用（保留完整实现），流程与下载对称，但方向相反：
 *   先发 put 告知文件名+大小 → 等服务器建好文件回 ok → 再发裸数据。
 *
 * ★★ 已知缺陷（P0，值得修）★★
 *   第 2 阶段的 while 循环里，net_write_full 失败时的处理是
 *   `timeouts++; ...; continue;` —— 也就是「重发当前这一块 buf」。
 *   但 net_write_full 是「循环 send 直到全部写完」的语义，
 *   它失败时可能已经把这块数据的一部分发出去了。
 *   重发整块会导致服务器端文件在断点处多出一段重复字节，
 *   而本函数只按 `sent == filesize` 判断成功，
 *   客户端仍会显示「上传成功」，服务器文件却已经损坏 —— 静默数据损坏。
 *
 *   正确做法应当是：让 net_write_full 返回「实际写出的字节数」，
 *   调用方按实际写出的字节数推进 sent 并从断点继续，
 *   而不是无条件重发整块。或者改成先算 CRC/大小由服务端校验。
 *   （本工程只用到下载，所以暂时不影响功能。）
 */
static int do_put(const char *local, const char *remote)
{
    char   buf[NET_PKT_MAX];
    char   rbuf[NET_PKT_MAX];
    cJSON *req, *resp;
    long   filesize = 0, sent = 0;
    FILE  *fp;
    int    r, ok = 0, n, timeouts = 0;

    fp = fopen(local, "rb");
    if(fp == NULL) {
        set_msg("本地文件打开失败");
        return 0;
    }

    /* 用「跳到末尾读位置」的办法求文件大小，再跳回开头准备读 */
    fseek(fp, 0, SEEK_END);
    filesize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if(filesize <= 0) {
        fclose(fp);
        set_msg("本地文件是空的，不上传");
        return 0;
    }

    /* 1. 先报文件名和大小，等服务器建好文件再发数据
     *    （如果不先告知大小，服务器不知道该读多少字节） */
    req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "cmd", "put");
    cJSON_AddStringToObject(req, "filename", remote);
    cJSON_AddNumberToObject(req, "filesize", (double)filesize);
    if(send_json(g_sock, req) != 0) {
        fclose(fp);                          /* 失败路径记得关文件 */
        set_msg("发送 put 请求失败");
        return 0;
    }

    r = net_recv_packet(g_sock, rbuf, (int)sizeof(rbuf));
    if(r <= 0) {
        fclose(fp);
        set_msg(r == NET_PKT_TIMEOUT ? "服务器无响应" : "连接已断开");
        return 0;
    }
    resp = cJSON_Parse(rbuf);
    if(resp == NULL || !json_is_ok(resp)) {
        set_msg(resp ? json_err_msg(resp) : "响应不是合法 JSON");
        if(resp) cJSON_Delete(resp);
        fclose(fp);
        return 0;
    }
    cJSON_Delete(resp);

    /* 2. 发裸数据（无长度头，服务器按上面告知的 filesize 读满为止） */
    while(sent < filesize) {
        n = (int)fread(buf, 1, sizeof(buf), fp);
        if(n <= 0) break;                    /* 读文件失败/到末尾，退出 */

        if(net_write_full(g_sock, buf, n) != NET_PKT_OK) {
            /* ★ 见函数头部「已知缺陷」：这里重发整块会导致对端多出重复数据 */
            timeouts++;
            if(timeouts > FTP_MAX_TIMEOUTS) {
                set_msg("上传超时，已中断");
                break;
            }
            continue;
        }
        sent += n;
        if(filesize > 0)
            g_progress = (int)(sent * 100 / filesize);
    }
    fclose(fp);

    if(sent == filesize) {
        ok = 1;
        g_progress = 100;
        {
            char tip[128];
            snprintf(tip, sizeof(tip), "已上传 %s（%ld 字节）", remote, filesize);
            set_msg(tip);
        }
    }
    else if(g_message[0] == '\0') {
        set_msg("上传失败");
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* 任务线程                                                           */
/* ------------------------------------------------------------------ */

/**
 * 任务线程主体：从任务槽取任务、执行、写回状态。
 *
 * 【为什么第一行要取 g_job 的副本】
 *   主线程提交下一个任务时会覆写 g_job 的内容。
 *   虽然 g_busy 机制保证「本线程跑完前不会再提交」，
 *   但取副本是最省心的防御 —— 一行拷贝换来「任务参数不可能中途被改」的保证，
 *   而且 job 这个局部变量后续用起来也不用担心被别的线程改。
 *
 * 【为什么要持有 g_io_lock 跑完整个任务】
 *   如果只在每次 send/recv 前后加解锁，ftp_disconnect() 就可能在
 *   两次收发之间挤进来把 g_sock 关掉，任务线程随后用一个已关闭（甚至
 *   编号被复用）的 fd 继续收发 —— 后果不可预期。
 *   整段持锁 + ftp_disconnect 里也去抢这把锁，
 *   就保证了「要么整个任务完整跑完，要么从没开始」。
 *   代价是断开时最多要等一个任务跑完（所以 disconnect 里加了最多 5 秒的等待）。
 */
static void *ftp_thread(void *arg)
{
    ftp_job_t job = g_job;      /* 取副本，避免外部再提交时被改写 */
    int ok = 0;

    (void)arg;

    pthread_mutex_lock(&g_io_lock);
    if(g_sock >= 0) {
        switch(job.type) {
        case JOB_LS:  ok = do_ls();                        break;
        case JOB_GET: ok = do_get(job.remote, job.local);  break;
        case JOB_PUT: ok = do_put(job.local, job.remote);  break;
        default: break;                                    /* JOB_NONE：什么都不做 */
        }
    }
    else {
        /* 任务排队期间连接被断开了 */
        set_msg("未连接服务器");
    }
    pthread_mutex_unlock(&g_io_lock);

    if(ok) g_progress = 100;
    g_state = ok ? FTP_ST_DONE : FTP_ST_ERROR;

    /* ★ 顺序不能反：先写 state，最后清 busy。
     *   界面线程看到 busy 变 0 才会去读 state；
     *   如果先清 busy 再写 state，界面可能抢在中间读到「busy=0 但 state 还是
     *   RUNNING」的瞬间，于是不关进度条、不刷新结果 —— 任务看起来卡住了。
     *   这是典型的「发布顺序」问题：先准备好数据，再放行读方。 */
    g_busy  = 0;                            /* 先写 state 再清 busy，顺序不能反 */
    return NULL;
}

/**
 * 提交任务（在主线程里调用）。
 *
 * @return 0 提交成功；-1 被拒绝（未连接 / 已有任务在跑 / 建线程失败）
 *
 * 【两道闸门】
 *   ① g_sock < 0 → 未连接，直接报错。
 *   ② g_busy    → 已有任务在跑，礼貌拒绝并给提示
 *      （界面会显示「上一个任务还没完成，请稍候」）。
 *      这两道检查都在主线程、单线程语境下执行，所以不需要额外加锁 ——
 *      g_busy 只在主线程置 1（这里）、在任务线程清 0。
 *
 * 【为什么要 pthread_detach】
 *   不 detach 又没人 join 的话，线程结束后会留下「zombie」资源
 *   （内核里还占着一份线程结构），跑几十次下载就会耗尽。
 *   detach 表示「线程自己管自己，退出时自动回收」，正好匹配
 *   「界面只轮询状态、不等待线程」的用法。
 */
static int submit(ftp_job_type_t type, const char *remote, const char *local)
{
    pthread_t tid;

    if(g_sock < 0) {
        set_msg("未连接服务器");
        g_state = FTP_ST_ERROR;
        return -1;
    }
    if(g_busy) {
        set_msg("上一个任务还没完成，请稍候");
        return -1;
    }

    /* 先把任务参数和状态全部准备好，最后才起线程 ——
     * 保证线程一启动就能看到完整的任务信息 */
    g_busy     = 1;
    g_state    = FTP_ST_RUNNING;
    g_progress = 0;
    g_message[0] = '\0';        /* ★ 清空旧消息：新任务的失败原因才不会被旧文案盖住 */
    g_job.type = type;
    copy_str(g_job.remote, remote, FTP_NAME_LEN);
    copy_str(g_job.local,  local,  (int)sizeof(g_job.local));

    if(pthread_create(&tid, NULL, ftp_thread, NULL) != 0) {
        /* 起线程失败要回滚 busy，否则之后所有任务都被闸门②挡住，
         * 界面表现为「永远提示上一个任务没完成」 */
        g_busy  = 0;
        g_state = FTP_ST_ERROR;
        set_msg("创建传输线程失败");
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

/**
 * 连接 FTP 服务器（只建立 TCP 连接，没有 FTP 协议里的登录流程 ——
 * 本工程的「FTP」是自定协议，只是名字借用）。
 *
 * @param timeout_ms <= 0 时用 3000ms
 * @return 0 成功；-1 失败
 */
int ftp_connect(const char *ip, int port, int timeout_ms)
{
    char err[128] = {0};
    int  fd;

    /* 已连接先断干净，避免旧 fd 与新 fd 编号复用导致两个线程操作同一个 fd */
    if(g_sock >= 0) ftp_disconnect();

    fd = net_connect_timeout(ip, port, timeout_ms > 0 ? timeout_ms : 3000,
                             err, (int)sizeof(err));
    if(fd < 0) {
        set_msg(err[0] ? err : "连接失败");
        g_state = FTP_ST_ERROR;
        return -1;
    }

    /* 5 秒接收超时：见文件头 FTP_RECV_TIMEOUT_MS 的说明 */
    net_set_recv_timeout(fd, FTP_RECV_TIMEOUT_MS);
    net_set_send_timeout(fd, FTP_RECV_TIMEOUT_MS);

    /* 写 g_sock 必须持锁：此刻可能有一个残留任务线程正准备释放锁，
     * 也可能有新的任务即将启动，必须让它们看到完整的赋值结果。 */
    pthread_mutex_lock(&g_io_lock);
    g_sock = fd;
    pthread_mutex_unlock(&g_io_lock);

    g_progress = 0;
    g_state    = FTP_ST_IDLE;
    set_msg("已连接 FTP 服务器");
    return 0;
}

/**
 * 断开连接。
 *
 * 【三步：唤醒 → 等任务收尾 → 关 fd】
 *   ① shutdown(SHUT_RDWR)
 *      —— 让正在传输的任务线程立刻从阻塞的 recv/send 里醒过来
 *         （否则要等满 5 秒的 SO_RCVTIMEO，界面会感觉卡住）。
 *   ② 轮询等 g_busy 变 0（最多 5 秒）
 *      —— ★ 不能在任务还在跑的时候直接 close(g_sock)。
 *      因为任务线程整个任务期间都持着 g_io_lock，
 *      而下面的 close 也要抢同一把锁 —— 这样其实已经天然互斥了，
 *      这里的 busy 轮询主要是为了「等得优雅」：
 *      主动等待比在锁上硬碰硬更容易加日志、也更容易设上限。
 *   ③ 持锁 close
 *      —— 拿到锁说明任务已经结束（或从未开始），此时关闭才是安全的。
 */
void ftp_disconnect(void)
{
    if(g_sock < 0) return;

    /* 先 shutdown 打断可能正在进行的传输，再等任务线程收尾 */
    shutdown(g_sock, SHUT_RDWR);

    {
        int wait = 0;
        while(g_busy && wait < 500) {       /* 最多等 500 * 10ms = 5 秒 */
            usleep(10 * 1000);              /* 10ms 轮询间隔：比 sleep(1) 细腻，
                                             * 又不至于忙等到耗 CPU */
            wait++;
        }
        /* ★ 等到 5 秒还是 busy，也照样往下走 ——
         *   这里不做无限等待，避免界面被一个卡死的任务永久拖住。
         *   剩下的收尾由 detach 的线程自己完成（它会拿到 -1 的 fd 报错退出）。 */
    }

    pthread_mutex_lock(&g_io_lock);
    if(g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    pthread_mutex_unlock(&g_io_lock);

    g_busy     = 0;
    g_progress = 0;
    g_state    = FTP_ST_IDLE;
    set_msg("未连接");
}

int ftp_is_connected(void)
{
    return (g_sock >= 0);
}

/* ---------- 三个「开始任务」的入口：都只是 submit 的薄封装 ---------- */

int ftp_list_begin(void)
{
    /* ls 不需要参数，remote/local 传 NULL */
    return submit(JOB_LS, NULL, NULL);
}

int ftp_get_begin(const char *remote_name, const char *local_path)
{
    if(remote_name == NULL || local_path == NULL) return -1;
    return submit(JOB_GET, remote_name, local_path);
}

int ftp_put_begin(const char *local_path, const char *remote_name)
{
    if(local_path == NULL || remote_name == NULL) return -1;
    /* ★ 注意参数顺序：本函数是 (local, remote)，
     *   而 submit 的签名是 (type, remote, local) —— 所以这里要倒着传。
     *   写反了会把本地路径当远端文件名发给服务器。 */
    return submit(JOB_PUT, remote_name, local_path);
}

/* ---------- 下面都是给界面层轮询状态的只读接口 ---------- */

ftp_state_t ftp_state(void)    { return (ftp_state_t)g_state; }
int ftp_progress(void)         { return g_progress; }

const char *ftp_message(void)
{
    return g_message;               /* 内部静态缓冲，不要 free */
}

/**
 * 复位状态（界面处理完一次任务结果后调用，好让下一次任务从干净状态开始）。
 * ★ 任务在跑时直接返回 —— 否则界面会把 RUNNING 状态误清成 IDLE，
 *   表现为「进度条还在动，但状态显示空闲」，逻辑就乱了。
 */
void ftp_reset_state(void)
{
    if(g_busy) return;                  /* 任务在跑时别复位，免得界面误判 */
    g_state    = FTP_ST_IDLE;
    g_progress = 0;
}

/* ---------- 目录列表访问（都持 g_file_lock，返回拷贝而非内部指针） ---------- */

int ftp_file_count(void)
{
    int n;
    pthread_mutex_lock(&g_file_lock);
    n = g_file_count;
    pthread_mutex_unlock(&g_file_lock);
    return n;
}

/**
 * 取第 idx 个文件名。
 * @return 文件名指针；idx 越界时返回空字符串（不返回 NULL，
 *         这样调用方可以直接拿去做 UI 显示而不用判空）
 * ★ 返回的是函数内 static 缓冲 —— 不要保存这个指针跨多次调用使用，
 *   下一次调用会覆盖它。要长期保存请当场 strcpy 走。
 */
const char *ftp_file_name(int idx)
{
    static char name[FTP_NAME_LEN];
    int n;

    pthread_mutex_lock(&g_file_lock);
    n = g_file_count;
    copy_str(name, (idx >= 0 && idx < n) ? g_files[idx].name : "", FTP_NAME_LEN);
    pthread_mutex_unlock(&g_file_lock);
    return name;
}

/** 取第 idx 个文件大小；idx 越界返回 0 */
long ftp_file_size(int idx)
{
    long sz = 0;
    pthread_mutex_lock(&g_file_lock);
    if(idx >= 0 && idx < g_file_count) sz = g_files[idx].size;
    pthread_mutex_unlock(&g_file_lock);
    return sz;
}
