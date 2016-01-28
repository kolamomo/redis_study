# 请求处理

## 入口

```
int main(int argc, char **argv) {
	...
	
    //初始化server的相关配置
    initServerConfig();

    //解析用户启动时指定的配置项
    if (argc >= 2) {
        ...
        //重置保存条件
        resetServerSaveParams();
        //载入配置文件
        loadServerConfig(configfile,options);
        sdsfree(options);
    } else {
        redisLog(REDIS_WARNING, "Warning");
    }
    //设置守护进程
    if (server.daemonize) daemonize();
    //初始化服务器
    initServer();
    //如果服务器是守护进程，创建pid文件
    if (server.daemonize) createPidFile();
    //为服务器进程设置名称
    redisSetProcTitle(argv[0]);
    redisAsciiArt();
    checkTcpBacklogSettings();

    if (server.maxmemory > 0 && server.maxmemory < 1024*1024) {
        redisLog(REDIS_WARNING,"WARNING);
    }

    aeSetBeforeSleepProc(server.el,beforeSleep);
    //运行事件处理循环
    aeMain(server.el);
    //服务器关闭，停止事件循环
    aeDeleteEventLoop(server.el);
    return 0;
}
```

```
void initServer(void) {
    int j;

    //设置信号处理函数
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    //初始化server的相关属性
    server.pid = getpid();
    server.current_client = NULL;
    server.clients = listCreate();
    ...

    //开启事件循环
    server.el = aeCreateEventLoop(server.maxclients+REDIS_EVENTLOOP_FDSET_INCR);
    //为db申请原始空间
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    //监听端口，等待客户端请求
    if (server.port != 0 &&
        listenToPort(server.port,server.ipfd,&server.ipfd_count) == REDIS_ERR)
        exit(1);
	
	...
	
    //创建并初始化数据库结构
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        ...
    }
    
    ...
    //为serverCron()创建时间事件
    if(aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        exit(1);
    }

    //为tcp连接创建io事件
    //用于相应客户端的connect()请求
    for (j = 0; j < server.ipfd_count; j++) {
        if (aeCreateFileEvent(server.el, server.ipfd[j], AE_READABLE,
            acceptTcpHandler,NULL) == AE_ERR)
            {
                redisPanic(
                    "Unrecoverable error creating server.ipfd file event.");
            }
    }
    //为本地socket创建io事件
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) redisPanic("Unrecoverable error creating server.sofd file event.");

	...
}
```

## 监听连接

```
//监听连接
int listenToPort(int port, int *fds, int *count) {
    int j;

    if (server.bindaddr_count == 0) server.bindaddr[0] = NULL;
    for (j = 0; j < server.bindaddr_count || j == 0; j++) {
        if (server.bindaddr[j] == NULL) {  //同时监听ipv4和ipv6
            fds[*count] = anetTcp6Server(server.neterr,port,NULL,
                server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            }
            fds[*count] = anetTcpServer(server.neterr,port,NULL,
                server.tcp_backlog);
            if (fds[*count] != ANET_ERR) {
                anetNonBlock(NULL,fds[*count]);
                (*count)++;
            }
            if (*count) break;
        } else if (strchr(server.bindaddr[j],':')) {  //监听ipv6
            fds[*count] = anetTcp6Server(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        } else {  //监听ipv4
            fds[*count] = anetTcpServer(server.neterr,port,server.bindaddr[j],
                server.tcp_backlog);
        }
        if (fds[*count] == ANET_ERR) {
            return REDIS_ERR;
        }
        anetNonBlock(NULL,fds[*count]);  //设置为非阻塞模式
        (*count)++;
    }
    return REDIS_OK;
}
```

```
//创建socket，绑定地址并监听请求
static int _anetTcpServer(char *err, int port, char *bindaddr, int af, int backlog)
{
    int s, rv;
    char _port[6];  
    struct addrinfo hints, *servinfo, *p;

    memset(&hints,0,sizeof(hints));
    hints.ai_family = af;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* No effect if bindaddr != NULL */

    //获取ip地址，完成主机名到地址的解析，结果保存在servinfo链表中
    if ((rv = getaddrinfo(bindaddr,_port,&hints,&servinfo)) != 0) {
        return ANET_ERR;
    }
    for (p = servinfo; p != NULL; p = p->ai_next) {
        //创建socket
        if ((s = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1)
            continue;

        if (af == AF_INET6 && anetV6Only(err,s) == ANET_ERR) goto error;
        //设置可重用端口
        if (anetSetReuseAddr(err,s) == ANET_ERR) goto error;
        //监听端口
        if (anetListen(err,s,p->ai_addr,p->ai_addrlen,backlog) == ANET_ERR) goto error;
        goto end;
    }
    if (p == NULL) {
        goto error;
    }

error:
    s = ANET_ERR;
end:
    freeaddrinfo(servinfo);
    return s;
}
```

```
//为socket绑定地址，监听端口
static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len, int backlog) {
    //将socket与ip地址和端口进行绑定
    if (bind(s,sa,len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }

    //socket监听请求
    if (listen(s, backlog) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return ANET_OK;
}
```

## 接收连接

```
//接收客户端连接请求
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[REDIS_IP_STR_LEN];

    while(max--) {
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            return;
        }
        redisLog(REDIS_VERBOSE,"Accepted %s:%d", cip, cport);
        acceptCommonHandler(cfd,0);
    }
}

```

```
//接收tcp连接，返回客户端的socket_fd
int anetTcpAccept(char *err, int s, char *ip, size_t ip_len, int *port) {
    int fd;
    struct sockaddr_storage sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == -1)
        return ANET_ERR;

    if (sa.ss_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)&sa;
        if (ip) inet_ntop(AF_INET,(void*)&(s->sin_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin_port);
    } else {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&sa;
        if (ip) inet_ntop(AF_INET6,(void*)&(s->sin6_addr),ip,ip_len);
        if (port) *port = ntohs(s->sin6_port);
    }
    return fd;
}
```

```
static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len) {
    int fd;
    while(1) {
        fd = accept(s,sa,len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            else {
                return ANET_ERR;
            }
        }
        break;
    }
    return fd;
}
```

```
//接收连接请求处理器
static void acceptCommonHandler(int fd, int flags) {
    redisClient *c;
    //创建客户端连接对象
    if ((c = createClient(fd)) == NULL) {
        close(fd); 
        return;
    }
    //如果客户端超过了服务器设置的最大客户端数量，则向客户端写入错误信息，并关闭客户端
    if (listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        if (write(c->fd,err,strlen(err)) == -1) {
        }
        server.stat_rejected_conn++; //拒绝连接数加1
        freeClient(c);
        return;
    }
    server.stat_numconnections++;  //连接次数加1
    c->flags |= flags;
}
```

```
//创建一个新的客户端连接对象
redisClient *createClient(int fd) {
    redisClient *c = zmalloc(sizeof(redisClient));

    if (fd != -1) {
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if (server.tcpkeepalive)
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        //创建io事件，接收客户端发送的命令
        if (aeCreateFileEvent(server.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    //初始化客户端的属性
    selectDb(c,0); //选择默认数据库
    c->id = server.next_client_id++;  //客户端id
    c->fd = fd;  //客户端socket_fd
    c->name = NULL;  
    ...
    //如果socket_fd不为-1，将其添加到服务器的客户端链表中
    if (fd != -1) listAddNodeTail(server.clients,c);
    //初始化客户端的事务状态
    initClientMultiState(c);
    return c;
}
```

## 接收命令

```
//接收客户端发送的命令
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = (redisClient*) privdata;
    int nread, readlen;
    size_t qblen;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    server.current_client = c;  //设置服务器的当前客户端
    readlen = REDIS_IOBUF_LEN;
    if (c->reqtype == REDIS_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= REDIS_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    //获取当前查询缓冲区的长度
    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    //读取请求数据到查询缓存
    nread = read(fd, c->querybuf+qblen, readlen);
    if (nread == -1) {   //读入出错
        if (errno == EAGAIN) {
            nread = 0;
        } else {
            redisLog(REDIS_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    } else if (nread == 0) {  //遇到EOF
        redisLog(REDIS_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    //根据内容，更新查询缓冲区（SDS）的free和len属性
    if (nread) {
        sdsIncrLen(c->querybuf,nread);
        c->lastinteraction = server.unixtime;
        if (c->flags & REDIS_MASTER) c->reploff += nread;
        server.stat_net_input_bytes += nread;
    } else {
        server.current_client = NULL;
        return;
    }
    //查询缓冲区长度超出服务器最大缓冲区长度
    //清空缓冲区并释放客户端
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }
    //从查询缓存中读取内容，创建参数，并执行命令
    processInputBuffer(c);
    server.current_client = NULL;
}
```

```
//处理客户端输入的命令内容
void processInputBuffer(redisClient *c) {
    //尽可能的处理查询缓冲区的内容
    //如果出现short read，那么会有一部分内容不构成一个完整的命令，等待下次读取时在进行处理
    while(sdslen(c->querybuf)) {
        //判断客户端状态，如果客户端处于暂停，阻塞，关闭状态，则直接返回
        if (!(c->flags & REDIS_SLAVE) && clientsArePaused()) return;

        if (c->flags & REDIS_BLOCKED) return;

        if (c->flags & REDIS_CLOSE_AFTER_REPLY) return;

        //解析命令的协议
        //Redis支持两种协议，一种是inline，一种是multibulk。inline协议是老协议，现在一般只在命令行下的redis客户端使用，其他情况一般是使用multibulk协议。
        //如果客户端传送的数据的第一个字符时‘*’，那么传送数据将被当做multibulk协议处理，否则将被当做inline协议处理。
        if (!c->reqtype) {
            if (c->querybuf[0] == '*') {
                c->reqtype = REDIS_REQ_MULTIBULK;
            } else {
                c->reqtype = REDIS_REQ_INLINE;
            }
        }

        if (c->reqtype == REDIS_REQ_INLINE) {
            if (processInlineBuffer(c) != REDIS_OK) break;
        } else if (c->reqtype == REDIS_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != REDIS_OK) break;
        } else {
            redisPanic("Unknown request type");
        }

        if (c->argc == 0) {
            resetClient(c);
        } else {
            if (processCommand(c) == REDIS_OK)
                resetClient(c);
        }
    }
}
```

## 解析命令

```
//解析inline协议的命令，并创建命令的参数对象
int processInlineBuffer(redisClient *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    //读取一行数据
    newline = strchr(c->querybuf,'\n');

    //内容为空
    if (newline == NULL) {
        return REDIS_ERR;
    }

    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    //根据空格分割命令参数
    //eg: SET name lalala \r\n
    //arg[0]=SET arg[1]=name arg[2]=lalala argc=3
    querylen = newline-(c->querybuf);
    aux = sdsnewlen(c->querybuf,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError(c,0);
        return REDIS_ERR;
    }

    if (querylen == 0 && c->flags & REDIS_SLAVE)
        c->repl_ack_time = server.unixtime;

    //从缓冲区中删除已读取的内容
    sdsrange(c->querybuf,querylen+2,-1);

    //为客户端参数分配空间
    if (argc) {
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*argc);
    }

    //将解析出来的参数设置到客户端的参数属性中
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(REDIS_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return REDIS_OK;
}
```

```
//解析multibulk协议的命令
//eg:  *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nlalala\r\n
int processMultibulkBuffer(redisClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    //读取命令的参数个数
    if (c->multibulklen == 0) {
        redisAssertWithInfo(c,NULL,c->argc == 0);

        //读取新的一行
        newline = strchr(c->querybuf,'\r');
        if (newline == NULL) {
            return REDIS_ERR;
        }

        if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return REDIS_ERR;

        //协议的第一个字符必须是’*‘
        redisAssertWithInfo(c,NULL,c->querybuf[0] == '*');
        //将第一行的数据从第二个字符开始到结束转换为整数，即为命令的参数个数
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        //转换失败或参数数量超限
        if (!ok || ll > 1024*1024) {
            return REDIS_ERR;
        }

        //定位到下一行
        pos = (newline-c->querybuf)+2;
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return REDIS_OK;
        }

        c->multibulklen = ll;

        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }

    //依次解析每个参数
    //第一行为参数字符数，第二行为具体参数
    while(c->multibulklen) {
        //读取参数字符数
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > REDIS_INLINE_MAX_SIZE) {
                    return REDIS_ERR;
                }
                break;
            }

            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            //参数字符数这一行第一个字符必须是'$'
            if (c->querybuf[pos] != '$') {
                return REDIS_ERR;
            }

            //将从第二个字符至这一行结束的字符串转换为整数
            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                return REDIS_ERR;
            }

            //定位到下一行
            pos += newline-(c->querybuf+pos)+2;
            //如果参数很长，那么做一些预备措施来优化接下来读取参数的操作
            if (ll >= REDIS_MBULK_BIG_ARG) {
                size_t qblen;
                sdsrange(c->querybuf,pos,-1);
                pos = 0;
                qblen = sdslen(c->querybuf);
                if (qblen < (size_t)ll+2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
            }
            c->bulklen = ll;
        }

        //读入参数
        //确保内容符合协议格式
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            break;
        } else {
            //为参数创建字符串对象
            if (pos == 0 &&
                c->bulklen >= REDIS_MBULK_BIG_ARG &&
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(REDIS_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                c->querybuf = sdsempty();
                c->querybuf = sdsMakeRoomFor(c->querybuf,c->bulklen+2);
                pos = 0;
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }
            //清空参数字符数
            c->bulklen = -1;
            //还需读取的参数个数减1
            c->multibulklen--;
        }
    }

    //删除已读取的内容
    if (pos) sdsrange(c->querybuf,pos,-1);

    //如果命令的所有参数都已读取完，返回OK，否则返回错误
    if (c->multibulklen == 0) return REDIS_OK;
    return REDIS_ERR;
}
```

## 处理命令

processCommand() 这里主要做三件事：  
1. 检查命令的合法性  
2. 根据服务器的状态和模式以及命令的类型进行一些权限校验和预处理 
3. 执行命令

```
//处理客户端的命令
int processCommand(redisClient *c) {
	...
	
    //查找命令，并进行命令合法性检查，以及命令参数个数检查
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {  //没有找到指定的命令
        flagTransaction(c);
        addReplyErrorFormat(c,"unknown command '%s'",
            (char*)c->argv[0]->ptr);
        return REDIS_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {  //参数个数错误
        flagTransaction(c);
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return REDIS_OK;
    }

    //检查认证信息
    if (server.requirepass && !c->authenticated && c->cmd->proc != authCommand)
    {
        flagTransaction(c);
        addReply(c,shared.noautherr);
        return REDIS_OK;
    }

	...
    //如果设置了最大内存，检查内存是否超过限制
    if (server.maxmemory) {
        //如果内存已超过限制，尝试通过删除过期键来释放内存
        int retval = freeMemoryIfNeeded();
        if (server.current_client == NULL) return REDIS_ERR;

        //如果即将执行的命令可能占用大量内存，并且前面内存释放失败的话，那么像客户端返回内存错误
        if ((c->cmd->flags & REDIS_CMD_DENYOOM) && retval == REDIS_ERR) {
            flagTransaction(c);
            addReply(c, shared.oomerr);
            return REDIS_OK;
        }
    }

    ...
    
    if (c->flags & REDIS_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        //事务上下文中，除EXEC，DISCARD，MULTI和WATCH命令之外，其他所有命令都会被入队到事务队列中
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
        //执行命令
        call(c,REDIS_CALL_FULL);
        c->woff = server.master_repl_offset;
        //处理解除了阻塞的键
        if (listLength(server.ready_keys))
            handleClientsBlockedOnLists();
    }
    return REDIS_OK;
}
```

```
//执行命令
void call(redisClient *c, int flags) {
	...

    //保留旧dirty计数器的值
    dirty = server.dirty;
    //记录命令开始执行的时间
    start = ustime();
    //执行命令
    c->cmd->proc(c);
    //计算耗费的时间
    duration = ustime()-start;
    //计算命令执行之后的dirty值
    dirty = server.dirty-dirty;
    if (dirty < 0) dirty = 0;
	
	...
	
    //如果有需要，将命令放到SHOWLOG里
    if (flags & REDIS_CALL_SLOWLOG && c->cmd->proc != execCommand) {
        char *latency_event = (c->cmd->flags & REDIS_CMD_FAST) ?
                              "fast-command" : "command";
        latencyAddSampleIfNeeded(latency_event,duration/1000);
        slowlogPushEntryIfNeeded(c->argv,c->argc,duration);
    }
    //更新命令的统计信息
    if (flags & REDIS_CALL_STATS) {
        c->cmd->microseconds += duration;
        c->cmd->calls++;
    }

    /* Propagate the command into the AOF and replication link */
    //将命令复制到AOF和Slave节点
    if (flags & REDIS_CALL_PROPAGATE) {
        int flags = REDIS_PROPAGATE_NONE;

        if (c->flags & REDIS_FORCE_REPL) flags |= REDIS_PROPAGATE_REPL;
        if (c->flags & REDIS_FORCE_AOF) flags |= REDIS_PROPAGATE_AOF;
        if (dirty)
            flags |= (REDIS_PROPAGATE_REPL | REDIS_PROPAGATE_AOF);
        if (flags != REDIS_PROPAGATE_NONE)
            propagate(c->cmd,c->db->id,c->argv,c->argc,flags);
    }

    ...
    //服务器执行的命令计数加1
    server.stat_numcommands++;
}
```

c->cmd->proc(c) 这里调用命令对应的方法来执行命令。  
先看一下调用链，以getCommand为例来看处理命令以及接下来的流程。

```
typedef struct redisClient {
    struct redisCommand *cmd, *lastcmd;
    ...
} redisClient;

struct redisCommand {
    char *name;
    redisCommandProc *proc;
    ...
};

typedef void redisCommandProc(redisClient *c);

struct redisCommand redisCommandTable[] = {
    {"get",getCommand,2,"rF",0,NULL,1,1,1,0,0},
    {"set",setCommand,-3,"wm",0,NULL,1,1,1,0,0},
    ...
}
```


```
//t_string.c

void getCommand(redisClient *c) {
    getGenericCommand(c);
}

int getGenericCommand(redisClient *c) {
    robj *o;

    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return REDIS_OK;

    if (o->type != REDIS_STRING) {
        addReply(c,shared.wrongtypeerr);
        return REDIS_ERR;
    } else {
        addReplyBulk(c,o);
        return REDIS_OK;
    }
}
```

## 写入返回数据

根据请求命令的协议，按照对应的协议写入返回数据。  

```
//写入文本协议的回复消息
void addReply(redisClient *c, robj *obj) {
    //为客户端添加io事件，准备写入回复消息
    if (prepareClientToWrite(c) != REDIS_OK) return;

    //写入字符串类型的回复消息
    if (sdsEncodedObject(obj)) {
        //尝试将内容复制到c->buf中，这样可以避免内存分配
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            //如果c->buf空间不够，就复制到c->reply链表中
            _addReplyObjectToList(c,obj);
    }
    //写入整数类型的回复消息
    else if (obj->encoding == REDIS_ENCODING_INT) {
        //如果c->buf中有大于等于32个字节的空间，将整数直接以字符串的形式复制到c->buf中
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;

            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            if (_addReplyToBuffer(c,buf,len) == REDIS_OK)
                return;
        }
        //如果整数长度大于32位，则将其转换为字符串，并写入c->reply链表中
        obj = getDecodedObject(obj);
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != REDIS_OK)
            _addReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        redisPanic("Wrong obj->encoding in addReply()");
    }
}
```

```
//写入bulk协议的回复消息
void addReplyBulk(redisClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}
```

```
//为客户端添加io事件，准备发送回复数据
int prepareClientToWrite(redisClient *c) {
    //如果时lua脚本的伪客户端，直接返回
    if (c->flags & REDIS_LUA_CLIENT) return REDIS_OK;

    //客户端时主服务器且不接受查询，则报错
    if ((c->flags & REDIS_MASTER) &&
        !(c->flags & REDIS_MASTER_FORCE_REPLY)) return REDIS_ERR;

    //无连接的伪客户端不可写
    if (c->fd <= 0) return REDIS_ERR;

    if (c->bufpos == 0 && listLength(c->reply) == 0 &&
        (c->replstate == REDIS_REPL_NONE ||
         (c->replstate == REDIS_REPL_ONLINE && !c->repl_put_online_on_ack)))
    {
        //添加io事件，准备向客户端发送数据
        if (aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
                sendReplyToClient, c) == AE_ERR)
        {
            freeClientAsync(c);
            return REDIS_ERR;
        }
    }

    return REDIS_OK;
}
```

```
//向客户端回复消息
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    redisClient *c = privdata;
    int nwritten = 0, totwritten = 0, objlen;
    size_t objmem;
    robj *o;
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(mask);

    //一直循环直到缓冲区为空
    while(c->bufpos > 0 || listLength(c->reply)) {
        //写入buf中的数据
        if (c->bufpos > 0) {
            //将buf中的内容写入socket
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            if (nwritten <= 0) break;
            //写入成功，更新计数器
            c->sentlen += nwritten;
            totwritten += nwritten;

            //如果缓冲区的内容全部写入完毕，清空两个计数器
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } 
        //写入回复队列中的数据
        else {
            //取出队列头结点
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = getStringObjectSdsUsedMemory(o);

            //略过空对象
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }

            //将内容写入到socket中
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            //如果缓冲队列内容全部写入完毕，清空队列
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        server.stat_net_output_bytes += totwritten;
        //为避免一个非常大的回复独占服务器，当陷入数据量大于REDIS_MAX_WRITE_PER_EVENT，临时中断写入
        //将处理时间让给其他客户端，剩下的内容等下次事件就绪再继续写入
        if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    }
    //写入出错检查
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            redisLog(REDIS_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return;
        }
    }
    if (totwritten > 0) {
        if (!(c->flags & REDIS_MASTER)) c->lastinteraction = server.unixtime;
    }
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;
        //写入完毕，删除io事件
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

        //如果指定写入之后关闭客户端flag，则关闭客户端
        if (c->flags & REDIS_CLOSE_AFTER_REPLY) freeClient(c);
    }
}
```