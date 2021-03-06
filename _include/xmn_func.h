﻿/*****************************************************************************************
 * @function    存放全局函数。
 * @time    2019-08-15
*****************************************************************************************/

#ifndef XMOON__INCLUDE_XMN_FUNC_H_
#define XMOON__INCLUDE_XMN_FUNC_H_

#include <string>

/******************************  设置进程相关函数  *******************************/
/**
 * @function    初始化函数，用于分配内存，转移 envion 至新的内存中，为进程标题腾出位置。
 * @paras   none 。
 * @ret  none 。
 * @time    2019-08-17
*/
void XMNSetProcTitleInit();

/**
 * @function    设置当前进程标题。
 * @paras   strtitle    标题。
 * @ret  0   设置成功。
 *          -1  标题过长，设置失败。
 * @time    2019-08-17
*/
int XMNSetProcTitle(const std::string &strtitle);

/******************************  日志打印相关函数  *******************************/
int XMNLogInit();
void XMNLogStdErr(int err, const char *fmt, ...);
void XMNLogInfo(int level, int err, const char *fmt, ...);

u_char *xmn_log_errno(u_char *buf, u_char *last, int err);
u_char *xmn_slprintf(u_char *buf, u_char *last, const char *fmt, ...);
u_char *xmn_vslprintf(u_char *buf, u_char *last, const char *fmt, va_list args);

/******************************  信号相关函数  *******************************/
/**
 * @function    信号初始化函数，设置各个信号的信号处理函数。
 * @paras   none 。
 * @ret none 。
 * @time    2019-08-17
*/
int XMNSignalInit();

/******************************  主流程相关函数  *******************************/
/**
 * @function    开始运行主进程，会创建指定数目的子进程并自身进入死循环中。
 * @paras   none 。
 * @ret none 。
 * @time    2019-08-17
*/
void XMNMasterProcessCycle();

/**
 * @function    创建守护进程。
 * @paras   none 。
 * @ret 1   主进程返回。
 *      0   子进程返回。
 *      -1  创建失败。
 * @time    2019-08-17
*/
int XMNCreateDaemon();

/**
 * @function    处理子进程的网络事件和定时器事件。
 * @paras   none 。
 * @ret 0   操作成功。
 * @time    2019-08-27
*/
int XMNProcessEventsTimers();

#endif