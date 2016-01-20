# 事件管理

redis没有像memcached一样，使用libevent等现成的网络库，而是自己实现了一套事件管理模型。  
redis中的事件有两种类型：io事件和时间事件。  

## 数据结构

事件管理的相关数据结构定义在ae.h中

```
//IO事件结构
typedef struct aeFileEvent {
    int mask; /* one of AE_(READABLE|WRITABLE) */ //监听事件类型掩码
    aeFileProc *rfileProc;  //读事件回调函数
    aeFileProc *wfileProc;  //写事件回调函数
    void *clientData;  //回调函数参数
} aeFileEvent;
```

```
//时间事件结构
typedef struct aeTimeEvent {
    long long id; /* time event identifier. */  //时间事件标识符
    //事件的到达时间
    long when_sec; /* seconds */      
    long when_ms; /* milliseconds */
    aeTimeProc *timeProc;  //事件回调函数
    aeEventFinalizerProc *finalizerProc;  //事件释放函数
    void *clientData;   //回调函数参数
    struct aeTimeEvent *next;  //指向下一个时间事件结构，形成链表
} aeTimeEvent;
```

```
//已就绪io事件
typedef struct aeFiredEvent {
    int fd;   //已就绪文件描述符
    int mask; //事件类型掩码
} aeFiredEvent;
```

```
//事件处理循环
typedef struct aeEventLoop {
    //当前已注册的最大描述符
    int maxfd;   /* highest file descriptor currently registered */
    //当前已追踪的最大描述符
    int setsize; /* max number of file descriptors tracked */
    //用于生成时间事件id
    long long timeEventNextId;
    //上一次执行时间事件的事件
    time_t lastTime;     /* Used to detect system clock skew */
    //已注册的io事件数组
    aeFileEvent *events; /* Registered events */
    //已就绪的io事件数组
    aeFiredEvent *fired; /* Fired events */
    //时间事件队列
    aeTimeEvent *timeEventHead;
    //事件处理器的开关
    int stop;
    //多路复用库的私有数据
    void *apidata; /* This is used for polling API specific data */
    //在处理事件前要执行的函数
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;
```

事件相关处理函数定义：

```
//IO事件就绪回调函数
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
//时间事件就绪回调函数
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
//事件事件删除回调函数
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
//就绪事件处理前执行的函数
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);
```

## IO多路复用

这里以epoll为例，介绍redis中实现的io多路复用。(ae_epoll.c)

aeApiState 用于记录一个epoll实例所包含的事件队列。

```
//事件状态
typedef struct aeApiState {
    int epfd;   //epoll 实例对应的fd
    struct epoll_event *events;  //事件队列
} aeApiState;
```

#### 创建epoll实例

```
//创建epoll实例，并将其赋值给eventLoop
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    //为state的事件队列申请空间
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    //创建epoll实例
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    //将事件状态赋给eventLoop
    eventLoop->apidata = state;
    return 0;
}
```

#### 添加事件

```
//添加事件到epoll中
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee;
    //如果fd没有关联任何事件，这是一个add操作；如果关联了事件，这是一个mod操作
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    //设置要注册事件的fd和类型
    ee.events = 0;
    mask |= eventLoop->events[fd].mask;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0;
    ee.data.fd = fd;
    //调用epoll的事件注册函数注册事件
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}
```

#### 获取就绪事件

```
//获取就绪事件
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    //等待就绪事件的发生，如果没有设置timeout时间，则一直等待下去
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        //为就绪事件设置相应的类型，并加入到eventLoop的就绪事件数组中
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    //返回已就绪事件的个数
    return numevents;
}
```

## 事件循环

#### 创建事件循环

```
//创建事件处理器循环
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    //为事件循环结构体及相关属性申请空间
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    //设置数组大小
    eventLoop->setsize = setsize;
    //初始化上一次执行的时间
    eventLoop->lastTime = time(NULL);
    //初始化时间事件结构
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;
    eventLoop->beforesleep = NULL;
    //创建后端io多路复用服务实例
    if (aeApiCreate(eventLoop) == -1) goto err;
    //初始化监听事件
    for (i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;
    //返回事件循环
    return eventLoop;

err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}
```

#### 添加IO事件

```
//创建io事件，并加入事件循环
//监听fd文件，当fd可用时，执行回调函数proc
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }
    //取出IO事件结构
    aeFileEvent *fe = &eventLoop->events[fd];

    //调用底层io多路复用服务监听fd上的指定事件
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    //根据事件类型，设置相应的回调函数及参数
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData;
    //如有需要更新事件处理器的最大fd
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}
```

#### 添加时间事件

```
//添加时间事件
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    //更新事件事件计数器
    long long id = eventLoop->timeEventNextId++;
    //创建时间事件结构并申请空间
    aeTimeEvent *te;
    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    //设定处理事件的时间
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    //设定事件事件的回调函数及参数
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    //将新的事件插入到队列头
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}
```

#### 事件处理主循环

```
//事件处理主循环
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        //如果定义了事件处理前执行的函数，那么先执行这个函数
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
        //进行一次就绪事件的处理
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}
```

```
//等待就绪事件的发生，并执行事件的回调函数
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        //如果设置了时间事件，那么获取最近的时间事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            //如果存在时间事件，那么根据最近可执行时间事件和现在事件的时间差决定io多路复用的阻塞时间
            long now_sec, now_ms;

            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            //时间差小于0，说明时间事件已经就绪，重置tvp=0，不等待io事件的发生，先执行就绪的时间事件
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {
            //不存在时间事件，根据flag是否设置了AE_DONT_WAIT决定是否阻塞
            if (flags & AE_DONT_WAIT) {  //设置IO事件不阻塞
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {  //IO事件一直阻塞直到有就绪事件发生
                tvp = NULL; /* wait forever */
            }
        }

        //调用IO多路复用，等待就绪事件的发生
        numevents = aeApiPoll(eventLoop, tvp);
        //遍历各个fd，获取已就绪的fd，并执行相应的回调函数
        for (j = 0; j < numevents; j++) {
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            int fd = eventLoop->fired[j].fd;
            int rfired = 0;

            if (fe->mask & mask & AE_READABLE) {  //读事件
                rfired = 1; //确保读/写事件只能执行一个
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
            if (fe->mask & mask & AE_WRITABLE) {  //写事件
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            processed++;
        }
    }
    //如果设置了时间事件，在处理完IO事件后，再处理时间事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* return the number of processed file/time events */
}
```

```
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);  //获取当前事件

    //校对时间
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }
    //更新上一次处理时间事件的时间
    eventLoop->lastTime = now;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    //遍历时间事件列表，找出已就绪的时间事件，并执行回调函数
    while(te) {
        long now_sec, now_ms;
        long long id;

        //跳过无效时间事件
        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        //如果当前事件大于或等于事件的设定时间，说明时间事件已就绪，执行事件
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id
            //执行时间事件的回调函数
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;  //事件计数加1

            //时间事件分为一次性定时事件和周期性事件
            //对于周期性事件，更新下一次就绪的设定事件
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {  //对于一次性定时事件，从时间事件中删除
                aeDeleteTimeEvent(eventLoop, id);
            }
            //执行完时间事件后，对于一次性事件，将从队列中删除，所以这里要将te重置到队列头继续遍历
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
    //返回已处理的时间事件数
    return processed;
}
```
