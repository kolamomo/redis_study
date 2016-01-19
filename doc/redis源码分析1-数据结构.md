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

#### 插入数据
```
//将数据插入到hash表中
int dictAdd(dict *d, void *key, void *val)
{
    dictEntry *entry = dictAddRaw(d,key); //将key插入到hash表中

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val); //设置value
    return DICT_OK;
}
```

```
//将数据插入到hash表中
//如果key已存在，返回NULL
//如果key不存在，则创建新的hash表节点，并插入到hash表中
dictEntry *dictAddRaw(dict *d, void *key)
{
    int index;
    dictEntry *entry;
    dictht *ht;

    //判断字典是否正在进行rehash，如果正在进行，则进行一次单步的rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    //计算key在hash表中的索引，根据需要进行hash表的扩容
    if ((index = _dictKeyIndex(d, key)) == -1)
        return NULL;

    //判断hash表是否正在rehash的过程中，只要hash表在rehash的过程中，就将数据插入到ht[1]中
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    //为元素分配空间
    entry = zmalloc(sizeof(*entry));
    //将结点插入hash表对应桶的表头
    entry->next = ht->table[index];
    ht->table[index] = entry;
    //hash表已存储元素数加1
    ht->used++;

    //设置新节点的键
    dictSetKey(d, entry, key);
    //返回新插入节点的指针
    return entry;
}
```

```
static int _dictKeyIndex(dict *d, const void *key)
{
    unsigned int h, idx, table;
    dictEntry *he;

    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    h = dictHashKey(d, key);
    //这里要分别查询两个hash表，检查key是否已经存在
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
            //如果key已存在，则返回-1
            if (dictCompareKeys(d, key, he->key))
                return -1;
            he = he->next;
        }
        //如果hash表没有处于rehash的过程中，说明ht[1]没有被使用，则不需要再查询ht[1]
        if (!dictIsRehashing(d)) break;
    }
    return idx;
}
```

#### 删除元素

```
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0);
}
```

```
//查找并删除节点
static int dictGenericDelete(dict *d, const void *key, int nofree)
{
    unsigned int h, idx;
    dictEntry *he, *prevHe;
    int table;

    if (d->ht[0].size == 0) return DICT_ERR;
    //如果hash表正处于rehash的过程中，则进行一次单步的rehash操作
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);  //计算节点的hash_key

    //分别在两个hash表中进行查找
    for (table = 0; table <= 1; table++) {
        //确定节点在hash表中对应的桶位置
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        prevHe = NULL;
        //遍历所在桶的链表进行查找
        while(he) {
            //找到了key对应的结点
            if (dictCompareKeys(d, key, he->key)) {
                //从链表中删除节点
                if (prevHe)
                    prevHe->next = he->next;  //删除非表头结点
                else
                    d->ht[table].table[idx] = he->next;  //删除表头结点
                //如果nofree为0，则还需要调用节点的相关函数进行清理操作
                if (!nofree) {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                }
                zfree(he);
                //hash表存储计数减1
                d->ht[table].used--;
                return DICT_OK;
            }
            prevHe = he;
            he = he->next;
        }
        //如果没有处于rehash的过程中，说明ht[1]没有使用，则不再查找ht[1]
        if (!dictIsRehashing(d)) break;
    }
    return DICT_ERR; /* not found */
}
```

#### 查找元素

```
//在hash表中根据key值查找结点
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    unsigned int h, idx, table;

    if (d->ht[0].size == 0) return NULL; /* We don't have a table at all */
    if (dictIsRehashing(d)) _dictRehashStep(d);
    h = dictHashKey(d, key);  
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;
        he = d->ht[table].table[idx];
        while(he) {
            if (dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}
```

#### rehash

hash表的rehash操作分为两步：  
1. 扩容  
2. rehash

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

```
//根据需要决定是否扩展hash表
static int _dictExpandIfNeeded(dict *d)
{
    //如果正在rehash的过程中，则直接返回
    if (dictIsRehashing(d)) return DICT_OK;

    //如果hash表ht[0]大小为空，则初始化hash表ht[0]
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    //判断hash表扩展的条件
    //当dict_can_resize设置为1时，hash表中已存储元素数与hash表大小之比为1时需要进行扩展
    //当dict_can_resize设置为0时，hash表中已存储元素数与hash表大小之比为5时需要进行扩展
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {
        //进行hash表的扩容，扩容的大小为当前hash表大小的两倍
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}
```

```
//将hash表的大小扩展为传入参数size的大小
int dictExpand(dict *d, unsigned long size)
{
    dictht n;
    unsigned long realsize = _dictNextPower(size);

    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    if (realsize == d->ht[0].size) return DICT_ERR;

    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = 0;

    //如果主hash表没有初始化，则初始化主hash表，然后直接返回
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    //如果主hash表非空，则设置hash表ht[1]的空间，并将rehash索引初始化为0
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}
```

### 跳跃表

跳表是链表的一种，它在链表的基础上增加了跳跃功能，使得在查找元素时，跳表能够提供O(log n)的时间复杂度。
节点定义见redis.h 实现见t_zset.c
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

ziplist是用一个字节数组来实现的双向链表结构，主要是节省了链表指针的存储，但是每次向链表增加元素都需要重新分配内存。

```
typedef struct zlentry {
    // prevrawlen：上一个节点的长度
    // prevrawlensize：编码 prevrawlen 所需的字节大小    
    unsigned int prevrawlensize, prevrawlen;
    // len ：当前节点的长度
    // lensize ：编码 len 所需的字节大小
    unsigned int lensize, len;
    // 当前节点 header 的大小, 等于 prevrawlensize + lensize
    unsigned int headersize;
    // 当前节点值所使用的编码类型
    unsigned char encoding;
    //指向当前节点的指针
    unsigned char *p;
} zlentry;
```

```
// 定位到 ziplist 的 bytes 属性，该属性记录了整个 ziplist 所占用的内存字节数
// 用于取出 bytes 属性的现有值，或者为 bytes 属性赋予新值
#define ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))
// 定位到 ziplist 的 offset 属性，该属性记录了到达表尾节点的偏移量
// 用于取出 offset 属性的现有值，或者为 offset 属性赋予新值
#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))
// 定位到 ziplist 的 length 属性，该属性记录了 ziplist 包含的节点数量
// 用于取出 length 属性的现有值，或者为 length 属性赋予新值
#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
// 返回 ziplist 表头的大小
#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))
// 返回指向 ziplist 第一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)
// 返回指向 ziplist 最后一个节点（的起始位置）的指针
#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
// 返回指向 ziplist 末端 ZIP_END （的起始位置）的指针
#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)
```

**压缩列表的存储结构**

< zlbytes>< zltail>< zllen>< entry>< entry>< zlend>

Zlbytes：一个4字节的无符号整型，存储的是整个ziplist占用的字节数，用于重分配内存时使用。
Zltail：一个4字节的无符号整型，存储的是链表最后一个结点的偏移值，即链表开头地址+zltail即为最后一个结点的起始地址
Zllen：一个2字节的无符号整型，存储的是链表中存储的结点数，当这个值存储的是2字节无符号整型的最大值时，需要遍历链表获取链表的结点数
Entry：链表结点，链表结点的存储格式见结点存储结构
Zlend：占用1字节的链表的结尾符，值为255

**节点存储结构**

<上一个链表结点占用的长度><当前链表结点元素占用的长度><当前结点数据>

1 上一个链表结点占用的长度  
当长度数据小于254使用一个字节存储，该字节存储的数值就是该长度，  
当长度数据大于等于254时，使用5个字节存储，第一个字节的数值为254，表示接下来的4个字节才真正表示长度  

2 当前链表结点元素占用的长度
与上一个链表结点占用的长度不同，当前链表节点元素占用的长度除了记录长度外，还要记录当前结点数据的编码类型
第一个字节的前两位用于区分长度存储编码类型和数据编码类型，具体如下

* 字符串类型编码  
|00pppppp|  
长度小于等于63（2^6-1）字节的字符串，后6位用于存储字符串长度，长度与类型总共占用了1个字节  
|01pppppp|qqqqqqqq|  
长度小于等于16383（2^14-1）字节的字符串，后14位用于存储字符串长度，长度与类型总共占用了2个字节  
|10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt|   
长度大于等于16384字节的字符串，后4个字节用于存储字符串长度，长度与类型总共占用了5个字节  

* 整型编码  
|1100____|   
整型类型，后2个字节存储的值就是该整数  
|1101____|   
整型类型，后4个字节存储的值就是该整数  
|1110____|   
整型类型，后8个字节存储的值就是该整数  

#### 创建压缩列表
```
//创建一个空的压缩列表
unsigned char *ziplistNew(void) {
    //ZIPLIST_HEADER_SIZE 是 ziplist 表头的大小, 1 字节是表末端 ZIP_END 的大小
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;
    // 为空表（表头和表末端）分配空间
    unsigned char *zl = zmalloc(bytes);
    // 初始化表属性
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);
    ZIPLIST_LENGTH(zl) = 0;
    // 设置表末端
    zl[bytes-1] = ZIP_END;
    return zl;
}
```

#### 插入数据
```
//插入数据
//将长度为slen的字节数组s插入到zl中，where决定了插入的方向（表头or表尾）
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);
}
```

```
//插入数据，将s插入到p指针所指的位置
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen;
    unsigned int prevlensize, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; 
    zlentry tail;

    //找到p指针所指的当前节点的prevlen的大小
    if (p[0] != ZIP_END) {  //p指针指向的不是列表尾部
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
    } else {  //p指针指向列表尾部
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);
        if (ptail[0] != ZIP_END) {
            prevlen = zipRawEntryLength(ptail);
        }
    }

    //确定传入字符的编码
    //尝试将其转换为整数，如果成功，value将保存转换后的整数值，encoding为value对应的编码方式
    //reqlen保存的是当前节点值的长度
    if (zipTryEncoding(s,slen,&value,&encoding)) {
        reqlen = zipIntSize(encoding);
    } else {  //传入的是字符串
        reqlen = slen;
    }
    //计算编码前置节点的长度所需的大小
    reqlen += zipPrevEncodeLength(NULL,prevlen);
    //计算编码当前节点的长度所需的大小
    reqlen += zipEncodeLength(NULL,encoding,slen);

    //如果插入的位置不在尾部的话，需要检查后一个节点的header能否编码插入节点的长度
    //nextdiff不为0，则需要对后置节点进行扩容
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    //realloc操作可能会改变zl指针指向的地址，所以这里要记录p指针相对zl的偏移量，以便还原p指针的位置
    offset = p-zl;
    //对压缩列表空间进行扩容
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);
    p = zl+offset;

    if (p[0] != ZIP_END) {
        //移动现有元素，为插入的节点腾出位置
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);

        //将新节点的长度编码至后置节点
        zipPrevEncodeLength(p+reqlen,reqlen);

        //更新列表到达表尾的偏移量
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        tail = zipEntry(p+reqlen);
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    } else {
        //新元素是表尾节点
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    //nextdiff不为0，需要更新后置节点header的长度
    if (nextdiff != 0) {
        offset = p-zl;
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    //将前置节点的长度写入新节点的header
    p += zipPrevEncodeLength(p,prevlen);
    //将当前节点元素的长度写入节点的header
    p += zipEncodeLength(p,encoding,slen);
    //写入节点值
    if (ZIP_IS_STR(encoding)) {  //写入字符串
        memcpy(p,s,slen);
    } else {  //写入整数
        zipSaveInteger(p,value,encoding);
    }
    //更新列表中节点数量
    ZIPLIST_INCR_LENGTH(zl,1);
    return zl;
}
```

#### 删除数据

```
//删除*p所指向的节点
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    //删除过程中会进行内存充分配，可能改变zl的地址，所以需要记录偏移量，以计算删除后*p的位置
    size_t offset = *p-zl;
    zl = __ziplistDelete(zl,*p,1);

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset;
    return zl;
}
```

```
//删除从p指向位置开始的num个节点
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    //记录p指向的节点first（删除的起始节点）
    first = zipEntry(p);
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        //计算p指向节点占用的总字节数，将p移至下一节点
        p += zipRawEntryLength(p);
        deleted++;
    }

    totlen = p-first.p;  //totlen为删除的总字节数
    if (totlen > 0) {
        //如果删除节点的后置还有节点
        if (p[0] != ZIP_END) {
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            p -= nextdiff;
            zipPrevEncodeLength(p,first.prevrawlen);

            //更新到表尾的偏移量
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            tail = zipEntry(p);
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =
                   intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            //将后置节点位置向前移动，覆盖删除节点的数据
            memmove(first.p,p,
                intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {  //删除节点的后置为空
            //更新到表尾的偏移量
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        offset = first.p-zl;
        //对压缩列表的空间进行缩容，并在结尾添加标记字符ZIP_END
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);
        ZIPLIST_INCR_LENGTH(zl,-deleted);
        p = zl+offset;

        //如果nextdiff不为0，更新p指向后置节点的大小
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl,p);
    }
    return zl;
}
```

```
//计算p指向节点所占用的字节数
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
    return prevlensize + lensize + len;
}
```

#### 查找数据

```
//查找节点值和str相等的列表节点，skip为每次比较跳过的节点数
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    while (p[0] != ZIP_END) {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        ZIP_DECODE_PREVLENSIZE(p, prevlensize);
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);
        //q记录的是节点值的起始地址
        q = p + prevlensize + lensize;

        if (skipcnt == 0) {
            //对字符串类型的数据进行比较
            if (ZIP_IS_STR(encoding)) {  
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {
                    return p;
                }
            } else {  //对整数类型的数据进行比较
                if (vencoding == 0) {
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        vencoding = UCHAR_MAX;
                    }
                    assert(vencoding);
                }

                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding);
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            skipcnt = skip;
        } else {
            skipcnt--;
        }

        //移动指针，指向下一个节点
        p = q + len;
    }

    return NULL;
}
```

