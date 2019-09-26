#include "comm/xmn_socket.h"
#include "xmn_config.h"
#include "xmn_func.h"
#include "xmn_macro.h"
#include "xmn_lockmutex.hpp"
#include "xmn_memory.h"

#include "sys/socket.h"
#include "sys/types.h"
#include "sys/ioctl.h"
#include "linux/sockios.h"
#include "arpa/inet.h"
#include "errno.h"
#include "unistd.h"
#include <errno.h>

#include <cstdio>
#include <sstream>

XMNSocket::XMNSocket()
{
    msgheaderlen_ = sizeof(XMNMsgHeader);
    pkgheaderlen_ = sizeof(XMNPkgHeader);
    listenport_count_ = 0;
    pportsum_ = nullptr;
    worker_connection_count_ = 0;
    epoll_handle_ = 0;
    pool_connsock_count_ = 0;
    pool_free_connsock_count_ = 0;
    pool_recyconnsock_count_ = 0;
    queue_senddata_count_ = 0;
    recyconnsockinfowaittime_ = 0;
    memset(wait_events_, 0, sizeof(struct epoll_event) * XMN_EPOLL_WAIT_MAX_EVENTS);
}

XMNSocket::~XMNSocket()
{
    std::vector<XMNListenSockInfo *>::iterator it;
    for (it = vlistenportsockinfolist_.begin(); it != vlistenportsockinfolist_.end(); ++it)
    {
        delete *it;
        *it = nullptr;
    }
    vlistenportsockinfolist_.clear();

    if (pportsum_ != nullptr)
    {
        delete[] pportsum_;
        pportsum_ = nullptr;
    }
}

int XMNSocket::Initialize()
{
    int r = ReadConf();
    if (r != 0)
    {
        return r;
    }
    /**
     * 开启指定的端口号。
    */
    return OpenListenSocket(pportsum_, listenport_count_);
}

int XMNSocket::InitializeWorker()
{
    /**
     * （1）初始化互斥量。
     * a、与连接池操作相关的互斥量。
     * b、与回收连接池相关的互斥量。
     * c、与发送消息相关的互斥量。
    */
    if (pthread_mutex_init(&connsock_pool_mutex_, nullptr) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::InitializeWorker 中 pthread_mutex_init(&connsock_pool_mutex_) 执行失败。");
        return -1;
    }
    if (pthread_mutex_init(&connsock_pool_recy_mutex_, nullptr) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::InitializeWorker 中 pthread_mutex_init(&connsock_pool_recy_mutex_) 执行失败。");
        return -2;
    }
    if (pthread_mutex_init(&senddata_queue_mutex_, nullptr) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::InitializeWorker 中 pthread_mutex_init(&senddata_queue_mutex_) 执行失败。");
        return -3;
    }

    /**
     * （2）初始化信号量。
    */
    if (sem_init(&senddata_queue_sem_, 0, 0) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::InitializeWorker()中sem_init()执行失败。");
        return -4;
    }

    /**
     * （3）创建线程。
     * a、创建用于回收连接的线程。
     * b、创建用于发送数据的线程。
    */
    ThreadInfo *pthreadinfo_recysockinfo = nullptr;
    vthreadinfo_.push_back(pthreadinfo_recysockinfo = new ThreadInfo(this));
    pthread_create(&pthreadinfo_recysockinfo->threadhandle_, nullptr, ConnSockInfoRecycleThread, (void *)pthreadinfo_recysockinfo);

    ThreadInfo *pthreadinfo_senddata = nullptr;
    vthreadinfo_.push_back(pthreadinfo_senddata = new ThreadInfo(this));
    pthread_create(&pthreadinfo_senddata->threadhandle_, nullptr, SendDataThread, (void *)pthreadinfo_senddata);
    return 0;
}

int XMNSocket::EndWorker()
{
    /**
     * （1）终止线程。
     * 在执行该函数之前，全局变量 g_isquit 应该置为 true 。
    */
    if (sem_post(&senddata_queue_sem_) == -1)
    {
        xmn_log_stderr(0, "XMNSocket::EndWorker()中sem_post()执行失败。");
    }

    std::vector<ThreadInfo *>::iterator it;
    for (it = vthreadinfo_.begin(); it != vthreadinfo_.end(); it++)
    {
        pthread_join((*it)->threadhandle_, nullptr);
    }
    for (it = vthreadinfo_.begin(); it != vthreadinfo_.end(); it++)
    {
        delete (*it);
        (*it) = nullptr;
    }
    vthreadinfo_.clear();

    /**
     * （2）回收线程池、发送消息队列。
    */
    FreeConnSockInfoPool();
    FreeSendDataQueue();

    /**
     * （3）销毁所有的互斥量、信号量。
    */
    pthread_mutex_destroy(&connsock_pool_mutex_);
    pthread_mutex_destroy(&connsock_pool_recy_mutex_);
    pthread_mutex_destroy(&senddata_queue_mutex_);
    sem_destroy(&senddata_queue_sem_);
    return 0;
}

int XMNSocket::OpenListenSocket(const size_t *const pport, const size_t &listenportcount)
{
    int exitcode = 0;
    int r = 0;

    /**
     * 存放创建的监听 socket 。
    */
    int *psocksum = new int[listenportcount]();

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    /**
     * 0,0,0,0 该地址代表本机所有 IP 。
    */
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    /**
     * 对每一个 port 创建一个 socket 。
    */
    for (size_t i = 0; i < listenportcount; i++)
    {
        /**
         *  创建连接 socket 。
        */
        psocksum[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (psocksum[i] <= 0)
        {
            xmn_log_info(XMN_LOG_EMERG, errno, "OpenListenSocket create listen socket failed.");
            exitcode = -1;
            goto exitlabel;
        }

        /**
         * 设置 server 关闭之后可以立刻重启 server 的功能，即：地址重用功能。
        */
        int reuseaddr = 1;
        r = setsockopt(psocksum[i], SOL_SOCKET, SO_REUSEADDR, (const void *)&reuseaddr, sizeof(reuseaddr));
        if (r != 0)
        {
            xmn_log_info(XMN_LOG_EMERG, errno, "OpenListenSocket setsockopt failed.");
            exitcode = -2;
            goto exitlabel;
        }

        /**
         * 设置 socket 为非堵塞模式。
        */
        r = SetNonBlocking(psocksum[i]);
        if (r != 0)
        {
            xmn_log_info(XMN_LOG_EMERG, errno, "OpenListenSocket SetNonBlocking failed.");
            exitcode = -3;
            goto exitlabel;
        }

        /**
         * 绑定 IP 和 port 。
        */
        addr.sin_port = htons(pport[i]);
        r = bind(psocksum[i], (struct sockaddr *)&addr, sizeof(addr));
        if (r != 0)
        {
            xmn_log_info(XMN_LOG_EMERG, errno, "OpenListenSocket bind failed.");
            exitcode = -4;
            goto exitlabel;
        }

        /**
         * 开始监听。
        */
        r = listen(psocksum[i], XMN_LISTEN_BACKLOG);
        if (r != 0)
        {
            xmn_log_info(XMN_LOG_EMERG, errno, "OpenListenSocket listen failed.");
            exitcode = -5;
            goto exitlabel;
        }

        /**
         * 将 port 和 soket 插入 vector 中。
        */
        XMNListenSockInfo *pitem = new XMNListenSockInfo;
        pitem->fd = psocksum[i];
        pitem->port = pport[i];
        vlistenportsockinfolist_.push_back(pitem);

        xmn_log_info(XMN_LOG_INFO, 0, "监听端口 %d 的socket 创建成功！", pport[i]);
    }
    return 0;

exitlabel:
    for (size_t i = 0; i < listenportcount; i++)
    {
        close(psocksum[i]);
    }
    delete[] psocksum;
    psocksum = nullptr;
    return exitcode;
}

int XMNSocket::CloseListenSocket()
{
    std::vector<XMNListenSockInfo *>::iterator it;
    for (it = vlistenportsockinfolist_.begin(); it != vlistenportsockinfolist_.end(); it++)
    {
        close((*it)->fd);
        xmn_log_info(XMN_LOG_INFO, 0, "监听端口 %d 的 socket 已经关闭！", (*it)->port);
    }
    return 0;
}

int XMNSocket::SetNonBlocking(const int &sockfd)
{
    int setnoblock = 1;
    return ioctl(sockfd, FIONBIO, &setnoblock);
}

int XMNSocket::ReadConf()
{
    XMNConfig *pconfig = SingletonBase<XMNConfig>::GetInstance();

    /**
     * （1）获取 port 的数量。
    */
    listenport_count_ = atoi(pconfig->GetConfigItem("ListenPortCount", "59002").c_str());
    if (listenport_count_ <= 0)
    {
        return 1;
    }

    /**
     * （2）获取所有的 port 。
    */
    pportsum_ = new size_t[listenport_count_];
    std::string str;
    std::stringstream s;
    for (size_t i = 0; i < listenport_count_; i++)
    {
        s << i;
        str = "ListenPort" + s.str();
        pportsum_[i] = atoi(pconfig->GetConfigItem(str).c_str());
        if (pportsum_[i] <= 0)
        {
            return 2;
        }
        s.clear();
        s.str("");
    }

    /**
     * （3）获取每个 worker 进程的 epoll 连接的最大项数。
    */
    worker_connection_count_ = atoi(pconfig->GetConfigItem("worker_connections", "1024").c_str());
    if (worker_connection_count_ <= 0)
    {
        return 3;
    }

    /**
     * （4）获取连接回收的等待时间。
    */
    g_socket.recyconnsockinfowaittime_ = atoi(pconfig->GetConfigItem("RecyConnSockInfoWaitTime", "60").c_str());
    if (g_socket.recyconnsockinfowaittime_ <= 0)
    {
        return 4;
    }

    return 0;
}

int XMNSocket::EpollInit()
{
    /**
     * （1）创建 epoll 对象。
    */
    epoll_handle_ = epoll_create(worker_connection_count_);
    if (epoll_handle_ <= 0)
    {
        xmn_log_stderr(errno, "EpollInit 中的 epoll_create()执行失败！");
        return -1;
    }

    /**
     * （2）连接池初始化。
    */
    InitConnSockInfoPool();

    /**
     * （2）创建指定数量的连接池和空闲连接的单向链表。
    */
    /*
    pool_connsock_count_ = worker_connection_count_;
    pconnsock_pool_ = new XMNConnSockInfo[pool_connsock_count_];
    memset(pconnsock_pool_, 0, sizeof(XMNConnSockInfo) * pool_connsock_count_);
    size_t conn_count = pool_connsock_count_;
    XMNConnSockInfo *next = nullptr;
    */
    /**
     * 从数组末尾向头部进行链表的串联。
    */
    /*
    do
    {
        --conn_count;
        pconnsock_pool_[conn_count].next = next;
        pconnsock_pool_[conn_count].fd = -1;
        pconnsock_pool_[conn_count].instance = 1;
        pconnsock_pool_[conn_count].currsequence = 0;
        pconnsock_pool_[conn_count].logicprocmutex = PTHREAD_MUTEX_INITIALIZER;

        next = &pconnsock_pool_[conn_count];
    } while (conn_count);
    */
    /**
     * 赋值空闲链表的头指针，使其指向数组的第一个元素。
    */
    /*
    pfree_connsock_list_head_ = next;
    pool_free_connsock_count_ = pool_connsock_count_;
    */

    /**
     * （3）循环遍历所有监听 socket ，为每个 socket 绑定一个连接池中的连接，用于记录相关信息。
    */
    XMNConnSockInfo *pconnsockinfo = nullptr;
    std::vector<XMNListenSockInfo *>::iterator it;
    for (it = vlistenportsockinfolist_.begin(); it != vlistenportsockinfolist_.end(); ++it)
    {
        /**
         * 从连接池中取出空闲节点。
        */
        pconnsockinfo = PutOutConnSockInfofromPool((*it)->fd);
        if (pconnsockinfo == nullptr)
        {
            xmn_log_stderr(errno, "EpollInit 中 PutOutConnSockInfofromPool() 执行失败！");
            return -2;
        }
        /**
         * 连接对象和监听对象进行关联。
        */
        pconnsockinfo->plistensockinfo = (*it);
        /**
         * 监听对象和连接对象进行关联。
        */
        (*it)->pconnsockinfo = pconnsockinfo;

        /**
         * 对监听 socket 读事件设置处理函数，开始让监听 sokcet 履行职责。
        */
        pconnsockinfo->rhandler = &XMNSocket::EventAcceptHandler;
        /*
        if (EpollAddEvent(
                (*it)->fd,     //socekt句柄
                1, 0,          //读，写【只关心读事件，所以参数2：readevent=1,而参数3：writeevent=0】
                0,             //其他补充标记
                EPOLL_CTL_ADD, //事件类型【增加，还有删除/修改】
                pconnsockinfo  //连接池中的连接
                ) == -1)
        {
            return -3;
        }
        */
        if (EpollOperationEvent((*it)->fd,
                                EPOLL_CTL_ADD,
                                EPOLLIN | EPOLLRDHUP,
                                0,
                                pconnsockinfo) != 0)
        {
            return -3;
        }
    }
    return 0;
}

/*
int XMNSocket::EpollAddEvent(const int &fd,
                             const int &readevent, const int &writeevent,
                             const uint32_t &otherflag,
                             const uint32_t &eventtype,
                             XMNConnSockInfo *pconnsockinfo)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event) * 1);

    if (readevent == 1)
    {
        ev.events = EPOLLIN | EPOLLRDHUP;

        //https://blog.csdn.net/q576709166/article/details/8649911
        //找下EPOLLERR的一些说法：
        //a)对端正常关闭（程序里close()，shell下kill或ctr+c），触发EPOLLIN和EPOLLRDHUP，但是不触发EPOLLERR 和EPOLLHUP。
        //b)EPOLLRDHUP    这个好像有些系统检测不到，可以使用EPOLLIN，read返回0，删除掉事件，关闭close(fd);如果有EPOLLRDHUP，检测它就可以直到是对方关闭；否则就用上面方法。
        //c)client 端close()联接,server 会报某个sockfd可读，即epollin来临,然后recv一下 ， 如果返回0再掉用epoll_ctl 中的EPOLL_CTL_DEL , 同时close(sockfd)。
        //有些系统会收到一个EPOLLRDHUP，当然检测这个是最好不过了。只可惜是有些系统，上面的方法最保险；如果能加上对EPOLLRDHUP的处理那就是万能的了。
        //d)EPOLLERR      只有采取动作时，才能知道是否对方异常。即对方突然断掉，是不可能有此事件发生的。只有自己采取动作（当然自己此刻也不知道），read，write时，出EPOLLERR错，说明对方已经异常断开。
        //e)EPOLLERR 是服务器这边出错（自己出错当然能检测到，对方出错你咋能知道啊）
        //f)给已经关闭的socket写时，会发生EPOLLERR，也就是说，只有在采取行动（比如读一个已经关闭的socket，或者写一个已经关闭的socket）时候，才知道对方是否关闭了。
        //这个时候，如果对方异常关闭了，则会出现EPOLLERR，出现Error把对方DEL掉，close就可以了。
    }
    else
    {
        //其他事件……
    }

    ev.events |= otherflag;

    ev.data.ptr = (void *)((uintptr_t)pconnsockinfo | pconnsockinfo->instance);

    int r = epoll_ctl(epoll_handle_, eventtype, fd, &ev);
    if (r == -1)
    {
        xmn_log_stderr(errno, "EpollAddEvent 中 epoll_ctl执行失败！");
        return -1;
    }

    return 0;
}
*/

int XMNSocket::EpollOperationEvent(const int &fd,
                                   const uint32_t &eventtype,
                                   const uint32_t &flag,
                                   const int &bcaction,
                                   XMNConnSockInfo *pconnsockinfo)
{
    /**
     * （1）epoll_event 变量赋值。
    */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));
    if (eventtype == EPOLL_CTL_ADD)
    {
        ev.events = flag;
        //ev.data.ptr = (void *)pconnsockinfo;
        pconnsockinfo->eventtype = flag;
    }
    else if (eventtype == EPOLL_CTL_MOD)
    {
        /**
         * 恢复标记。
        */
        ev.events = pconnsockinfo->eventtype;
        if (bcaction == 0)
        {
            /**
             * 增加该标记。
            */
            ev.events |= flag;
        }
        else if (bcaction == 1)
        {
            /**
             * 删除该标记。
            */
            ev.events ^= ~flag;
        }
        else if (bcaction == 2)
        {
            /**
             * 覆盖该标记。
            */
            ev.events = flag;
        }
        else
        {
            return -3;
        }

        pconnsockinfo->eventtype = ev.events;
    }
    else if (eventtype == EPOLL_CTL_DEL)
    {
        /* code */
    }
    else
    {
        return -1;
    }

    /**
     * （2）epoll_ctl()函数的调用。
    */
    ev.data.ptr = (void *)pconnsockinfo;
    if (epoll_ctl(epoll_handle_, eventtype, fd, &ev) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::EpollOperationEvent 中 epoll_ctl 执行失败。");
        return -2;
    }

    return 0;
}

int XMNSocket::EpollProcessEvents(const int &timer)
{
    int eventcount = 0;
    /**
     * （1）取出发生的事件信息。
    */
    /**
     * @function    从双向链表中获取 XMN_EPOLL_WAIT_MAX_EVENTS 个 epoll_event 对象。
     * @paras   epoll_handle_ epoll 对象，相当于事件代理。
     *          wait_events_   epoll_event 对象存储池。
     *          XMN_EPOLL_WAIT_MAX_EVENTS   wait_events_ 大小。
     *          timer   超时时间，若为-1，则一直堵塞，直至有事件到来。
     * @return  > 0  实际返回的 epoll_event 对象的数量，即：事件的数量。
     *          = 0  等待超时。
     *          -1   有错误发生，报错代码保存在 errno 中。
     * @notice  该函数的返回条件如下：
     * （1）等待超时。
     * （2）有事件发生。
     * （3）有信号发生。                                                                 
    */
    eventcount = epoll_wait(epoll_handle_, wait_events_, XMN_EPOLL_WAIT_MAX_EVENTS, timer);

    /**
     * TODO：这里有惊群效应，后续对该问题进行处理。
     * 由于多个进程同时监控 port ，导致客户端来了连接，多个进程的 epoll_wait 会部分返回，
     * 然后只有一个进程的 accept 会获取到该连接，其他的进程白走一遭，浪费了 CPU 资源。
    */

    /**
     * 对事件进行过滤。
    */
    if (eventcount == -1)
    {
        /**
         * 信号所致。不是错误，但是得记录。
        */
        if (errno == EINTR)
        {
            xmn_log_info(XMN_LOG_INFO, errno, "EpollProcessEvents 中 epoll_wait 执行错误，因为有信号到来。");
            return 0;
        }
        /**
         * 发生错误，需要处理。
        */
        else
        {
            xmn_log_info(XMN_LOG_ALERT, errno, "EpollProcessEvents 中 epoll_wait 执行错误！");
            return -2;
        }
    }
    else if (eventcount == 0)
    {
        /**
         * 正常超时返回。
        */
        if (timer != -1)
        {
            return 0;
        }
        /**
         * epoll_wait 在设置了一直堵塞的情况下返回了超时状态，肯定有问题。
        */
        else
        {
            xmn_log_info(XMN_LOG_ALERT, errno, "EpollProcessEvents 中 epoll_wait 在设置一直堵塞的情况下返回了超时！");
            return -3;
        }
    }

    /**
     * （2）对每一个事件进行处理。
    */
    /**
     * 执行到这里说明收到了事件。
    */
    XMNConnSockInfo *pconnsockinfo = nullptr;
    uint32_t flags;
    //int instance = 0;
    for (size_t i = 0; i < eventcount; ++i)
    {
        /**
         *  获取该事件对应的连接的相关信息。
        */
        pconnsockinfo = (XMNConnSockInfo *)(wait_events_ + i)->data.ptr;
        /*
        instance = (uintptr_t)pconnsockinfo & 1;
        pconnsockinfo = (XMNConnSockInfo *)((uintptr_t)pconnsockinfo & (uintptr_t)~1);
        */
        /**
         * 处理过期事件。
        */
        /*
        if (pconnsockinfo->fd == -1)
        {
            //来了3个事件，
            //第1个事件关闭连接，fd == -1 。
            //第2个事件正常处理。
            //第3个事件则是第1个连接的事件，为过期事件。
            xmn_log_info(XMN_LOG_DEBUG, 0, "EpollProcessEvents 遇到了 fd == -1 的过期事件 %p", pconnsockinfo);
            continue;
        }
        
        if (pconnsockinfo->instance != instance)
        {
            //来了3个事件。
            //第1个事件关闭连接，fd == -1。
            //第2个事件建立连接，该新连接恰好用到了线程池中第1个事件释放的连接。
            //第3个事件是第1个事件对应的连接的事件，为过期事件。
            //判断的原理就是每次从连接池中获取连接时，instance 都会取反。
            xmn_log_info(XMN_LOG_DEBUG, 0, "EpollProcessEvents 遇到了 instance 改变的过期事件 %p", pconnsockinfo);
            continue;
        }
        */
        /**
         * 程序走到这里，可以认为事件是非过期事件。
         * 确定事件类型，根据不同的类型来调用不同的处理函数。
        */
        flags = wait_events_[i].events;
        /**
         * TODO：正常关闭连接，具体代码是不是这么写，后续确认！
        */
        /*
        if (events_type & (EPOLLERR | EPOLLHUP))
        {
            //加上读写标记方便后续处理。
            //EPOLLIN  表示指定的 epoll_event 对应的连接有可读数据。
            //EPOLLOUT 表示指定的 epoll_event 对应的连接是可写的。
            events_type |= EPOLLIN | EPOLLOUT;
        }
        */
        /**
         * 读事件。触发条件，
         * （1）客户端新连入。
         * （2）已连接发送了数据。
        */
        if (flags & EPOLLIN)
        {
            (this->*(pconnsockinfo->rhandler))(pconnsockinfo);
        }
        /**
         * 写事件。server 可以向 client 发送数据了。
         * 注意：对方关闭连接时也执行这部分代码，
         * 因为 events_type & (EPOLLERR | EPOLLHUP) 时，events_type |= EPOLLIN | EPOLLOUT 。
        */
        if (flags & EPOLLOUT)
        {
            if (flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                /**
                 * server 挂了一个可写通知，但是 client 却关闭了，则此处会被执行。
                 * EPOLLERR：对应的连接发生了错误。
                 * EPOLLHUP：对应的连接被挂起。
                 * EPOLLRDHUP：表示TCP连接，远端处于关闭或者办关闭的状态。
                */
                xmn_log_stderr(0, " XMNSocket::EpollProcessEvents()中flags & EPOLLOUT成立，\
但是flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)也成立，flags的值为%d。",
                               flags);
                --pconnsockinfo->throwepollsendcount;
            }
            else
            {
                (this->*(pconnsockinfo->whandler))(pconnsockinfo);
            }

            //xmn_log_stderr(0, "收到可写事件。");
        }
    }

    return 0;
}

int XMNSocket::PutInSendDataQueue(char *psenddata)
{
    XMNLockMutex lockmutex_senddata(&senddata_queue_mutex_);
    senddata_queue_.push(psenddata);
    ++queue_senddata_count_;

    if (sem_post(&senddata_queue_sem_) != 0)
    {
        xmn_log_stderr(0, "XMNSocket::MsgSend()中sem_post()执行失败。");
    }
    return 0;
}

char *XMNSocket::PutOutSendDataFromQueue()
{
    XMNLockMutex lockmutex_senddata(&senddata_queue_mutex_);
    char *psenddata = nullptr;
    if (queue_senddata_count_ != 0)
    {
        psenddata = senddata_queue_.front();
        senddata_queue_.pop();
        --queue_senddata_count_;
        return psenddata;
    }
    return nullptr;
}

void *XMNSocket::SendDataThread(void *pthreadinfo)
{
    if (pthreadinfo == nullptr)
    {
        xmn_log_stderr(0, "XMNSocket::SendDataThread() 中形参 pthreadinfo 为 nullptr 。");
        return nullptr;
    }

    /**
     * （1）定义变量。
    */
    ThreadInfo *pthreadinfo_new = (ThreadInfo *)pthreadinfo;
    XMNSocket *psocket = pthreadinfo_new->pthis_;
    XMNMsgHeader *pmsgheader = nullptr;
    XMNPkgHeader *ppkgheader = nullptr;
    char *psendalldata = nullptr;
    XMNConnSockInfo *pconnsockinfo = nullptr;
    XMNMemory *pmemory = (XMNMemory *)SingletonBase<XMNMemory>::GetInstance();
    int err = 0;
    int sendsize = 0;

    while (!g_isquit)
    {
        /**
         * （2）等待待发送的消息。
        */
        if (sem_wait(&psocket->senddata_queue_sem_) != 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            xmn_log_stderr(0, "XMNSocket::SendDataThread() 中 sem_wait 执行失败。");
            continue;
        }
        if (g_isquit)
        {
            break;
        }

        /**
         * 将发送消息队列中的消息全部发送完毕。
        */
        while (true)
        {
            psendalldata = psocket->PutOutSendDataFromQueue();
            if (psendalldata == nullptr)
            {
                break;
            }

            pmsgheader = (XMNMsgHeader *)psendalldata;
            ppkgheader = (XMNPkgHeader *)(psendalldata + psocket->msgheaderlen_);
            pconnsockinfo = pmsgheader->pconnsockinfo;

            /**
             * （3）判断消息是否过期。
            */
            if (pconnsockinfo->currsequence != pmsgheader->currsequence)
            {
                pmemory->FreeMemory(psendalldata);
                psendalldata = nullptr;
                pconnsockinfo->psenddata = nullptr;
                pconnsockinfo->senddatalen = 0;
                pconnsockinfo->throwepollsendcount = 0;
                continue;
            }

            /**
             * （4）发送消息。
            */
            if (pconnsockinfo->throwepollsendcount > 0)
            {
                /**
                 * 说明该消息已经确定靠 epoll 驱动发送了，所以程序不用再向下执行了。
                */
                //PutInSendDataQueue(psenddata);
                continue;
            }
            pconnsockinfo->psendalldataforfree = psendalldata;
            pconnsockinfo->psenddata = psendalldata + psocket->msgheaderlen_;
            pconnsockinfo->senddatalen = (size_t)ntohs(ppkgheader->pkglen);

            xmn_log_stderr(0, "即将发送的数据大小为 %d", pconnsockinfo->senddatalen);
            sendsize = psocket->MsgSend(pconnsockinfo);
            if (sendsize > 0)
            {
                if (sendsize == pconnsockinfo->senddatalen)
                {
                    /**
                     * 全部正常发送成功。
                    */
                    pmemory->FreeMemory(pconnsockinfo->psendalldataforfree);
                    pconnsockinfo->psendalldataforfree = nullptr;
                    pconnsockinfo->psenddata = nullptr;
                    pconnsockinfo->senddatalen = 0;
                    pconnsockinfo->throwepollsendcount = 0;
                }
                else
                {
                    /**
                     * 数据不能全部发送，说明发送缓冲区已经满了。
                    */
                    pconnsockinfo->psenddata = pconnsockinfo->psenddata + sendsize;
                    pconnsockinfo->senddatalen = pconnsockinfo->senddatalen - sendsize;
                    ++pconnsockinfo->throwepollsendcount;
                    if (psocket->EpollOperationEvent(pconnsockinfo->fd, EPOLL_CTL_MOD, EPOLLOUT, 0, pconnsockinfo) != 0)
                    {
                        xmn_log_stderr(0, "XMNSocket::SendDataThread()中执行EpollOperationEvent()失败。");
                    }
                    xmn_log_stderr(0,
                                   "XMNSocket::SendDataThread()中理论发送 %d 个字节，实际发送 %d 个字节。",
                                   pconnsockinfo->senddatalen,
                                   sendsize);
                }
                continue;
            }
            /**
             * 程序运行到这里说明发送数据时出现了问题。
            */
            else if (sendsize == 0)
            {
                /**
                 * 发送端已断开连接。
                */
                pmemory->FreeMemory(pconnsockinfo->psendalldataforfree);
                pconnsockinfo->psendalldataforfree = nullptr;
                pconnsockinfo->psenddata = nullptr;
                pconnsockinfo->senddatalen = 0;
                pconnsockinfo->throwepollsendcount = 0;
                continue;
            }
            else if (sendsize == -1)
            {
                /**
                 * 发送缓冲区已满。
                */
                ++pconnsockinfo->throwepollsendcount;
                if (psocket->EpollOperationEvent(pconnsockinfo->fd, EPOLL_CTL_MOD, EPOLLOUT, 0, pconnsockinfo) != 0)
                {
                    xmn_log_stderr(0, "XMNSocket::SendDataThread()中执行EpollOperationEvent()失败。");
                }
                continue;
            }
            else
            {
                pmemory->FreeMemory(pconnsockinfo->psendalldataforfree);
                pconnsockinfo->psendalldataforfree = nullptr;
                pconnsockinfo->psenddata = nullptr;
                pconnsockinfo->senddatalen = 0;
                pconnsockinfo->throwepollsendcount = 0;
                continue;
            }

        } //end while (true)

    } //end while (!g_isquit)
    return nullptr;
}

int XMNSocket::MsgSend(XMNConnSockInfo *pconnsockinfo)
{
    size_t n = 0;
    while (true)
    {
        n = send(pconnsockinfo->fd, pconnsockinfo->psenddata, pconnsockinfo->senddatalen);
        /**
         * （1）成功地发送了数据。
        */
        if (n > 0)
        {
            return n;
        }
        /**
         * （2）异常处理。
        */
        else if (n == 0)
        {
            /**
             * a、发送的数据量为0，对于本网络库该可能性理论上不存在。
             * b、发送超时，对端已经关闭连接。
            */
            return 0;
        }
        if (errno == EAGAIN)
        {
            return -1;
        }
        else if (errno == EINTR)
        {
            /**
             * send()被中断打断，下次再次运行试一下。
            */
            xmn_log_stderr(0, "XMNSocket::MsgSend()中send()运行被中断打断。");
        }
        else
        {
            return -2;
        }
    }
    return 0;
}

int XMNSocket::FreeSendDataQueue()
{
    XMNMemory *pmemory = SingletonBase<XMNMemory>::GetInstance();
    char *ptmp = nullptr;
    while (!senddata_queue_.empty())
    {
        ptmp = senddata_queue_.front();
        senddata_queue_.pop_front();
        pmemory->FreeMemory(ptmp);
    }
    return 0;
}