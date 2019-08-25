﻿/*****************************************************************************************
 * @function    与日志相关。
 * @author      xuchanglong
 * @time            2019-08-15
*****************************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   //uintptr_t]
#include <stdarg.h>   //va_start....
#include <unistd.h>   //STDERR_FILENO等
#include <sys/time.h> //gettimeofday
#include <time.h>     //localtime_r
#include <fcntl.h>    //open
#include <errno.h>    //errno

#include "xmn_global.h"
#include "xmn_macro.h"
#include "xmn_func.h"
#include "xmn_config.h"
#include <stdio.h>
#include <stdlib.h>

/**
 * 错误等级，和xmn_macro.h里定义的日志等级宏是一一对应关系。
*/
static u_char err_levels[][20] =
    {
        {"stderr"}, //0：控制台错误
        {"emerg"},  //1：紧急
        {"alert"},  //2：警戒
        {"crit"},   //3：严重
        {"error"},  //4：错误
        {"warn"},   //5：警告
        {"notice"}, //6：注意
        {"info"},   //7：信息
        {"debug"}   //8：调试
};
XMNLog g_xmn_log;

//----------------------------------------------------------------------------------------------------------------------
//描述：通过可变参数组合出字符串【支持...省略号形参】，自动往字符串最末尾增加换行符【所以调用者不用加\n】， 往标准错误上输出这个字符串；
//     如果err不为0，表示有错误，会将该错误编号以及对应的错误信息一并放到组合出的字符串中一起显示；

//《c++从入门到精通》里老师讲解过，比较典型的C语言中的写法，就是这种va_start,va_end
//fmt:通过这第一个普通参数来寻址后续的所有可变参数的类型及其值
//调用格式比如：xmn_log_stderr(0, "invalid option: \"%s\",%d", "testinfo",123);
/* 
    xmn_log_stderr(0, "invalid option: \"%s\"", argv[0]);  //xmoon: invalid option: "./xmoon"
    xmn_log_stderr(0, "invalid option: %10d", 21);         //xmoon: invalid option:         21  ---21前面有8个空格
    xmn_log_stderr(0, "invalid option: %.6f", 21.378);     //xmoon: invalid option: 21.378000   ---%.这种只跟f配合有效，往末尾填充0
    xmn_log_stderr(0, "invalid option: %.6f", 12.999);     //xmoon: invalid option: 12.999000
    xmn_log_stderr(0, "invalid option: %.2f", 12.999);     //xmoon: invalid option: 13.00
    xmn_log_stderr(0, "invalid option: %xd", 1678);        //xmoon: invalid option: 68E
    xmn_log_stderr(0, "invalid option: %Xd", 1678);        //xmoon: invalid option: 68E
    xmn_log_stderr(15, "invalid option: %s , %d", "testInfo",326);        //xmoon: invalid option: testInfo , 326
    xmn_log_stderr(0, "invalid option: %d", 1678); 
    */
void xmn_log_stderr(int err, const char *fmt, ...)
{
    va_list args;                         //创建一个va_list类型变量
    u_char errstr[XMN_MAX_ERROR_STR + 1]; //2048  -- ************  +1是我自己填的，感谢官方写法有点小瑕疵，所以动手调整一下
    u_char *p, *last;

    memset(errstr, 0, sizeof(errstr)); //我个人加的，这块有必要加，至少在va_end处理之前有必要，否则字符串没有结束标记不行的；***************************

    last = errstr + XMN_MAX_ERROR_STR; //last指向整个buffer最后去了【指向最后一个有效位置的后面也就是非有效位】，作为一个标记，防止输出内容超过这么长,
                                       //其实我认为这有问题，所以我才在上边errstr[XMN_MAX_ERROR_STR+1]; 给加了1
                                       //比如你定义 char tmp[2]; 你如果last = tmp+2，那么last实际指向了tmp[2]，而tmp[2]在使用中是无效的

    p = xmn_cpymem(errstr, "xmoon: ", 7); //p指向"xmoon: "之后

    va_start(args, fmt);                   //使args指向起始的参数
    p = xmn_vslprintf(p, last, fmt, args); //组合出这个字符串保存在errstr里
    va_end(args);                          //释放args

    if (err) //如果错误代码不是0，表示有错误发生
    {
        //错误代码和错误信息也要显示出来
        p = xmn_log_errno(p, last, err);
    }

    //若位置不够，那换行也要硬插入到末尾，哪怕覆盖到其他内容
    if (p >= (last - 1))
    {
        p = (last - 1) - 1; //把尾部空格留出来，这里感觉xmoon处理的似乎就不对
                            //我觉得，last-1，才是最后 一个而有效的内存，而这个位置要保存\0，所以我认为再减1，这个位置，才适合保存\n
    }
    *p++ = '\n'; //增加个换行符

    //往标准错误【一般是屏幕】输出信息
    write(STDERR_FILENO, errstr, p - errstr); //三章七节讲过，这个叫标准错误，一般指屏幕

    if (g_xmn_log.fd > STDERR_FILENO)
    {
        xmn_log_info(XMN_LOG_STDERR, err, (const char *)errstr);
    }

    return;
}

//----------------------------------------------------------------------------------------------------------------------
//描述：给一段内存，一个错误编号，我要组合出一个字符串，形如：   (错误编号: 错误原因)，放到给的这段内存中去
//     这个函数我改造的比较多，和原始的xmoon代码多有不同
//buf：是个内存，要往这里保存数据
//last：放的数据不要超过这里
//err：错误编号，我们是要取得这个错误编号对应的错误字符串，保存到buffer中
u_char *xmn_log_errno(u_char *buf, u_char *last, int err)
{
    //以下代码是我自己改造，感觉作者的代码有些瑕疵
    char *perrorinfo = strerror(err); //根据资料不会返回NULL;
    size_t len = strlen(perrorinfo);

    //然后我还要插入一些字符串： (%d:)
    char leftstr[10] = {0};
    sprintf(leftstr, " (%d: ", err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") ";
    size_t rightlen = strlen(rightstr);

    size_t extralen = leftlen + rightlen; //左右的额外宽度
    if ((buf + len + extralen) < last)
    {
        //保证整个我装得下，我就装，否则我全部抛弃 ,xmoon的做法是 如果位置不够，就硬留出50个位置【哪怕覆盖掉以往的有效内容】，也要硬往后边塞，这样当然也可以；
        buf = xmn_cpymem(buf, leftstr, leftlen);
        buf = xmn_cpymem(buf, perrorinfo, len);
        buf = xmn_cpymem(buf, rightstr, rightlen);
    }
    return buf;
}

//----------------------------------------------------------------------------------------------------------------------
//往日志文件中写日志，代码中有自动加换行符，所以调用时字符串不用刻意加\n；
//    日过定向为标准错误，则直接往屏幕上写日志【比如日志文件打不开，则会直接定位到标准错误，此时日志就打印到屏幕上，参考xmn_log_init()】
//level:一个等级数字，我们把日志分成一些等级，以方便管理、显示、过滤等等，如果这个等级数字比配置文件中的等级数字"LogLevel"大，那么该条信息不被写到日志文件中
//err：是个错误代码，如果不是0，就应该转换成显示对应的错误信息,一起写到日志文件中，
//xmn_log_error_core(5,8,"这个XXX工作的有问题,显示的结果是=%s","YYYY");
void xmn_log_info(int level, int err, const char *fmt, ...)
{
    u_char *last;
    u_char errstr[XMN_MAX_ERROR_STR + 1]; //这个+1也是我放入进来的，本函数可以参考xmn_log_stderr()函数的写法；

    memset(errstr, 0, sizeof(errstr));
    last = errstr + XMN_MAX_ERROR_STR;

    struct timeval tv;
    struct tm tm;
    time_t sec; //秒
    u_char *p;  //指向当前要拷贝数据到其中的内存位置
    va_list args;

    memset(&tv, 0, sizeof(struct timeval));
    memset(&tm, 0, sizeof(struct tm));

    gettimeofday(&tv, NULL); //获取当前时间，返回自1970-01-01 00:00:00到现在经历的秒数【第二个参数是时区，一般不关心】

    sec = tv.tv_sec;        //秒
    localtime_r(&sec, &tm); //把参数1的time_t转换为本地时间，保存到参数2中去，带_r的是线程安全的版本，尽量使用
    tm.tm_mon++;            //月份要调整下正常
    tm.tm_year += 1900;     //年份要调整下才正常

    u_char strcurrtime[40] = {0}; //先组合出一个当前时间字符串，格式形如：2019/01/08 19:57:11
    xmn_slprintf(strcurrtime,
                 (u_char *)-1,                   //若用一个u_char *接一个 (u_char *)-1,则 得到的结果是 0xffffffff....，这个值足够大
                 "%4d/%02d/%02d %02d:%02d:%02d", //格式是 年/月/日 时:分:秒
                 tm.tm_year, tm.tm_mon,
                 tm.tm_mday, tm.tm_hour,
                 tm.tm_min, tm.tm_sec);
    p = xmn_cpymem(errstr, strcurrtime, strlen((const char *)strcurrtime)); //日期增加进来，得到形如：     2019/01/08 20:26:07
    p = xmn_slprintf(p, last, " [%s] ", err_levels[level]);                 //日志级别增加进来，得到形如：  2019/01/08 20:26:07 [crit]
    p = xmn_slprintf(p, last, "%P: ", g_xmn_pid);                             //支持%P格式，进程id增加进来，得到形如：   2019/01/08 20:50:15 [crit] 2037:

    va_start(args, fmt);                   //使args指向起始的参数
    p = xmn_vslprintf(p, last, fmt, args); //把fmt和args参数弄进去，组合出来这个字符串
    va_end(args);                          //释放args

    if (err) //如果错误代码不是0，表示有错误发生
    {
        //错误代码和错误信息也要显示出来
        p = xmn_log_errno(p, last, err);
    }
    //若位置不够，那换行也要硬插入到末尾，哪怕覆盖到其他内容
    if (p >= (last - 1))
    {
        p = (last - 1) - 1; //把尾部空格留出来，这里感觉xmoon处理的似乎就不对
                            //我觉得，last-1，才是最后 一个而有效的内存，而这个位置要保存\0，所以我认为再减1，这个位置，才适合保存\n
    }
    *p++ = '\n'; //增加个换行符

    //这么写代码是图方便：随时可以把流程弄到while后边去；大家可以借鉴一下这种写法
    ssize_t n;
    while (1)
    {
        if (level > g_xmn_log.log_level)
        {
            //要打印的这个日志的等级太落后（等级数字太大，比配置文件中的数字大)
            //这种日志就不打印了
            break;
        }
        //磁盘是否满了的判断，先算了吧，还是由管理员保证这个事情吧；

        //写日志文件
        n = write(g_xmn_log.fd, errstr, p - errstr); //文件写入成功后，如果中途
        if (n == -1)
        {
            //写失败有问题
            if (errno == ENOSPC) //写失败，且原因是磁盘没空间了
            {
                //磁盘没空间了
                //没空间还写个毛线啊
                //先do nothing吧；
            }
            else
            {
                //这是有其他错误，那么我考虑把这个错误显示到标准错误设备吧；
                if (g_xmn_log.fd != STDERR_FILENO) //当前是定位到文件的，则条件成立
                {
                    n = write(STDERR_FILENO, errstr, p - errstr);
                }
            }
        }
        break;
    } //end while
    return;
}

//----------------------------------------------------------------------------------------------------------------------
//描述：日志初始化，就是把日志文件打开 ，注意这里边涉及到释放的问题，如何解决？
void xmn_log_init()
{
    std::string strplogname = "";
    size_t nlen;

    //从配置文件中读取和日志相关的配置信息
    XMNConfig *p_config = XMNConfig::GetInstance();
    strplogname = p_config->GetConfigItem("Log");
    if (strplogname == "")
    {
        //没读到，就要给个缺省的路径文件名了
        strplogname = XMN_ERROR_LOG_PATH; //"logs/error.log" ,logs目录需要提前建立出来
    }
    g_xmn_log.log_level = atoi(p_config->GetConfigItem("LogLevel", std::to_string(XMN_LOG_NOTICE)).c_str()); //缺省日志等级为6【注意】 ，如果读失败，就给缺省日志等级
    //nlen = strlen((const char *)plogname);

    //只写打开|追加到末尾|文件不存在则创建【这个需要跟第三参数指定文件访问权限】
    //mode = 0644：文件访问权限， 6: 110    , 4: 100：     【用户：读写， 用户所在组：读，其他：读】 老师在第三章第一节介绍过
    g_xmn_log.fd = open(strplogname.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (g_xmn_log.fd == -1) //如果有错误，则直接定位到 标准错误上去
    {
        xmn_log_stderr(errno, "[alert] could not open error log file: open() \"%s\" failed", strplogname.c_str());
        g_xmn_log.fd = STDERR_FILENO; //直接定位到标准错误去了
    }
    return;
}
