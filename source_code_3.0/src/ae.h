/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __AE_H__
#define __AE_H__

#define AE_OK 0
#define AE_ERR -1

#define AE_NONE 0
#define AE_READABLE 1
#define AE_WRITABLE 2

#define AE_FILE_EVENTS 1
#define AE_TIME_EVENTS 2
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT 4

#define AE_NOMORE -1

/* Macros */
#define AE_NOTUSED(V) ((void) V)

struct aeEventLoop;

/* Types and data structures */
//IO事件就绪回调函数
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
//时间事件就绪回调函数
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
//事件事件删除回调函数
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
//就绪事件处理前执行的函数
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* File event structure */
//io事件结构
typedef struct aeFileEvent {
    int mask; /* one of AE_(READABLE|WRITABLE) */ //监听事件类型掩码
    aeFileProc *rfileProc;  //读事件回调函数
    aeFileProc *wfileProc;  //写事件回调函数
    void *clientData;  //回调函数参数
} aeFileEvent;

/* Time event structure */
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

/* A fired event */
//已就绪io事件
typedef struct aeFiredEvent {
    int fd;   //已就绪文件描述符
    int mask; //事件类型掩码
} aeFiredEvent;

/* State of an event based program */
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
    //后端io多路复用库的私有数据，用于记录事件状态
    void *apidata; /* This is used for polling API specific data */
    //在处理事件前要执行的函数
    aeBeforeSleepProc *beforesleep;
} aeEventLoop;

/* Prototypes */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);

#endif
