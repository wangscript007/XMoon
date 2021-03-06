#ifndef XMOON__INCLUDE_XMN_THREADPOOL_H_
#define XMOON__INCLUDE_XMN_THREADPOOL_H_

#include "base/noncopyable.h"

#include <pthread.h>
#include <string.h>
#include <vector>
#include <atomic>
#include <queue>
#include <memory>

class XMNThreadPool : public NonCopyable
{
private:
    /**
     * 保存单个线程的信息。
    */
    class ThreadInfo : public NonCopyable
    {
    public:
        ThreadInfo() = delete;
        ThreadInfo(XMNThreadPool *ppool) : pthreadpool_(ppool)
        {
            isrunning_ = false;
            threadhandle_ = 0;
        }
        ~ThreadInfo(){};

    public:
        pthread_cond_t *GetCond()
        {
            return pcond_;
        }

        void SetCond(pthread_cond_t *pcond)
        {
            pcond_ = pcond;;
        }

    public:
        /**
         * 该线程所在的线程池的首地址。
        */
        XMNThreadPool *pthreadpool_;

        /**
         * 该线程是否在运行。
        */
        bool isrunning_;

        /**
         * 该线程的描述符。
        */
        pthread_t threadhandle_;

        /**
         * 线程专属条件变量。
        */
        pthread_cond_t *pcond_;
    };

public:
    XMNThreadPool();
    ~XMNThreadPool();

public:
    /**
     * @function    创建线程池。
     * @paras   kThreadCount 线程池中线程的数量。
     * @ret  0   操作成功。
     * @time    2019-09-04
    */
    int Create(const size_t &kThreadCount);

    /**
     * @funtion 释放线程池中所有线程。
     * @paras   none 。
     * @ret  0   操作成功。
     * @time    2019-09-04
    */
    int Destroy();

    /**
     * @function    唤醒一个线程开始执行任务。
     * @paras   none 。
     * @ret  0   操作成功
     * @time    2019-09-05
    */
    int Call();

    /**
     * @function    将接收到的数据压入消息队列中。
     * @paras   data   接收到的数据。
     * @ret  none 。
     * @time    2019-09-01
    */
    int PutInRecvDataQueue_Signal(char *data);

    /**
     * @function    获取消息的数量
     * @paras   none 。
     * @ret  消息的数量
     * @time    2019-09-12
    */
    size_t RecvDataQueueSize();

private:
    /**
     * @function    线程的执行入口函数。
     * @paras   pthreaddata 保存当前线程信息的内存。
     * @ret  nullptr 。
     * @time 2019-09-07
    */
    static void *ThreadFunc(void *pthreaddata);

    /**
     * @function    从消息队列中获取消息。
     * @paras   none 。
     * @ret  非0 获取消息成功。
     *       nullptr 获取消息失败。
     * @time    2019-09-06
     * @notice  该函数内部不加锁的原因时该函数的使用时已经在上锁的状态。
    */
    char *PutOutRecvDataQueue();

private:
    /**
     * 线程池中线程的数量。
    */
    size_t threadpoolsize_;

    /**
     * 保持线程池中每个线程的信息。
    */
    std::vector<ThreadInfo *> vthreadinfo_;

    /**
     * 线程是否退出的标识。
    */
    bool isquit_;

    /**
     * 线程池中正在运行的线程的数量。
    */
    std::atomic<size_t> threadrunningcount_;

    /**
     * 线程同步互斥量。
    */
    pthread_mutex_t thread_mutex_;

    /**
     * 线程同步条件数组。
    */
    std::queue<pthread_cond_t *> queue_thread_cond_;

    /**
     * vthread_cond_ 同步锁。
    */
    pthread_mutex_t queue_thread_cond_mutex;

    /**
     * 记录上次线程池中的线程全都工作时的时间。
    */
    time_t allthreadswork_lasttime_;

    /**
     * 接收消息队列的同步互斥量。
    */
    pthread_mutex_t recvdata_queue_mutex_;

    /**
     * 存放接收的数据的消息队列。
    */
    std::queue<char *> recvdata_queue_;

    /**
     * 存放接收的数据的消息队列的大小。
    */
    std::atomic<size_t> queue_recvdata_count_;
};

#endif