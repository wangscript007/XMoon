#include "xmn_global.h"

int XMNProcessEventsTimers()
{
    /**
     * （1）处理网络事件。
    */
    g_socket.EpollProcessEvents(-1);

    /**
     * （2）处理定时器事件。
    */
    /**
     * TODO：后续补充。
    */
    return 0;
}