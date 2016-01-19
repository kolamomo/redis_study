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
    long long fingerprint;
} dictIterator;
```

#### rehash
```
//进行rehash操作
int dictRehash(dict *d, int n) {
    int empty_visits = n*10; 
    //判断rehash是否正在进行中
    if (!dictIsRehashing(d)) return 0;

    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        //找到当前需要进行rehash的桶位置，略过数据为空的桶
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        de = d->ht[0].table[d->rehashidx];
        //遍历hash表ht[0]当前桶中的每一个元素，将其放入放入hash表ht[1]中
        while(de) {
            unsigned int h;

            nextde = de->next;
            //获取当前元素的key在新的hash表中的位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            d->ht[0].used--;
            d->ht[1].used++;
            de = nextde;
        }
        //将hash表ht[0]当前桶的元素清空
        d->ht[0].table[d->rehashidx] = NULL;
        //将rehash的桶索引加1
        d->rehashidx++;
    }

    //检查是否完成了全部的rehash操作
    if (d->ht[0].used == 0) {
        //清空hash表ht[0],将hash表ht[1]设为ht[0],并重置hash表ht[1]
        zfree(d->ht[0].table);
        d->ht[0] = d->ht[1];
        _dictReset(&d->ht[1]);
        //将rehash索引设为-1，表示rehash操作全部完成
        d->rehashidx = -1;
        return 0;
    }

    return 1;
}
```

### 跳跃表

```
//redis.h

//跳跃表节点
typedef struct zskiplistNode {
    robj *obj;    //成员对象
    double score;  //分值
    struct zskiplistNode *backward; //后退指针
    struct zskiplistLevel {
        struct zskiplistNode *forward;  //前进指针
        unsigned int span;    //跨度
    } level[];
} zskiplistNode;

//跳跃表
typedef struct zskiplist {
    struct zskiplistNode *header, *tail; //头节点，尾节点指针
    unsigned long length;  //长度
    int level;    //层数
} zskiplist;
```

### 整数集合

```
//intset.h

typedef struct intset {
    uint32_t encoding;  //整数的编码类型，包括INTSET_ENC_INT16，INTSET_ENC_INT32，INTSET_ENC_INT64
    uint32_t length;    //整数集合的大小
    int8_t contents[];  //具体存放数据的数组
} intset;
```

#### 初始化
```
intset *intsetNew(void) {
    intset *is = zmalloc(sizeof(intset));
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);
    is->length = 0;
    return is;
}
```

#### 插入数据

```
//向整数集合中插入数据
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {
    uint8_t valenc = _intsetValueEncoding(value); //获取要插入数据的编码方式
    uint32_t pos;
    if (success) *success = 1;

    //如果要插入的数据编码方式大于当前整数集合的编码方式，需要先对整数集合进行升级，再插入数据
    if (valenc > intrev32ifbe(is->encoding)) {  
        return intsetUpgradeAndAdd(is,value);
    } else {
        //如果value已经存在于原集合中，则不执行插入，返回原集合
        if (intsetSearch(is,value,&pos)) {
            if (success) *success = 0;
            return is;
        }

        is = intsetResize(is,intrev32ifbe(is->length)+1);
        //如果pos位置不在集合尾部，则集合中pos位置开始元素整体后移一个位置
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);
    }

    //将value插入到pos位置上
    _intsetSet(is,pos,value);
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}
```

```
//对整数集合的编码方式进行升级
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) {
    uint8_t curenc = intrev32ifbe(is->encoding);
    uint8_t newenc = _intsetValueEncoding(value);
    int length = intrev32ifbe(is->length);
    //由value导致的升级，说明value要么比原集合中所有数都大，要么都小
    int prepend = value < 0 ? 1 : 0; 

    is->encoding = intrev32ifbe(newenc);
    is = intsetResize(is,intrev32ifbe(is->length)+1);

    //从尾部开始将整数集合中的数据插入到新的位置上
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    //插入新加入的整数
    if (prepend)
        _intsetSet(is,0,value);
    else
        _intsetSet(is,intrev32ifbe(is->length),value);
    //集合元素个数加1
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);
    return is;
}
```

```
//使用二分查找在整数集合中查找指定的value
//如果存在，返回1，并将其位置记录在pos中
//如果不存在，返回0，并将其应该插入的位置记录在pos中
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    if (intrev32ifbe(is->length) == 0) {
        if (pos) *pos = 0;
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > _intsetGet(is,intrev32ifbe(is->length)-1)) {
            if (pos) *pos = intrev32ifbe(is->length);
            return 0;
        } else if (value < _intsetGet(is,0)) {
            if (pos) *pos = 0;
            return 0;
        }
    }

    while(max >= min) {
        mid = ((unsigned int)min + (unsigned int)max) >> 1;
        cur = _intsetGet(is,mid);
        if (value > cur) {
            min = mid+1;
        } else if (value < cur) {
            max = mid-1;
        } else {
            break;
        }
    }

    if (value == cur) {
        if (pos) *pos = mid;
        return 1;
    } else {
        if (pos) *pos = min;
        return 0;
    }
}
```

```
//在指定位置插入元素
static void _intsetSet(intset *is, int pos, int64_t value) {
    uint32_t encoding = intrev32ifbe(is->encoding);

    if (encoding == INTSET_ENC_INT64) {
        ((int64_t*)is->contents)[pos] = value;
        memrev64ifbe(((int64_t*)is->contents)+pos);
    } else if (encoding == INTSET_ENC_INT32) {
        ((int32_t*)is->contents)[pos] = value;
        memrev32ifbe(((int32_t*)is->contents)+pos);
    } else {
        ((int16_t*)is->contents)[pos] = value;
        memrev16ifbe(((int16_t*)is->contents)+pos);
    }
}
```

#### 删除数据

插入数据可能导致集合数组的升级，但是删除数据不会导致集合数组的降级。

```
//从集合中删除数据
intset *intsetRemove(intset *is, int64_t value, int *success) {
    uint8_t valenc = _intsetValueEncoding(value);
    uint32_t pos;
    if (success) *success = 0;

    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) {
        uint32_t len = intrev32ifbe(is->length);

        if (success) *success = 1;

        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);
        is = intsetResize(is,len-1);
        is->length = intrev32ifbe(len-1);
    }
    return is;
}
```

#### 获取数据

查找某个指定位置pos的数据的方法如下：

```
//获取集合中指定位置的元素
//如果查找到，返回1，并将元素保存在指针value中
//如果未查找到，返回0
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {
    if (pos < intrev32ifbe(is->length)) {
        *value = _intsetGet(is,pos);
        return 1;
    }
    return 0;
}
```

```
static int64_t _intsetGet(intset *is, int pos) {
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
}

static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));
        memrev64ifbe(&v64);
        return v64;
    } else if (enc == INTSET_ENC_INT32) {
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));
        memrev32ifbe(&v32);
        return v32;
    } else {
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));
        memrev16ifbe(&v16);
        return v16;
    }
}
```

这里注意，在插入数据以及获取数据时，需要根据环境的大小端设定对数据进行转换，大小端转换的方法在endianconv.h endianconv.c中

```
#if (BYTE_ORDER == LITTLE_ENDIAN)
#define memrev32ifbe(p)
#define intrev32ifbe(v) (v)
...
#else
#define memrev32ifbe(p) memrev32(p)
#define intrev32ifbe(v) intrev32(v)
...
#endif
```

```
void memrev32(void *p) {
    unsigned char *x = p, t;

    t = x[0];
    x[0] = x[3];
    x[3] = t;
    t = x[1];
    x[1] = x[2];
    x[2] = t;
}
```

```
uint32_t intrev32(uint32_t v) {
    memrev32(&v);
    return v;
}
```
### 压缩列表