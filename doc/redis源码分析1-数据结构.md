## 对象

```
//redis.c

typedef struct redisObject {
    unsigned type:4;      //类型
    unsigned encoding:4;  //编码
    unsigned lru:REDIS_LRU_BITS; 
    int refcount;
    void *ptr;            //指向底层数据结构的指针
} robj;
```

redis存储的对象共有5种类型，每种类型都具有多种不同的具体实现（编码）。

具体类型和编码如下：

1. REDIS_STRING 字节流对象
    * REDIS_ENCODING_INT 整数值
    * REDIS_ENCODING_EMBSTR 使用embstr编码的简单动态字符串实现的字节流对象
    * REDIS_ENCODING_RAW 原生字节流
2. REDIS_LIST 列表对象
    * REDIS_ENCODING_ZIPLIST
    * REDIS_ENCODING_LINKEDLIST
3. REDIS_HASH 哈希对象
    * REDIS_ENCODING_ZIPLIST
    * REDIS_ENCODING_HT
4. REDIS_SET 集合对象
    * REDIS_ENCODING_INTSET
    * REDIS_ENCODING_HT
5. REDIS_ZSET 有序结合对象
    * REDIS_ENCODING_ZIPLIST
    * REDIS_ENCODING_SKIPLIST

## 数据结构

### 简单动态字符串SDS

```
//redis.h

struct sdshdr {
    unsigned int len;   //字节流总长度
    unsigned int free;  //字节流未使用空间的大小
    char buf[];         //字节流数组
};

typedef char *sds;
```

sds初始化时会创建一个刚好可以存放所需字节流大小的空间。
```
//sds.c
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

sds sdsnewlen(const void *init, size_t initlen) {
    struct sdshdr *sh;

    if (init) {
        sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
    } else {
        sh = zcalloc(sizeof(struct sdshdr)+initlen+1);
    }
    if (sh == NULL) return NULL;
    sh->len = initlen;
    sh->free = 0;
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    sh->buf[initlen] = '\0';
    return (char*)sh->buf;
}

```

### 双向链表list

```
//adlist.h

typedef struct listNode {  //声明链表节点
    struct listNode *prev; //前一个节点的指针
    struct listNode *next; //后一个节点的指针
    void *value;  //节点的值
} listNode;

typedef struct listIter { //声明链表迭代器
    listNode *next;   //下一个节点的指针
    int direction;    //迭代方向
} listIter;

typedef struct list {  //声明链表
    listNode *head;  //链表头节点
    listNode *tail;  //链表尾节点
    void *(*dup)(void *ptr);   //复制节点的值
    void (*free)(void *ptr);   //释放节点的值
    int (*match)(void *ptr, void *key);  //比较节点的值与传入的值是否相等
    unsigned long len;
} list;
```

### hash表

```
//dic.h

//声明字典
typedef struct dict {
    dictType *type;  //根据字典的不同用途，对应特定的函数（dictType定义了一组函数指针）
    void *privdata;  //私有数据
    dictht ht[2];    //hash表，字典里包含两个hash表，一个存储数据，一个用于扩容rehash
    //rehash索引，当rehash结束时，rehashidx设为-1
    long rehashidx; /* rehashing not in progress if rehashidx == -1 */
    //正在运行的安全迭代起个数
    int iterators; /* number of iterators currently running */
} dict;

//声明hash表
typedef struct dictht {
    dictEntry **table;   //hash表数组
    unsigned long size;  //hash表大小
    unsigned long sizemask;  //hash表掩码，用于计算索引值
    unsigned long used;  //hash表已有节点数量
} dictht;

//声明hash表节点
typedef struct dictEntry {
    void *key;   //键
    union {   //值
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;
    struct dictEntry *next;  //指向同一hash值的下一个节点的指针
} dictEntry;

//hash表迭代器
typedef struct dictIterator {
    dict *d;  //被迭代的字典
    long index;  //迭代器指向的hash表的索引位置
    //table: 迭代字典中哪一个hash表，值为0或1
    //safe: 标识迭代器是否安全
    int table, safe;
    //entry: 当前指向的节点
    //nextEntry: 当前指向节点的下一个节点，在迭代时，entry所指向的节点可能会被修改，所以这里需要一个指针保存下一个节点的指针
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;
} dictIterator;
```
