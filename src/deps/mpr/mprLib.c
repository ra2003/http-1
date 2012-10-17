/*
    mprLib.c -- Multithreaded Portable Runtime Library Source

    This file is a catenation of all the source code. Amalgamating into a
    single file makes embedding simpler and the resulting application faster.

    Prepared by: magnetar.local
 */

#include "mpr.h"

/************************************************************************/
/*
    Start of file "src/mprMem.c"
 */
/************************************************************************/

/**
    mprMem.c - Memory Allocator and Garbage Collector. 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/******************************* Local Defines ********************************/

#if BIT_HAS_MMU 
    #define VALLOC 1                /* Use virtual memory allocations */
#else
    #define VALLOC 0
#endif

/*
    Set this address to break when this address is allocated or freed
    Only used for debug, but defined regardless so we can have constant exports.
 */
PUBLIC MprMem *stopAlloc = 0;
PUBLIC int stopSeqno = -1;

#define GET_MEM(ptr)                ((MprMem*) (((char*) (ptr)) - sizeof(MprMem)))
#define GET_PTR(mp)                 ((char*) (((char*) mp) + sizeof(MprMem)))
#define GET_USIZE(mp)               ((ssize) (GET_SIZE(mp) - sizeof(MprMem) - (HAS_MANAGER(mp) * sizeof(void*))))
#define UNMARKED                    MPR_GEN_ETERNAL

#define GET_NEXT(mp)                (IS_LAST(mp)) ? NULL : ((MprMem*) ((char*) mp + GET_SIZE(mp)))
#define GET_REGION(mp)              ((MprRegion*) (((char*) mp) - MPR_ALLOC_ALIGN(sizeof(MprRegion))))

/*
    Macros to set and extract "prior" fields. All accesses (read and write) must be done locked.
        prior | last << 1 | hasManager
 */
#define GET_PRIOR(mp)               ((MprMem*) ((mp->field1 & MPR_MASK_PRIOR) >> MPR_SHIFT_PRIOR))
#define SET_PRIOR(mp, value)        mp->field1 = ((((size_t) value) << MPR_SHIFT_PRIOR) | (mp->field1 & ~MPR_MASK_PRIOR))
#define IS_LAST(mp)                 ((mp->field1 & MPR_MASK_LAST) >> MPR_SHIFT_LAST)
#define SET_LAST(mp, value)         mp->field1 = ((value << MPR_SHIFT_LAST) | (mp->field1 & ~MPR_MASK_LAST))
#define SET_FIELD1(mp, prior, last, hasManager) mp->field1 = (((size_t) prior) << MPR_SHIFT_PRIOR) | \
                                    ((last) << MPR_SHIFT_LAST) | ((hasManager) << MPR_SHIFT_HAS_MANAGER)

#define HAS_MANAGER(mp)             ((int) ((mp->field1 & MPR_MASK_HAS_MANAGER) >> MPR_SHIFT_HAS_MANAGER))
#define SET_HAS_MANAGER(mp, value)  mp->field1 = ((mp->field1 & ~MPR_MASK_HAS_MANAGER) | (value << MPR_SHIFT_HAS_MANAGER))

/*
    Macros to set and extract "size" fields. Accesses can be done unlocked. Updates must be done lock-free.
        gen/2 << 30 | free/1 << 29 | size/29 | mark/2
 */
#define GET_SIZE(mp)                ((ssize) ((mp->field2 & MPR_MASK_SIZE) >> MPR_SHIFT_SIZE))
#define SET_SIZE(mp, value)         mp->field2 = ((value) << MPR_SHIFT_SIZE) | (mp->field2 & ~MPR_MASK_SIZE)
#define IS_FREE(mp)                 ((mp->field2 & MPR_MASK_FREE) >> MPR_SHIFT_FREE)
#define SET_FREE(mp, value)         mp->field2 = (((size_t) (value)) << MPR_SHIFT_FREE) | (mp->field2 & ~MPR_MASK_FREE)
#define GET_GEN(mp)                 ((int) ((mp->field2 & MPR_MASK_GEN) >> MPR_SHIFT_GEN))
#define SET_GEN(mp, value)          mp->field2 = (((size_t) value) << MPR_SHIFT_GEN) | (mp->field2 & ~MPR_MASK_GEN)
#define GET_MARK(mp)                (mp->field2 & MPR_MASK_MARK)
#define SET_MARK(mp, value)         mp->field2 = (value) | (mp->field2 & ~MPR_MASK_MARK)
#define SET_FIELD2(mp, size, gen, mark, free) mp->field2 = \
                                        (((size_t) (gen)) << MPR_SHIFT_GEN) | \
                                        (((size_t) (free)) << MPR_SHIFT_FREE) | \
                                        ((size) << MPR_SHIFT_SIZE) | \
                                        ((mark) << MPR_SHIFT_MARK)
/*
    Padding fields (only manager stored in padding region)
 */
#define PAD_PTR(mp, offset)     ((void*) (((char*) mp) + GET_SIZE(mp) - ((offset) * sizeof(void*))))
#define MANAGER_SIZE            1
#define MANAGER_OFFSET          1
#define GET_MANAGER(mp)         ((MprManager) (*(void**) ((PAD_PTR(mp, MANAGER_OFFSET)))))
#define SET_MANAGER(mp, fn)     *((MprManager*) PAD_PTR(mp, MANAGER_OFFSET)) = fn

/*
    Memory checking and breakpoints
 */
#if BIT_MEMORY_DEBUG
#define BREAKPOINT(mp)          breakpoint(mp)
#define CHECK(mp)               mprCheckBlock((MprMem*) mp)
#define CHECK_FREE_MEMORY(mp)   checkFreeMem(mp)
#define CHECK_PTR(ptr)          CHECK(GET_MEM(ptr))
#define SCRIBBLE(mp)            if (heap->scribble && mp != GET_MEM(MPR)) { \
                                    memset((char*) mp + sizeof(MprFreeMem), 0xFE, GET_SIZE(mp) - sizeof(MprFreeMem)); \
                                } else
#define SCRIBBLE_RANGE(ptr, size) if (heap->scribble) { \
                                    memset((char*) ptr, 0xFE, size); \
                                } else
#define SET_MAGIC(mp)           mp->magic = MPR_ALLOC_MAGIC
#define SET_SEQ(mp)             mp->seqno = heap->nextSeqno++
#define VALID_BLK(mp)           validBlk(mp)
#define SET_NAME(mp, value)     mp->name = value

#else /* Release mode */
#define BREAKPOINT(mp)
#define CHECK(mp)           
#define CHECK_PTR(mp)           
#define SCRIBBLE(mp)           
#define SCRIBBLE_RANGE(ptr, size)
#define CHECK_FREE_MEMORY(mp)           
#define SET_NAME(mp, value)
#define SET_MAGIC(mp)
#define SET_SEQ(mp)           
#define VALID_BLK(mp)           1
#endif

#if BIT_MEMORY_STATS
    #define INC(field)          if (1) { heap->stats.field++; } else 
#else
    #define INC(field)
#endif

#define INIT_BLK(mp, size, hasManager, last, prior) if (1) { \
    SET_FIELD1(mp, prior, last, hasManager); \
    SET_FIELD2(mp, size, heap->active, heap->eternal, 0); \
    SET_MAGIC(mp); \
    SET_SEQ(mp); \
    SET_NAME(mp, NULL); \
    } else

#define lockHeap()              mprSpinLock(&heap->heapLock);
#define unlockHeap()            mprSpinUnlock(&heap->heapLock);

#define percent(a,b) ((int) ((a) * 100 / (b)))

/*
    Fast find first/last bit set
 */
#if LINUX
    #define NEED_FLSL 1
    #if BIT_CPU_ARCH == MPR_CPU_X86 || BIT_CPU_ARCH == MPR_CPU_X64
        #define USE_FLSL_ASM_X86 1
    #endif
    static MPR_INLINE int flsl(ulong word);

#elif BIT_WIN_LIKE
    #define NEED_FFSL 1
    #define NEED_FLSL 1
    static MPR_INLINE int ffsl(ulong word);
    static MPR_INLINE int flsl(ulong word);

#elif !MACOSX && !FREEBSD
    #define NEED_FFSL 1
    #define NEED_FLSL 1
    static MPR_INLINE int ffsl(ulong word);
    static MPR_INLINE int flsl(ulong word);
#endif

/********************************** Data **************************************/

#undef              MPR
PUBLIC Mpr                 *MPR;
static MprHeap      *heap;
static MprMemStats  memStats;
static int          padding[] = { 0, MANAGER_SIZE };

/***************************** Forward Declarations ***************************/

static void allocException(int cause, ssize size);
static void checkYielded();
static void dummyManager(void *ptr, int flags);
static ssize fastMemSize();
static void *getNextRoot();
static void getSystemInfo();
static void initGen();
static void mark();
static void marker(void *unused, MprThread *tp);
static void markRoots();
static void nextGen();
static int pauseThreads();
static void sweep();
static void resumeThreads();
static void triggerGC(int flags);

#if BIT_WIN_LIKE
    static int winPageModes(int flags);
#endif
#if BIT_MEMORY_DEBUG
    static void breakpoint(MprMem *mp);
    static int validBlk(MprMem *mp);
    static void checkFreeMem(MprMem *mp);
#endif
#if BIT_MEMORY_STATS
#if FUTURE
    static void showMem(MprMem *mp);
#endif
    static void freeLocation(cchar *name, ssize size);
    static void printQueueStats();
    static void printGCStats();
#endif
#if BIT_MEMORY_STACK
static void monitorStack();
#endif

static int initFree();
static MprMem *allocMem(ssize size, int flags);
static MprMem *freeBlock(MprMem *mp);
static int getQueueIndex(ssize size, int roundup);
static MprMem *growHeap(ssize size, int flags);
static void linkBlock(MprMem *mp); 
static void unlinkBlock(MprFreeMem *fp);
static void *vmalloc(ssize size, int mode);
static void vmfree(void *ptr, ssize size);
#if BIT_MEMORY_STATS
    static MprFreeMem *getQueue(ssize size);
#endif

/************************************* Code ***********************************/

PUBLIC Mpr *mprCreateMemService(MprManager manager, int flags)
{
    MprMem      *mp;
    MprMem      *spare;
    MprRegion   *region;
    ssize       size, mprSize, spareSize, regionSize;

    getSystemInfo();

    size = MPR_PAGE_ALIGN(sizeof(MprHeap), memStats.pageSize);
    if ((heap = vmalloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        return NULL;
    }
    memset(heap, 0, sizeof(MprHeap));
    heap->stats.maxMemory = MAXINT;
    heap->stats.redLine = MAXINT / 100 * 99;
    mprInitSpinLock(&heap->heapLock);
    initGen();

    /*
        Hand-craft the Mpr structure
     */
    mprSize = MPR_ALLOC_ALIGN(sizeof(MprMem) + sizeof(Mpr) + (MANAGER_SIZE * sizeof(void*)));
    regionSize = MPR_ALLOC_ALIGN(sizeof(MprRegion));
    size = max(mprSize + regionSize, MPR_MEM_REGION_SIZE);
    if ((region = mprVirtAlloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        return NULL;
    }
    mp = region->start = (MprMem*) (((char*) region) + regionSize);
    region->size = size;

    MPR = (Mpr*) GET_PTR(mp);
    INIT_BLK(mp, mprSize, 1, 0, NULL);
    SET_MANAGER(mp, manager);
    mprSetName(MPR, "Mpr");
    MPR->heap = heap;

    heap->flags = flags | MPR_THREAD_PATTERN;
    heap->nextSeqno = 1;
    heap->chunkSize = MPR_MEM_REGION_SIZE;
    heap->stats.maxMemory = MAXINT;
    heap->stats.redLine = MAXINT / 100 * 99;
    heap->newQuota = MPR_NEW_QUOTA;
    heap->earlyYieldQuota = MPR_NEW_QUOTA * 5;
    heap->enabled = !(heap->flags & MPR_DISABLE_GC);
    if (scmp(getenv("MPR_DISABLE_GC"), "1") == 0) {
        heap->enabled = 0;
    }
    if (scmp(getenv("MPR_VERIFY_MEM"), "1") == 0) {
        heap->verify = 1;
    }
    if (scmp(getenv("MPR_SCRIBBLE_MEM"), "1") == 0) {
        heap->scribble = 1;
    }
    if (scmp(getenv("MPR_TRACK_MEM"), "1") == 0) {
        heap->track = 1;
    }
    heap->stats.bytesAllocated += size;
    INC(allocs);

    mprInitSpinLock(&heap->heapLock);
    mprInitSpinLock(&heap->rootLock);
    initGen();
    initFree();

    spareSize = size - regionSize - mprSize;
    if (spareSize > 0) {
        spare = (MprMem*) (((char*) mp) + mprSize);
        INIT_BLK(spare, size - regionSize - mprSize, 0, 1, mp);
        SET_GEN(spare, heap->eternal);
        SET_FREE(spare, 1);
        heap->regions = region;
        SCRIBBLE(spare);
        linkBlock(spare);
    }
    heap->markerCond = mprCreateCond();
    heap->mutex = mprCreateLock();
    heap->roots = mprCreateList(-1, MPR_LIST_STATIC_VALUES);
    mprAddRoot(MPR);
    return MPR;
}


/*
    Shutdown memory service. Run managers on all allocated blocks.
 */
PUBLIC void mprDestroyMemService()
{
    volatile MprRegion  *region;
    MprMem              *mp, *next;

    if (heap->destroying) {
        return;
    }
    heap->destroying = 1;
    for (region = heap->regions; region; region = region->next) {
        for (mp = region->start; mp; mp = next) {
            next = GET_NEXT(mp);
            if (unlikely(HAS_MANAGER(mp))) {
                (GET_MANAGER(mp))(GET_PTR(mp), MPR_MANAGE_FREE);
                SET_HAS_MANAGER(mp, 0);
            }
        }
    }
}


PUBLIC void *mprAllocMem(ssize usize, int flags)
{
    MprMem      *mp;
    void        *ptr;
    ssize       size;
    int         padWords;

    mprAssert(!heap->marking);
    mprAssert(usize >= 0);

    padWords = padding[flags & MPR_ALLOC_PAD_MASK];
    size = usize + sizeof(MprMem) + (padWords * sizeof(void*));
    size = max(size, usize + (ssize) sizeof(MprFreeMem));
    size = MPR_ALLOC_ALIGN(size);
    
    if ((mp = allocMem(size, flags)) == NULL) {
        return NULL;
    }
    ptr = GET_PTR(mp);
    if (flags & MPR_ALLOC_ZERO) {
        /* Note: real usize may be bigger than requested */
        memset(ptr, 0, GET_USIZE(mp));
    }
    BREAKPOINT(mp);
    CHECK(mp);
    mprAssert(GET_GEN(mp) != heap->eternal);
    return ptr;
}


/*
    Realloc will always zero new memory
 */
PUBLIC void *mprReallocMem(void *ptr, ssize usize)
{
    MprMem      *mp, *newb;
    void        *newptr;
    ssize       oldSize, oldUsize;
    int         flags, hasManager;

    mprAssert(usize > 0);

    if (ptr == 0) {
        return mprAllocMem(usize, 0);
    }
    mp = GET_MEM(ptr);
    CHECK(mp);
    mprAssert(!IS_FREE(mp));
    mprAssert(GET_GEN(mp) != heap->dead);
    oldUsize = GET_USIZE(mp);

    if (usize <= oldUsize) {
        return ptr;
    }
    hasManager = HAS_MANAGER(mp);
    flags = hasManager ? MPR_ALLOC_MANAGER : 0;
    if ((newptr = mprAllocMem(usize, flags)) == NULL) {
        return 0;
    }
    newb = GET_MEM(newptr);
    if (hasManager) {
        SET_MANAGER(newb, GET_MANAGER(mp));
    }
    if (GET_GEN(mp) == heap->eternal) {
        /* Lock-free update */
        SET_FIELD2(newb, GET_SIZE(newb), heap->eternal, UNMARKED, 0);
    }
    oldSize = GET_SIZE(mp);
    memcpy(newptr, ptr, oldSize - sizeof(MprMem));
    /* Note: real usize may be bigger than requested */
    memset(&((char*) newptr)[oldUsize], 0, GET_USIZE(newb) - oldUsize);
    return newptr;
}


PUBLIC void *mprMemdupMem(cvoid *ptr, ssize usize)
{
    char    *newp;

    if ((newp = mprAllocMem(usize, 0)) != 0) {
        memcpy(newp, ptr, usize);
    }
    return newp;
}


PUBLIC int mprMemcmp(cvoid *s1, ssize s1Len, cvoid *s2, ssize s2Len)
{
    int         rc;

    mprAssert(s1);
    mprAssert(s2);
    mprAssert(s1Len >= 0);
    mprAssert(s2Len >= 0);

    if ((rc = memcmp(s1, s2, min(s1Len, s2Len))) == 0) {
        if (s1Len < s2Len) {
            return -1;
        } else if (s1Len > s2Len) {
            return 1;
        }
    }
    return rc;
}


/*
    mprMemcpy will support insitu copy where src and destination overlap
 */
PUBLIC ssize mprMemcpy(void *dest, ssize destMax, cvoid *src, ssize nbytes)
{
    mprAssert(dest);
    mprAssert(destMax <= 0 || destMax >= nbytes);
    mprAssert(src);
    mprAssert(nbytes >= 0);

    if (destMax > 0 && nbytes > destMax) {
        mprAssert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    if (nbytes > 0) {
        memmove(dest, src, nbytes);
        return nbytes;
    } else {
        return 0;
    }
}

/*************************** Virtual Heap Allocations *************************/
/*
    Initialize the free space map and queues.

    The free map is a two dimensional array of free queues. The first dimension is indexed by
    the most significant bit (MSB) set in the requested block size. The second dimension is the next 
    MPR_ALLOC_BUCKET_SHIFT (4) bits below the MSB.

    +-------------------------------+
    |       |MSB|  Bucket   | rest  |
    +-------------------------------+
    | 0 | 0 | 1 | 1 | 1 | 1 | X | X |
    +-------------------------------+
 */
static int initFree() 
{
    MprFreeMem  *freeq;
#if BIT_MEMORY_STATS
    ssize       bit, size, groupBits, bucketBits;
    int         index, group, bucket;
#endif
    
    heap->freeEnd = &heap->freeq[MPR_ALLOC_NUM_GROUPS * MPR_ALLOC_NUM_BUCKETS];
    for (freeq = heap->freeq; freeq != heap->freeEnd; freeq++) {
#if BIT_MEMORY_STATS
        /*
            NOTE: skip the buckets with MSB == 0 (round up)
         */
        index = (int) (freeq - heap->freeq);
        group = index / MPR_ALLOC_NUM_BUCKETS;
        bucket = index % MPR_ALLOC_NUM_BUCKETS;

        bit = (group != 0);
        groupBits = bit << (group + MPR_ALLOC_BUCKET_SHIFT - 1);
        bucketBits = ((ssize) bucket) << (max(0, group - 1));

        size = groupBits | bucketBits;
        freeq->info.stats.minSize = (int) (size << MPR_ALIGN_SHIFT);
#endif
        freeq->next = freeq->prev = freeq;
    }
    return 0;
}


static MprMem *allocMem(ssize required, int flags)
{
    MprFreeMem  *freeq, *fp;
    MprMem      *mp, *after, *spare;
    ssize       size, maxBlock;
    ulong       groupMap, bucketMap;
    int         bucket, baseGroup, group, index;
    
#if BIT_MEMORY_STACK
    monitorStack();
#endif

    index = getQueueIndex(required, 1);
    baseGroup = index / MPR_ALLOC_NUM_BUCKETS;
    bucket = index % MPR_ALLOC_NUM_BUCKETS;
    heap->newCount += index;
    INC(requests);

    /*
        OPT - could break this locked section up.
        - Can update bit maps conservatively and lockfree
        - Put locks around freeq unqueue
        - use unlinkBlock or linkBlock only. Do locks internally in these routines
        - Probably need unlinkFirst
        - Long term use lockfree
     */
    lockHeap();
    
    /* Mask groups lower than the base group */
    groupMap = heap->groupMap & ~((((ssize) 1) << baseGroup) - 1);
    while (groupMap) {
        group = (int) (ffsl(groupMap) - 1);
        if (groupMap & ((((ssize) 1) << group))) {
            bucketMap = heap->bucketMap[group];
            if (baseGroup == group) {
                /* Mask buckets lower than the base bucket */
                bucketMap &= ~((((ssize) 1) << bucket) - 1);
            }
            while (bucketMap) {
                bucket = (int) (ffsl(bucketMap) - 1);
                index = (group * MPR_ALLOC_NUM_BUCKETS) + bucket;
                freeq = &heap->freeq[index];

                if (freeq->next != freeq) {
                    fp = freeq->next;
                    mp = (MprMem*) fp;
                    mprAssert(IS_FREE(mp));
                    unlinkBlock(fp);

                    mprAssert(GET_GEN(mp) == heap->eternal);
                    SET_GEN(mp, heap->active);

                    //  OPT
                    mprAtomicBarrier();
                    if (flags & MPR_ALLOC_MANAGER) {
                        SET_MANAGER(mp, dummyManager);
                        SET_HAS_MANAGER(mp, 1);
                    }
                    INC(reuse);
                    CHECK(mp);
                    CHECK_FREE_MEMORY(mp);
                    if (GET_SIZE(mp) >= (ssize) (required + MPR_ALLOC_MIN_SPLIT)) {
                        maxBlock = (((ssize) 1 ) << group | (((ssize) bucket) << (max(0, group - 1)))) << MPR_ALIGN_SHIFT;
                        maxBlock += sizeof(MprMem);

                        size = GET_SIZE(mp);
                        if (size > maxBlock) {
                            spare = (MprMem*) ((char*) mp + required);
                            INIT_BLK(spare, size - required, 0, IS_LAST(mp), mp);
                            if ((after = GET_NEXT(spare)) != NULL) {
                                SET_PRIOR(after, spare);
                            }
                            SET_SIZE(mp, required);
                            mprAtomicBarrier();
                            SET_LAST(mp, 0);
                            mprAtomicBarrier();
                            INC(splits);
                            linkBlock(spare);
                        }
                    }
                    unlockHeap();
                    return mp;
                }
                bucketMap &= ~(((ssize) 1) << bucket);
                heap->bucketMap[group] &= ~(((ssize) 1) << bucket);
            }
            groupMap &= ~(((ssize) 1) << group);
            heap->groupMap &= ~(((ssize) 1) << group);
#if UNUSED && KEEP
            triggerGC(0);
#endif
        }
    }
    unlockHeap();
    triggerGC(0);
    return growHeap(required, flags);
}


/*
    Grow the heap and return a block of the required size (unqueued)
 */
static MprMem *growHeap(ssize required, int flags)
{
    MprRegion           *region;
    MprMem              *mp, *spare;
    ssize               size, rsize, spareLen;
    int                 hasManager;

    mprAssert(required > 0);

    rsize = MPR_ALLOC_ALIGN(sizeof(MprRegion));
    size = max(required + rsize, (ssize) heap->chunkSize);
    size = MPR_PAGE_ALIGN(size, memStats.pageSize);
    if (size < 0 || size >= ((ssize) 1 << MPR_SIZE_BITS)) {
        allocException(MPR_MEM_TOO_BIG, size);
        return 0;
    }
#if KEEP
{
    static ssize hiwat = 0;
    ssize used = mprGetMem();
    if (used > hiwat) {
        // printf("Grow %ld K, new total %ld K\n", size / 1024, (used + size) / 1024);
        hiwat = used;
    }
}
#endif
    if ((region = mprVirtAlloc(size, MPR_MAP_READ | MPR_MAP_WRITE)) == NULL) {
        return 0;
    }
    mprInitSpinLock(&((MprRegion*) region)->lock);
    region->size = size;
    region->start = (MprMem*) (((char*) region) + rsize);
    region->freeable = 0;
    mp = (MprMem*) region->start;
    hasManager = (flags & MPR_ALLOC_MANAGER) ? 1 : 0;
    spareLen = size - required - rsize;
    if (spareLen < sizeof(MprFreeMem)) {
        required = size - rsize; 
        spareLen = 0;
    }
    INIT_BLK(mp, required, hasManager, spareLen > 0 ? 0 : 1, NULL);
    if (hasManager) {
        SET_MANAGER(mp, dummyManager);
    }
    CHECK(mp);

    lockHeap();
    region->next = heap->regions;
    heap->regions = region;

    if (spareLen > 0) {
        mprAssert(spareLen >= sizeof(MprFreeMem));
        spare = (MprMem*) ((char*) mp + required);
        INIT_BLK(spare, spareLen, 0, 1, mp);
        CHECK(spare);
        INC(allocs);
        linkBlock(spare);
    } else {
        INC(allocs);
    }
    unlockHeap();
    return mp;
}


/*
    Free a block. MUST only ever be called by the sweeper. The sweeper takes advantage of the fact that only it 
    coalesces blocks.
 */
static MprMem *freeBlock(MprMem *mp)
{
    MprMem      *prev, *next, *after;
    MprRegion   *region;
    ssize       size;

    BREAKPOINT(mp);
    SCRIBBLE(mp);
    size = GET_SIZE(mp);
    prev = NULL;
    lockHeap();
    
    /*
        Coalesce with next if it is free
     */
    next = GET_NEXT(mp);
    if (next && IS_FREE(next)) {
        BREAKPOINT(next);
        unlinkBlock((MprFreeMem*) next);
        if ((after = GET_NEXT(next)) != NULL) {
            mprAssert(GET_PRIOR(after) == next);
            SET_PRIOR(after, mp);
        } else {
            SET_LAST(mp, 1);
        }
        size += GET_SIZE(next);
        SET_SIZE(mp, size);
        INC(joins);
        SCRIBBLE_RANGE(next, sizeof(MprFreeMem));
    }

    /*
        Coalesce with previous if it is free
     */
    prev = GET_PRIOR(mp);
    if (prev && IS_FREE(prev)) {
        BREAKPOINT(prev);
        unlinkBlock((MprFreeMem*) prev);
        if ((after = GET_NEXT(mp)) != NULL) {
            mprAssert(GET_PRIOR(after) == mp);
            SET_PRIOR(after, prev);
        } else {
            SET_LAST(prev, 1);
        }
        size += GET_SIZE(prev);
        SET_SIZE(prev, size);
        SCRIBBLE_RANGE(mp, sizeof(MprFreeMem));
        mp = prev;
        INC(joins);
        prev = GET_PRIOR(mp);
        if (prev) {
            CHECK(prev);
        }
        mprAssert(prev == 0 || !IS_FREE(prev));
    }
    next = GET_NEXT(mp);

    /*
        Release entire regions back to the O/S. (Blocks equal to Empty regions have no prior and are last)
     */
    if (GET_PRIOR(mp) == NULL && IS_LAST(mp) && heap->stats.bytesFree > (MPR_MEM_REGION_SIZE * 4)) {
        INC(unpins);
        unlockHeap();
        region = GET_REGION(mp);
        region->freeable = 1;
        mprAssert(next == NULL);
    } else {
        linkBlock(mp);
        unlockHeap();
    }
    /*
        WARN: there is a race here. Another thread may allocate and split the block just freed. So next will be
        pessimistic and there may be newly created intervening blocks.
     */
    return next;
}


static int getQueueIndex(ssize size, int roundup)
{   
    ssize       usize;
    int         asize, aligned, bucket, group, index, msb;
    
    mprAssert(MPR_ALLOC_ALIGN(size) == size);

    /*
        Allocate based on user sizes (sans header). This permits block searches to avoid scanning the next 
        highest queue for common block sizes: eg. 1K.
     */
    usize = (size - sizeof(MprMem));
    asize = (int) (usize >> MPR_ALIGN_SHIFT);

    /* Zero based most significant bit */
    msb = (flsl((int) asize) - 1);

    group = max(0, msb - MPR_ALLOC_BUCKET_SHIFT + 1);
    mprAssert(group < MPR_ALLOC_NUM_GROUPS);

    bucket = (asize >> max(0, group - 1)) & (MPR_ALLOC_NUM_BUCKETS - 1);
    mprAssert(bucket < MPR_ALLOC_NUM_BUCKETS);

    index = (group * MPR_ALLOC_NUM_BUCKETS) + bucket;
    mprAssert(index < (heap->freeEnd - heap->freeq));
    
#if BIT_MEMORY_STATS
    mprAssert(heap->freeq[index].info.stats.minSize <= (int) usize && 
        (int) usize < heap->freeq[index + 1].info.stats.minSize);
#endif
    if (roundup) {
        /*
            Good-fit strategy: check if the requested size is the smallest possible size in a queue. If not the smallest,
            must look at the next queue higher up to guarantee a block of sufficient size.
            Blocks of of size <= 512 bytes (0x20 shifted) are mapped directly to queues. ie. There is only one block size
            per queue. Otherwise, get a mask of the bits below the group and bucket bits. If any are set, then not the 
            lowest size in the queue.
         */
        if (asize > 0x20) {
            ssize mask = (((ssize) 1) << (msb - MPR_ALLOC_BUCKET_SHIFT)) - 1;
            aligned = (asize & mask) == 0;
            if (!aligned) {
                index++;
            }
        }
    }
    return index;
}


/*
    Add a block to a free q. Must be called locked.
    Called by user threads from allocMem and by sweeper from freeBlock.
 */
static void linkBlock(MprMem *mp) 
{
    MprFreeMem  *freeq, *fp;
    ssize       size;
    int         index, group, bucket;

    CHECK(mp);

    /* 
        Mark block as free and eternal so sweeper will skip 
     */
    size = GET_SIZE(mp);
    SET_FIELD2(mp, size, heap->eternal, UNMARKED, 1);
    SET_HAS_MANAGER(mp, 0);
    
    /*
        Set free space bitmap
     */
    index = getQueueIndex(size, 0);
    group = index / MPR_ALLOC_NUM_BUCKETS;
    bucket = index % MPR_ALLOC_NUM_BUCKETS;
    heap->groupMap |= (((ssize) 1) << group);
    heap->bucketMap[group] |= (((ssize) 1) << bucket);

    /*
        Link onto free queue
     */
    fp = (MprFreeMem*) mp;
    freeq = &heap->freeq[index];
    mprAssert(fp != freeq);
    fp->next = freeq->next;
    fp->prev = freeq;
    freeq->next->prev = fp;
    freeq->next = fp;
    mprAssert(fp != fp->next);
    mprAssert(fp != fp->prev);

    heap->stats.bytesFree += size;
#if BIT_MEMORY_STATS
    freeq->info.stats.count++;
#endif
}


/*
    Remove a block from a free q. Must be called locked.
 */
static void unlinkBlock(MprFreeMem *fp) 
{
    MprMem  *mp;
    ssize   size;

    CHECK(fp);
    fp->prev->next = fp->next;
    fp->next->prev = fp->prev;
#if BIT_MEMORY_DEBUG
    fp->next = fp->prev = NULL;
#endif

    mp = (MprMem*) fp;
    size = GET_SIZE(mp);
    heap->stats.bytesFree -= size;
    mprAssert(IS_FREE(mp));
    SET_FREE(mp, 0);
    mprAtomicBarrier();
#if BIT_MEMORY_STATS
{
    MprFreeMem *freeq = getQueue(size);
    freeq->info.stats.count--;
    mprAssert(freeq->info.stats.count >= 0);
}
#endif
}


#if BIT_MEMORY_STATS
static MprFreeMem *getQueue(ssize size)
{   
    MprFreeMem  *freeq;
    int         index;
    
    index = getQueueIndex(size, 0);
    freeq = &heap->freeq[index];
    return freeq;
}
#endif


/*
    Allocate virtual memory and check a memory allocation request against configured maximums and redlines. 
    An application-wide memory allocation failure routine can be invoked from here when a memory redline is exceeded. 
    It is the application's responsibility to set the red-line value suitable for the system.
    Memory is zereod on all platforms.
 */
PUBLIC void *mprVirtAlloc(ssize size, int mode)
{
    ssize       used;
    void        *ptr;

    used = fastMemSize();
    if (memStats.pageSize) {
        size = MPR_PAGE_ALIGN(size, memStats.pageSize);
    }
    if ((size + used) > heap->stats.maxMemory) {
        allocException(MPR_MEM_LIMIT, size);
    } else if ((size + used) > heap->stats.redLine) {
        allocException(MPR_MEM_REDLINE, size);
    }
    if ((ptr = vmalloc(size, mode)) == 0) {
        allocException(MPR_MEM_FAIL, size);
        return 0;
    }
    lockHeap();
    heap->stats.bytesAllocated += size;
    unlockHeap();
    return ptr;
}


PUBLIC void mprVirtFree(void *ptr, ssize size)
{
    vmfree(ptr, size);
    lockHeap();
    heap->stats.bytesAllocated -= size;
    mprAssert(heap->stats.bytesAllocated >= 0);
    unlockHeap();
}


static void *vmalloc(ssize size, int mode)
{
    void    *ptr;

#if VALLOC
    #if BIT_UNIX_LIKE
        if ((ptr = mmap(0, size, mode, MAP_PRIVATE | MAP_ANON, -1, 0)) == (void*) -1) {
            return 0;
        }
    #elif BIT_WIN_LIKE
        ptr = VirtualAlloc(0, size, MEM_RESERVE | MEM_COMMIT, winPageModes(mode));
    #else
        if ((ptr = malloc(size)) != 0) {
            memset(ptr, 0, size);
        }
    #endif
#else
    if ((ptr = malloc(size)) != 0) {
        memset(ptr, 0, size);
    }
#endif
    return ptr;
}


static void vmfree(void *ptr, ssize size)
{
#if VALLOC
    #if BIT_UNIX_LIKE
        if (munmap(ptr, size) != 0) {
            mprAssert(0);
        }
    #elif BIT_WIN_LIKE
        VirtualFree(ptr, 0, MEM_RELEASE);
    #else
        if (heap->scribble) {
            memset(ptr, 0x11, size);
        }
        free(ptr);
    #endif
#else
    free(ptr);
#endif
}


/***************************************************** Garbage Colllector *************************************************/

PUBLIC void mprStartGCService()
{
    if (heap->enabled) {
        if (heap->flags & MPR_MARK_THREAD) {
            LOG(7, "DEBUG: startMemWorkers: start marker");
            if ((heap->marker = mprCreateThread("marker", marker, NULL, 0)) == 0) {
                mprError("Can't create marker thread");
                MPR->hasError = 1;
            } else {
                mprStartThread(heap->marker);
            }
        }
#if FUTURE && KEEP
        if (heap->flags & MPR_SWEEP_THREAD) {
            LOG(7, "DEBUG: startMemWorkers: start sweeper");
            heap->hasSweeper = 1;
            if ((heap->sweeper = mprCreateThread("sweeper", sweeper, NULL, 0)) == 0) {
                mprError("Can't create sweeper thread");
                MPR->hasError = 1;
            } else {
                mprStartThread(heap->sweeper);
            }
        }
#endif
    }
}


PUBLIC void mprStopGCService()
{
    mprWakeGCService();
    mprNap(1);
}


PUBLIC void mprWakeGCService()
{
    mprSignalCond(heap->markerCond);
    mprResumeThreads();
}


static void triggerGC(int flags)
{
    if (!heap->gc && ((flags & MPR_FORCE_GC) || (heap->newCount > heap->newQuota))) {
        heap->gc = 1;
#if !PARALLEL_GC
        heap->mustYield = 1;
#endif
        if (heap->flags & MPR_MARK_THREAD) {
            mprSignalCond(heap->markerCond);
        }
    }
}


PUBLIC void mprRequestGC(int flags)
{
    int     i, count;

    LOG(7, "DEBUG: mprRequestGC");

    count = (flags & MPR_COMPLETE_GC) ? 3 : 1;
    for (i = 0; i < count; i++) {
        if ((flags & MPR_FORCE_GC) || (heap->newCount > heap->newQuota)) {
#if PARALLEL_GC
            heap->mustYield = 1;
#endif
            triggerGC(MPR_FORCE_GC);
        }
        mprYield((flags & MPR_WAIT_GC) ? MPR_YIELD_BLOCK: 0);
    }
}


/*
    Marker synchronization point. At the end of each GC mark/sweep, all threads must rendezvous at the 
    synchronization point.  This happens infrequently and is essential to safely move to a new generation.
    All threads must yield to the marker (including sweeper)
 */
static void resumeThreads()
{
#if BIT_MEMORY_STATS
    LOG(7, "GC: MARKED %,d/%,d, SWEPT %,d/%,d, freed %,d, bytesFree %,d (prior %,d), newCount %,d/%,d, " 
            "blocks %,d bytes %,d",
            heap->stats.marked, heap->stats.markVisited, heap->stats.swept, heap->stats.sweepVisited, 
            (int) heap->stats.freed, (int) heap->stats.bytesFree, (int) heap->priorFree, heap->priorNewCount, heap->newQuota,
            heap->stats.sweepVisited - heap->stats.swept, (int) heap->stats.bytesAllocated);
#endif
#if PARALLEL_GC
    heap->mustYield = 1;
    if (heap->notifier) {
        (heap->notifier)(MPR_MEM_ATTENTION, 0);
    }
    if (pauseThreads()) {
        nextGen();
    } else {
        LOG(7, "DEBUG: Pause for GC sync timed out");
    }
#endif
    heap->mustYield = 0;
    mprResumeThreads();
}


static void mark()
{
    LOG(7, "GC: mark started");

    /*
        When parallel, we mark blocks using the current heap->active mark. After marking, synchronization will rotate
        the active/stale/dead markers. After this, existing alive blocks may be marked stale. No blocks will be marked
        active.
        When !parallel, we swap the active/dead markers first and mark all blocks. After marking and synchronization, 
        existing alive blocks will always be marked active.
     */
#if PARALLEL_GC
    if (heap->newCount > heap->earlyYieldQuota) {
        heap->mustYield = 1;
    }
#else
    heap->mustYield = 1;
    if (!pauseThreads()) {
        LOG(6, "DEBUG: GC synchronization timed out, some threads did not yield.");
        LOG(6, "This is most often caused by a thread doing a long running operation and not first calling mprYield.");
        LOG(6, "If debugging, run the process with -D to enable debug mode.");
        return;
    }
    nextGen();
#endif
    heap->priorNewCount = heap->newCount;
    heap->priorFree = heap->stats.bytesFree;
    heap->newCount = 0;
    heap->gc = 0;
    checkYielded();
    markRoots();
    heap->marking = 0;
    if (!heap->hasSweeper) {
        MPR_MEASURE(7, "GC", "sweep", sweep());
    }
    resumeThreads();
}


/*
    Sweep up the garbage.
    WARNING: This code uses lock-free algorithms. The sweeper traverses the region list and block list without locking. 
    Other code must similarly use lock-free code -- only add regions to the start of the regions list and never 
    otherwise modify the region list. Other code may modify blocks on the list, but must atomically update MprMem.field1.
    The sweeper is the only routine to do coalesing, other code may split blocks, but this can be done in a lock-free 
    manner by creating the spare 2nd half block first and then updating mp->field2 with the size and last bit.
*/
static void sweep()
{
    MprRegion   *region, *nextRegion, *prior;
    MprMem      *mp, *next;
    MprManager  mgr;
    
    if (!heap->enabled) {
        LOG(7, "DEBUG: sweep: Abort sweep - GC disabled");
        return;
    }
    LOG(7, "GC: sweep started");
    heap->stats.freed = 0;

    if (heap->newCount > heap->earlyYieldQuota) {
        heap->mustYield = 1;
    }

    /*
        Run all destructors first so all destructors can guarantee dependant memory blocks will still exist.
        Actually free the memory in a 2nd pass below.
     */
    for (region = heap->regions; region; region = region->next) {
        /*
            This code assumes that no other code coalesces blocks and that splitting blocks will be done lock-free
         */
        for (mp = region->start; mp; mp = GET_NEXT(mp)) {
            if (unlikely(GET_GEN(mp) == heap->dead && HAS_MANAGER(mp))) {
                mgr = GET_MANAGER(mp);
                mprAssert(!IS_FREE(mp));
                CHECK(mp);
                BREAKPOINT(mp);
                if (mgr && VALID_BLK(mp)) {
                    (mgr)(GET_PTR(mp), MPR_MANAGE_FREE);
                }
            }
        }
    }
    heap->stats.sweepVisited = 0;
    heap->stats.swept = 0;

    /*
        growHeap() will append new regions to the front of heap->regions and so will not race with this code. This code
        is the only code that frees regions.
        RACE: Take from the front. Racing with growHeap.
     */
    prior = NULL;
    for (region = heap->regions; region; region = nextRegion) {
        mprAssert(region->freeable == 0 || region->freeable == 1);
        nextRegion = region->next;

        /*
            This code assumes that no other code coalesces blocks and that splitting blocks will be done lock-free
         */
        for (mp = region->start; mp; mp = next) {
            CHECK(mp);
            INC(sweepVisited);
            if (unlikely(GET_GEN(mp) == heap->dead)) {
                mprAssert(!IS_FREE(mp));
                CHECK(mp);
                BREAKPOINT(mp);
                INC(swept);
#if BIT_DEBUG && BIT_MEMORY_STATS
                if (heap->track) {
                    freeLocation(mp->name, GET_SIZE(mp));
                }
#endif
                heap->stats.freed += GET_SIZE(mp);
                next = freeBlock(mp);
            } else {
                /*
                    RACE: Block could be allocated here, but will never be coalesced (sweeper is the only one to do that).
                    So mp->field2 may be reduced so we may skip a newly created block -- no problem. Get it next scan.
                 */
                next = GET_NEXT(mp);
            }
        }
        /*
            The sweeper is the only one who removes regions. 
            Currently all threads are suspended so no locks needed. FUTURE - When doing parallel collection, do this 
            lock-free because user code traverses the region list.
         */ 
        if (region->freeable) {
            lockHeap();
            if (prior) {
                prior->next = nextRegion;
            } else {
                heap->regions = nextRegion;
            }
            unlockHeap();
            LOG(9, "DEBUG: Unpin %p to %p size %d, used %d", region, 
                ((char*) region) + region->size, region->size,fastMemSize());
            mprManageSpinLock(&region->lock, MPR_MANAGE_FREE);
            mprVirtFree(region, region->size);
        } else {
            prior = region;
        }
    }
}


static void markRoots()
{
    void    *root;

    heap->stats.markVisited = 0;
    heap->stats.marked = 0;
    mprMark(heap->roots);
    mprMark(heap->mutex);
    mprMark(heap->markerCond);

    heap->rootIndex = 0;
    while ((root = getNextRoot()) != 0) {
        checkYielded();
        mprMark(root);
    }
    heap->rootIndex = -1;
}


PUBLIC void mprMarkBlock(cvoid *ptr)
{
    MprMem      *mp;
    int         gen;
#if BIT_DEBUG
    static int  depth = 0;
#endif

    if (ptr == 0) {
        return;
    }
    mp = MPR_GET_MEM(ptr);
#if BIT_DEBUG
    if (!mprIsValid(ptr)) {
        mprError("Memory block is either not dynamically allocated, or is corrupted");
        return;
    }
    mprAssert(!IS_FREE(mp));
#if PARALLEL_GC
    mprAssert(GET_MARK(mp) != heap->dead);
    mprAssert(GET_GEN(mp) != heap->dead);
    if (GET_MARK(mp) == heap->dead || IS_FREE(mp)) {
        mprAssert(0);
        return;
    }
#endif
#endif
    CHECK(mp);
    INC(markVisited);
    mprAssert((GET_MARK(mp) != heap->active) || GET_GEN(mp) == heap->active);

    if (GET_MARK(mp) != heap->active) {
        BREAKPOINT(mp);
        INC(marked);
        gen = GET_GEN(mp);
        if (gen != heap->eternal) {
            gen = heap->active;
        }
        /* Lock-free update */
        SET_FIELD2(mp, GET_SIZE(mp), gen, heap->active, 0);
        if (HAS_MANAGER(mp)) {
#if BIT_DEBUG
            if (++depth > 400) {
                fprintf(stderr, "WARNING: Possibly too much recursion. Marking depth exceeds 400\n");
                mprBreakpoint();
            }
#endif
            (GET_MANAGER(mp))((void*) ptr, MPR_MANAGE_MARK);
#if BIT_DEBUG
            --depth;
#endif
        }
    }
}


//  WARNING: these do not mark component members
PUBLIC void mprHold(void *ptr)
{
    MprMem  *mp;

    if (ptr) {
        mp = GET_MEM(ptr);
        if (VALID_BLK(mp)) {
            /* Lock-free update of mp->gen */
            SET_FIELD2(mp, GET_SIZE(mp), heap->eternal, UNMARKED, 0);
        }
    }
}


PUBLIC void mprRelease(void *ptr)
{
    MprMem  *mp;

    if (ptr) {
        mp = GET_MEM(ptr);
        if (VALID_BLK(mp)) {
            mprAssert(!IS_FREE(mp));
            /* Lock-free update of mp->gen */
            SET_FIELD2(mp, GET_SIZE(mp), heap->active, UNMARKED, 0);
        }
    }
}


/*
    If dispatcher is 0, will use MPR->nonBlock if MPR_EVENT_QUICK else MPR->dispatcher
 */
PUBLIC int mprCreateEventOutside(MprDispatcher *dispatcher, void *proc, void *data)
{
    MprEvent    *event;

    heap->pauseGC++;
    mprAtomicBarrier();
    while (heap->mustYield) {
        mprNap(0);
    }
    event = mprCreateEvent(dispatcher, "relay", 0, proc, data, MPR_EVENT_STATIC_DATA);
    heap->pauseGC--;
    if (!event) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


/*
    Marker thread main program
 */
static void marker(void *unused, MprThread *tp)
{
    LOG(5, "DEBUG: marker thread started");
    tp->stickyYield = 1;
    tp->yielded = 1;

    while (!mprIsFinished()) {
        if (!heap->mustYield) {
            mprWaitForCond(heap->markerCond, -1);
            if (mprIsFinished()) {
                break;
            }
        }
        MPR_MEASURE(7, "GC", "mark", mark());
    }
    heap->mustYield = 0;
}


#if UNUSED && KEEP
/*
    Sweeper thread main program. May be called from the marker thread.
 */
static void sweeper(void *unused, MprThread *tp) 
{
    LOG(5, "DEBUG: sweeper thread started");

    heap->sweeper = 1;
    while (!mprIsStoppingCore()) {
        MPR_MEASURE(7, "GC", "sweep", sweep());
        mprYield(MPR_YIELD_BLOCK);
    }
    heap->sweeper = 0;
}
#endif


/*
    Called by user code to signify the thread is ready for GC and all object references are saved. 
    If the GC marker is synchronizing, this call will block at the GC sync point (should be brief).
    NOTE: if called by ResetYield, we may be already marking.
 */
PUBLIC void mprYield(int flags)
{
    MprThreadService    *ts;
    MprThread           *tp;

    ts = MPR->threadService;
    if ((tp = mprGetCurrentThread()) == 0) {
        mprError("Yield called from an unknown thread");
        /* Called from a non-mpr thread */
        return;
    }
    /*
        Must not call mprLog or derviatives here as it will allocate memory and assert
     */
    tp->yielded = 1;
    if (flags & MPR_YIELD_STICKY) {
        tp->stickyYield = 1;
    }
    mprAssert(tp->yielded);
    while (tp->yielded && (heap->mustYield || (flags & MPR_YIELD_BLOCK)) && heap->marker) {
        if (heap->flags & MPR_MARK_THREAD) {
            mprSignalCond(ts->cond);
        }
        mprWaitForCond(tp->cond, -1);
        flags &= ~MPR_YIELD_BLOCK;
    }
    if (!tp->stickyYield) {
        tp->yielded = 0;
    }
    mprAssert(!heap->marking);
}


PUBLIC void mprResetYield()
{
    MprThreadService    *ts;
    MprThread           *tp;

    ts = MPR->threadService;
    mprAssert(mprGetCurrentThread());
    if ((tp = mprGetCurrentThread()) != 0) {
        tp->stickyYield = 0;
    }
    /*
        May have been sticky yielded and so marking could be active. If so, must yield here regardless.
     */
    lock(ts->threads);
    if (heap->marking) {
        unlock(ts->threads);
        mprYield(0);
    } else {
        tp->yielded = 0;
        unlock(ts->threads);
    }
}


/*
    Pause until all threads have yielded. Called by the GC marker only.
    NOTE: this functions differently if parallel. If so, then it will abort waiting. If !parallel, it waits for all
    threads to yield.
 */
static int pauseThreads()
{
    MprThreadService    *ts;
    MprThread           *tp;
    MprTime             mark;
    int                 i, allYielded, timeout;

#if BIT_DEBUG
    uint64  ticks = mprGetTicks();
#endif
    ts = MPR->threadService;
    timeout = MPR_TIMEOUT_GC_SYNC;

    LOG(7, "pauseThreads: wait for threads to yield, timeout %d", timeout);
    mark = mprGetTime();
    if (mprGetDebugMode()) {
        timeout = timeout * 500;
    }
    do {
        /*
            Use the thread list lock to serialize access to heap->marking. 
            NOTE: mprResetYield has a race where its thread will have been yielded.
         */
        lock(ts->threads);
        if (!heap->pauseGC) {
            allYielded = 1;
            for (i = 0; i < ts->threads->length; i++) {
                tp = (MprThread*) mprGetItem(ts->threads, i);
                if (!tp->yielded) {
                    allYielded = 0;
                    if (mprGetElapsedTime(mark) > 1000) {
                        LOG(7, "Thread %s is not yielding", tp->name);
                    }
                    break;
                }
            }
            if (allYielded) {
                heap->marking = 1;
                unlock(ts->threads);
                break;
            }
        } else {
            allYielded = 0;
        }
        unlock(ts->threads);
        LOG(7, "pauseThreads: waiting for threads to yield");
        mprWaitForCond(ts->cond, 20);

    } while (!allYielded && mprGetElapsedTime(mark) < timeout);

#if BIT_DEBUG
    LOG(7, "TIME: pauseThreads elapsed %,d msec, %,d ticks", mprGetElapsedTime(mark), mprGetTicks() - ticks);
#endif
    if (allYielded) {
        checkYielded();
    }
    return (allYielded) ? 1 : 0;
}


/*
    Resume all yielded threads. Called by the GC marker only and when destroying the app.
 */
PUBLIC void mprResumeThreads()
{
    MprThreadService    *ts;
    MprThread           *tp;
    int                 i;

    ts = MPR->threadService;
    LOG(7, "mprResumeThreadsAfterGC sync");

    lock(ts->threads);
    for (i = 0; i < ts->threads->length; i++) {
        tp = (MprThread*) mprGetItem(ts->threads, i);
        if (tp && tp->yielded) {
            if (!tp->stickyYield) {
                tp->yielded = 0;
            }
            mprSignalCond(tp->cond);
        }
    }
    unlock(ts->threads);
}


PUBLIC void mprVerifyMem()
{
#if BIT_MEMORY_DEBUG
    MprRegion   *region;
    MprMem      *mp;
    MprFreeMem  *freeq, *fp;
    int         i;
    
    if (!heap->verify) {
        return;
    }
    lockHeap();
    for (region = heap->regions; region; region = region->next) {
        for (mp = region->start; mp; mp = GET_NEXT(mp)) {
            CHECK(mp);
        }
    }
    for (i = 0, freeq = heap->freeq; freeq != heap->freeEnd; freeq++, i++) {
        for (fp = freeq->next; fp != freeq; fp = fp->next) {
            mp = (MprMem*) fp;
            CHECK(mp);
            mprAssert(GET_GEN(mp) == heap->eternal);
            mprAssert(IS_FREE(mp));
#if FUTURE
            uchar *ptr;
            int  usize;
            if (heap->verifyFree) {
                ptr = (uchar*) ((char*) mp + sizeof(MprFreeMem));
                usize = GET_SIZE(mp) - sizeof(MprFreeMem);
                if (HAS_MANAGER(mp)) {
                    usize -= sizeof(MprManager);
                }
                for (i = 0; i < usize; i++) {
                    if (ptr[i] != 0xFE) {
                        mprError("Free memory block %x has been modified at offset %d (MprBlk %x, seqno %d)\n"
                                       "Memory was last allocated by %s", GET_PTR(mp), i, mp, mp->seqno, mp->name);
                    }
                }
            }
#endif
        }
    }
    unlockHeap();
#endif
}


/*
    WARNING: Caller must be locked so that the sweeper will not free this block. 
 */
PUBLIC int mprIsDead(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    if (VALID_BLK(mp)) {
        return GET_GEN(mp) == heap->dead;
    }
    return 0;
}


/*
    Revive a block that is scheduled for sweeping.
    WARNING: Caller must be locked so that the sweeper will not free this block. 
 */
PUBLIC void mprRevive(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    SET_GEN(mp, heap->active);
    SET_MARK(mp, heap->eternal);
}


PUBLIC bool mprEnableGC(bool on)
{
    bool    old;

    old = heap->enabled;
    heap->enabled = on;
    return old;
}


static void initGen()
{
    heap->eternal = MPR_GEN_ETERNAL;
    heap->active = heap->eternal - 1;
#if PARALLEL_GC
    heap->stale = heap->active - 1;
    heap->dead = heap->stale - 1;
#else
    heap->dead = heap->active - 1;
#endif
}


static void nextGen() 
{
    int     active;

#if PARALLEL_GC
    active = (heap->active + 1) % MPR_MAX_GEN;
    heap->active = active;
    heap->stale = (active - 1 + MPR_MAX_GEN) % MPR_MAX_GEN;
    heap->dead = (active - 2 + MPR_MAX_GEN) % MPR_MAX_GEN;
    LOG(7, "GC: Iteration %d, active %d, stale %d, dead %d, eternal %d",
        heap->iteration, heap->active, heap->stale, heap->dead, heap->eternal);
#else
    active = heap->active;
    heap->active = heap->dead;
    heap->dead = active;
    LOG(7, "GC: Iteration %d, active %d, dead %d, eternal %d",
        heap->iteration, heap->active, heap->dead, heap->eternal);
#endif
    heap->iteration++;
}


PUBLIC void mprAddRoot(void *root)
{
    /*
        Need to use root lock because mprAddItem may allocate
     */
    mprSpinLock(&heap->rootLock);
    mprAddItem(heap->roots, root);
    mprSpinUnlock(&heap->rootLock);
}


PUBLIC void mprRemoveRoot(void *root)
{
    ssize   index;

    mprSpinLock(&heap->rootLock);
    index = mprRemoveItem(heap->roots, root);
    /*
        RemoveItem copies down. If the item was equal or before the current marker root, must adjust the marker rootIndex
        so we don't skip a root.
     */
    if (index <= heap->rootIndex && heap->rootIndex > 0) {
        heap->rootIndex--;
    }
    mprSpinUnlock(&heap->rootLock);
}


static void *getNextRoot()
{
    void    *root;

    mprSpinLock(&heap->rootLock);
    root = mprGetNextItem(heap->roots, &heap->rootIndex);
    mprSpinUnlock(&heap->rootLock);
    return root;
}


/****************************************************** Debug *************************************************************/

#if BIT_MEMORY_STATS
static void printQueueStats() 
{
    MprFreeMem  *freeq;
    int         i;

    printf("\nFree Queue Stats\n Bucket                     Size   Count\n");
    for (i = 0, freeq = heap->freeq; freeq != heap->freeEnd; freeq++, i++) {
        if (freeq->info.stats.count) {
            printf("%7d %24d %7d\n", i, freeq->info.stats.minSize, freeq->info.stats.count);
        }
    }
}


static MprLocationStats sortLocations[MPR_TRACK_HASH];

static int sortLocation(cvoid *l1, cvoid *l2)
{
    MprLocationStats    *lp1, *lp2;

    lp1 = (MprLocationStats*) l1;
    lp2 = (MprLocationStats*) l2;
    if (lp1->count < lp2->count) {
        return -1;
    } else if (lp1->count == lp2->count) {
        return 0;
    }
    return 1;
}


static void printTracking() 
{
    MprLocationStats     *lp;
    cchar                **np;

    printf("\nManager Allocation Stats\n Size                       Location\n");
    memcpy(sortLocations, heap->stats.locations, sizeof(sortLocations));
    qsort(sortLocations, MPR_TRACK_HASH, sizeof(MprLocationStats), sortLocation);

    for (lp = sortLocations; lp < &sortLocations[MPR_TRACK_HASH]; lp++) {
        if (lp->count) {
            for (np = &lp->names[0]; *np && np < &lp->names[MPR_TRACK_NAMES]; np++) {
                if (*np) {
                    if (np == lp->names) {
                        printf("%10d %-24s\n", (int) lp->count, *np);
                    } else {
                        printf("           %-24s\n", *np);
                    }
                }
            }
        }
    }
}


static void printGCStats()
{
    MprRegion   *region;
    MprMem      *mp;
    ssize       size, bytes[MPR_MAX_GEN + 2];
    int         regionCount, i, freeCount, allocatedCount, counts[MPR_MAX_GEN + 2], free, gen;

    for (i = 0; i < (MPR_MAX_GEN + 2); i++) {
        counts[i] = 0;
        bytes[i] = 0;
    }
    printf("\nRegion Stats\n");
    regionCount = 0;
    free = heap->eternal + 1;
    for (region = heap->regions; region; region = region->next) {
        freeCount = allocatedCount = 0;
        for (mp = region->start; mp; mp = GET_NEXT(mp)) {
            size = GET_SIZE(mp);
            gen = GET_GEN(mp);
            if (IS_FREE(mp)) {
                freeCount++;
                counts[free]++;
                bytes[free] += size;
            } else {
                counts[gen]++;
                bytes[gen] += size;
                allocatedCount++;
            }
        }
        regionCount++;
        printf("  Region %d is %d bytes, has %d allocated %d free\n", regionCount, (int) region->size, 
            allocatedCount, freeCount);
    }
    printf("Regions: %d\n", regionCount);

    printf("\nGC Stats\n");
    printf("  Eternal generation has %9d blocks, %12d bytes\n", counts[heap->eternal], (int) bytes[heap->eternal]);
#if PARALLEL_GC
    printf("  Stale generation has   %9d blocks, %12d bytes\n", counts[heap->stale], (int) bytes[heap->stale]);
#endif
    printf("  Active generation has  %9d blocks, %12d bytes\n", counts[heap->active], (int) bytes[heap->active]);
    printf("  Dead generation has    %9d blocks, %12d bytes\n", counts[heap->dead], (int) bytes[heap->dead]);
    printf("  Free generation has    %9d blocks, %12d bytes\n", counts[free], (int) bytes[free]);
}
#endif /* BIT_MEMORY_STATS */


PUBLIC void mprPrintMem(cchar *msg, int detail)
{
#if BIT_MEMORY_STATS
    MprMemStats   *ap;

    ap = mprGetMemStats();

    printf("\n\nMPR Memory Report %s\n", msg);
    printf("------------------------------------------------------------------------------------------\n");
    printf("  Total memory        %14d K\n",             (int) (mprGetMem() / 1024));
    printf("  Current heap memory %14d K\n",             (int) (ap->bytesAllocated / 1024));
    printf("  Free heap memory    %14d K\n",             (int) (ap->bytesFree / 1024));
    printf("  Allocation errors   %14d\n",               ap->errors);
    printf("  Memory limit        %14d MB (%d %%)\n",    (int) (ap->maxMemory / (1024 * 1024)),
       percent(ap->bytesAllocated / 1024, ap->maxMemory / 1024));
    printf("  Memory redline      %14d MB (%d %%)\n",    (int) (ap->redLine / (1024 * 1024)),
       percent(ap->bytesAllocated / 1024, ap->redLine / 1024));

    printf("  Memory requests     %14d\n",               (int) ap->requests);
    printf("  O/S allocations     %14d %%\n",            percent(ap->allocs, ap->requests));
    printf("  Block unpinns       %14d %%\n",            percent(ap->unpins, ap->requests));
    printf("  Block reuse         %14d %%\n",            percent(ap->reuse, ap->requests));
    printf("  Joins               %14d %%\n",            percent(ap->joins, ap->requests));
    printf("  Splits              %14d %%\n",            percent(ap->splits, ap->requests));

    printGCStats();
    if (detail) {
        printQueueStats();
        if (heap->track) {
            printTracking();
        }
    }
#endif /* BIT_MEMORY_STATS */
}


#if BIT_MEMORY_DEBUG
static int validBlk(MprMem *mp)
{
    ssize   size;

    size = GET_SIZE(mp);
    mprAssert(mp->magic == MPR_ALLOC_MAGIC);
    mprAssert(size > 0);
    return (mp->magic == MPR_ALLOC_MAGIC) && (size > 0);
}


PUBLIC void mprCheckBlock(MprMem *mp)
{
    ssize   size;

    size = GET_SIZE(mp);
    if (mp->magic != MPR_ALLOC_MAGIC || size <= 0) {
        mprError("Memory corruption in memory block %x (MprBlk %x, seqno %d)\n"
            "This most likely happend earlier in the program execution", GET_PTR(mp), mp, mp->seqno);
    }
}


static void checkFreeMem(MprMem *mp)
{
#if FUTURE
    uchar   *ptr;
    int     usize, i;

    if (heap->verify) {
        ptr = (uchar*) ((char*) mp + sizeof(MprFreeMem));
        usize = GET_SIZE(mp) - sizeof(MprFreeMem);
        if (HAS_MANAGER(mp)) {
            usize -= sizeof(MprManager);
        }
        for (i = 0; i < usize; i++) {
            if (ptr[i] != 0xFE) {
                mprError("Free memory block %x has been modified at offset %d (MprBlk %x, seqno %d)\n"
                    "Memory was last allocated by %s", GET_PTR(mp), i, mp, mp->seqno, mp->name);
                break;
            }
        }
    }
#endif
}


static void breakpoint(MprMem *mp) 
{
    if (mp == stopAlloc || mp->seqno == stopSeqno) {
        mprBreakpoint();
    }
}


/*
    Called to set the memory block name when doing an allocation
 */
PUBLIC void *mprSetAllocName(void *ptr, cchar *name)
{
    MPR_GET_MEM(ptr)->name = name;

#if BIT_MEMORY_STATS
    if (heap->track) {
        MprLocationStats    *lp;
        cchar               **np;
        int                 index;
        if (name == 0) {
            name = "";
        }
        index = shash(name, strlen(name)) % MPR_TRACK_HASH;
        lp = &heap->stats.locations[index];
        for (np = lp->names; np <= &lp->names[MPR_TRACK_NAMES]; np++) {
            if (*np == 0 || *np == name || strcmp(*np, name) == 0) {
                break;
            }
        }
        //  mprAssert(np < &lp->names[MPR_TRACK_NAMES]);
        if (np < &lp->names[MPR_TRACK_NAMES]) {
            *np = (char*) name;
        }
        lp->count += GET_SIZE(GET_MEM(ptr));
    }
#endif
    return ptr;
}


static void freeLocation(cchar *name, ssize size)
{
#if BIT_MEMORY_STATS
    MprLocationStats    *lp;
    int                 index, i;

    if (name == 0) {
        name = "";
    }
    index = shash(name, strlen(name)) % MPR_TRACK_HASH;
    lp = &heap->stats.locations[index];
    lp->count -= size;
    if (lp->count <= 0) {
        for (i = 0; i < MPR_TRACK_NAMES; i++) {
            lp->names[i] = 0;
        }
    }
#endif
}


PUBLIC void *mprSetName(void *ptr, cchar *name) 
{
#if BIT_MEMORY_STATS
    MprMem  *mp = GET_MEM(ptr);
    if (mp->name) {
        freeLocation(mp->name, GET_SIZE(mp));
        mprSetAllocName(ptr, name);
    }
#else
    MPR_GET_MEM(ptr)->name = name;
#endif
    return ptr;
}


PUBLIC void *mprCopyName(void *dest, void *src) 
{
    return mprSetName(dest, mprGetName(src));
}
#endif


/********************************************* Misc ***************************************************/

static void allocException(int cause, ssize size)
{
    ssize   used;

    lockHeap();
    INC(errors);
    if (heap->stats.inMemException || mprIsStopping()) {
        unlockHeap();
        return;
    }
    heap->stats.inMemException = 1;
    used = fastMemSize();
    unlockHeap();

    if (cause == MPR_MEM_FAIL) {
        heap->hasError = 1;
        mprLog(0, "%s: Can't allocate memory block of size %,d bytes.", MPR->name, size);

    } else if (cause == MPR_MEM_TOO_BIG) {
        heap->hasError = 1;
        mprLog(0, "%s: Can't allocate memory block of size %,d bytes.", MPR->name, size);

    } else if (cause == MPR_MEM_REDLINE) {
        mprLog(0, "%s: Memory request for %,d bytes exceeds memory red-line.", MPR->name, size);
        mprPruneCache(NULL);

    } else if (cause == MPR_MEM_LIMIT) {
        mprLog(0, "%s: Memory request for %,d bytes exceeds memory limit.", MPR->name, size);
    }
    mprLog(0, "%s: Memory used %,d, redline %,d, limit %,d.", MPR->name, (int) used, (int) heap->stats.redLine,
        (int) heap->stats.maxMemory);
    mprLog(0, "%s: Consider increasing memory limit.", MPR->name);
    
    if (heap->notifier) {
        (heap->notifier)(cause, heap->allocPolicy,  size, used);
    }
    if (cause & (MPR_MEM_TOO_BIG | MPR_MEM_FAIL)) {
        /*
            Allocation failed
         */
        mprError("Application exiting immediately due to memory depletion.");
        mprTerminate(MPR_EXIT_IMMEDIATE, 2);

    } else if (cause & MPR_MEM_LIMIT) {
        if (heap->allocPolicy == MPR_ALLOC_POLICY_RESTART) {
            mprError("Application restarting due to low memory condition.");
            mprTerminate(MPR_EXIT_GRACEFUL | MPR_EXIT_RESTART, 1);

        } else if (heap->allocPolicy == MPR_ALLOC_POLICY_EXIT) {
            mprError("Application exiting immediately due to memory depletion.");
            mprTerminate(MPR_EXIT_IMMEDIATE, 2);
        }
    }
    heap->stats.inMemException = 0;
}


static void getSystemInfo()
{
    memStats.numCpu = 1;

#if MACOSX
    #ifdef _SC_NPROCESSORS_ONLN
        memStats.numCpu = (uint) sysconf(_SC_NPROCESSORS_ONLN);
    #else
        memStats.numCpu = 1;
    #endif
    memStats.pageSize = (uint) sysconf(_SC_PAGESIZE);
#elif SOLARIS
{
    FILE *ptr;
    if  ((ptr = popen("psrinfo -p", "r")) != NULL) {
        fscanf(ptr, "%d", &alloc.numCpu);
        (void) pclose(ptr);
    }
    alloc.pageSize = sysconf(_SC_PAGESIZE);
}
#elif BIT_WIN_LIKE
{
    SYSTEM_INFO     info;

    GetSystemInfo(&info);
    memStats.numCpu = info.dwNumberOfProcessors;
    memStats.pageSize = info.dwPageSize;

}
#elif FREEBSD
    {
        int     cmd[2];
        ssize   len;

        cmd[0] = CTL_HW;
        cmd[1] = HW_NCPU;
        len = sizeof(memStats.numCpu);
        memStats.numCpu = 0;
        if (sysctl(cmd, 2, &memStats.numCpu, &len, 0, 0) < 0) {
            memStats.numCpu = 1;
        }
        memStats.pageSize = sysconf(_SC_PAGESIZE);
    }
#elif LINUX
    {
        static const char processor[] = "processor\t:";
        char    c;
        int     fd, col, match;

        fd = open("/proc/cpuinfo", O_RDONLY);
        if (fd < 0) {
            return;
        }
        match = 1;
        for (col = 0; read(fd, &c, 1) == 1; ) {
            if (c == '\n') {
                col = 0;
                match = 1;
            } else {
                if (match && col < (sizeof(processor) - 1)) {
                    if (c != processor[col]) {
                        match = 0;
                    }
                    col++;

                } else if (match) {
                    memStats.numCpu++;
                    match = 0;
                }
            }
        }
        --memStats.numCpu;
        close(fd);
        memStats.pageSize = sysconf(_SC_PAGESIZE);
    }
#else
        memStats.pageSize = 4096;
#endif
    if (memStats.pageSize <= 0 || memStats.pageSize >= (16 * 1024)) {
        memStats.pageSize = 4096;
    }
}


#if BIT_WIN_LIKE
static int winPageModes(int flags)
{
    if (flags & MPR_MAP_EXECUTE) {
        return PAGE_EXECUTE_READWRITE;
    } else if (flags & MPR_MAP_WRITE) {
        return PAGE_READWRITE;
    }
    return PAGE_READONLY;
}
#endif


PUBLIC MprMemStats *mprGetMemStats()
{
#if LINUX
    char            buf[1024], *cp;
    size_t          len;
    int             fd;

    heap->stats.ram = MAXSSIZE;
    if ((fd = open("/proc/meminfo", O_RDONLY)) >= 0) {
        if ((len = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[len] = '\0';
            if ((cp = strstr(buf, "MemTotal:")) != 0) {
                for (; *cp && !isdigit((uchar) *cp); cp++) {}
                heap->stats.ram = ((ssize) atoi(cp) * 1024);
            }
        }
        close(fd);
    }
#endif
#if MACOSX || FREEBSD
    size_t len;
    int         mib[2];
#if FREEBSD
    ssize ram, usermem;
    mib[1] = HW_MEMSIZE;
#else
    int64 ram, usermem;
    mib[1] = HW_PHYSMEM;
#endif
    mib[0] = CTL_HW;
    len = sizeof(ram);
    ram = 0;
    sysctl(mib, 2, &ram, &len, NULL, 0);
    heap->stats.ram = ram;

    mib[0] = CTL_HW;
    mib[1] = HW_USERMEM;
    len = sizeof(usermem);
    usermem = 0;
    sysctl(mib, 2, &usermem, &len, NULL, 0);
    heap->stats.user = usermem;
#endif
    heap->stats.rss = mprGetMem();
    return &heap->stats;
}


/*
    Return the amount of memory currently in use. This routine may open files and thus is not very quick on some 
    platforms. On FREEBDS it returns the peak resident set size using getrusage. If a suitable O/S API is not available,
    the amount of heap memory allocated by the MPR is returned.
 */
PUBLIC ssize mprGetMem()
{
    ssize size = 0;

#if LINUX
    int fd;
    char path[MPR_MAX_PATH];
    sprintf(path, "/proc/%d/status", getpid());
    if ((fd = open(path, O_RDONLY)) >= 0) {
        char buf[MPR_BUFSIZE], *tok;
        int nbytes = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (nbytes > 0) {
            buf[nbytes] = '\0';
            if ((tok = strstr(buf, "VmRSS:")) != 0) {
                for (tok += 6; tok && isspace((uchar) *tok); tok++) {}
                size = stoi(tok) * 1024;
            }
        }
    }
    if (size == 0) {
        struct rusage rusage;
        getrusage(RUSAGE_SELF, &rusage);
        size = rusage.ru_maxrss * 1024;
    }
#elif MACOSX
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        size = info.resident_size;
    }
#elif FREEBSD
    struct rusage   rusage;
    getrusage(RUSAGE_SELF, &rusage);
    size = rusage.ru_maxrss;
#endif
    if (size == 0) {
        size = heap->stats.bytesAllocated;
    }
    return size;
}


/*
    Fast routine to teturn the approximately the amount of memory currently in use. If a fast method is not available,
    use the amount of heap memory allocated by the MPR.
    WARNING: this routine must be FAST as it is used by the MPR memory allocation mechanism when more memory is allocated
    from the O/S (i.e. not on every block allocation).
 */
static ssize fastMemSize()
{
    ssize   size = 0;

#if LINUX
    struct rusage rusage;
    getrusage(RUSAGE_SELF, &rusage);
    size = rusage.ru_maxrss * 1024;
#elif MACOSX
    struct task_basic_info info;
    mach_msg_type_number_t count = TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS) {
        size = info.resident_size;
    }
#endif
    if (size == 0) {
        size = heap->stats.bytesAllocated;
    }
    return size;
}


#if NEED_FFSL
/* 
    Find first bit set in word 
 */
#if USE_FFSL_ASM_X86
static MPR_INLINE int ffsl(ulong x)
{
    long    r;

    asm("bsf %1,%0\n\t"
        "jnz 1f\n\t"
        "mov $-1,%0\n"
        "1:" : "=r" (r) : "rm" (x));
    return (int) r + 1;
}
#else
static MPR_INLINE int ffsl(ulong word)
{
    int     b;

    for (b = 0; word; word >>= 1, b++) {
        if (word & 0x1) {
            b++;
            break;
        }
    }
    return b;
}
#endif
#endif


#if NEED_FLSL
/* 
    Find last bit set in word 
 */
#if USE_FFSL_ASM_X86
static MPR_INLINE int flsl(ulong x)
{
    long r;

    asm("bsr %1,%0\n\t"
        "jnz 1f\n\t"
        "mov $-1,%0\n"
        "1:" : "=r" (r) : "rm" (x));
    return (int) r + 1;
}
#else /* USE_FFSL_ASM_X86 */ 

static MPR_INLINE int flsl(ulong word)
{
    int     b;

    for (b = 0; word; word >>= 1, b++) ;
    return b;
}
#endif /* !USE_FFSL_ASM_X86 */
#endif /* NEED_FFSL */


#if BIT_WIN_LIKE
PUBLIC Mpr *mprGetMpr()
{
    return MPR;
}
#endif


PUBLIC int mprGetPageSize()
{
    return memStats.pageSize;
}


PUBLIC ssize mprGetBlockSize(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    if (ptr == 0 || !VALID_BLK(mp)) {
        return 0;
    }
    CHECK(mp);
    return GET_USIZE(mp);
}


PUBLIC int mprGetHeapFlags()
{
    return heap->flags;
}


PUBLIC void mprSetMemNotifier(MprMemNotifier cback)
{
    heap->notifier = cback;
}


PUBLIC void mprSetMemLimits(ssize redLine, ssize maxMemory)
{
    if (redLine > 0) {
        heap->stats.redLine = redLine;
    }
    if (maxMemory > 0) {
        heap->stats.maxMemory = maxMemory;
    }
}


PUBLIC void mprSetMemPolicy(int policy)
{
    heap->allocPolicy = policy;
}


PUBLIC void mprSetMemError()
{
    heap->hasError = 1;
}


PUBLIC bool mprHasMemError()
{
    return heap->hasError;
}


PUBLIC void mprResetMemError()
{
    heap->hasError = 0;
}


PUBLIC int mprIsValid(cvoid *ptr)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
#if BIT_WIN
    if (isBadWritePtr(mp, sizeof(MprMem))) {
        return 0;
    }
    if (!VALID_BLK(GET_MEM(ptr)) {
        return 0;
    }
    if (isBadWritePtr(ptr, GET_SIZE(mp))) {
        return 0;
    }
    return 0;
#else
#if BIT_DEBUG
    return ptr && mp->magic == MPR_ALLOC_MAGIC && GET_SIZE(mp) > 0;
#else
    return ptr && GET_SIZE(mp) > 0;
#endif
#endif
}


static void dummyManager(void *ptr, int flags) 
{
}


PUBLIC void *mprSetManager(void *ptr, MprManager manager)
{
    MprMem      *mp;

    mp = GET_MEM(ptr);
    mprAssert(HAS_MANAGER(mp));
    if (HAS_MANAGER(mp)) {
        if (!manager) {
            manager = dummyManager;
        }
        SET_MANAGER(mp, manager);
    }
    return ptr;
}


#if BIT_MEMORY_STATS && FUTURE
static void showMem(MprMem *mp)
{
    char    *gen, *mark, buf[MPR_MAX_STRING];
    int     g, m;

    g = GET_GEN(mp);
    m = GET_MARK(mp);
    if (g == heap->eternal) {
        gen = "eternal";
    } else if (g == heap->active) {
        gen = "active";
    } else if (g == heap->stale) {
        gen = "stale";
    } else if (g == heap->dead) {
        gen = "dead";
    } else {
        gen = "INVALID";
    }
    if (m == heap->eternal) {
        mark = "eternal";
    } else if (m == heap->active) {
        mark = "active";
    } else if (m == heap->stale) {
        mark = "stale";
    } else if (m == heap->dead) {
        mark = "dead";
    } else {
        mark = "INVALID";
    }
    sprintf(buf, "Mem 0x%p, size %d, free %d, mgr %d, last %d, prior 0x%p, gen \"%s\", mark \"%s\"\n",
        mp, (int) GET_SIZE(mp), (int) IS_FREE(mp), (int) HAS_MANAGER(mp), (int) IS_LAST(mp), GET_PRIOR(mp), gen, mark);
#if BIT_WIN
    OutputDebugString(buf);
#else
    print(buf);
#endif
}
#endif


static void checkYielded()
{
#if BIT_DEBUG
    MprThreadService    *ts;
    MprThread           *tp;
    int                 i;

    ts = MPR->threadService;
    lock(ts->threads);
    for (i = 0; i < ts->threads->length; i++) {
        tp = (MprThread*) mprGetItem(ts->threads, i);
        mprAssert(tp->yielded);
    }
    unlock(ts->threads);
#endif
}


#if BIT_MEMORY_STACK
static void monitorStack()
{
    MprThread   *tp;
    int         diff;

    if (MPR->threadService && (tp = mprGetCurrentThread()) != 0) {
        if (tp->stackBase == 0) {
            tp->stackBase = &tp;
        }
        diff = (int) ((char*) tp->stackBase - (char*) &diff);
        if (diff < 0) {
            tp->peakStack -= diff;
            tp->stackBase = (void*) &diff;
            diff = 0;
        }
        if (diff > tp->peakStack) {
            tp->peakStack = diff;
        }
    }
}
#endif

#if !BIT_MEMORY_DEBUG
#undef mprSetName
#undef mprCopyName
#undef mprSetAllocName

/*
    Define stubs so windows can use same *.def for debug or release
 */
PUBLIC void mprCheckBlock(MprMem *mp) {}
PUBLIC void *mprSetName(void *ptr, cchar *name) { return 0; }
PUBLIC void *mprCopyName(void *dest, void *src) { return 0; }
PUBLIC void *mprSetAllocName(void *ptr, cchar *name) { return 0; }

/*
    Re-instate defines for combo releases, where source will be appended below here
 */
#define mprCopyName(dest, src)
#define mprGetName(ptr) ""
#define mprSetAllocName(ptr, name) ptr
#define mprSetName(ptr, name)
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mpr.c"
 */
/************************************************************************/

/*
    mpr.c - Multithreaded Portable Runtime (MPR). Initialization, start/stop and control of the MPR.

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************** Includes **********************************/



/**************************** Forward Declarations ****************************/

static void manageMpr(Mpr *mpr, int flags);
static void serviceEventsThread(void *data, MprThread *tp);

/************************************* Code ***********************************/
/*
    Create and initialize the MPR service.
 */
PUBLIC Mpr *mprCreate(int argc, char **argv, int flags)
{
    MprFileSystem   *fs;
    Mpr             *mpr;

    srand((uint) time(NULL));

    if ((mpr = mprCreateMemService((MprManager) manageMpr, flags)) == 0) {
        mprAssert(mpr);
        return 0;
    }
    mpr->start = mprGetTime(); 
    mpr->exitStrategy = MPR_EXIT_NORMAL;
    mpr->emptyString = sclone("");
    mpr->exitTimeout = MPR_TIMEOUT_STOP;
    mpr->title = sclone(BIT_TITLE);
    mpr->version = sclone(BIT_VERSION);
    mpr->idleCallback = mprServicesAreIdle;
    mpr->mimeTypes = mprCreateMimeTypes(NULL);
    mpr->terminators = mprCreateList(0, MPR_LIST_STATIC_VALUES);

    mprCreateTimeService();
    mprCreateOsService();
    mpr->mutex = mprCreateLock();
    mpr->spin = mprCreateSpinLock();
#if UNUSED
    mpr->dtoaSpin[0] = mprCreateSpinLock();
    mpr->dtoaSpin[1] = mprCreateSpinLock();
#endif

    fs = mprCreateFileSystem("/");
    mprAddFileSystem(fs);
    mprCreateLogService();
    
    if (argv) {
#if BIT_WIN_LIKE
        if (argc >= 2 && strstr(argv[1], "--cygroot") != 0) {
            /*
                Cygwin shebang is broken. It will catenate args into argv[1]
             */
            char *args, *arg0;
            int  i;
            args = argv[1];
            for (i = 2; i < argc; i++) {
                args = sjoin(args, " ", argv[i], NULL);
            }
            arg0 = argv[0];
            argc = mprMakeArgv(args, &mpr->argBuf, MPR_ARGV_ARGS_ONLY);
            argv = mpr->argBuf;
            argv[0] = arg0;
        }
#endif
        mpr->argc = argc;
        mpr->argv = (cchar**) argv;
        if (!mprIsPathAbs(mpr->argv[0])) {
            mpr->argv[0] = mprGetAppPath();
        }
        mpr->name = mprTrimPathExt(mprGetPathBase(mpr->argv[0]));
    } else {
        mpr->name = sclone(BIT_PRODUCT);
        mpr->argv = mprAllocZeroed(sizeof(void*));
        mpr->argc = 0;
    }
    mpr->signalService = mprCreateSignalService();
    mpr->threadService = mprCreateThreadService();
    mpr->moduleService = mprCreateModuleService();
    mpr->eventService = mprCreateEventService();
    mpr->cmdService = mprCreateCmdService();
    mpr->workerService = mprCreateWorkerService();
    mpr->waitService = mprCreateWaitService();
    mpr->socketService = mprCreateSocketService();

    mpr->dispatcher = mprCreateDispatcher("main", 1);
    mpr->nonBlock = mprCreateDispatcher("nonblock", 1);
    mpr->pathEnv = sclone(getenv("PATH"));

    if (flags & MPR_USER_EVENTS_THREAD) {
        if (!(flags & MPR_NO_WINDOW)) {
            mprInitWindow();
        }
    } else {
        mprStartEventsThread();
    }
    mprStartGCService();

    if (MPR->hasError || mprHasMemError()) {
        return 0;
    }
    return mpr;
}


static void manageMpr(Mpr *mpr, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mpr->logPath);
        mprMark(mpr->logFile);
        mprMark(mpr->mimeTypes);
        mprMark(mpr->timeTokens);
        mprMark(mpr->pathEnv);
        mprMark(mpr->name);
        mprMark(mpr->title);
        mprMark(mpr->version);
        mprMark(mpr->domainName);
        mprMark(mpr->hostName);
        mprMark(mpr->ip);
        mprMark(mpr->stdError);
        mprMark(mpr->stdInput);
        mprMark(mpr->stdOutput);
        mprMark(mpr->serverName);
        mprMark(mpr->appPath);
        mprMark(mpr->appDir);
        mprMark(mpr->cmdService);
        mprMark(mpr->eventService);
        mprMark(mpr->fileSystem);
        mprMark(mpr->moduleService);
        mprMark(mpr->osService);
        mprMark(mpr->signalService);
        mprMark(mpr->socketService);
        mprMark(mpr->threadService);
        mprMark(mpr->workerService);
        mprMark(mpr->waitService);
        mprMark(mpr->dispatcher);
        mprMark(mpr->nonBlock);
        mprMark(mpr->appwebService);
        mprMark(mpr->ediService);
        mprMark(mpr->ejsService);
        mprMark(mpr->espService);
        mprMark(mpr->httpService);
        mprMark(mpr->testService);
        mprMark(mpr->terminators);
        mprMark(mpr->mutex);
        mprMark(mpr->spin);
#if UNUSED
        mprMark(mpr->dtoaSpin[0]);
        mprMark(mpr->dtoaSpin[1]);
#endif
        mprMark(mpr->cond);
        mprMark(mpr->emptyString);
        mprMark(mpr->argBuf);
    }
}

static void wgc(int mode)
{
    mprRequestGC(mode);
}

/*
    Destroy the Mpr and all services
 */
PUBLIC void mprDestroy(int how)
{
    int         gmode;

    if (!(how & MPR_EXIT_DEFAULT)) {
        MPR->exitStrategy = how;
    }
    how = MPR->exitStrategy;
    if (how & MPR_EXIT_IMMEDIATE) {
        if (how & MPR_EXIT_RESTART) {
            mprRestart();
            /* No return */
            return;
        }
        exit(0);
    }
    mprYield(MPR_YIELD_STICKY);
    if (MPR->state < MPR_STOPPING) {
        mprTerminate(how, -1);
    }
    gmode = MPR_FORCE_GC | MPR_COMPLETE_GC | MPR_WAIT_GC;
    mprRequestGC(gmode);

    if (how & MPR_EXIT_GRACEFUL) {
        mprWaitTillIdle(MPR->exitTimeout);
    }
    MPR->state = MPR_STOPPING_CORE;
    MPR->exitStrategy &= MPR_EXIT_GRACEFUL;
    MPR->exitStrategy |= MPR_EXIT_IMMEDIATE;

    mprWakeWorkers();
    mprStopCmdService();
    mprStopModuleService();
    mprStopEventService();
    mprStopSignalService();

    /* Final GC to run all finalizers */
    wgc(gmode);

    if (how & MPR_EXIT_RESTART) {
        mprLog(3, "Restarting\n\n");
    } else {
        mprLog(3, "Exiting");
    }
    MPR->state = MPR_FINISHED;
    mprStopGCService();
    mprStopThreadService();
    mprStopOsService();
    mprDestroyMemService();

    if (how & MPR_EXIT_RESTART) {
        mprRestart();
    }
}



/*
    Start termination of the Mpr. May be called by mprDestroy or elsewhere.
 */
PUBLIC void mprTerminate(int how, int status)
{
    MprTerminator   terminator;
    int             next;

    /*
        Set the stopping flag. Services should stop accepting new requests. Current requests should be allowed to
        complete if graceful exit strategy.
     */
    if (MPR->state >= MPR_STOPPING) {
        /* Already stopping and done the code below */
        return;
    }
    MPR->state = MPR_STOPPING;

    MPR->exitStatus = status;
    if (!(how & MPR_EXIT_DEFAULT)) {
        MPR->exitStrategy = how;
    }
    how = MPR->exitStrategy;
    if (how & MPR_EXIT_IMMEDIATE) {
        mprLog(3, "Immediate exit. Terminate all requests and services.");
        exit(status);

    } else if (how & MPR_EXIT_NORMAL) {
        mprLog(3, "Normal exit.");

    } else if (how & MPR_EXIT_GRACEFUL) {
        mprLog(3, "Graceful exit. Waiting for existing requests to complete.");

    } else {
        mprLog(7, "mprTerminate: how %d", how);
    }

    /*
        Invoke terminators, set stopping state and wake up everybody
        Must invoke terminators before setting stopping state. Otherwise, the main app event loop will return from
        mprServiceEvents and starting calling destroy before we have completed this routine.
     */
    for (ITERATE_ITEMS(MPR->terminators, terminator, next)) {
        (terminator)(how, status);
    }
    mprWakeWorkers();
    mprWakeGCService();
    mprWakeDispatchers();
    mprWakeNotifier();
}


PUBLIC int mprGetExitStatus()
{
    return MPR->exitStatus;
}


PUBLIC void mprAddTerminator(MprTerminator terminator)
{
    mprAddItem(MPR->terminators, terminator);
}


PUBLIC void mprRestart()
{
#if BIT_UNIX_LIKE
    int     i;
    for (i = 3; i < MPR_MAX_FILE; i++) {
        close(i);
    }
    execv(MPR->argv[0], (char*const*) MPR->argv);

    /*
        Last-ditch trace. Can only use stdout. Logging may be closed.
     */
    printf("Failed to exec errno %d: ", errno);
    for (i = 0; MPR->argv[i]; i++) {
        printf("%s ", MPR->argv[i]);
    }
    printf("\n");
#else
    mprError("mprRestart not supported on this platform");
#endif
}


PUBLIC int mprStart()
{
    int     rc;

    rc = mprStartOsService();
    rc += mprStartModuleService();
    rc += mprStartWorkerService();
    if (rc != 0) {
        mprUserError("Can't start MPR services");
        return MPR_ERR_CANT_INITIALIZE;
    }
    MPR->state = MPR_STARTED;
    mprLog(MPR_INFO, "MPR services are ready");
    return 0;
}


PUBLIC int mprStartEventsThread()
{
    MprThread   *tp;

    if ((tp = mprCreateThread("events", serviceEventsThread, NULL, 0)) == 0) {
        MPR->hasError = 1;
    } else {
        MPR->cond = mprCreateCond();
        mprStartThread(tp);
        mprWaitForCond(MPR->cond, MPR_TIMEOUT_START_TASK);
    }
    return 0;
}


static void serviceEventsThread(void *data, MprThread *tp)
{
    mprLog(MPR_CONFIG, "Service thread started");
    if (!(MPR->flags & MPR_NO_WINDOW)) {
        mprInitWindow();
    }
    mprSignalCond(MPR->cond);
    mprServiceEvents(-1, 0);
}


/*
    Services should call this to determine if they should accept new services
 */
PUBLIC bool mprShouldAbortRequests()
{
    return (mprIsStopping() && !(MPR->exitStrategy & MPR_EXIT_GRACEFUL));
}


PUBLIC bool mprShouldDenyNewRequests()
{
    return mprIsStopping();
}


PUBLIC bool mprIsStopping()
{
    return MPR->state >= MPR_STOPPING;
}


PUBLIC bool mprIsStoppingCore()
{
    return MPR->state >= MPR_STOPPING_CORE;
}


PUBLIC bool mprIsFinished()
{
    return MPR->state >= MPR_FINISHED;
}


PUBLIC int mprWaitTillIdle(MprTime timeout)
{
    MprTime     mark, remaining, lastTrace;

    lastTrace = mark = mprGetTime(); 
    while (!mprIsIdle() && (remaining = mprGetRemainingTime(mark, timeout)) > 0) {
        mprSleep(1);
        if ((lastTrace - remaining) > MPR_TICKS_PER_SEC) {
            mprLog(1, "Waiting for requests to complete, %d secs remaining ...", remaining / MPR_TICKS_PER_SEC);
            lastTrace = remaining;
        }
    }
    return mprIsIdle();
}


/*
    Test if the Mpr services are idle. Use mprIsIdle to determine if the entire process is idle.
 */
PUBLIC bool mprServicesAreIdle()
{
    bool    idle;

    /*
        Only test top level services. Dispatchers may have timers scheduled, but that is okay.
        If not, users can install their own idleCallback.
     */
    idle = mprGetListLength(MPR->workerService->busyThreads) == 0 && mprGetListLength(MPR->cmdService->cmds) == 0;
    if (!idle) {
        mprLog(6, "Not idle: cmds %d, busy threads %d, eventing %d",
            mprGetListLength(MPR->cmdService->cmds), mprGetListLength(MPR->workerService->busyThreads), MPR->eventing);
    }
    return idle;
}


PUBLIC bool mprIsIdle()
{
    return (MPR->idleCallback)();
}


/*
    Parse the args and return the count of args. If argv is NULL, the args are parsed read-only. If argv is set,
    then the args will be extracted, back-quotes removed and argv will be set to point to all the args.
    NOTE: this routine does not allocate.
 */
PUBLIC int mprParseArgs(char *args, char **argv, int maxArgc)
{
    char    *dest, *src, *start;
    int     quote, argc;

    /*
        Example     "showColors" red 'light blue' "yellow white" 'Can\'t \"render\"'
        Becomes:    ["showColors", "red", "light blue", "yellow white", "Can't \"render\""]
     */
    for (argc = 0, src = args; src && *src != '\0' && argc < maxArgc; argc++) {
        while (isspace((uchar) *src)) {
            src++;
        }
        if (*src == '\0')  {
            break;
        }
        start = dest = src;
        if (*src == '"' || *src == '\'') {
            quote = *src;
            src++; 
            dest++;
        } else {
            quote = 0;
        }
        if (argv) {
            argv[argc] = src;
        }
        while (*src) {
            if (*src == '\\' && src[1] && (src[1] == '\\' || src[1] == '"' || src[1] == '\'')) { 
                src++;
            } else {
                if (quote) {
                    if (*src == quote && !(src > start && src[-1] == '\\')) {
                        break;
                    }
                } else if (*src == ' ') {
                    break;
                }
            }
            if (argv) {
                *dest++ = *src;
            }
            src++;
        }
        if (*src != '\0') {
            src++;
        }
        if (argv) {
            *dest++ = '\0';
        }
    }
    return argc;
}


/*
    Make an argv array. All args are in a single memory block of which argv points to the start.
    Set MPR_ARGV_ARGS_ONLY if not passing in a program name. 
    Always returns and argv[0] reserved for the program name or empty string.  First arg starts at argv[1].
 */
PUBLIC int mprMakeArgv(cchar *command, cchar ***argvp, int flags)
{
    char    **argv, *vector, *args;
    ssize   len;
    int     argc;

    mprAssert(command);

    /*
        Allocate one vector for argv and the actual args themselves
     */
    len = slen(command) + 1;
    argc = mprParseArgs((char*) command, NULL, INT_MAX);
    if (flags & MPR_ARGV_ARGS_ONLY) {
        argc++;
    }
    if ((vector = (char*) mprAlloc(((argc + 1) * sizeof(char*)) + len)) == 0) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    args = &vector[(argc + 1) * sizeof(char*)];
    strcpy(args, command);
    argv = (char**) vector;

    if (flags & MPR_ARGV_ARGS_ONLY) {
        mprParseArgs(args, &argv[1], argc);
        argv[0] = MPR->emptyString;
    } else {
        mprParseArgs(args, argv, argc);
    }
    argv[argc] = 0;
    *argvp = (cchar**) argv;
    return argc;
}


PUBLIC MprIdleCallback mprSetIdleCallback(MprIdleCallback idleCallback)
{
    MprIdleCallback old;
    
    old = MPR->idleCallback;
    MPR->idleCallback = idleCallback;
    return old;
}


PUBLIC int mprSetAppName(cchar *name, cchar *title, cchar *version)
{
    char    *cp;

    if (name) {
        if ((MPR->name = (char*) mprGetPathBase(name)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
        if ((cp = strrchr(MPR->name, '.')) != 0) {
            *cp = '\0';
        }
    }
    if (title) {
        if ((MPR->title = sclone(title)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    if (version) {
        if ((MPR->version = sclone(version)) == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    return 0;
}


PUBLIC cchar *mprGetAppName()
{
    return MPR->name;
}


PUBLIC cchar *mprGetAppTitle()
{
    return MPR->title;
}


/*
    Full host name with domain. E.g. "server.domain.com"
 */
PUBLIC void mprSetHostName(cchar *s)
{
    MPR->hostName = sclone(s);
}


/*
    Return the fully qualified host name
 */
PUBLIC cchar *mprGetHostName()
{
    return MPR->hostName;
}


/*
    Server name portion (no domain name)
 */
PUBLIC void mprSetServerName(cchar *s)
{
    MPR->serverName = sclone(s);
}


PUBLIC cchar *mprGetServerName()
{
    return MPR->serverName;
}


PUBLIC void mprSetDomainName(cchar *s)
{
    MPR->domainName = sclone(s);
}


PUBLIC cchar *mprGetDomainName()
{
    return MPR->domainName;
}


/*
    Set the IP address
 */
PUBLIC void mprSetIpAddr(cchar *s)
{
    MPR->ip = sclone(s);
}


/*
    Return the IP address
 */
PUBLIC cchar *mprGetIpAddr()
{
    return MPR->ip;
}


PUBLIC cchar *mprGetAppVersion()
{
    return MPR->version;
}


PUBLIC bool mprGetDebugMode()
{
    return MPR->debugMode;
}


PUBLIC void mprSetDebugMode(bool on)
{
    MPR->debugMode = on;
}


PUBLIC MprDispatcher *mprGetDispatcher()
{
    return MPR->dispatcher;
}


PUBLIC MprDispatcher *mprGetNonBlockDispatcher()
{
    return MPR->nonBlock;
}


PUBLIC cchar *mprCopyright()
{
    return  "Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.\n"
            "Copyright (c) Michael O'Brien, 1993-2012. All Rights Reserved.";
}


PUBLIC int mprGetEndian()
{
    char    *probe;
    int     test;

    test = 1;
    probe = (char*) &test;
    return (*probe == 1) ? MPR_LITTLE_ENDIAN : MPR_BIG_ENDIAN;
}


PUBLIC char *mprEmptyString()
{
    return MPR->emptyString;
}


PUBLIC void mprSetExitStrategy(int strategy)
{
    MPR->exitStrategy = strategy;
}


PUBLIC void mprSetEnv(cchar *key, cchar *value)
{
#if !WINCE
#if BIT_UNIX_LIKE
    setenv(key, value, 1);
#else
    char *cmd = sjoin(key, "=", value, NULL);
    putenv(cmd);
#endif
#endif
    if (scaselessmatch(key, "PATH")) {
        MPR->pathEnv = sclone(value);
    }
}


PUBLIC void mprSetExitTimeout(MprTime timeout)
{
    MPR->exitTimeout = timeout;
}


PUBLIC void mprNop(void *ptr) {}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprAsync.c"
 */
/************************************************************************/

/**
    mprAsync.c - Wait for I/O on Windows.

    This module provides io management for sockets on Windows like systems. 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if MPR_EVENT_ASYNC
/***************************** Forward Declarations ***************************/

static LRESULT msgProc(HWND hwnd, UINT msg, UINT wp, LPARAM lp);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{   
    ws->socketMessage = MPR_SOCKET_MESSAGE;
    return 0;
}


PUBLIC int mprNotifyOn(MprWaitService *ws, MprWaitHandler *wp, int mask)
{
    int     winMask;

    mprAssert(ws->hwnd);

    lock(ws);
    winMask = 0;
    if (wp->desiredMask != mask) {
        if (mask & MPR_READABLE) {
            winMask |= FD_ACCEPT | FD_CONNECT | FD_CLOSE | FD_READ;
        }
        if (mask & MPR_WRITABLE) {
            winMask |= FD_WRITE;
        }
        wp->desiredMask = mask;
        WSAAsyncSelect(wp->fd, ws->hwnd, ws->socketMessage, winMask);
    }
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single descriptor. Return the number of I/O events found. Mask is the events of interest.
    Timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int desiredMask, MprTime timeout)
{
    HANDLE      h;
    int         winMask;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
    winMask = 0;
    if (desiredMask & MPR_READABLE) {
        winMask |= FD_CLOSE | FD_READ;
    }
    if (desiredMask & MPR_WRITABLE) {
        winMask |= FD_WRITE;
    }
    h = CreateEvent(NULL, FALSE, FALSE, T("mprWaitForSingleIO"));
    WSAEventSelect(fd, h, winMask);
    if (WaitForSingleObject(h, (DWORD) timeout) == WAIT_OBJECT_0) {
        CloseHandle(h);
        return desiredMask;
    }
    CloseHandle(h);
    return 0;
}


/*
    Wait for I/O on all registered descriptors. Timeout is in milliseconds. Return the number of events serviced.
    Should only be called by the thread that calls mprServiceEvents
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTime timeout)
{
    MSG     msg;

    mprAssert(ws->hwnd);

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if BIT_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    SetTimer(ws->hwnd, 0, (UINT) timeout, NULL);

    mprYield(MPR_YIELD_STICKY);
    if (GetMessage(&msg, NULL, 0, 0) == 0) {
        mprResetYield();
        mprTerminate(MPR_EXIT_DEFAULT, -1);
    } else {
        mprResetYield();
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    ws->wakeRequested = 0;
}


PUBLIC void mprServiceWinIO(MprWaitService *ws, int sockFd, int winMask)
{
    MprWaitHandler      *wp;
    int                 index;

    lock(ws);

    for (index = 0; (wp = (MprWaitHandler*) mprGetNextItem(ws->handlers, &index)) != 0; ) {
        if (wp->fd == sockFd) {
            break;
        }
    }
    if (wp == 0) {
        /* If the server forcibly closed the socket, we may still get a read event. Just ignore it.  */
        unlock(ws);
        return;
    }
    /*
        Mask values: READ==1, WRITE=2, ACCEPT=8, CONNECT=10, CLOSE=20
     */
    wp->presentMask = 0;
    if (winMask & (FD_READ | FD_ACCEPT | FD_CLOSE)) {
        wp->presentMask |= MPR_READABLE;
    }
    if (winMask & (FD_WRITE | FD_CONNECT)) {
        wp->presentMask |= MPR_WRITABLE;
    }
    wp->presentMask &= wp->desiredMask;
    if (wp->presentMask) {
        if (wp->presentMask) {
            mprNotifyOn(ws, wp, 0);
            mprQueueIOEvent(wp);
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;
   
    ws = MPR->waitService;
    if (!ws->wakeRequested && ws->hwnd) {
        ws->wakeRequested = 1;
        PostMessage(ws->hwnd, WM_NULL, 0, 0L);
    }
}


/*
    Create a default window if the application has not already created one.
 */ 
PUBLIC int mprInitWindow()
{
    MprWaitService  *ws;
    WNDCLASS        wc;
    HWND            hwnd;
	wchar			*name, *title;
    int             rc;

    ws = MPR->waitService;
    if (ws->hwnd) {
        return 0;
    }
	name = (wchar*) wide(mprGetAppName());
	title = (wchar*) wide(mprGetAppTitle());
    wc.style            = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground    = (HBRUSH) (COLOR_WINDOW+1);
    wc.hCursor          = LoadCursor(NULL, IDC_ARROW);
    wc.cbClsExtra       = 0;
    wc.cbWndExtra       = 0;
    wc.hInstance        = 0;
    wc.hIcon            = NULL;
    wc.lpfnWndProc      = (WNDPROC) msgProc;
    wc.lpszMenuName     = wc.lpszClassName = name;

    rc = RegisterClass(&wc);
    if (rc == 0) {
        mprError("Can't register windows class");
        return MPR_ERR_CANT_INITIALIZE;
    }
    hwnd = CreateWindow(name, title, WS_OVERLAPPED, CW_USEDEFAULT, 0, 0, 0, NULL, NULL, 0, NULL);
    if (!hwnd) {
        mprError("Can't create window");
        return -1;
    }
    ws->hwnd = hwnd;
    ws->socketMessage = MPR_SOCKET_MESSAGE;
    return 0;
}


/*
    Windows message processing loop for wakeup and socket messages
 */
static LRESULT msgProc(HWND hwnd, UINT msg, UINT wp, LPARAM lp)
{
    MprWaitService      *ws;
    int                 sock, winMask;

    ws = MPR->waitService;

    if (msg == WM_DESTROY || msg == WM_QUIT) {
        mprTerminate(MPR_EXIT_DEFAULT, -1);

    } else if (msg && msg == ws->socketMessage) {
        sock = wp;
        winMask = LOWORD(lp);
        mprServiceWinIO(MPR->waitService, sock, winMask);

    } else if (ws->msgCallback) {
        return ws->msgCallback(hwnd, msg, wp, lp);

    } else {
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}


PUBLIC void mprSetWinMsgCallback(MprMsgCallback callback)
{
    MprWaitService  *ws;

    ws = MPR->waitService;
    ws->msgCallback = callback;
}


#else
PUBLIC void stubMprAsync() {}
#endif /* MPR_EVENT_ASYNC */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprAtomic.c"
 */
/************************************************************************/

/**
    mprAtomic.c - Atomic operations

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/*********************************** Includes *********************************/



/************************************ Code ************************************/

PUBLIC void mprAtomicBarrier()
{
    #ifdef VX_MEM_BARRIER_RW
        VX_MEM_BARRIER_RW();
    #elif MACOSX
        OSMemoryBarrier();
    #elif BIT_WIN_LIKE
        MemoryBarrier();
    #elif BIT_HAS_SYNC
        __sync_synchronize();
    #elif __GNUC__ && (BIT_CPU_ARCH == MPR_CPU_X86 || BIT_CPU_ARCH == MPR_CPU_X64)
        asm volatile ("mfence" : : : "memory");
    #elif __GNUC__ && (BIT_CPU_ARCH == MPR_CPU_PPC)
        asm volatile ("sync" : : : "memory");
    #else
        getpid();
    #endif

#if FUTURE && KEEP
    asm volatile ("lock; add %eax,0");
#endif
}


/*
    Atomic Compare and swap a pointer with a full memory barrier
 */
PUBLIC int mprAtomicCas(void * volatile *addr, void *expected, cvoid *value)
{
    #if MACOSX
        return OSAtomicCompareAndSwapPtrBarrier(expected, (void*) value, (void*) addr);
    #elif BIT_WIN_LIKE
        {
            void *prev;
            prev = InterlockedCompareExchangePointer(addr, (void*) value, expected);
            return expected == prev;
        }
    #elif BIT_HAS_SYNC_CAS
        return __sync_bool_compare_and_swap(addr, expected, value);
    #elif VXWORKS && _VX_ATOMIC_INIT && !BIT_64
        /* vxCas operates with integer values */
        return vxCas((atomic_t*) addr, (atomicVal_t) expected, (atomicVal_t) value);
    #elif BIT_CPU_ARCH == MPR_CPU_X86
        {
            void *prev;
            asm volatile ("lock; cmpxchgl %2, %1"
                : "=a" (prev), "=m" (*addr)
                : "r" (value), "m" (*addr), "0" (expected));
            return expected == prev;
        }
    #elif BIT_CPU_ARCH == MPR_CPU_X64
        {
            void *prev;
            asm volatile ("lock; cmpxchgq %q2, %1"
                : "=a" (prev), "=m" (*addr)
                : "r" (value), "m" (*addr),
                  "0" (expected));
            return expected == prev;
        }
    #else
        mprGlobalLock();
        if (*addr == expected) {
            *addr = value;
            mprGlobalUnlock();
            return 1;
        }
        mprGlobalUnlock();
        return 0;
    #endif
}


/*
    Atomic add of a signed value. Used for add, subtract, inc, dec
 */
PUBLIC void mprAtomicAdd(volatile int *ptr, int value)
{
    #if MACOSX
        OSAtomicAdd32(value, ptr);
    #elif BIT_WIN_LIKE
        InterlockedExchangeAdd(ptr, value);
    #elif VXWORKS && _VX_ATOMIC_INIT
        vxAtomicAdd(ptr, value);
    #elif (BIT_CPU_ARCH == MPR_CPU_X86 || BIT_CPU_ARCH == MPR_CPU_X64) && FUTURE
        asm volatile ("lock; xaddl %0,%1"
            : "=r" (value), "=m" (*ptr)
            : "0" (value), "m" (*ptr)
            : "memory", "cc");
    #else
        mprGlobalLock();
        *ptr += value;
        mprGlobalUnlock();
    #endif
}


/*
    On some platforms, this operation is only atomic with respect to other calls to mprAtomicAdd64
 */
PUBLIC void mprAtomicAdd64(volatile int64 *ptr, int value)
{
#if MACOSX
    OSAtomicAdd64(value, ptr);
#elif BIT_WIN_LIKE && BIT_64
    InterlockedExchangeAdd64(ptr, value);
#elif BIT_UNIX_LIKE && FUTURE
    asm volatile ("lock; xaddl %0,%1"
        : "=r" (value), "=m" (*ptr)
        : "0" (value), "m" (*ptr)
        : "memory", "cc");
#else
    mprGlobalLock();
    *ptr += value;
    mprGlobalUnlock();
#endif
}


PUBLIC void *mprAtomicExchange(void * volatile *addr, cvoid *value)
{
#if MACOSX && 0
    return OSAtomicCompareAndSwapPtrBarrier(expected, value, addr);
#elif BIT_WIN_LIKE
    return (void*) InterlockedExchange((volatile LONG*) addr, (LONG) value);
#elif BIT_UNIX_LIKE && FUTURE
    return __sync_lock_test_and_set(addr, value);
#else
    {
        void    *old;
        mprGlobalLock();
        old = * (void**) addr;
        *addr = (void*) value;
        mprGlobalUnlock();
        return old;
    }
#endif
}


/*
    Atomic list insertion. Inserts "item" at the "head" of the list. The "link" field is the next field in item.
 */
PUBLIC void mprAtomicListInsert(void * volatile *head, volatile void **link, void *item)
{
    do {
        *link = *head;
    } while (mprAtomicCas(head, (void*) *link, item));
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprBuf.c"
 */
/************************************************************************/

/**
    mprBuf.c - Dynamic buffer module

    This module is not thread-safe for performance. Callers must do their own locking.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Forwards **********************************/

static void manageBuf(MprBuf *buf, int flags);

/*********************************** Code *************************************/
/*
    Create a new buffer. "maxsize" is the limit to which the buffer can ever grow. -1 means no limit. "initialSize" is 
    used to define the amount to increase the size of the buffer each time if it becomes full. (Note: mprGrowBuf() will 
    exponentially increase this number for performance.)
 */
PUBLIC MprBuf *mprCreateBuf(ssize initialSize, ssize maxSize)
{
    MprBuf      *bp;
    
    if (initialSize <= 0) {
        initialSize = MPR_BUFSIZE;
    }
    if ((bp = mprAllocObj(MprBuf, manageBuf)) == 0) {
        return 0;
    }
    bp->growBy = MPR_BUFSIZE;
    mprSetBufSize(bp, initialSize, maxSize);
    return bp;
}


static void manageBuf(MprBuf *bp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(bp->data);
        mprMark(bp->refillArg);
    } 
}


PUBLIC MprBuf *mprCloneBuf(MprBuf *orig)
{
    MprBuf      *bp;
    ssize       len;

    if ((bp = mprCreateBuf(orig->growBy, orig->maxsize)) == 0) {
        return 0;
    }
    bp->refillProc = orig->refillProc;
    bp->refillArg = orig->refillArg;
    if ((len = mprGetBufLength(orig)) > 0) {
        memcpy(bp->data, orig->data, len);
    }
    return bp;
}


PUBLIC char *mprGet(MprBuf *bp)
{
    return (char*) bp->start;
}


/*
    Set the current buffer size and maximum size limit.
 */
PUBLIC int mprSetBufSize(MprBuf *bp, ssize initialSize, ssize maxSize)
{
    mprAssert(bp);

    if (initialSize <= 0) {
        if (maxSize > 0) {
            bp->maxsize = maxSize;
        }
        return 0;
    }
    if (maxSize > 0 && initialSize > maxSize) {
        initialSize = maxSize;
    }
    mprAssert(initialSize > 0);

    if (bp->data) {
        /*
            Buffer already exists
         */
        if (bp->buflen < initialSize) {
            if (mprGrowBuf(bp, initialSize - bp->buflen) < 0) {
                return MPR_ERR_MEMORY;
            }
        }
        bp->maxsize = maxSize;
        return 0;
    }
    if ((bp->data = mprAlloc(initialSize)) == 0) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    bp->growBy = initialSize;
    bp->maxsize = maxSize;
    bp->buflen = initialSize;
    bp->endbuf = &bp->data[bp->buflen];
    bp->start = bp->data;
    bp->end = bp->data;
    *bp->start = '\0';
    return 0;
}


PUBLIC void mprSetBufMax(MprBuf *bp, ssize max)
{
    bp->maxsize = max;
}


/*
    This appends a silent null. It does not count as one of the actual bytes in the buffer
 */
PUBLIC void mprAddNullToBuf(MprBuf *bp)
{
    ssize      space;

    space = bp->endbuf - bp->end;
    if (space < sizeof(char)) {
        if (mprGrowBuf(bp, 1) < 0) {
            return;
        }
    }
    mprAssert(bp->end < bp->endbuf);
    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
}


PUBLIC void mprAdjustBufEnd(MprBuf *bp, ssize size)
{
    mprAssert(bp->buflen == (bp->endbuf - bp->data));
    mprAssert(size <= bp->buflen);
    mprAssert((bp->end + size) >= bp->data);
    mprAssert((bp->end + size) <= bp->endbuf);

    bp->end += size;
    if (bp->end > bp->endbuf) {
        mprAssert(bp->end <= bp->endbuf);
        bp->end = bp->endbuf;
    }
    if (bp->end < bp->data) {
        bp->end = bp->data;
    }
}


/*
    Adjust the start pointer after a user copy. Note: size can be negative.
 */
PUBLIC void mprAdjustBufStart(MprBuf *bp, ssize size)
{
    mprAssert(bp->buflen == (bp->endbuf - bp->data));
    mprAssert(size <= bp->buflen);
    mprAssert((bp->start + size) >= bp->data);
    mprAssert((bp->start + size) <= bp->end);

    bp->start += size;
    if (bp->start > bp->end) {
        bp->start = bp->end;
    }
    if (bp->start <= bp->data) {
        bp->start = bp->data;
    }
}


PUBLIC void mprFlushBuf(MprBuf *bp)
{
    bp->start = bp->data;
    bp->end = bp->data;
}


PUBLIC int mprGetCharFromBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return (uchar) *bp->start++;
}


PUBLIC ssize mprGetBlockFromBuf(MprBuf *bp, char *buf, ssize size)
{
    ssize     thisLen, bytesRead;

    mprAssert(buf);
    mprAssert(size >= 0);
    mprAssert(bp->buflen == (bp->endbuf - bp->data));

    /*
        Get the max bytes in a straight copy
     */
    bytesRead = 0;
    while (size > 0) {
        thisLen = mprGetBufLength(bp);
        thisLen = min(thisLen, size);
        if (thisLen <= 0) {
            break;
        }

        memcpy(buf, bp->start, thisLen);
        buf += thisLen;
        bp->start += thisLen;
        size -= thisLen;
        bytesRead += thisLen;
    }
    return bytesRead;
}


#ifndef mprGetBufLength
PUBLIC ssize mprGetBufLength(MprBuf *bp)
{
    return (bp->end - bp->start);
}
#endif


#ifndef mprGetBufSize
PUBLIC ssize mprGetBufSize(MprBuf *bp)
{
    return bp->buflen;
}
#endif


#ifndef mprGetBufSpace
PUBLIC ssize mprGetBufSpace(MprBuf *bp)
{
    return (bp->endbuf - bp->end);
}
#endif


#ifndef mprGetBuf
PUBLIC char *mprGetBuf(MprBuf *bp)
{
    return (char*) bp->data;
}
#endif


#ifndef mprGetBufStart
PUBLIC char *mprGetBufStart(MprBuf *bp)
{
    return (char*) bp->start;
}
#endif


#ifndef mprGetBufEnd
PUBLIC char *mprGetBufEnd(MprBuf *bp)
{
    return (char*) bp->end;
}
#endif


PUBLIC int mprInsertCharToBuf(MprBuf *bp, int c)
{
    if (bp->start == bp->data) {
        return MPR_ERR_BAD_STATE;
    }
    *--bp->start = c;
    return 0;
}


PUBLIC int mprLookAtNextCharInBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return *bp->start;
}


PUBLIC int mprLookAtLastCharInBuf(MprBuf *bp)
{
    if (bp->start == bp->end) {
        return -1;
    }
    return bp->end[-1];
}


PUBLIC int mprPutCharToBuf(MprBuf *bp, int c)
{
    char       *cp;
    ssize      space;

    mprAssert(bp->buflen == (bp->endbuf - bp->data));

    space = bp->buflen - mprGetBufLength(bp);
    if (space < sizeof(char)) {
        if (mprGrowBuf(bp, 1) < 0) {
            return -1;
        }
    }
    cp = (char*) bp->end;
    *cp++ = (char) c;
    bp->end = (char*) cp;

    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return 1;
}


/*
    Return the number of bytes written to the buffer. If no more bytes will fit, may return less than size.
    Never returns < 0.
 */
PUBLIC ssize mprPutBlockToBuf(MprBuf *bp, cchar *str, ssize size)
{
    ssize      thisLen, bytes, space;

    mprAssert(str);
    mprAssert(size >= 0);
    mprAssert(size < MAXINT);

    bytes = 0;
    while (size > 0) {
        space = mprGetBufSpace(bp);
        thisLen = min(space, size);
        if (thisLen <= 0) {
            if (mprGrowBuf(bp, size) < 0) {
                break;
            }
            space = mprGetBufSpace(bp);
            thisLen = min(space, size);
        }
        memcpy(bp->end, str, thisLen);
        str += thisLen;
        bp->end += thisLen;
        size -= thisLen;
        bytes += thisLen;
    }
    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return bytes;
}


PUBLIC ssize mprPutStringToBuf(MprBuf *bp, cchar *str)
{
    if (str) {
        return mprPutBlockToBuf(bp, str, slen(str));
    }
    return 0;
}


PUBLIC ssize mprPutSubStringToBuf(MprBuf *bp, cchar *str, ssize count)
{
    ssize     len;

    if (str) {
        len = slen(str);
        len = min(len, count);
        if (len > 0) {
            return mprPutBlockToBuf(bp, str, len);
        }
    }
    return 0;
}


PUBLIC ssize mprPutPadToBuf(MprBuf *bp, int c, ssize count)
{
    mprAssert(count < MAXINT);

    while (count-- > 0) {
        if (mprPutCharToBuf(bp, c) < 0) {
            return -1;
        }
    }
    return count;
}


PUBLIC ssize mprPutFmtToBuf(MprBuf *bp, cchar *fmt, ...)
{
    va_list     ap;
    char        *buf;

    if (fmt == 0) {
        return 0;
    }
    va_start(ap, fmt);
    buf = sfmtv(fmt, ap);
    va_end(ap);
    return mprPutStringToBuf(bp, buf);
}


/*
    Grow the buffer. Return 0 if the buffer grows. Increase by the growBy size specified when creating the buffer. 
 */
PUBLIC int mprGrowBuf(MprBuf *bp, ssize need)
{
    char    *newbuf;
    ssize   growBy;

    if (bp->maxsize > 0 && bp->buflen >= bp->maxsize) {
        return MPR_ERR_TOO_MANY;
    }
    if (bp->start > bp->end) {
        mprCompactBuf(bp);
    }
    if (need > 0) {
        growBy = max(bp->growBy, need);
    } else {
        growBy = bp->growBy;
    }
    if ((newbuf = mprAlloc(bp->buflen + growBy)) == 0) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    if (bp->data) {
        memcpy(newbuf, bp->data, bp->buflen);
    }
    bp->buflen += growBy;
    bp->end = newbuf + (bp->end - bp->data);
    bp->start = newbuf + (bp->start - bp->data);
    bp->data = newbuf;
    bp->endbuf = &bp->data[bp->buflen];

    /*
        Increase growBy to reduce overhead
     */
    if (bp->maxsize > 0) {
        if ((bp->buflen + (bp->growBy * 2)) > bp->maxsize) {
            bp->growBy = min(bp->maxsize - bp->buflen, bp->growBy * 2);
        }
    } else {
        if ((bp->buflen + (bp->growBy * 2)) > bp->maxsize) {
            bp->growBy = min(bp->buflen, bp->growBy * 2);
        }
    }
    return 0;
}


/*
    Add a number to the buffer (always null terminated).
 */
PUBLIC ssize mprPutIntToBuf(MprBuf *bp, int64 i)
{
    ssize       rc;

    rc = mprPutStringToBuf(bp, itos(i));
    if (bp->end < bp->endbuf) {
        *((char*) bp->end) = (char) '\0';
    }
    return rc;
}


PUBLIC void mprCompactBuf(MprBuf *bp)
{
    if (mprGetBufLength(bp) == 0) {
        mprFlushBuf(bp);
        return;
    }
    if (bp->start > bp->data) {
        memmove(bp->data, bp->start, (bp->end - bp->start));
        bp->end -= (bp->start - bp->data);
        bp->start = bp->data;
    }
}


PUBLIC MprBufProc mprGetBufRefillProc(MprBuf *bp) 
{
    return bp->refillProc;
}


PUBLIC void mprSetBufRefillProc(MprBuf *bp, MprBufProc fn, void *arg)
{ 
    bp->refillProc = fn; 
    bp->refillArg = arg; 
}


PUBLIC int mprRefillBuf(MprBuf *bp) 
{ 
    return (bp->refillProc) ? (bp->refillProc)(bp, bp->refillArg) : 0; 
}


PUBLIC void mprResetBufIfEmpty(MprBuf *bp)
{
    if (mprGetBufLength(bp) == 0) {
        mprFlushBuf(bp);
    }
}


#if BIT_CHAR_LEN > 1 && UNUSED
PUBLIC void mprAddNullToWideBuf(MprBuf *bp)
{
    ssize      space;

    space = bp->endbuf - bp->end;
    if (space < sizeof(wchar)) {
        if (mprGrowBuf(bp, sizeof(wchar)) < 0) {
            return;
        }
    }
    mprAssert(bp->end < bp->endbuf);
    if (bp->end < bp->endbuf) {
        *((wchar*) bp->end) = (char) '\0';
    }
}


PUBLIC int mprPutCharToWideBuf(MprBuf *bp, int c)
{
    wchar *cp;
    ssize   space;

    mprAssert(bp->buflen == (bp->endbuf - bp->data));

    space = bp->buflen - mprGetBufLength(bp);
    if (space < (sizeof(wchar) * 2)) {
        if (mprGrowBuf(bp, sizeof(wchar) * 2) < 0) {
            return -1;
        }
    }
    cp = (wchar*) bp->end;
    *cp++ = (wchar) c;
    bp->end = (char*) cp;

    if (bp->end < bp->endbuf) {
        *((wchar*) bp->end) = (char) '\0';
    }
    return 1;
}


PUBLIC ssize mprPutFmtToWideBuf(MprBuf *bp, cchar *fmt, ...)
{
    va_list     ap;
    wchar     *wbuf;
    char        *buf;
    ssize       len, space;
    ssize       rc;

    if (fmt == 0) {
        return 0;
    }
    va_start(ap, fmt);
    space = mprGetBufSpace(bp);
    space += (bp->maxsize - bp->buflen);
    buf = sfmtv(fmt, ap);
    wbuf = amtow(buf, &len);
    rc = mprPutBlockToBuf(bp, (char*) wbuf, len * sizeof(wchar));
    va_end(ap);
    return rc;
}


PUBLIC ssize mprPutStringToWideBuf(MprBuf *bp, cchar *str)
{
    wchar     *wstr;
    ssize       len;

    if (str) {
        wstr = amtow(str, &len);
        return mprPutBlockToBuf(bp, (char*) wstr, len);
    }
    return 0;
}

#endif /* BIT_CHAR_LEN > 1 */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprCache.c"
 */
/************************************************************************/

/**
    mprCache.c - In-process caching

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/************************************ Locals **********************************/

static MprCache *shared;                /* Singleton shared cache */

typedef struct CacheItem
{
    char        *key;                   /* Original key */
    char        *data;                  /* Cache data */
    MprTime     lastAccessed;           /* Last accessed time */
    MprTime     lastModified;           /* Last update time */
    MprTime     expires;                /* Fixed expiry date. If zero, key is imortal */
    MprTime     lifespan;               /* Lifespan after each access to key (msec) */
    int64       version;
} CacheItem;

#define CACHE_TIMER_PERIOD      (60 * MPR_TICKS_PER_SEC)
#define CACHE_HASH_SIZE         257
#define CACHE_LIFESPAN          (86400 * MPR_TICKS_PER_SEC)

/*********************************** Forwards *********************************/

static void manageCache(MprCache *cache, int flags);
static void manageCacheItem(CacheItem *item, int flags);
static void pruneCache(MprCache *cache, MprEvent *event);
static void removeItem(MprCache *cache, CacheItem *item);

/************************************* Code ***********************************/

PUBLIC MprCache *mprCreateCache(int options)
{
    MprCache    *cache;
    int         wantShared;

    if ((cache = mprAllocObj(MprCache, manageCache)) == 0) {
        return 0;
    }
    wantShared = (options & MPR_CACHE_SHARED);
    if (wantShared && shared) {
        cache->shared = shared;
    } else {
        cache->mutex = mprCreateLock();
        cache->store = mprCreateHash(CACHE_HASH_SIZE, 0);
        cache->maxMem = MAXSSIZE;
        cache->maxKeys = MAXSSIZE;
        cache->resolution = CACHE_TIMER_PERIOD;
        cache->lifespan = CACHE_LIFESPAN;
        if (wantShared) {
            shared = cache;
        }
    }
    return cache;
}


PUBLIC void *mprDestroyCache(MprCache *cache)
{
    mprAssert(cache);

    if (cache->timer && cache != shared) {
        mprRemoveEvent(cache->timer);
        cache->timer = 0;
    }
    if (cache == shared) {
        shared = 0;
    }
    return 0;
}


PUBLIC int mprExpireCache(MprCache *cache, cchar *key, MprTime expires)
{
    CacheItem   *item;

    mprAssert(cache);
    mprAssert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        unlock(cache);
        return MPR_ERR_CANT_FIND;
    }
    if (expires == 0) {
        removeItem(cache, item);
    } else {
        item->expires = expires;
    }
    unlock(cache);
    return 0;
}


PUBLIC int64 mprIncCache(MprCache *cache, cchar *key, int64 amount)
{
    CacheItem   *item;
    int64       value;

    mprAssert(cache);
    mprAssert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    value = amount;

    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        if ((item = mprAllocObj(CacheItem, manageCacheItem)) == 0) {
            return 0;
        }
    } else {
        value += stoi(item->data);
    }
    if (item->data) {
        cache->usedMem -= slen(item->data);
    }
    item->data = itos(value);
    cache->usedMem += slen(item->data);
    item->version++;
    item->lastAccessed = mprGetTime();
    item->expires = item->lastAccessed + item->lifespan;
    unlock(cache);
    return value;
}


PUBLIC char *mprReadCache(MprCache *cache, cchar *key, MprTime *modified, int64 *version)
{
    CacheItem   *item;
    char        *result;

    mprAssert(cache);
    mprAssert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    lock(cache);
    if ((item = mprLookupKey(cache->store, key)) == 0) {
        unlock(cache);
        return 0;
    }
    if (item->expires && item->expires <= mprGetTime()) {
        unlock(cache);
        return 0;
    }
    if (version) {
        *version = item->version;
    }
    if (modified) {
        *modified = item->lastModified;
    }
    item->lastAccessed = mprGetTime();
    item->expires = item->lastAccessed + item->lifespan;
    result = item->data;
    unlock(cache);
    return result;
}


PUBLIC bool mprRemoveCache(MprCache *cache, cchar *key)
{
    CacheItem   *item;
    bool        result;

    mprAssert(cache);
    mprAssert(key && *key);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    lock(cache);
    if (key) {
        if ((item = mprLookupKey(cache->store, key)) != 0) {
            cache->usedMem -= (slen(key) + slen(item->data));
            mprRemoveKey(cache->store, key);
            result = 1;
        } else {
            result = 0;
        }

    } else {
        /* Remove all keys */
        result = mprGetHashLength(cache->store) ? 1 : 0;
        cache->store = mprCreateHash(CACHE_HASH_SIZE, 0);
        cache->usedMem = 0;
    }
    unlock(cache);
    return result;
}


PUBLIC void mprSetCacheLimits(MprCache *cache, int64 keys, MprTime lifespan, int64 memory, int resolution)
{
    mprAssert(cache);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    if (keys > 0) {
        cache->maxKeys = (ssize) keys;
        if (cache->maxKeys <= 0) {
            cache->maxKeys = MAXSSIZE;
        }
    }
    if (lifespan > 0) {
        cache->lifespan = lifespan;
    }
    if (memory > 0) {
        cache->maxMem = (ssize) memory;
        if (cache->maxMem <= 0) {
            cache->maxMem = MAXSSIZE;
        }
    }
    if (resolution > 0) {
        cache->resolution = resolution;
        if (cache->resolution <= 0) {
            cache->resolution = CACHE_TIMER_PERIOD;
        }
    }
}


PUBLIC ssize mprWriteCache(MprCache *cache, cchar *key, cchar *value, MprTime modified, MprTime lifespan, 
    int64 version, int options)
{
    CacheItem   *item;
    MprKey      *kp;
    ssize       len, oldLen;
    int         exists, add, set, prepend, append, throw;

    mprAssert(cache);
    mprAssert(key && *key);
    mprAssert(value);

    if (cache->shared) {
        cache = cache->shared;
        mprAssert(cache == shared);
    }
    exists = add = prepend = append = throw = 0;
    add = options & MPR_CACHE_ADD;
    append = options & MPR_CACHE_APPEND;
    prepend = options & MPR_CACHE_PREPEND;
    set = options & MPR_CACHE_SET;
    if ((add + append + prepend) == 0) {
        set = 1;
    }
    lock(cache);
    if ((kp = mprLookupKeyEntry(cache->store, key)) != 0) {
        exists++;
        item = (CacheItem*) kp->data;
        if (version) {
            if (item->version != version) {
                unlock(cache);
                return MPR_ERR_BAD_STATE;
            }
        }
    } else {
        if ((item = mprAllocObj(CacheItem, manageCacheItem)) == 0) {
            unlock(cache);
            return 0;
        }
        mprAddKey(cache->store, key, item);
        item->key = sclone(key);
        set = 1;
    }
    oldLen = (item->data) ? (slen(item->key) + slen(item->data)) : 0;
    if (set) {
        item->data = sclone(value);
    } else if (add) {
        if (exists) {
            return 0;
        }
        item->data = sclone(value);
    } else if (append) {
        item->data = sjoin(item->data, value, NULL);
    } else if (prepend) {
        item->data = sjoin(value, item->data, NULL);
    }
    if (lifespan >= 0) {
        item->lifespan = lifespan;
    }
    item->lastAccessed = mprGetTime();
    item->lastAccessed = item->lastModified = modified ? modified : item->lastAccessed;
    item->expires = item->lastAccessed + item->lifespan;
    item->version++;
    len = slen(item->key) + slen(item->data);
    cache->usedMem += (len - oldLen);

    if (cache->timer == 0) {
        mprLog(5, "Start Cache pruner with resolution %d", cache->resolution);
        /* 
            Use the MPR dispatcher incase this VM is destroyed 
         */
        cache->timer = mprCreateTimerEvent(MPR->dispatcher, "localCacheTimer", cache->resolution, pruneCache, cache, 
            MPR_EVENT_STATIC_DATA); 
    }
    unlock(cache);
    return len;
}


static void removeItem(MprCache *cache, CacheItem *item)
{
    mprAssert(cache);
    mprAssert(item);

    lock(cache);
    mprRemoveKey(cache->store, item->key);
    cache->usedMem -= (slen(item->key) + slen(item->data));
    unlock(cache);
}


static void pruneCache(MprCache *cache, MprEvent *event)
{
    MprTime         when, factor;
    MprKey          *kp;
    CacheItem       *item;
    ssize           excessKeys;

    if (!cache) {
        cache = shared;
        if (!cache) {
            return;
        }
    }
    if (event) {
        when = mprGetTime();
    } else {
        /* Expire all items by setting event to NULL */
        when = MAXINT64;
    }
    if (mprTryLock(cache->mutex)) {
        /*
            Check for expired items
         */
        for (kp = 0; (kp = mprGetNextKey(cache->store, kp)) != 0; ) {
            item = (CacheItem*) kp->data;
            mprLog(6, "Cache: \"%s\" lifespan %d, expires in %d secs", item->key, 
                    item->lifespan / 1000, (item->expires - when) / 1000);
            if (item->expires && item->expires <= when) {
                mprLog(5, "Cache prune expired key %s", kp->key);
                removeItem(cache, item);
            }
        }
        mprAssert(cache->usedMem >= 0);

        /*
            If too many keys or too much memory used, prune keys that expire soonest.
         */
        if (cache->maxKeys < MAXSSIZE || cache->maxMem < MAXSSIZE) {
            /*
                Look for those expiring in the next 5 minutes, then 20 mins, then 80 ...
             */
            excessKeys = mprGetHashLength(cache->store) - cache->maxKeys;
            factor = 5 * 60 * MPR_TICKS_PER_SEC; 
            when += factor;
            while (excessKeys > 0 || cache->usedMem > cache->maxMem) {
                for (kp = 0; (kp = mprGetNextKey(cache->store, kp)) != 0; ) {
                    item = (CacheItem*) kp->data;
                    if (item->expires && item->expires <= when) {
                        mprLog(5, "Cache too big execess keys %Ld, mem %Ld, prune key %s", 
                                excessKeys, (cache->maxMem - cache->usedMem), kp->key);
                        removeItem(cache, item);
                    }
                }
                factor *= 4;
                when += factor;
            }
        }
        mprAssert(cache->usedMem >= 0);

        if (mprGetHashLength(cache->store) == 0) {
            if (event) {
                mprRemoveEvent(event);
                cache->timer = 0;
            }
        }
        unlock(cache);
    }
}


PUBLIC void mprPruneCache(MprCache *cache)
{
    pruneCache(cache, NULL);
}


static void manageCache(MprCache *cache, int flags) 
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cache->store);
        mprMark(cache->mutex);
        mprMark(cache->timer);
        mprMark(cache->shared);

    } else if (flags & MPR_MANAGE_FREE) {
        if (cache == shared) {
            shared = 0;
        }
    }
}


static void manageCacheItem(CacheItem *item, int flags) 
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(item->key);
        mprMark(item->data);
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprCmd.c"
 */
/************************************************************************/

/* 
    mprCmd.c - Run external commands

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/******************************* Forward Declarations *************************/

static int blendEnv(MprCmd *cmd, cchar **env, int flags);
static void closeFiles(MprCmd *cmd);
static ssize cmdCallback(MprCmd *cmd, int channel, void *data);
static int makeChannel(MprCmd *cmd, int index);
static int makeCmdIO(MprCmd *cmd);
static void manageCmdService(MprCmdService *cmd, int flags);
static void manageCmd(MprCmd *cmd, int flags);
static void reapCmd(MprCmd *cmd, MprSignal *sp);
static void resetCmd(MprCmd *cmd);
static int sanitizeArgs(MprCmd *cmd, int argc, cchar **argv, cchar **env, int flags);
static int startProcess(MprCmd *cmd);
static void stdinCallback(MprCmd *cmd, MprEvent *event);
static void stdoutCallback(MprCmd *cmd, MprEvent *event);
static void stderrCallback(MprCmd *cmd, MprEvent *event);
static void vxCmdManager(MprCmd *cmd);
#if BIT_WIN_LIKE
static cchar *makeWinEnvBlock(MprCmd *cmd);
#endif

#if VXWORKS
typedef int (*MprCmdTaskFn)(int argc, char **argv, char **envp);
static void cmdTaskEntry(char *program, MprCmdTaskFn entry, int cmdArg);
#endif

/*
    Cygwin process creation is not thread-safe (1.7)
 */
#if CYGWIN
    #define slock(cmd) mprLock(MPR->cmdService->mutex)
    #define sunlock(cmd) mprUnlock(MPR->cmdService->mutex)
#else
    #define slock(cmd) 
    #define sunlock(cmd) 
#endif

/************************************* Code ***********************************/

PUBLIC MprCmdService *mprCreateCmdService()
{
    MprCmdService   *cs;

    if ((cs = (MprCmdService*) mprAllocObj(MprCmd, manageCmdService)) == 0) {
        return 0;
    }
    cs->cmds = mprCreateList(0, MPR_LIST_STATIC_VALUES);
    cs->mutex = mprCreateLock();
    return cs;
}


PUBLIC void mprStopCmdService()
{
    mprClearList(MPR->cmdService->cmds);
}


static void manageCmdService(MprCmdService *cs, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cs->cmds);
        mprMark(cs->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        mprStopCmdService();
        cs->mutex = 0;
    }
}


PUBLIC MprCmd *mprCreateCmd(MprDispatcher *dispatcher)
{
    MprCmd          *cmd;
    MprCmdFile      *files;
    int             i;
    
    if ((cmd = mprAllocObj(MprCmd, manageCmd)) == 0) {
        return 0;
    }
#if UNUSED && KEEP
    cmd->timeoutPeriod = MPR_TIMEOUT_CMD;
    cmd->timestamp = mprGetTime();
#endif
    cmd->forkCallback = (MprForkCallback) closeFiles;
    cmd->dispatcher = dispatcher ? dispatcher : MPR->dispatcher;
    cmd->status = -1;

#if VXWORKS
    cmd->startCond = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    cmd->exitCond = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#endif
    files = cmd->files;
    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        files[i].clientFd = -1;
        files[i].fd = -1;
    }
    cmd->mutex = mprCreateLock();
    mprAddItem(MPR->cmdService->cmds, cmd);
    return cmd;
}


static void manageCmd(MprCmd *cmd, int flags)
{
    int             i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(cmd->program);
        mprMark(cmd->makeArgv);
        mprMark(cmd->defaultEnv);
        mprMark(cmd->env);
        mprMark(cmd->dir);
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            mprMark(cmd->files[i].name);
        }
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            mprMark(cmd->handlers[i]);
        }
        mprMark(cmd->dispatcher);
        mprMark(cmd->callbackData);
        mprMark(cmd->signal);
        mprMark(cmd->forkData);
        mprMark(cmd->stdoutBuf);
        mprMark(cmd->stderrBuf);
        mprMark(cmd->userData);
        mprMark(cmd->mutex);
        mprMark(cmd->searchPath);
#if BIT_WIN_LIKE
        mprMark(cmd->command);
        mprMark(cmd->arg0);
#endif

    } else if (flags & MPR_MANAGE_FREE) {
        resetCmd(cmd);
        vxCmdManager(cmd);
        if (cmd->signal) {
            mprRemoveSignalHandler(cmd->signal);
            cmd->signal = 0;
        }
        mprRemoveItem(MPR->cmdService->cmds, cmd);
    }
}


static void vxCmdManager(MprCmd *cmd)
{
#if VXWORKS
    MprCmdFile      *files;
    int             i;

    if (cmd->startCond) {
        semDelete(cmd->startCond);
    }
    if (cmd->exitCond) {
        semDelete(cmd->exitCond);
    }
    files = cmd->files;
    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        if (files[i].name) {
            DEV_HDR *dev;
#if _WRS_VXWORKS_MAJOR >= 6
            cchar   *tail;
#else
            char    *tail;
#endif
            if ((dev = iosDevFind(files[i].name, &tail)) != NULL) {
                iosDevDelete(dev);
            }
        }
    }
#endif
}


PUBLIC void mprDestroyCmd(MprCmd *cmd)
{
    mprAssert(cmd);
    resetCmd(cmd);
    if (cmd->signal) {
        mprRemoveSignalHandler(cmd->signal);
        cmd->signal = 0;
    }
}


static void resetCmd(MprCmd *cmd)
{
    MprCmdFile      *files;
    int             i;

    mprAssert(cmd);
    files = cmd->files;
    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->handlers[i]) {
            mprRemoveWaitHandler(cmd->handlers[i]);
            cmd->handlers[i] = 0;
        }
        if (files[i].clientFd >= 0) {
            close(files[i].clientFd);
            files[i].clientFd = -1;
        }
        if (files[i].fd >= 0) {
            close(files[i].fd);
            files[i].fd = -1;
        }
    }
    cmd->eofCount = 0;
    cmd->complete = 0;
    cmd->status = -1;

    if (cmd->pid && !(cmd->flags & MPR_CMD_DETACH)) {
        mprStopCmd(cmd, -1);
        reapCmd(cmd, 0);
        cmd->pid = 0;
    }
}


PUBLIC void mprDisconnectCmd(MprCmd *cmd)
{
    int     i;

    mprAssert(cmd);

    for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->handlers[i]) {
            mprRemoveWaitHandler(cmd->handlers[i]);
            cmd->handlers[i] = 0;
        }
    }
}


/*
    Close a command channel. Must be able to be called redundantly.
 */
PUBLIC void mprCloseCmdFd(MprCmd *cmd, int channel)
{
    mprAssert(cmd);
    mprAssert(0 <= channel && channel <= MPR_CMD_MAX_PIPE);

    if (cmd->handlers[channel]) {
        mprRemoveWaitHandler(cmd->handlers[channel]);
        cmd->handlers[channel] = 0;
    }
    if (cmd->files[channel].fd != -1) {
        close(cmd->files[channel].fd);
        cmd->files[channel].fd = -1;
#if BIT_WIN_LIKE
        cmd->files[channel].handle = 0;
#endif
        if (channel != MPR_CMD_STDIN) {
            cmd->eofCount++;
            if (cmd->eofCount >= cmd->requiredEof) {
#if VXWORKS
                reapCmd(cmd, 0);
#endif
                if (cmd->pid == 0) {
                    cmd->complete = 1;
                }
            }
        }
    }
    mprLog(6, "Close channel %d eof %d/%d, pid %d", channel, cmd->eofCount, cmd->requiredEof, cmd->pid);
}


PUBLIC void mprFinalizeCmd(MprCmd *cmd)
{
    mprLog(6, "mprFinalizeCmd");
    mprAssert(cmd);
    mprCloseCmdFd(cmd, MPR_CMD_STDIN);
}


PUBLIC int mprIsCmdComplete(MprCmd *cmd)
{
    return cmd->complete;
}


/*
    Run a simple blocking command. See arg usage below in mprRunCmdV.
 */
PUBLIC int mprRunCmd(MprCmd *cmd, cchar *command, cchar **envp, char **out, char **err, MprTime timeout, int flags)
{
    cchar   **argv;
    int     argc;

    mprAssert(cmd);
    if ((argc = mprMakeArgv(command, &argv, 0)) < 0 || argv == 0) {
        return 0;
    }
    cmd->makeArgv = argv;
    return mprRunCmdV(cmd, argc, argv, envp, out, err, timeout, flags);
}


/*
    Env is an array of "KEY=VALUE" strings. Null terminated
    The user must preserve the environment. This module does not clone the environment and uses the supplied reference.
 */
PUBLIC void mprSetCmdDefaultEnv(MprCmd *cmd, cchar **env)
{
    /* WARNING: defaultEnv is not cloned, but is marked */
    cmd->defaultEnv = env;
}


PUBLIC void mprSetCmdSearchPath(MprCmd *cmd, cchar *search)
{
    cmd->searchPath = sclone(search);
}


/*
    This routine runs a command and waits for its completion. Stdoutput and Stderr are returned in *out and *err 
    respectively. The command returns the exit status of the command.
    Valid flags are:
        MPR_CMD_NEW_SESSION     Create a new session on Unix
        MPR_CMD_SHOW            Show the commands window on Windows
        MPR_CMD_IN              Connect to stdin
 */
PUBLIC int mprRunCmdV(MprCmd *cmd, int argc, cchar **argv, cchar **envp, char **out, char **err, MprTime timeout, int flags)
{
    int     rc, status;

    mprAssert(cmd);
    if (err) {
        *err = 0;
        flags |= MPR_CMD_ERR;
    } else {
        flags &= ~MPR_CMD_ERR;
    }
    if (out) {
        *out = 0;
        flags |= MPR_CMD_OUT;
    } else {
        flags &= ~MPR_CMD_OUT;
    }
    if (flags & MPR_CMD_OUT) {
        cmd->stdoutBuf = mprCreateBuf(MPR_BUFSIZE, -1);
    }
    if (flags & MPR_CMD_ERR) {
        cmd->stderrBuf = mprCreateBuf(MPR_BUFSIZE, -1);
    }
    mprSetCmdCallback(cmd, cmdCallback, NULL);
    rc = mprStartCmd(cmd, argc, argv, envp, flags);

    /*
        Close the pipe connected to the client's stdin
     */
    if (cmd->files[MPR_CMD_STDIN].fd >= 0) {
        mprFinalizeCmd(cmd);
    }
    if (rc < 0) {
        if (err) {
            if (rc == MPR_ERR_CANT_ACCESS) {
                *err = sfmt("Can't access command %s", cmd->program);
            } else if (MPR_ERR_CANT_OPEN) {
                *err = sfmt("Can't open standard I/O for command %s", cmd->program);
            } else if (rc == MPR_ERR_CANT_CREATE) {
                *err = sfmt("Can't create process for %s", cmd->program);
            }
        }
        return rc;
    }
    if (cmd->flags & MPR_CMD_DETACH) {
        return 0;
    }
    if (mprWaitForCmd(cmd, timeout) < 0) {
        return MPR_ERR_NOT_READY;
    }
    if ((status = mprGetCmdExitStatus(cmd)) < 0) {
        return MPR_ERR;
    }
    if (err && flags & MPR_CMD_ERR) {
        *err = mprGetBufStart(cmd->stderrBuf);
    }
    if (out && flags & MPR_CMD_OUT) {
        *out = mprGetBufStart(cmd->stdoutBuf);
    }
    return status;
}


static void addCmdHandlers(MprCmd *cmd)
{
    int     stdinFd, stdoutFd, stderrFd;
  
    stdinFd = cmd->files[MPR_CMD_STDIN].fd; 
    stdoutFd = cmd->files[MPR_CMD_STDOUT].fd; 
    stderrFd = cmd->files[MPR_CMD_STDERR].fd; 

    if (stdinFd >= 0 && cmd->handlers[MPR_CMD_STDIN] == 0) {
        cmd->handlers[MPR_CMD_STDIN] = mprCreateWaitHandler(stdinFd, MPR_WRITABLE, cmd->dispatcher, stdinCallback, cmd, 0);
    }
    if (stdoutFd >= 0 && cmd->handlers[MPR_CMD_STDOUT] == 0) {
        cmd->handlers[MPR_CMD_STDOUT] = mprCreateWaitHandler(stdoutFd, MPR_READABLE, cmd->dispatcher, stdoutCallback, cmd,0);
    }
    if (stderrFd >= 0 && cmd->handlers[MPR_CMD_STDERR] == 0) {
        cmd->handlers[MPR_CMD_STDERR] = mprCreateWaitHandler(stderrFd, MPR_READABLE, cmd->dispatcher, stderrCallback, cmd,0);
    }
}


/*
    Start the command to run (stdIn and stdOut are named from the client's perspective). This is the lower-level way to 
    run a command. The caller needs to do code like mprRunCmd() themselves to wait for completion and to send/receive data.
    The routine does not wait. Callers must call mprWaitForCmd to wait for the command to complete.
 */
PUBLIC int mprStartCmd(MprCmd *cmd, int argc, cchar **argv, cchar **envp, int flags)
{
    MprPath     info;
    cchar       *program, *search, *pair;
    int         rc, next, i;

    mprAssert(cmd);
    mprAssert(argv);

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return MPR_ERR_BAD_ARGS;
    }
    resetCmd(cmd);
    program = argv[0];
    cmd->program = sclone(program);
    cmd->flags = flags;

    if (sanitizeArgs(cmd, argc, argv, envp, flags) < 0) {
        return MPR_ERR_MEMORY;
    }
    if (envp == 0) {
        envp = cmd->defaultEnv;
    }
    if (blendEnv(cmd, envp, flags) < 0) {
        return MPR_ERR_MEMORY;
    }
    search = cmd->searchPath ? cmd->searchPath : MPR->pathEnv;
    if ((program = mprSearchPath(program, MPR_SEARCH_EXE, search, NULL)) == 0) {
        mprLog(1, "cmd: can't access %s, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_ACCESS;
    }
    cmd->program = cmd->argv[0] = program;

    if (mprGetPathInfo(program, &info) == 0 && info.isDir) {
        mprLog(1, "cmd: program \"%s\", is a directory", program);
        return MPR_ERR_CANT_ACCESS;
    }
    mprLog(4, "mprStartCmd %s", cmd->program);
    for (i = 0; i < cmd->argc; i++) {
        mprLog(4, "    arg[%d]: %s", i, cmd->argv[i]);
    }
    for (ITERATE_ITEMS(cmd->env, pair, next)) {
        mprLog(4, "    env[%d]: %s", next, pair);
    }
    slock(cmd);
    if (makeCmdIO(cmd) < 0) {
        sunlock(cmd);
        return MPR_ERR_CANT_OPEN;
    }
    /*
        Determine how many end-of-files will be seen when the child dies
     */
    cmd->requiredEof = 0;
    if (cmd->flags & MPR_CMD_OUT) {
        cmd->requiredEof++;
    }
    if (cmd->flags & MPR_CMD_ERR) {
        cmd->requiredEof++;
    }
    addCmdHandlers(cmd);
    rc = startProcess(cmd);
    sunlock(cmd);
    return rc;
}


static int makeCmdIO(MprCmd *cmd)
{
    int     rc;

    rc = 0;
    if (cmd->flags & MPR_CMD_IN) {
        rc += makeChannel(cmd, MPR_CMD_STDIN);
    }
    if (cmd->flags & MPR_CMD_OUT) {
        rc += makeChannel(cmd, MPR_CMD_STDOUT);
    }
    if (cmd->flags & MPR_CMD_ERR) {
        rc += makeChannel(cmd, MPR_CMD_STDERR);
    }
    return rc;
}


/*
    Stop the command
 */
PUBLIC int mprStopCmd(MprCmd *cmd, int signal)
{
    mprLog(7, "cmd: stop");

    if (signal < 0) {
        signal = SIGTERM;
    }
    cmd->stopped = 1;
    if (cmd->pid) {
#if BIT_WIN_LIKE
        return TerminateProcess(cmd->process, 2) == 0;
#elif VXWORKS
        return taskDelete(cmd->pid);
#else
        return kill(cmd->pid, signal);
#endif
    }
    return 0;
}


/*
    Do non-blocking I/O - except on windows - will block
 */
PUBLIC ssize mprReadCmd(MprCmd *cmd, int channel, char *buf, ssize bufsize)
{
#if BIT_WIN_LIKE
    int     rc, count;
    /*
        Need to detect EOF in windows. Pipe always in blocking mode, but reads block even with no one on the other end.
     */
    mprAssert(cmd->files[channel].handle);
    rc = PeekNamedPipe(cmd->files[channel].handle, NULL, 0, NULL, &count, NULL);
    if (rc > 0 && count > 0) {
        return read(cmd->files[channel].fd, buf, (uint) bufsize);
    } 
    if (cmd->process == 0 || WaitForSingleObject(cmd->process, 0) == WAIT_OBJECT_0) {
        /* Process has exited - EOF */
        return 0;
    }
    /* This maps to EAGAIN */
    SetLastError(WSAEWOULDBLOCK);
    return -1;

#elif VXWORKS
    /*
        Only needed when using non-blocking I/O
     */
    int     rc;

    rc = read(cmd->files[channel].fd, buf, bufsize);

    /*
        VxWorks can't signal EOF on non-blocking pipes. Need a pattern indicator.
     */
    if (rc == MPR_CMD_VXWORKS_EOF_LEN && strncmp(buf, MPR_CMD_VXWORKS_EOF, MPR_CMD_VXWORKS_EOF_LEN) == 0) {
        /* EOF */
        return 0;

    } else if (rc == 0) {
        rc = -1;
        errno = EAGAIN;
    }
    return rc;

#else
    mprAssert(cmd->files[channel].fd >= 0);
    return read(cmd->files[channel].fd, buf, bufsize);
#endif
}


/*
    Do non-blocking I/O - except on windows - will block
 */
PUBLIC ssize mprWriteCmd(MprCmd *cmd, int channel, char *buf, ssize bufsize)
{
#if BIT_WIN_LIKE
    /*
        No waiting. Use this just to check if the process has exited and thus EOF on the pipe.
     */
    if (cmd->pid == 0 || WaitForSingleObject(cmd->process, 0) == WAIT_OBJECT_0) {
        return -1;
    }
#endif
    return write(cmd->files[channel].fd, buf, (wsize) bufsize);
}


PUBLIC void mprEnableCmdEvents(MprCmd *cmd, int channel)
{
    int mask = (channel == MPR_CMD_STDIN) ? MPR_WRITABLE : MPR_READABLE;
    if (cmd->handlers[channel]) {
        mprWaitOn(cmd->handlers[channel], mask);
    }
}


PUBLIC void mprDisableCmdEvents(MprCmd *cmd, int channel)
{
    if (cmd->handlers[channel]) {
        mprWaitOn(cmd->handlers[channel], 0);
    }
}


#if BIT_WIN_LIKE && !WINCE
/*
    Windows only routine to wait for I/O on the channels to the gateway and the child process.
    NamedPipes can't use WaitForMultipleEvents (can use overlapped I/O)
    WARNING: this should not be called from a dispatcher other than cmd->dispatcher. If so, then the calls to
    mprWaitForEvent may occur after the event has been processed.
 */
static void waitForWinEvent(MprCmd *cmd, MprTime timeout)
{
    MprTime     mark, remaining, delay;
    int         i, rc, nbytes;

    mark = mprGetTime();
    if (cmd->stopped) {
        timeout = 0;
    }
    for (i = MPR_CMD_STDOUT; i < MPR_CMD_MAX_PIPE; i++) {
        if (cmd->files[i].handle) {
            rc = PeekNamedPipe(cmd->files[i].handle, NULL, 0, NULL, &nbytes, NULL);
            if (rc && nbytes > 0 || cmd->process == 0) {
                mprQueueIOEvent(cmd->handlers[i]);
                mprWaitForEvent(cmd->dispatcher, timeout);
                return;
            }
        }
    }
    if (cmd->files[MPR_CMD_STDIN].handle) {
        /* Not finalized */
        mprQueueIOEvent(cmd->handlers[MPR_CMD_STDIN]);
        mprWaitForEvent(cmd->dispatcher, timeout);
        return;
    }
    if (cmd->process) {
        delay = (cmd->eofCount == cmd->requiredEof && cmd->files[MPR_CMD_STDIN].handle == 0) ? timeout : 0;
        mprYield(MPR_YIELD_STICKY);
        if (WaitForSingleObject(cmd->process, (DWORD) delay) == WAIT_OBJECT_0) {
            mprResetYield();
            reapCmd(cmd, 0);
            return;
        }
        mprResetYield();
        if (cmd->eofCount == cmd->requiredEof) {
            remaining = mprGetRemainingTime(mark, timeout);
            mprYield(MPR_YIELD_STICKY);
            rc = WaitForSingleObject(cmd->process, (DWORD) remaining);
            mprResetYield();
            if (rc == WAIT_OBJECT_0) {
                reapCmd(cmd, 0);
                return;
            }
            mprError("Error waiting CGI I/O, error %d", mprGetOsError());
        }
    }
    /* Stop busy waiting */
    mprSleep(10);
}
#endif


/*
    Wait for a command to complete. Return 0 if the command completed, otherwise it will return MPR_ERR_TIMEOUT. 
 */
PUBLIC int mprWaitForCmd(MprCmd *cmd, MprTime timeout)
{
    MprTime     expires, remaining;

    mprAssert(cmd);

    if (timeout < 0) {
        timeout = MAXINT;
    }
    if (mprGetDebugMode()) {
        timeout = MAXINT;
    }
    if (cmd->stopped) {
        timeout = 0;
    }
    expires = mprGetTime() + timeout;
    remaining = timeout;

    /* Add root to allow callers to use mprRunCmd without first managing the cmd */
    mprAddRoot(cmd);

    while (!cmd->complete && remaining > 0) {
        if (mprShouldAbortRequests()) {
            break;
        }
#if BIT_WIN_LIKE
        waitForWinEvent(cmd, remaining);
#else
        mprWaitForEvent(cmd->dispatcher, remaining);
#endif
        remaining = (expires - mprGetTime());
    }
    mprRemoveRoot(cmd);
    if (cmd->pid) {
        return MPR_ERR_TIMEOUT;
    }
    mprLog(6, "cmd: waitForChild: status %d", cmd->status);
    return 0;
}


/*
    Gather the child's exit status. 
    WARNING: this may be called with a false-positive, ie. SIGCHLD will get invoked for all process deaths and not just
    when this cmd has completed.
 */
static void reapCmd(MprCmd *cmd, MprSignal *sp)
{
    ssize   got, nbytes;
    int     status, rc;

    mprLog(6, "reapCmd CHECK pid %d, eof %d, required %d\n", cmd->pid, cmd->eofCount, cmd->requiredEof);
    
    status = 0;
    if (cmd->pid == 0) {
        return;
    }
#if BIT_UNIX_LIKE
    if ((rc = waitpid(cmd->pid, &status, WNOHANG | __WALL)) < 0) {
        mprLog(6, "waitpid failed for pid %d, errno %d", cmd->pid, errno);

    } else if (rc == cmd->pid) {
        mprLog(6, "waitpid pid %d, thread %s", cmd->pid, mprGetCurrentThreadName());
        if (!WIFSTOPPED(status)) {
            if (WIFEXITED(status)) {
                cmd->status = WEXITSTATUS(status);
                mprLog(6, "waitpid exited pid %d, status %d", cmd->pid, cmd->status);
            } else if (WIFSIGNALED(status)) {
                cmd->status = WTERMSIG(status);
            } else {
                mprLog(7, "waitpid FUNNY pid %d, errno %d", cmd->pid, errno);
            }
            cmd->pid = 0;
            mprAssert(cmd->signal);
            mprRemoveSignalHandler(cmd->signal);
            cmd->signal = 0;
        } else {
            mprLog(7, "waitpid ELSE pid %d, errno %d", cmd->pid, errno);
        }
    } else {
        mprLog(6, "waitpid still running pid %d, thread %s", cmd->pid, mprGetCurrentThreadName());
    }
#endif
#if VXWORKS
    /*
        The command exit status (cmd->status) is set in cmdTaskEntry
     */
    if (!cmd->stopped) {
        if (semTake(cmd->exitCond, MPR_TIMEOUT_STOP_TASK) != OK) {
            mprError("cmd: child %s did not exit, errno %d", cmd->program);
            return;
        }
    }
    semDelete(cmd->exitCond);
    cmd->exitCond = 0;
    cmd->pid = 0;
    rc = 0;
#endif
#if BIT_WIN_LIKE
    if (GetExitCodeProcess(cmd->process, (ulong*) &status) == 0) {
        mprLog(3, "cmd: GetExitProcess error");
        return;
    }
    if (status != STILL_ACTIVE) {
        cmd->status = status;
        rc = CloseHandle(cmd->process);
        mprAssert(rc != 0);
        rc = CloseHandle(cmd->thread);
        mprAssert(rc != 0);
        cmd->process = 0;
        cmd->thread = 0;
        cmd->pid = 0;
    }
#endif
    if (cmd->pid == 0) {
        if (cmd->eofCount >= cmd->requiredEof) {
            cmd->complete = 1;
        }
        if (cmd->callback) {
            (cmd->callback)(cmd, -1, cmd->callbackData);
        }
        mprLog(6, "Cmd reaped: status %d, pid %d, eof %d / %d\n", cmd->status, cmd->pid, cmd->eofCount, cmd->requiredEof);

        if (cmd->callback) {
            /*
                Read outstanding data
             */  
            while (cmd->eofCount < cmd->requiredEof) {
                got = 0;
                if (cmd->files[MPR_CMD_STDERR].fd >= 0) {
                    if ((nbytes = (cmd->callback)(cmd, MPR_CMD_STDERR, cmd->callbackData)) > 0) {
                        got += nbytes;
                    }
                }
                if (cmd->files[MPR_CMD_STDOUT].fd >= 0) {
                    if ((nbytes = (cmd->callback)(cmd, MPR_CMD_STDOUT, cmd->callbackData)) > 0) {
                        got += nbytes;
                    }
                }
                if (got <= 0) {
                    break;
                }
            }
            if (cmd->files[MPR_CMD_STDERR].fd >= 0) {
                mprCloseCmdFd(cmd, MPR_CMD_STDERR);
            }
            if (cmd->files[MPR_CMD_STDOUT].fd >= 0) {
                mprCloseCmdFd(cmd, MPR_CMD_STDOUT);
            }
#if UNUSED && DONT_USE && KEEP
            if (cmd->eofCount != cmd->requiredEof) {
                mprLog(0, "reapCmd: insufficient EOFs %d %d, complete %d", cmd->eofCount, cmd->requiredEof, cmd->complete);
            }
            mprAssert(cmd->eofCount == cmd->requiredEof);
            mprAssert(cmd->complete);
#endif
        }
    }
}


/*
    Default callback routine for the mprRunCmd routines. Uses may supply their own callback instead of this routine. 
    The callback is run whenever there is I/O to read/write to the CGI gateway.
 */
static ssize cmdCallback(MprCmd *cmd, int channel, void *data)
{
    MprBuf      *buf;
    ssize       len, space;

    /*
        Note: stdin, stdout and stderr are named from the client's perspective
     */
    buf = 0;
    switch (channel) {
    case MPR_CMD_STDIN:
        return 0;
    case MPR_CMD_STDOUT:
        buf = cmd->stdoutBuf;
        break;
    case MPR_CMD_STDERR:
        buf = cmd->stderrBuf;
        break;
    default:
        /* Child death notification */
        return 0;
    }
    /*
        Read and aggregate the result into a single string
     */
    space = mprGetBufSpace(buf);
    if (space < (MPR_BUFSIZE / 4)) {
        if (mprGrowBuf(buf, MPR_BUFSIZE) < 0) {
            mprCloseCmdFd(cmd, channel);
            return 0;
        }
        space = mprGetBufSpace(buf);
    }
    len = mprReadCmd(cmd, channel, mprGetBufEnd(buf), space);
    mprLog(6, "cmdCallback channel %d, read len %d, pid %d, eof %d/%d", channel, len, cmd->pid, cmd->eofCount, 
        cmd->requiredEof);
    if (len <= 0) {
        if (len == 0 || (len < 0 && !(errno == EAGAIN || errno == EWOULDBLOCK))) {
            mprCloseCmdFd(cmd, channel);
            return len;
        }
    } else {
        mprAdjustBufEnd(buf, len);
    }
    mprAddNullToBuf(buf);
    mprEnableCmdEvents(cmd, channel);
    return len;
}


static void stdinCallback(MprCmd *cmd, MprEvent *event)
{
    if (cmd->callback && cmd->files[MPR_CMD_STDIN].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDIN, cmd->callbackData);
    }
}


static void stdoutCallback(MprCmd *cmd, MprEvent *event)
{
    /*
        reapCmd can consume data from the client and close the fd
     */
    if (cmd->callback && cmd->files[MPR_CMD_STDOUT].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDOUT, cmd->callbackData);
    }
}


static void stderrCallback(MprCmd *cmd, MprEvent *event)
{
    /*
        reapCmd can consume data from the client and close the fd
     */
    if (cmd->callback && cmd->files[MPR_CMD_STDERR].fd >= 0) {
        (cmd->callback)(cmd, MPR_CMD_STDERR, cmd->callbackData);
    }
}


PUBLIC void mprSetCmdCallback(MprCmd *cmd, MprCmdProc proc, void *data)
{
    cmd->callback = proc;
    cmd->callbackData = data;
}


PUBLIC int mprGetCmdExitStatus(MprCmd *cmd)
{
    mprAssert(cmd);

    if (cmd->pid == 0) {
        return cmd->status;
    }
    return MPR_ERR_NOT_READY;
}


PUBLIC bool mprIsCmdRunning(MprCmd *cmd)
{
    return cmd->pid > 0;
}


/* FUTURE - not yet supported */

PUBLIC void mprSetCmdTimeout(MprCmd *cmd, MprTime timeout)
{
    mprAssert(0);
#if UNUSED && KEEP
    cmd->timeoutPeriod = timeout;
#endif
}


PUBLIC int mprGetCmdFd(MprCmd *cmd, int channel) 
{ 
    return cmd->files[channel].fd; 
}


PUBLIC MprBuf *mprGetCmdBuf(MprCmd *cmd, int channel)
{
    return (channel == MPR_CMD_STDOUT) ? cmd->stdoutBuf : cmd->stderrBuf;
}


PUBLIC void mprSetCmdDir(MprCmd *cmd, cchar *dir)
{
#if VXWORKS
    mprError("WARNING: Setting working directory on VxWorks is global: %s", dir);
#else
    mprAssert(dir && *dir);

    cmd->dir = sclone(dir);
#endif
}


#if BIT_WIN_LIKE
static int sortEnv(char **str1, char **str2)
{
    cchar    *s1, *s2;
    int     c1, c2;

    for (s1 = *str1, s2 = *str2; *s1 && *s2; s1++, s2++) {
        c1 = tolower((uchar) *s1);
        c2 = tolower((uchar) *s2);
        if (c1 < c2) {
            return -1;
        } else if (c1 > c2) {
            return 1;
        }
    }
    if (*s2) {
        return -1;
    } else if (*s1) {
        return 1;
    }
    return 0;
}
#endif


/*
    Match two environment keys up to the '='
 */
static bool matchEnvKey(cchar *s1, cchar *s2) 
{
    for (; *s1 && *s2; s1++, s2++) {
        if (*s1 != *s2) {
            break;
        } else if (*s1 == '=') {
            return 1;
        }
    }
    return 0;
}


static int blendEnv(MprCmd *cmd, cchar **env, int flags)
{
    cchar       **ep, *prior;
    int         next;

    cmd->env = 0;

    if ((cmd->env = mprCreateList(128, MPR_LIST_STATIC_VALUES)) == 0) {
        return MPR_ERR_MEMORY;
    }
#if !VXWORKS
    /*
        Add prior environment to the list
     */
    if (!(flags & MPR_CMD_EXACT_ENV)) {
        for (ep = (cchar**) environ; ep && *ep; ep++) {
            mprAddItem(cmd->env, *ep);
        }
    }
#endif
    /*
        Add new env keys. Detect and overwrite duplicates
     */
    for (ep = env; ep && *ep; ep++) {
        prior = 0;
        for (ITERATE_ITEMS(cmd->env, prior, next)) {
            if (matchEnvKey(*ep, prior)) {
                mprSetItem(cmd->env, next - 1, *ep);
                break;
            }
        }
        if (prior == 0) {
            mprAddItem(cmd->env, *ep);
        }
    }
#if BIT_WIN_LIKE
    /*
        Windows requires a caseless sort with two trailing nulls
     */
    mprSortList(cmd->env, (MprSortProc) sortEnv, 0);
#endif
    mprAddItem(cmd->env, NULL);
    return 0;
}


#if BIT_WIN_LIKE
static cchar *makeWinEnvBlock(MprCmd *cmd)
{
    char    *item, *dp, *ep, *env;
    ssize   len;
    int     next;

    for (len = 2, ITERATE_ITEMS(cmd->env, item, next)) {
        len += slen(item) + 1;
    }
    if ((env = mprAlloc(len)) == 0) {
        return 0;
    }
    ep = &env[len];
    dp = env;
    for (ITERATE_ITEMS(cmd->env, item, next)) {
        strcpy(dp, item);
        dp += slen(item) + 1;
    }
    /* Windows requires two nulls */
    *dp++ = '\0';
    *dp++ = '\0';
    mprAssert(dp <= ep);
    return env;
}
#endif


/*
    Sanitize args. Convert "/" to "\" and converting '\r' and '\n' to spaces, quote all args and put the program as argv[0].
 */
static int sanitizeArgs(MprCmd *cmd, int argc, cchar **argv, cchar **env, int flags)
{
#if BIT_UNIX_LIKE || VXWORKS
    cmd->argv = argv;
    cmd->argc = argc;
#endif

#if BIT_WIN_LIKE
    /*
        WARNING: If starting a program compiled with Cygwin, there is a bug in Cygwin's parsing of the command
        string where embedded quotes are parsed incorrectly by the Cygwin CRT runtime. If an arg starts with a 
        drive spec, embedded backquoted quotes will be stripped and the backquote will be passed in. Windows CRT 
        handles this correctly.  For example:  
            ./args "c:/path \"a b\"
            Cygwin will parse as  argv[1] == c:/path \a \b
            Windows will parse as argv[1] == c:/path "a b"
     */
    cchar       *saveArg0, **ap, *start, *cp;
    char        *pp, *program, *dp, *localArgv[2];
    ssize       len;
    int         quote;

    mprAssert(argc > 0 && argv[0] != NULL);

    cmd->argv = argv;
    cmd->argc = argc;

    program = cmd->arg0 = mprAlloc(slen(argv[0]) * 2 + 1);
    strcpy(program, argv[0]);

    for (pp = program; *pp; pp++) {
        if (*pp == '/') {
            *pp = '\\';
        } else if (*pp == '\r' || *pp == '\n') {
            *pp = ' ';
        }
    }
    if (*program == '\"') {
        if ((pp = strrchr(++program, '"')) != 0) {
            *pp = '\0';
        }
    }
    if (argv == 0) {
        argv = localArgv;
        argv[1] = 0;
        saveArg0 = program;
    } else {
        saveArg0 = argv[0];
    }
    /*
        Set argv[0] to the program name while creating the command line. Restore later.
     */
    argv[0] = program;
    argc = 0;
    for (len = 0, ap = argv; *ap; ap++) {
        len += (slen(*ap) * 2) + 1 + 2;         /* Space and possible quotes and worst case backquoting */
        argc++;
    }
    cmd->command = mprAlloc(len + 1);
    cmd->command[len] = '\0';
    
    /*
        Add quotes around all args that have spaces and backquote [", ', \\]
        Example:    ["showColors", "red", "light blue", "Can't \"render\""]
        Becomes:    "showColors" "red" "light blue" "Can't \"render\""
     */
    dp = cmd->command;
    for (ap = &argv[0]; *ap; ) {
        start = cp = *ap;
        quote = '"';
        if (strchr(cp, ' ') != 0 && cp[0] != quote) {
            for (*dp++ = quote; *cp; ) {
                if (*cp == quote && !(cp > start && cp[-1] == '\\')) {
                    *dp++ = '\\';
                }
                *dp++ = *cp++;
            }
            *dp++ = quote;
        } else {
            strcpy(dp, cp);
            dp += strlen(cp);
        }
        if (*++ap) {
            *dp++ = ' ';
        }
    }
    *dp = '\0';
    argv[0] = saveArg0;
    mprLog(5, "Windows command line: %s", cmd->command);
#endif /* BIT_WIN_LIKE */
    return 0;
}


#if BIT_WIN_LIKE
static int startProcess(MprCmd *cmd)
{
    PROCESS_INFORMATION procInfo;
    STARTUPINFO         startInfo;
    cchar               *envBlock;
    int                 err;

    memset(&startInfo, 0, sizeof(startInfo));
    startInfo.cb = sizeof(startInfo);

    startInfo.dwFlags = STARTF_USESHOWWINDOW;
    if (cmd->flags & MPR_CMD_SHOW) {
        startInfo.wShowWindow = SW_SHOW;
    } else {
        startInfo.wShowWindow = SW_HIDE;
    }
    startInfo.dwFlags |= STARTF_USESTDHANDLES;

    if (cmd->flags & MPR_CMD_IN) {
        if (cmd->files[MPR_CMD_STDIN].clientFd > 0) {
            startInfo.hStdInput = (HANDLE) _get_osfhandle(cmd->files[MPR_CMD_STDIN].clientFd);
        }
    } else {
        startInfo.hStdInput = (HANDLE) _get_osfhandle((int) fileno(stdin));
    }
    if (cmd->flags & MPR_CMD_OUT) {
        if (cmd->files[MPR_CMD_STDOUT].clientFd > 0) {
            startInfo.hStdOutput = (HANDLE)_get_osfhandle(cmd->files[MPR_CMD_STDOUT].clientFd);
        }
    } else {
        startInfo.hStdOutput = (HANDLE)_get_osfhandle((int) fileno(stdout));
    }
    if (cmd->flags & MPR_CMD_ERR) {
        if (cmd->files[MPR_CMD_STDERR].clientFd > 0) {
            startInfo.hStdError = (HANDLE) _get_osfhandle(cmd->files[MPR_CMD_STDERR].clientFd);
        }
    } else {
        startInfo.hStdError = (HANDLE) _get_osfhandle((int) fileno(stderr));
    }
    envBlock = makeWinEnvBlock(cmd);
    if (! CreateProcess(0, wide(cmd->command), 0, 0, 1, 0, (char*) envBlock, wide(cmd->dir), &startInfo, &procInfo)) {
        err = mprGetOsError();
        if (err == ERROR_DIRECTORY) {
            mprError("Can't create process: %s, directory %s is invalid", cmd->program, cmd->dir);
        } else {
            mprError("Can't create process: %s, %d", cmd->program, err);
        }
        return MPR_ERR_CANT_CREATE;
    }
    cmd->thread = procInfo.hThread;
    cmd->process = procInfo.hProcess;
    cmd->pid = procInfo.dwProcessId;
    return 0;
}


#if WINCE
//  FUTURE - merge this with WIN
static int makeChannel(MprCmd *cmd, int index)
{
    SECURITY_ATTRIBUTES clientAtt, serverAtt, *att;
    HANDLE              readHandle, writeHandle;
    MprCmdFile          *file;
    char                *path;
    int                 readFd, writeFd;

    memset(&clientAtt, 0, sizeof(clientAtt));
    clientAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    clientAtt.bInheritHandle = 1;

    /*
        Server fds are not inherited by the child
     */
    memset(&serverAtt, 0, sizeof(serverAtt));
    serverAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    serverAtt.bInheritHandle = 0;

    file = &cmd->files[index];
    path = mprGetTempPath(cmd, NULL);

    att = (index == MPR_CMD_STDIN) ? &clientAtt : &serverAtt;
    readHandle = CreateFile(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, att, OPEN_ALWAYS, 
        FILE_ATTRIBUTE_NORMAL,0);
    if (readHandle == INVALID_HANDLE_VALUE) {
        mprError(cmd, "Can't create stdio pipes %s. Err %d\n", path, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    readFd = (int) (int64) _open_osfhandle((int*) readHandle, 0);

    att = (index == MPR_CMD_STDIN) ? &serverAtt: &clientAtt;
    writeHandle = CreateFile(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, att, OPEN_ALWAYS, 
        FILE_ATTRIBUTE_NORMAL, 0);
    writeFd = (int) _open_osfhandle((int*) writeHandle, 0);

    if (readFd < 0 || writeFd < 0) {
        mprError(cmd, "Can't create stdio pipes %s. Err %d\n", path, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (index == MPR_CMD_STDIN) {
        file->clientFd = readFd;
        file->fd = writeFd;
        file->handle = writeHandle;
    } else {
        file->clientFd = writeFd;
        file->fd = readFd;
        file->handle = readHandle;
    }
    return 0;
}

#else /* !WINCE */
static int makeChannel(MprCmd *cmd, int index)
{
    SECURITY_ATTRIBUTES clientAtt, serverAtt, *att;
    HANDLE              readHandle, writeHandle;
    MprCmdFile          *file;
    MprTime             now;
    char                *pipeName;
    int                 openMode, pipeMode, readFd, writeFd;
    static int          tempSeed = 0;

    memset(&clientAtt, 0, sizeof(clientAtt));
    clientAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    clientAtt.bInheritHandle = 1;

    /*
        Server fds are not inherited by the child
     */
    memset(&serverAtt, 0, sizeof(serverAtt));
    serverAtt.nLength = sizeof(SECURITY_ATTRIBUTES);
    serverAtt.bInheritHandle = 0;

    file = &cmd->files[index];
    now = ((int) mprGetTime() & 0xFFFF) % 64000;

    lock(MPR->cmdService);
    pipeName = sfmt("\\\\.\\pipe\\MPR_%d_%d_%d.tmp", getpid(), (int) now, ++tempSeed);
    unlock(MPR->cmdService);

    /*
        Pipes are always inbound. The file below is outbound. we swap whether the client or server
        inherits the pipe or file. MPR_CMD_STDIN is the clients input pipe.
        Pipes are blocking since both ends share the same blocking mode. Client must be blocking.
     */
    openMode = PIPE_ACCESS_INBOUND;
    pipeMode = 0;

    att = (index == MPR_CMD_STDIN) ? &clientAtt : &serverAtt;
    readHandle = CreateNamedPipe(wide(pipeName), openMode, pipeMode, 1, 0, 256 * 1024, 1, att);
    if (readHandle == INVALID_HANDLE_VALUE) {
        mprError("Can't create stdio pipes %s. Err %d\n", pipeName, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    readFd = (int) (int64) _open_osfhandle((long) readHandle, 0);

    att = (index == MPR_CMD_STDIN) ? &serverAtt: &clientAtt;
    writeHandle = CreateFile(wide(pipeName), GENERIC_WRITE, 0, att, OPEN_EXISTING, openMode, 0);
    writeFd = (int) _open_osfhandle((long) writeHandle, 0);

    if (readFd < 0 || writeFd < 0) {
        mprError("Can't create stdio pipes %s. Err %d\n", pipeName, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (index == MPR_CMD_STDIN) {
        file->clientFd = readFd;
        file->fd = writeFd;
        file->handle = writeHandle;
    } else {
        file->clientFd = writeFd;
        file->fd = readFd;
        file->handle = readHandle;
    }
    return 0;
}
#endif /* WINCE */


#elif BIT_UNIX_LIKE
static int makeChannel(MprCmd *cmd, int index)
{
    MprCmdFile      *file;
    int             fds[2];

    file = &cmd->files[index];

    if (pipe(fds) < 0) {
        mprError("Can't create stdio pipes. Err %d", mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    if (index == MPR_CMD_STDIN) {
        file->clientFd = fds[0];        /* read fd */
        file->fd = fds[1];              /* write fd */
    } else {
        file->clientFd = fds[1];        /* write fd */
        file->fd = fds[0];              /* read fd */
    }
    fcntl(file->fd, F_SETFL, fcntl(file->fd, F_GETFL) | O_NONBLOCK);
    mprLog(7, "makeChannel: pipe handles[%d] read %d, write %d", index, fds[0], fds[1]);
    return 0;
}

#elif VXWORKS
static int makeChannel(MprCmd *cmd, int index)
{
    MprCmdFile      *file;
    int             nonBlock;
    static int      tempSeed = 0;

    file = &cmd->files[index];
    file->name = sfmt("/pipe/%s_%d_%d", BIT_PRODUCT, taskIdSelf(), tempSeed++);

    if (pipeDevCreate(file->name, 5, MPR_BUFSIZE) < 0) {
        mprError("Can't create pipes to run %s", cmd->program);
        return MPR_ERR_CANT_OPEN;
    }
    /*
        Open the server end of the pipe. MPR_CMD_STDIN is from the client's perspective.
     */
    if (index == MPR_CMD_STDIN) {
        file->fd = open(file->name, O_WRONLY, 0644);
    } else {
        file->fd = open(file->name, O_RDONLY, 0644);
    }
    if (file->fd < 0) {
        mprError("Can't create stdio pipes. Err %d", mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    nonBlock = 1;
    ioctl(file->fd, FIONBIO, (int) &nonBlock);
    return 0;
}
#endif


#if BIT_UNIX_LIKE
static int startProcess(MprCmd *cmd)
{
    MprCmdFile      *files;
    int             rc, i, err;

    files = cmd->files;
    if (!cmd->signal) {
        cmd->signal = mprAddSignalHandler(SIGCHLD, reapCmd, cmd, cmd->dispatcher, MPR_SIGNAL_BEFORE);
    }
    /*
        Create the child
     */
    cmd->pid = vfork();

    if (cmd->pid < 0) {
        mprError("start: can't fork a new process to run %s, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_INITIALIZE;

    } else if (cmd->pid == 0) {
        /*
            Child
         */
        umask(022);
        if (cmd->flags & MPR_CMD_NEW_SESSION) {
            setsid();
        }
        if (cmd->dir) {
            if (chdir(cmd->dir) < 0) {
                mprError("cmd: Can't change directory to %s", cmd->dir);
                return MPR_ERR_CANT_INITIALIZE;
            }
        }
        if (cmd->flags & MPR_CMD_IN) {
            if (files[MPR_CMD_STDIN].clientFd >= 0) {
                dup2(files[MPR_CMD_STDIN].clientFd, 0);
                close(files[MPR_CMD_STDIN].fd);
            } else {
                close(0);
            }
        }
        if (cmd->flags & MPR_CMD_OUT) {
            if (files[MPR_CMD_STDOUT].clientFd >= 0) {
                dup2(files[MPR_CMD_STDOUT].clientFd, 1);
                close(files[MPR_CMD_STDOUT].fd);
            } else {
                close(1);
            }
        }
        if (cmd->flags & MPR_CMD_ERR) {
            if (files[MPR_CMD_STDERR].clientFd >= 0) {
                dup2(files[MPR_CMD_STDERR].clientFd, 2);
                close(files[MPR_CMD_STDERR].fd);
            } else {
                close(2);
            }
        }
        cmd->forkCallback(cmd->forkData);
        if (cmd->env) {
            rc = execve(cmd->program, (char**) cmd->argv, (char**) &cmd->env->items[0]);
        } else {
            rc = execv(cmd->program, (char**) cmd->argv);
        }
        err = errno;
        printf("Can't exec %s, rc %d, err %d\n", cmd->program, rc, err);

        /*
            Use _exit to avoid flushing I/O any other I/O.
         */
        _exit(-(MPR_ERR_CANT_INITIALIZE));

    } else {
        /*
            Close the client handles
         */
        for (i = 0; i < MPR_CMD_MAX_PIPE; i++) {
            if (files[i].clientFd >= 0) {
                close(files[i].clientFd);
                files[i].clientFd = -1;
            }
        }
    }
    return 0;
}


#elif VXWORKS
/*
    Start the command to run (stdIn and stdOut are named from the client's perspective)
 */
PUBLIC int startProcess(MprCmd *cmd)
{
    MprCmdTaskFn    entryFn;
    MprModule       *mp;
    SYM_TYPE        symType;
    char            *entryPoint, *program, *pair;
    int             pri, next;

    mprLog(4, "cmd: start %s", cmd->program);

    entryPoint = 0;
    if (cmd->env) {
        for (ITERATE_ITEMS(cmd->env, pair, next)) {
            if (sncmp(pair, "entryPoint=", 11) == 0) {
                entryPoint = sclone(&pair[11]);
            }
        }
    }
    program = mprGetPathBase(cmd->program);
    if (entryPoint == 0) {
        program = mprTrimPathExt(program);
        entryPoint = program;
    }
    if (symFindByName(sysSymTbl, entryPoint, (char**) &entryFn, &symType) < 0) {
        if ((mp = mprCreateModule(cmd->program, cmd->program, NULL, NULL)) == 0) {
            mprError("start: can't create module");
            return MPR_ERR_CANT_CREATE;
        }
        if (mprLoadModule(mp) < 0) {
            mprError("start: can't load DLL %s, errno %d", program, mprGetOsError());
            return MPR_ERR_CANT_READ;
        }
        if (symFindByName(sysSymTbl, entryPoint, (char**) &entryFn, &symType) < 0) {
            mprError("start: can't find symbol %s, errno %d", entryPoint, mprGetOsError());
            return MPR_ERR_CANT_ACCESS;
        }
    }
    taskPriorityGet(taskIdSelf(), &pri);

    cmd->pid = taskSpawn(entryPoint, pri, VX_FP_TASK | VX_PRIVATE_ENV, MPR_DEFAULT_STACK, (FUNCPTR) cmdTaskEntry, 
        (int) cmd->program, (int) entryFn, (int) cmd, 0, 0, 0, 0, 0, 0, 0);

    if (cmd->pid < 0) {
        mprError("start: can't create task %s, errno %d", entryPoint, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    mprLog(7, "cmd, child taskId %d", cmd->pid);

    if (semTake(cmd->startCond, MPR_TIMEOUT_START_TASK) != OK) {
        mprError("start: child %s did not initialize, errno %d", cmd->program, mprGetOsError());
        return MPR_ERR_CANT_CREATE;
    }
    semDelete(cmd->startCond);
    cmd->startCond = 0;
    return 0;
}


/*
    Executed by the child process
 */
static void cmdTaskEntry(char *program, MprCmdTaskFn entry, int cmdArg)
{
    MprCmd          *cmd;
    MprCmdFile      *files;
    WIND_TCB        *tcb;
    char            *item;
    int             inFd, outFd, errFd, id, next;

    cmd = (MprCmd*) cmdArg;

    /*
        Open standard I/O files (in/out are from the server's perspective)
     */
    files = cmd->files;
    inFd = open(files[MPR_CMD_STDIN].name, O_RDONLY, 0666);
    outFd = open(files[MPR_CMD_STDOUT].name, O_WRONLY, 0666);
    errFd = open(files[MPR_CMD_STDERR].name, O_WRONLY, 0666);

    if (inFd < 0 || outFd < 0 || errFd < 0) {
        exit(255);
    }
    id = taskIdSelf();
    ioTaskStdSet(id, 0, inFd);
    ioTaskStdSet(id, 1, outFd);
    ioTaskStdSet(id, 2, errFd);

    /*
        Now that we have opened the stdin and stdout, wakeup our parent.
     */
    semGive(cmd->startCond);

    /*
        Create the environment
     */
    if (envPrivateCreate(id, -1) < 0) {
        exit(254);
    }
    for (ITERATE_ITEMS(cmd->env, item, next)) {
        putenv(item);
    }

#if !VXWORKS
{
    char    *dir;
    int     rc;

    /*
        Set current directory if required
        WARNING: Setting working directory on VxWorks is global
     */
    if (cmd->dir) {
        rc = chdir(cmd->dir);
    } else {
        dir = mprGetPathDir(cmd->program);
        rc = chdir(dir);
    }
    if (rc < 0) {
        mprError("cmd: Can't change directory to %s", cmd->dir);
        exit(255);
    }
}
#endif

    /*
        Call the user's entry point
     */
    (entry)(cmd->argc, (char**) cmd->argv, (char**) cmd->env);

    tcb = taskTcb(id);
    cmd->status = tcb->exitCode;

    /*
        Cleanup
     */
    envPrivateDestroy(id);
    close(inFd);
    close(outFd);
    close(errFd);
    semGive(cmd->exitCond);
}


#endif /* VXWORKS */


static void closeFiles(MprCmd *cmd)
{
    int     i;
    for (i = 3; i < MPR_MAX_FILE; i++) {
        close(i);
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprCond.c"
 */
/************************************************************************/

/**
    mprCond.c - Thread Conditional variables

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */



/***************************** Forward Declarations ***************************/

static void manageCond(MprCond *cp, int flags);

/************************************ Code ************************************/
/*
    Create a condition variable for use by single or multiple waiters
 */

PUBLIC MprCond *mprCreateCond()
{
    MprCond     *cp;

    if ((cp = mprAllocObj(MprCond, manageCond)) == 0) {
        return 0;
    }
    cp->triggered = 0;
    cp->mutex = mprCreateLock();

#if BIT_WIN_LIKE
    cp->cv = CreateEvent(NULL, FALSE, FALSE, NULL);
#elif VXWORKS
    cp->cv = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#else
    pthread_cond_init(&cp->cv, NULL);
#endif
    return cp;
}


static void manageCond(MprCond *cp, int flags)
{
    mprAssert(cp);
    
    if (flags & MPR_MANAGE_MARK) {
        mprMark(cp->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        mprAssert(cp->mutex);
#if BIT_WIN_LIKE
        CloseHandle(cp->cv);
#elif VXWORKS
        semDelete(cp->cv);
#else
        pthread_cond_destroy(&cp->cv);
#endif
    }
}


/*
    Wait for the event to be triggered. Should only be used when there are single waiters. If the event is already
    triggered, then it will return immediately. Timeout of -1 means wait forever. Timeout of 0 means no wait.
    Returns 0 if the event was signalled. Returns < 0 for a timeout.
 */
PUBLIC int mprWaitForCond(MprCond *cp, MprTime timeout)
{
    MprTime             now, expire;
    int                 rc;
#if BIT_UNIX_LIKE
    struct timespec     waitTill;
    struct timeval      current;
    int                 usec;
#endif

    /*
        Avoid doing a mprGetTime() if timeout is < 0
     */
    rc = 0;
    if (timeout >= 0) {
        now = mprGetTime();
        expire = now + timeout;
#if BIT_UNIX_LIKE
        gettimeofday(&current, NULL);
        usec = current.tv_usec + ((int) (timeout % 1000)) * 1000;
        waitTill.tv_sec = current.tv_sec + ((int) (timeout / 1000)) + (usec / 1000000);
        waitTill.tv_nsec = (usec % 1000000) * 1000;
#endif
    } else {
        expire = -1;
        now = 0;
    }
    mprLock(cp->mutex);
    /*
        NOTE: The WaitForSingleObject and semTake APIs keeps state as to whether the object is signalled.
        WaitForSingleObject and semTake will not block if the object is already signalled. However, pthread_cond_ 
        is different and does not keep such state. If it is signalled before pthread_cond_wait, the thread will 
        still block. Consequently we need to keep our own state in cp->triggered. This also protects against 
        spurious wakeups which can happen (on windows).
     */
    do {
#if BIT_WIN_LIKE
        /*
            Regardless of the state of cp->triggered, we must call WaitForSingleObject to consume the signalled
            internal state of the object.
         */
        mprUnlock(cp->mutex);
        rc = WaitForSingleObject(cp->cv, (int) (expire - now));
        mprLock(cp->mutex);
        if (rc == WAIT_OBJECT_0) {
            rc = 0;
            ResetEvent(cp->cv);
        } else if (rc == WAIT_TIMEOUT) {
            rc = MPR_ERR_TIMEOUT;
        } else {
            rc = MPR_ERR;
        }
#elif VXWORKS
        /*
            Regardless of the state of cp->triggered, we must call semTake to consume the semaphore signalled state
         */
        mprUnlock(cp->mutex);
        rc = semTake(cp->cv, (int) (expire - now));
        mprLock(cp->mutex);
        if (rc != 0) {
            if (errno == S_objLib_OBJ_UNAVAILABLE) {
                rc = MPR_ERR_TIMEOUT;
            } else {
                rc = MPR_ERR;
            }
        }
        
#elif BIT_UNIX_LIKE
        /*
            NOTE: pthread_cond_timedwait can return 0 (MAC OS X and Linux). The pthread_cond_wait routines will 
            atomically unlock the mutex before sleeping and will relock on awakening.  
         */
        if (!cp->triggered) {
            if (now) {
                rc = pthread_cond_timedwait(&cp->cv, &cp->mutex->cs,  &waitTill);
            } else {
                rc = pthread_cond_wait(&cp->cv, &cp->mutex->cs);
            }
            if (rc == ETIMEDOUT) {
                rc = MPR_ERR_TIMEOUT;
            } else if (rc == EAGAIN) {
                rc = 0;
            } else if (rc != 0) {
                mprAssert(rc == 0);
                mprError("pthread_cond_timedwait error rc %d", rc);
                rc = MPR_ERR;
            }
        }
#endif
    } while (!cp->triggered && rc == 0 && (now && (now = mprGetTime()) < expire));

    if (cp->triggered) {
        cp->triggered = 0;
        rc = 0;
    } else if (rc == 0) {
        rc = MPR_ERR_TIMEOUT;
    }
    mprUnlock(cp->mutex);
    return rc;
}


/*
    Signal a condition and wakeup the waiter. Note: this may be called prior to the waiter waiting.
 */
PUBLIC void mprSignalCond(MprCond *cp)
{
    mprLock(cp->mutex);
    if (!cp->triggered) {
        cp->triggered = 1;
#if BIT_WIN_LIKE
        SetEvent(cp->cv);
#elif VXWORKS
        semGive(cp->cv);
#else
        pthread_cond_signal(&cp->cv);
#endif
    }
    mprUnlock(cp->mutex);
}


PUBLIC void mprResetCond(MprCond *cp)
{
    mprLock(cp->mutex);
    cp->triggered = 0;
#if BIT_WIN_LIKE
    ResetEvent(cp->cv);
#elif VXWORKS
    semDelete(cp->cv);
    cp->cv = semCCreate(SEM_Q_PRIORITY, SEM_EMPTY);
#else
    pthread_cond_destroy(&cp->cv);
    pthread_cond_init(&cp->cv, NULL);
#endif
    mprUnlock(cp->mutex);
}


/*
    Wait for the event to be triggered when there may be multiple waiters. This routine may return early due to
    other signals or events. The caller must verify if the signalled condition truly exists. If the event is already
    triggered, then it will return immediately. This call will not reset cp->triggered and must be reset manually.
    A timeout of -1 means wait forever. Timeout of 0 means no wait.  Returns 0 if the event was signalled. 
    Returns < 0 for a timeout.
 */
PUBLIC int mprWaitForMultiCond(MprCond *cp, MprTime timeout)
{
    int         rc;
#if BIT_UNIX_LIKE
    struct timespec     waitTill;
    struct timeval      current;
    int                 usec;
#else
    MprTime             now, expire;
#endif

    if (timeout < 0) {
        timeout = MAXINT;
    }
#if BIT_UNIX_LIKE
    gettimeofday(&current, NULL);
    usec = current.tv_usec + ((int) (timeout % 1000)) * 1000;
    waitTill.tv_sec = current.tv_sec + ((int) (timeout / 1000)) + (usec / 1000000);
    waitTill.tv_nsec = (usec % 1000000) * 1000;
#else
    now = mprGetTime();
    expire = now + timeout;
#endif

#if BIT_WIN_LIKE
    rc = WaitForSingleObject(cp->cv, (int) (expire - now));
    if (rc == WAIT_OBJECT_0) {
        rc = 0;
    } else if (rc == WAIT_TIMEOUT) {
        rc = MPR_ERR_TIMEOUT;
    } else {
        rc = MPR_ERR;
    }
#elif VXWORKS
    rc = semTake(cp->cv, (int) (expire - now));
    if (rc != 0) {
        if (errno == S_objLib_OBJ_UNAVAILABLE) {
            rc = MPR_ERR_TIMEOUT;
        } else {
            rc = MPR_ERR;
        }
    }
#elif BIT_UNIX_LIKE
    mprLock(cp->mutex);
    rc = pthread_cond_timedwait(&cp->cv, &cp->mutex->cs,  &waitTill);
    if (rc == ETIMEDOUT) {
        rc = MPR_ERR_TIMEOUT;
    } else if (rc != 0) {
        mprAssert(rc == 0);
        rc = MPR_ERR;
    }
    mprUnlock(cp->mutex);
#endif
    return rc;
}


/*
    Signal a condition and wakeup the all the waiters. Note: this may be called before or after to the waiter waiting.
 */
PUBLIC void mprSignalMultiCond(MprCond *cp)
{
    mprLock(cp->mutex);
#if BIT_WIN_LIKE
    /* Pulse event */
    SetEvent(cp->cv);
    ResetEvent(cp->cv);
#elif VXWORKS
    /* Reset sem count and then give once. Prevents accumulation */
    while (semTake(cp->cv, 0) == OK) ;
    semGive(cp->cv);
    semFlush(cp->cv);
#else
    pthread_cond_broadcast(&cp->cv);
#endif
    mprUnlock(cp->mutex);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprCrypt.c"
 */
/************************************************************************/

/*
    mprCrypt.c - Base-64 encoding and decoding and MD5 support.

    Algorithms by RSA. See license at the end of the file. 
    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Locals ***********************************/
/*
    MD5 Constants for transform routine.
 */
#define S11 7
#define S12 12
#define S13 17
#define S14 22
#define S21 5
#define S22 9
#define S23 14
#define S24 20
#define S31 4
#define S32 11
#define S33 16
#define S34 23
#define S41 6
#define S42 10
#define S43 15
#define S44 21

static uchar PADDING[64] = {
  0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/*
   MD5 F, G, H and I are basic MD5 functions.
 */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

/*
   MD5 ROTATE_LEFT rotates x left n bits.
 */
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

/*
     MD5 - FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4.
     Rotation is separate from addition to prevent recomputation.
 */
#define FF(a, b, c, d, x, s, ac) { \
    (a) += F ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define GG(a, b, c, d, x, s, ac) { \
    (a) += G ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define HH(a, b, c, d, x, s, ac) { \
    (a) += H ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}
#define II(a, b, c, d, x, s, ac) { \
    (a) += I ((b), (c), (d)) + (x) + (uint)(ac); \
    (a) = ROTATE_LEFT ((a), (s)); \
    (a) += (b); \
}

typedef struct {
    uint state[4];
    uint count[2];
    uchar buffer[64];
} MD5CONTEXT;

/******************************* Base 64 Data *********************************/

#define CRYPT_HASH_SIZE   16

/*
    Encoding map lookup
 */
static char encodeMap[] = {
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
    'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
    'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
    'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
    'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
    'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
    'w', 'x', 'y', 'z', '0', '1', '2', '3',
    '4', '5', '6', '7', '8', '9', '+', '/',
};


/*
    Decode map
 */
static signed char decodeMap[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, 
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};


#define SHA_SIZE   20

typedef struct MprSha {
    uint    hash[SHA_SIZE / 4];     /* Message Digest */
    uint    lowLength;              /* Message length in bits */
    uint    highLength;             /* Message length in bits */
    int     index;                  /* Index into message block array   */
    uchar   block[64];              /* 512-bit message blocks */
} MprSha;

#define shaShift(bits,word) (((word) << (bits)) | ((word) >> (32-(bits))))

/*************************** Forward Declarations *****************************/

static void decode(uint *output, uchar *input, uint len);
static void encode(uchar *output, uint *input, uint len);
static void finalizeMD5(uchar digest[16], MD5CONTEXT *context);
static void initMD5(MD5CONTEXT *context);
static void transform(uint state[4], uchar block[64]);
static void update(MD5CONTEXT *context, uchar *input, uint inputLen);

static void shaInit(MprSha *sha);
static void shaUpdate(MprSha *sha, cuchar *msg, ssize len);
static void shaFinalize(uchar *digest, MprSha *sha);
static void shaPad(MprSha *sha);
static void shaProcess(MprSha *sha);

/*********************************** Code *************************************/

PUBLIC int mprRandom()
{
#if WINDOWS || VXWORKS
    return rand();
#else
    return (int) random();
#endif
}


/*
    Decode a null terminated string and returns a null terminated string.
    Stops decoding at the end of string or '='
 */
PUBLIC char *mprDecode64(cchar *s)
{
    return mprDecode64Block(s, NULL, MPR_DECODE_TOKEQ);
}


/*
    Decode a null terminated string and return a block with length.
    Stops decoding at the end of the block or '=' if MPR_DECODE_TOKEQ is specified.
 */
PUBLIC char *mprDecode64Block(cchar *s, ssize *len, int flags)
{
    uint    bitBuf;
    char    *buffer, *bp;
    cchar   *end;
    ssize   size;
    int     c, i, j, shift;

    size = slen(s);
    if ((buffer = mprAlloc(size + 1)) == 0) {
        return NULL;
    }
    bp = buffer;
    *bp = '\0';
    end = &s[size];
    while (s < end && (*s != '=' || !(flags & MPR_DECODE_TOKEQ))) {
        bitBuf = 0;
        shift = 18;
        for (i = 0; i < 4 && (s < end && (*s != '=' || !(flags & MPR_DECODE_TOKEQ))); i++, s++) {
            c = decodeMap[*s & 0xff];
            if (c == -1) {
                return NULL;
            } 
            bitBuf = bitBuf | (c << shift);
            shift -= 6;
        }
        --i;
        mprAssert((bp + i) < &buffer[size]);
        for (j = 0; j < i; j++) {
            *bp++ = (char) ((bitBuf >> (8 * (2 - j))) & 0xff);
        }
        *bp = '\0';
    }
    if (len) {
        *len = bp - buffer;
    }
    return buffer;
}


/*
    Encode a null terminated string.
    Returns a null terminated block
 */
PUBLIC char *mprEncode64(cchar *s)
{
    return mprEncode64Block(s, slen(s));
}


/*
    Encode a block of a given length
    Returns a null terminated block
 */
PUBLIC char *mprEncode64Block(cchar *s, ssize len)
{
    uint    shiftbuf;
    char    *buffer, *bp;
    cchar   *end;
    ssize   size;
    int     i, j, shift;

    size = len * 2;
    if ((buffer = mprAlloc(size + 1)) == 0) {
        return NULL;
    }
    bp = buffer;
    *bp = '\0';
    end = &s[len];
    while (s < end) {
        shiftbuf = 0;
        for (j = 2; j >= 0; j--, s++) {
            shiftbuf |= ((*s & 0xff) << (j * 8));
        }
        shift = 18;
        for (i = ++j; i < 4 && bp < &buffer[size] ; i++) {
            *bp++ = encodeMap[(shiftbuf >> shift) & 0x3f];
            shift -= 6;
        }
        while (j-- > 0) {
            *bp++ = '=';
        }
        *bp = '\0';
    }
    return buffer;
}


PUBLIC char *mprGetMD5(cchar *s)
{
    return mprGetMD5WithPrefix(s, slen(s), NULL);
}


/*
    Return the MD5 hash of a block. Returns allocated string. A prefix for the result can be supplied.
 */
PUBLIC char *mprGetMD5WithPrefix(cchar *buf, ssize length, cchar *prefix)
{
    MD5CONTEXT      context;
    uchar           hash[CRYPT_HASH_SIZE];
    cchar           *hex = "0123456789abcdef";
    char            *r, *str;
    char            result[(CRYPT_HASH_SIZE * 2) + 1];
    ssize           len;
    int             i;

    if (length < 0) {
        length = slen(buf);
    }
    initMD5(&context);
    update(&context, (uchar*) buf, (uint) length);
    finalizeMD5(hash, &context);

    for (i = 0, r = result; i < 16; i++) {
        *r++ = hex[hash[i] >> 4];
        *r++ = hex[hash[i] & 0xF];
    }
    *r = '\0';
    len = (prefix) ? slen(prefix) : 0;
    str = mprAlloc(sizeof(result) + len);
    if (str) {
        if (prefix) {
            strcpy(str, prefix);
        }
        strcpy(str + len, result);
    }
    return str;
}


/*
    MD5 initialization. Begins an MD5 operation, writing a new context.
 */ 
static void initMD5(MD5CONTEXT *context)
{
    context->count[0] = context->count[1] = 0;
    context->state[0] = 0x67452301;
    context->state[1] = 0xefcdab89;
    context->state[2] = 0x98badcfe;
    context->state[3] = 0x10325476;
}


/*
    MD5 block update operation. Continues an MD5 message-digest operation, processing another message block, 
    and updating the context.
 */
static void update(MD5CONTEXT *context, uchar *input, uint inputLen)
{
    uint    i, index, partLen;

    index = (uint) ((context->count[0] >> 3) & 0x3F);

    if ((context->count[0] += ((uint)inputLen << 3)) < ((uint)inputLen << 3)){
        context->count[1]++;
    }
    context->count[1] += ((uint)inputLen >> 29);
    partLen = 64 - index;

    if (inputLen >= partLen) {
        memcpy((uchar*) &context->buffer[index], (uchar*) input, partLen);
        transform(context->state, context->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64) {
            transform(context->state, &input[i]);
        }
        index = 0;
    } else {
        i = 0;
    }
    memcpy((uchar*) &context->buffer[index], (uchar*) &input[i], inputLen-i);
}


/*
    MD5 finalization. Ends an MD5 message-digest operation, writing the message digest and zeroizing the context.
 */ 
static void finalizeMD5(uchar digest[16], MD5CONTEXT *context)
{
    uchar   bits[8];
    uint    index, padLen;

    /* Save number of bits */
    encode(bits, context->count, 8);

    /* Pad out to 56 mod 64. */
    index = (uint)((context->count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    update(context, PADDING, padLen);

    /* Append length (before padding) */
    update(context, bits, 8);
    /* Store state in digest */
    encode(digest, context->state, 16);

    /* Zero sensitive information. */
    memset((uchar*)context, 0, sizeof (*context));
}


/*
    MD5 basic transformation. Transforms state based on block.
 */
static void transform(uint state[4], uchar block[64])
{
    uint a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    decode(x, block, 64);

    /* Round 1 */
    FF(a, b, c, d, x[ 0], S11, 0xd76aa478); /* 1 */
    FF(d, a, b, c, x[ 1], S12, 0xe8c7b756); /* 2 */
    FF(c, d, a, b, x[ 2], S13, 0x242070db); /* 3 */
    FF(b, c, d, a, x[ 3], S14, 0xc1bdceee); /* 4 */
    FF(a, b, c, d, x[ 4], S11, 0xf57c0faf); /* 5 */
    FF(d, a, b, c, x[ 5], S12, 0x4787c62a); /* 6 */
    FF(c, d, a, b, x[ 6], S13, 0xa8304613); /* 7 */
    FF(b, c, d, a, x[ 7], S14, 0xfd469501); /* 8 */
    FF(a, b, c, d, x[ 8], S11, 0x698098d8); /* 9 */
    FF(d, a, b, c, x[ 9], S12, 0x8b44f7af); /* 10 */
    FF(c, d, a, b, x[10], S13, 0xffff5bb1); /* 11 */
    FF(b, c, d, a, x[11], S14, 0x895cd7be); /* 12 */
    FF(a, b, c, d, x[12], S11, 0x6b901122); /* 13 */
    FF(d, a, b, c, x[13], S12, 0xfd987193); /* 14 */
    FF(c, d, a, b, x[14], S13, 0xa679438e); /* 15 */
    FF(b, c, d, a, x[15], S14, 0x49b40821); /* 16 */

    /* Round 2 */
    GG(a, b, c, d, x[ 1], S21, 0xf61e2562); /* 17 */
    GG(d, a, b, c, x[ 6], S22, 0xc040b340); /* 18 */
    GG(c, d, a, b, x[11], S23, 0x265e5a51); /* 19 */
    GG(b, c, d, a, x[ 0], S24, 0xe9b6c7aa); /* 20 */
    GG(a, b, c, d, x[ 5], S21, 0xd62f105d); /* 21 */
    GG(d, a, b, c, x[10], S22,  0x2441453); /* 22 */
    GG(c, d, a, b, x[15], S23, 0xd8a1e681); /* 23 */
    GG(b, c, d, a, x[ 4], S24, 0xe7d3fbc8); /* 24 */
    GG(a, b, c, d, x[ 9], S21, 0x21e1cde6); /* 25 */
    GG(d, a, b, c, x[14], S22, 0xc33707d6); /* 26 */
    GG(c, d, a, b, x[ 3], S23, 0xf4d50d87); /* 27 */
    GG(b, c, d, a, x[ 8], S24, 0x455a14ed); /* 28 */
    GG(a, b, c, d, x[13], S21, 0xa9e3e905); /* 29 */
    GG(d, a, b, c, x[ 2], S22, 0xfcefa3f8); /* 30 */
    GG(c, d, a, b, x[ 7], S23, 0x676f02d9); /* 31 */
    GG(b, c, d, a, x[12], S24, 0x8d2a4c8a); /* 32 */

    /* Round 3 */
    HH(a, b, c, d, x[ 5], S31, 0xfffa3942); /* 33 */
    HH(d, a, b, c, x[ 8], S32, 0x8771f681); /* 34 */
    HH(c, d, a, b, x[11], S33, 0x6d9d6122); /* 35 */
    HH(b, c, d, a, x[14], S34, 0xfde5380c); /* 36 */
    HH(a, b, c, d, x[ 1], S31, 0xa4beea44); /* 37 */
    HH(d, a, b, c, x[ 4], S32, 0x4bdecfa9); /* 38 */
    HH(c, d, a, b, x[ 7], S33, 0xf6bb4b60); /* 39 */
    HH(b, c, d, a, x[10], S34, 0xbebfbc70); /* 40 */
    HH(a, b, c, d, x[13], S31, 0x289b7ec6); /* 41 */
    HH(d, a, b, c, x[ 0], S32, 0xeaa127fa); /* 42 */
    HH(c, d, a, b, x[ 3], S33, 0xd4ef3085); /* 43 */
    HH(b, c, d, a, x[ 6], S34,  0x4881d05); /* 44 */
    HH(a, b, c, d, x[ 9], S31, 0xd9d4d039); /* 45 */
    HH(d, a, b, c, x[12], S32, 0xe6db99e5); /* 46 */
    HH(c, d, a, b, x[15], S33, 0x1fa27cf8); /* 47 */
    HH(b, c, d, a, x[ 2], S34, 0xc4ac5665); /* 48 */

    /* Round 4 */
    II(a, b, c, d, x[ 0], S41, 0xf4292244); /* 49 */
    II(d, a, b, c, x[ 7], S42, 0x432aff97); /* 50 */
    II(c, d, a, b, x[14], S43, 0xab9423a7); /* 51 */
    II(b, c, d, a, x[ 5], S44, 0xfc93a039); /* 52 */
    II(a, b, c, d, x[12], S41, 0x655b59c3); /* 53 */
    II(d, a, b, c, x[ 3], S42, 0x8f0ccc92); /* 54 */
    II(c, d, a, b, x[10], S43, 0xffeff47d); /* 55 */
    II(b, c, d, a, x[ 1], S44, 0x85845dd1); /* 56 */
    II(a, b, c, d, x[ 8], S41, 0x6fa87e4f); /* 57 */
    II(d, a, b, c, x[15], S42, 0xfe2ce6e0); /* 58 */
    II(c, d, a, b, x[ 6], S43, 0xa3014314); /* 59 */
    II(b, c, d, a, x[13], S44, 0x4e0811a1); /* 60 */
    II(a, b, c, d, x[ 4], S41, 0xf7537e82); /* 61 */
    II(d, a, b, c, x[11], S42, 0xbd3af235); /* 62 */
    II(c, d, a, b, x[ 2], S43, 0x2ad7d2bb); /* 63 */
    II(b, c, d, a, x[ 9], S44, 0xeb86d391); /* 64 */

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;

    /* Zero sensitive information. */
    memset((uchar*) x, 0, sizeof(x));
}


/*
    Encodes input(uint) into output(uchar). Assumes len is a multiple of 4.
 */
static void encode(uchar *output, uint *input, uint len)
{
    uint i, j;

    for (i = 0, j = 0; j < len; i++, j += 4) {
        output[j] = (uchar) (input[i] & 0xff);
        output[j+1] = (uchar) ((input[i] >> 8) & 0xff);
        output[j+2] = (uchar) ((input[i] >> 16) & 0xff);
        output[j+3] = (uchar) ((input[i] >> 24) & 0xff);
    }
}


/*
    Decodes input(uchar) into output(uint). Assumes len is a multiple of 4.
 */
static void decode(uint *output, uchar *input, uint len)
{
    uint    i, j;

    for (i = 0, j = 0; j < len; i++, j += 4)
        output[i] = ((uint) input[j]) | (((uint) input[j+1]) << 8) | (((uint) input[j+2]) << 16) | 
            (((uint) input[j+3]) << 24);
}


/************************************* Sha1 **********************************/

PUBLIC char *mprGetSHA(cchar *s)
{
    return mprGetSHAWithPrefix(s, slen(s), NULL);
}


PUBLIC char *mprGetSHABase64(cchar *s)
{
    MprSha  sha;
    uchar   hash[SHA_SIZE + 1];

    shaInit(&sha);
    shaUpdate(&sha, (cuchar*) s, slen(s));
    shaFinalize(hash, &sha);
    hash[SHA_SIZE] = '\0';
    return mprEncode64Block((char*) hash, SHA_SIZE);
}


PUBLIC char *mprGetSHAWithPrefix(cchar *buf, ssize length, cchar *prefix)
{
    MprSha  sha;
    uchar   hash[SHA_SIZE];
    cchar   *hex = "0123456789abcdef";
    char    *r, *str;
    char    result[(SHA_SIZE * 2) + 1];
    ssize   len;
    int     i;

    if (length < 0) {
        length = slen(buf);
    }
    shaInit(&sha);
    shaUpdate(&sha, (cuchar*) buf, length);
    shaFinalize(hash, &sha);

    for (i = 0, r = result; i < SHA_SIZE; i++) {
        *r++ = hex[hash[i] >> 4];
        *r++ = hex[hash[i] & 0xF];
    }
    *r = '\0';
    len = (prefix) ? slen(prefix) : 0;
    str = mprAlloc(sizeof(result) + len);
    if (str) {
        if (prefix) {
            strcpy(str, prefix);
        }
        strcpy(str + len, result);
    }
    return str;
}


static void shaInit(MprSha *sha)
{
    sha->lowLength = 0;
    sha->highLength = 0;
    sha->index = 0;
    sha->hash[0] = 0x67452301;
    sha->hash[1] = 0xEFCDAB89;
    sha->hash[2] = 0x98BADCFE;
    sha->hash[3] = 0x10325476;
    sha->hash[4] = 0xC3D2E1F0;
}


static void shaUpdate(MprSha *sha, cuchar *msg, ssize len)
{
    while (len--) {
        sha->block[sha->index++] = (*msg & 0xFF);
        sha->lowLength += 8;
        if (sha->lowLength == 0) {
            sha->highLength++;
        }
        if (sha->index == 64) {
            shaProcess(sha);
        }
        msg++;
    }
}


static void shaFinalize(uchar *digest, MprSha *sha)
{
    int i;

    shaPad(sha);
    memset(sha->block, 0, 64);
    sha->lowLength = 0;
    sha->highLength = 0;
    for  (i = 0; i < SHA_SIZE; i++) {
        digest[i] = sha->hash[i >> 2] >> 8 * (3 - (i & 0x03));
    }
}


static void shaProcess(MprSha *sha)
{
    uint    K[] = { 0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6 };
    uint    temp, W[80], A, B, C, D, E;
    int     t;

    for  (t = 0; t < 16; t++) {
        W[t] = sha->block[t * 4] << 24;
        W[t] |= sha->block[t * 4 + 1] << 16;
        W[t] |= sha->block[t * 4 + 2] << 8;
        W[t] |= sha->block[t * 4 + 3];
    }
    for (t = 16; t < 80; t++) {
       W[t] = shaShift(1, W[t-3] ^ W[t-8] ^ W[t-14] ^ W[t-16]);
    }
    A = sha->hash[0];
    B = sha->hash[1];
    C = sha->hash[2];
    D = sha->hash[3];
    E = sha->hash[4];

    for (t = 0; t < 20; t++) {
        temp =  shaShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 20; t < 40; t++) {
        temp = shaShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 40; t < 60; t++) {
        temp = shaShift(5, A) + ((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    for (t = 60; t < 80; t++) {
        temp = shaShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
        E = D;
        D = C;
        C = shaShift(30, B);
        B = A;
        A = temp;
    }
    sha->hash[0] += A;
    sha->hash[1] += B;
    sha->hash[2] += C;
    sha->hash[3] += D;
    sha->hash[4] += E;
    sha->index = 0;
}


static void shaPad(MprSha *sha)
{
    if (sha->index > 55) {
        sha->block[sha->index++] = 0x80;
        while(sha->index < 64) {
            sha->block[sha->index++] = 0;
        }
        shaProcess(sha);
        while (sha->index < 56) {
            sha->block[sha->index++] = 0;
        }
    } else {
        sha->block[sha->index++] = 0x80;
        while(sha->index < 56) {
            sha->block[sha->index++] = 0;
        }
    }
    sha->block[56] = sha->highLength >> 24;
    sha->block[57] = sha->highLength >> 16;
    sha->block[58] = sha->highLength >> 8;
    sha->block[59] = sha->highLength;
    sha->block[60] = sha->lowLength >> 24;
    sha->block[61] = sha->lowLength >> 16;
    sha->block[62] = sha->lowLength >> 8;
    sha->block[63] = sha->lowLength;
    shaProcess(sha);
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprDisk.c"
 */
/************************************************************************/

/**
    mprDisk.c - File services for systems with a (disk) based file system.

    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



#if !BIT_ROM
/*********************************** Defines **********************************/

#if WINDOWS
/*
    Open/Delete retries to circumvent windows pending delete problems
 */
#define RETRIES 40

/*
    Windows only permits owner bits
 */
#define MASK_PERMS(perms)    perms & 0600
#else
#define MASK_PERMS(perms)    perms
#endif

/********************************** Forwards **********************************/

static int closeFile(MprFile *file);
static void manageDiskFile(MprFile *file, int flags);
static int getPathInfo(MprDiskFileSystem *fs, cchar *path, MprPath *info);

/************************************ Code ************************************/
#if FUTURE
/*
    Open a file with support for cygwin paths. Tries windows path first then under /cygwin.
 */
static int cygOpen(MprFileSystem *fs, cchar *path, int omode, int perms)
{
    int     fd;

    fd = open(path, omode, MASK_PERMS(perms));
#if WINDOWS
    if (fd < 0) {
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        fd = open(path, omode, MASK_PERMS(perms));
    }
#endif
    return fd;
}
#endif

static MprFile *openFile(MprFileSystem *fs, cchar *path, int omode, int perms)
{
    MprFile     *file;
    
    mprAssert(path);

    if ((file = mprAllocObj(MprFile, manageDiskFile)) == 0) {
        return NULL;
    }
    file->path = sclone(path);
    file->fd = open(path, omode, MASK_PERMS(perms));
    if (file->fd < 0) {
#if WINDOWS
        /*
            Windows opens can fail of immediately following a delete. Windows uses pending deletes which prevent opens.
         */
        int i, err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            for (i = 0; i < RETRIES; i++) {
                file->fd = open(path, omode, MASK_PERMS(perms));
                if (file->fd >= 0) {
                    break;
                }
                mprNap(10);
            }
            if (file->fd < 0) {
                file = NULL;
            }
        } else {
            file = NULL;
        }
#else
        file = NULL;
#endif
    }
    return file;
}


static void manageDiskFile(MprFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->path);
        mprMark(file->fileSystem);
        mprMark(file->buf);
#if BIT_ROM
        mprMark(file->inode);
#endif

    } else if (flags & MPR_MANAGE_FREE) {
        closeFile(file);
    }
}


static int closeFile(MprFile *file)
{
    MprBuf  *bp;

    mprAssert(file);

    if (file == 0) {
        return 0;
    }
    bp = file->buf;
    if (bp && (file->mode & (O_WRONLY | O_RDWR))) {
        mprFlushFile(file);
    }
    if (file->fd >= 0) {
        close(file->fd);
        file->fd = -1;
    }
    return 0;
}


static ssize readFile(MprFile *file, void *buf, ssize size)
{
    mprAssert(file);
    mprAssert(buf);

    return read(file->fd, buf, (uint) size);
}


static ssize writeFile(MprFile *file, cvoid *buf, ssize count)
{
    mprAssert(file);
    mprAssert(buf);

#if VXWORKS
    return write(file->fd, (void*) buf, count);
#else
    return write(file->fd, buf, (uint) count);
#endif
}


static MprOff seekFile(MprFile *file, int seekType, MprOff distance)
{
    mprAssert(file);

    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
#if BIT_WIN_LIKE
    return (MprOff) _lseeki64(file->fd, (int64) distance, seekType);
#elif BIT_HAS_OFF64
    return (MprOff) lseek64(file->fd, (off64_t) distance, seekType);
#else
    return (MprOff) lseek(file->fd, (off_t) distance, seekType);
#endif
}


static bool accessPath(MprDiskFileSystem *fs, cchar *path, int omode)
{
#if BIT_WIN && FUTURE
    if (access(path, omode) < 0) {
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
    }
#endif
    return access(path, omode) == 0;
}


static int deletePath(MprDiskFileSystem *fs, cchar *path)
{
    MprPath     info;

    if (getPathInfo(fs, path, &info) == 0 && info.isDir) {
        return rmdir((char*) path);
    }
#if WINDOWS
{
    /*
        NOTE: Windows delete makes a file pending delete which prevents immediate recreation. Rename and then delete.
     */
    int i, err;
    for (i = 0; i < RETRIES; i++) {
        if (DeleteFile(wide(path)) != 0) {
            return 0;
        }
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION) {
            break;
        }
        mprNap(10);
    }
    return MPR_ERR_CANT_DELETE;
}
#else
    return unlink((char*) path);
#endif
}
 

static int makeDir(MprDiskFileSystem *fs, cchar *path, int perms, int owner, int group)
{
    int     rc;

#if VXWORKS
    rc = mkdir((char*) path);
#else
    rc = mkdir(path, perms);
#endif
    if (rc < 0) {
        return MPR_ERR_CANT_CREATE;
    }
#if BIT_UNIX_LIKE
    if ((owner != -1 || group != -1) && chown(path, owner, group) < 0) {
        rmdir(path);
        return MPR_ERR_CANT_COMPLETE;
    }
#endif
    return 0;
}


static int makeLink(MprDiskFileSystem *fs, cchar *path, cchar *target, int hard)
{
#if BIT_UNIX_LIKE
    if (hard) {
        return link(target, path);
    } else {
        return symlink(target, path);
    }
#else
    return MPR_ERR_BAD_STATE;
#endif
}


static int getPathInfo(MprDiskFileSystem *fs, cchar *path, MprPath *info)
{
#if WINCE
    struct stat s;
    cchar       *ext;

    mprAssert(path);
    mprAssert(info);

    info->checked = 1;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;

    if (_stat64(path, &s) < 0) {
        return -1;
    }
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
    info->inode = s.st_ino;
    info->isDir = (s.st_mode & S_IFDIR) != 0;
    info->isReg = (s.st_mode & S_IFREG) != 0;
    info->isLink = 0;
    ext = mprGetPathExt(path);
    if (ext && strcmp(ext, "lnk") == 0) {
        info->isLink = 1;
    }

#elif BIT_WIN_LIKE
    struct __stat64     s;
    cchar               *ext;

    mprAssert(path);
    mprAssert(info);
    info->checked = 1;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    if (_stat64(path, &s) < 0) {
#if BIT_WIN && FUTURE
        /*
            Try under /cygwin
         */
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        if (_stat64(path, &s) < 0) {
            return -1;
        }
#else
        return -1;
#endif
    }
    ext = mprGetPathExt(path);
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
    info->inode = s.st_ino;
    info->isDir = (s.st_mode & S_IFDIR) != 0;
    info->isReg = (s.st_mode & S_IFREG) != 0;
    info->isLink = 0;
    if (ext) {
        if (strcmp(ext, "lnk") == 0) {
            info->isLink = 1;
        } else if (strcmp(ext, "dll") == 0) {
            info->perms |= 111;
        }
    }
    /*
        Work hard on windows to determine if the file is a regular file.
     */
    if (info->isReg) {
        long    att;

        if ((att = GetFileAttributes(wide(path))) == -1) {
            return -1;
        }
        if (att & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ENCRYPTED |
                FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_OFFLINE)) {
            /*
                Catch accesses to devices like CON, AUX, NUL, LPT etc att will be set to ENCRYPTED on Win9X and NT.
             */
            info->isReg = 0;
        }
        if (info->isReg) {
            HANDLE handle;
            handle = CreateFile(wide(path), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
            if (handle == INVALID_HANDLE_VALUE) {
                info->isReg = 0;
            } else {
                long    fileType;
                fileType = GetFileType(handle);
                if (fileType == FILE_TYPE_CHAR || fileType == FILE_TYPE_PIPE) {
                    info->isReg = 0;
                }
                CloseHandle(handle);
            }
        }
    }
    if (strcmp(path, "nul") == 0) {
        info->isReg = 0;
    }

#elif VXWORKS
    struct stat s;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    info->checked = 1;
    if (stat((char*) path, &s) < 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->inode = s.st_ino;
    info->isDir = S_ISDIR(s.st_mode);
    info->isReg = S_ISREG(s.st_mode);
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
#else
    struct stat s;
    info->valid = 0;
    info->isReg = 0;
    info->isDir = 0;
    info->checked = 1;
    if (lstat((char*) path, &s) < 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    #ifdef S_ISLNK
        info->isLink = S_ISLNK(s.st_mode);
        if (info->isLink) {
            if (stat((char*) path, &s) < 0) {
                return MPR_ERR_CANT_ACCESS;
            }
        }
    #endif
    info->valid = 1;
    info->size = s.st_size;
    info->atime = s.st_atime;
    info->ctime = s.st_ctime;
    info->mtime = s.st_mtime;
    info->inode = s.st_ino;
    info->isDir = S_ISDIR(s.st_mode);
    info->isReg = S_ISREG(s.st_mode);
    info->perms = s.st_mode & 07777;
    info->owner = s.st_uid;
    info->group = s.st_gid;
    if (strcmp(path, "/dev/null") == 0) {
        info->isReg = 0;
    }
#endif
    return 0;
}
 
static char *getPathLink(MprDiskFileSystem *fs, cchar *path)
{
#if BIT_UNIX_LIKE
    char    pbuf[MPR_MAX_PATH];
    ssize   len;

    if ((len = readlink(path, pbuf, sizeof(pbuf) - 1)) < 0) {
        return NULL;
    }
    pbuf[len] = '\0';
    return sclone(pbuf);
#else
    return NULL;
#endif
}


static int truncateFile(MprDiskFileSystem *fs, cchar *path, MprOff size)
{
    if (!mprPathExists(path, F_OK)) {
#if BIT_WIN_LIKE && FUTURE
        /*
            Try under /cygwin
         */
        if (*path == '/') {
            path = sjoin(fs->cygwin, path, NULL);
        }
        if (!mprPathExists(path, F_OK))
#endif
        return MPR_ERR_CANT_ACCESS;
    }
#if BIT_WIN_LIKE
{
    HANDLE  h;

    h = CreateFile(wide(path), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    SetFilePointer(h, (LONG) size, 0, FILE_BEGIN);
    if (h == INVALID_HANDLE_VALUE || SetEndOfFile(h) == 0) {
        CloseHandle(h);
        return MPR_ERR_CANT_WRITE;
    }
    CloseHandle(h);
}
#elif VXWORKS
{
#if FUTURE
    int     fd;

    fd = open(path, O_WRONLY, 0664);
    if (fd < 0 || ftruncate(fd, size) < 0) {
        return MPR_ERR_CANT_WRITE;
    }
    close(fd);
#endif
    return MPR_ERR_CANT_WRITE;
}
#else
    if (truncate(path, size) < 0) {
        return MPR_ERR_CANT_WRITE;
    }
#endif
    return 0;
}


static void manageDiskFileSystem(MprDiskFileSystem *dfs, int flags)
{
#if !WINCE
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dfs->separators);
        mprMark(dfs->newline);
        mprMark(dfs->root);
#if BIT_WIN_LIKE || CYGWIN
        mprMark(dfs->cygdrive);
        mprMark(dfs->cygwin);
#endif
    }
#endif
}


PUBLIC MprDiskFileSystem *mprCreateDiskFileSystem(cchar *path)
{
    MprFileSystem       *fs;
    MprDiskFileSystem   *dfs;

    if ((dfs = mprAllocObj(MprDiskFileSystem, manageDiskFileSystem)) == 0) {
        return 0;
    }

    /*
        Temporary
     */
    fs = (MprFileSystem*) dfs;
    dfs->accessPath = accessPath;
    dfs->deletePath = deletePath;
    dfs->getPathInfo = getPathInfo;
    dfs->getPathLink = getPathLink;
    dfs->makeDir = makeDir;
    dfs->makeLink = makeLink;
    dfs->openFile = openFile;
    dfs->closeFile = closeFile;
    dfs->readFile = readFile;
    dfs->seekFile = seekFile;
    dfs->truncateFile = truncateFile;
    dfs->writeFile = writeFile;

#if !WINCE
    if ((MPR->stdError = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdError, "stderr");
    MPR->stdError->fd = 2;
    MPR->stdError->fileSystem = fs;
    MPR->stdError->mode = O_WRONLY;

    if ((MPR->stdInput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdInput, "stdin");
    MPR->stdInput->fd = 0;
    MPR->stdInput->fileSystem = fs;
    MPR->stdInput->mode = O_RDONLY;

    if ((MPR->stdOutput = mprAllocStruct(MprFile)) == 0) {
        return NULL;
    }
    mprSetName(MPR->stdOutput, "stdout");
    MPR->stdOutput->fd = 1;
    MPR->stdOutput->fileSystem = fs;
    MPR->stdOutput->mode = O_WRONLY;
#endif
    return dfs;
}
#endif /* !BIT_ROM */


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprDispatcher.c"
 */
/************************************************************************/

/*
    mprDispatcher.c - Event dispatch services

    This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/***************************** Forward Declarations ***************************/

static void dequeueDispatcher(MprDispatcher *dispatcher);
static int dispatchEvents(MprDispatcher *dispatcher);
static MprTime getDispatcherIdleTime(MprDispatcher *dispatcher, MprTime timeout);
static MprTime getIdleTime(MprEventService *es, MprTime timeout);
static MprDispatcher *getNextReadyDispatcher(MprEventService *es);
static void initDispatcher(MprDispatcher *q);
static int makeRunnable(MprDispatcher *dispatcher);
static void manageDispatcher(MprDispatcher *dispatcher, int flags);
static void manageEventService(MprEventService *es, int flags);
static void queueDispatcher(MprDispatcher *prior, MprDispatcher *dispatcher);
static void scheduleDispatcher(MprDispatcher *dispatcher);
static void serviceDispatcherMain(MprDispatcher *dispatcher);
static bool serviceDispatcher(MprDispatcher *dp);

#define isRunning(dispatcher) (dispatcher->parent == dispatcher->service->runQ)
#define isReady(dispatcher) (dispatcher->parent == dispatcher->service->readyQ)
#define isWaiting(dispatcher) (dispatcher->parent == dispatcher->service->waitQ)
#define isEmpty(dispatcher) (dispatcher->eventQ->next == dispatcher->eventQ)

/************************************* Code ***********************************/
/*
    Create the overall dispatch service. There may be many event dispatchers.
 */
PUBLIC MprEventService *mprCreateEventService()
{
    MprEventService     *es;

    if ((es = mprAllocObj(MprEventService, manageEventService)) == 0) {
        return 0;
    }
    MPR->eventService = es;
    es->now = mprGetTime();
    es->mutex = mprCreateLock();
    es->waitCond = mprCreateCond();
    es->runQ = mprCreateDispatcher("running", 0);
    es->readyQ = mprCreateDispatcher("ready", 0);
    es->idleQ = mprCreateDispatcher("idle", 0);
    es->pendingQ = mprCreateDispatcher("pending", 0);
    es->waitQ = mprCreateDispatcher("waiting", 0);
    return es;
}


static void manageEventService(MprEventService *es, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(es->runQ);
        mprMark(es->readyQ);
        mprMark(es->waitQ);
        mprMark(es->idleQ);
        mprMark(es->pendingQ);
        mprMark(es->waitCond);
        mprMark(es->mutex);

    } else if (flags & MPR_MANAGE_FREE) {
        /* Needed for race with manageDispatcher */
        es->mutex = 0;
    }
}


PUBLIC void mprStopEventService()
{
    mprWakeDispatchers();
    mprWakeNotifier();
}


/*
    Create a disabled dispatcher. A dispatcher schedules events on a single dispatch queue.
 */
PUBLIC MprDispatcher *mprCreateDispatcher(cchar *name, int enable)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;

    if ((dispatcher = mprAllocObj(MprDispatcher, manageDispatcher)) == 0) {
        return 0;
    }
    dispatcher->name = sclone(name);
    dispatcher->cond = mprCreateCond();
    dispatcher->enabled = enable;
    dispatcher->magic = MPR_DISPATCHER_MAGIC;
    es = dispatcher->service = MPR->eventService;
    dispatcher->eventQ = mprCreateEventQueue();
    if (enable) {
        queueDispatcher(es->idleQ, dispatcher);
    } else {
        initDispatcher(dispatcher);
    }
    return dispatcher;
}


PUBLIC void mprDestroyDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *q, *event, *next;

    if (dispatcher && !dispatcher->destroyed) {
        es = dispatcher->service;
        mprAssert(es == MPR->eventService);
        lock(es);
        mprAssert(dispatcher->service == MPR->eventService);
        mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
        dequeueDispatcher(dispatcher);
        mprAssert(dispatcher->parent == dispatcher);
        q = dispatcher->eventQ;
        dispatcher->enabled = 0;
        dispatcher->destroyed = 1;
        for (event = q->next; event != q; event = next) {
            mprAssert(event->magic == MPR_EVENT_MAGIC);
            next = event->next;
            if (event->dispatcher) {
                mprRemoveEvent(event);
            }
        }
        mprAssert(dispatcher->parent == dispatcher);
        unlock(es);
    }
}


static void manageDispatcher(MprDispatcher *dispatcher, int flags)
{
    MprEventService     *es;
    MprEvent            *q, *event;

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    es = dispatcher->service;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(dispatcher->name);
        mprMark(dispatcher->eventQ);
        mprMark(dispatcher->current);
        mprMark(dispatcher->cond);
        mprMark(dispatcher->parent);
        mprMark(dispatcher->service);
        mprMark(dispatcher->requiredWorker);

        lock(es);
        q = dispatcher->eventQ;
        for (event = q->next; event != q; event = event->next) {
            mprAssert(event->magic == MPR_EVENT_MAGIC);
            mprMark(event);
        }
        unlock(es);
        
    } else if (flags & MPR_MANAGE_FREE) {
        mprDestroyDispatcher(dispatcher);
        mprAssert(dispatcher->destroyed);
    }
}


PUBLIC void mprEnableDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    int                 mustWake;

    if (dispatcher == 0) {
        dispatcher = MPR->dispatcher;
    }
    es = dispatcher->service;
    mustWake = 0;

    lock(es);
    mprAssert(!dispatcher->destroyed);
    if (!dispatcher->enabled) {
        dispatcher->enabled = 1;
        LOG(7, "mprEnableDispatcher: %s", dispatcher->name);
        if (!isEmpty(dispatcher) && !isReady(dispatcher) && !isRunning(dispatcher)) {
            queueDispatcher(es->readyQ, dispatcher);
            if (es->waiting) {
                mustWake = 1;
            }
        }
    }
    unlock(es);
    if (mustWake) {
        mprWakeNotifier();
    }
}


/*
    Schedule events. This can be called by any thread. Typically an app will dedicate one thread to be an event service 
    thread. This call will service events until the timeout expires or if MPR_SERVICE_ONE_THING is specified in flags, 
    after one event. This will service all enabled dispatcher queues and pending I/O events.
    @param dispatcher Primary dispatcher to service. This dispatcher is set to the running state and events on this
        dispatcher will be serviced without starting a worker thread. This can be set to NULL.
    @param timeout Time in milliseconds to wait. Set to zero for no wait. Set to -1 to wait forever.
    @returns Zero if not events occurred. Otherwise returns non-zero.
 */
PUBLIC int mprServiceEvents(MprTime timeout, int flags)
{
    MprEventService     *es;
    MprDispatcher       *dp;
    MprTime             expires, delay;
    int                 beginEventCount, eventCount, justOne;

    if (MPR->eventing) {
        mprError("mprServiceEvents() called reentrantly");
        return 0;
    }
    MPR->eventing = 1;
    mprInitWindow();
    es = MPR->eventService;
    beginEventCount = eventCount = es->eventCount;

    es->now = mprGetTime();
    expires = timeout < 0 ? MAXINT64 : (es->now + timeout);
    if (expires < 0) {
        expires = MAXINT64;
    }
    justOne = (flags & MPR_SERVICE_ONE_THING) ? 1 : 0;

    while (es->now < expires && !mprIsStoppingCore()) {
        eventCount = es->eventCount;
        if (MPR->signalService->hasSignals) {
            mprServiceSignals();
        }
        while ((dp = getNextReadyDispatcher(es)) != NULL) {
            mprAssert(!dp->destroyed);
            mprAssert(dp->magic == MPR_DISPATCHER_MAGIC);
            if (!serviceDispatcher(dp)) {
                queueDispatcher(es->pendingQ, dp);
                continue;
            }
            if (justOne) {
                MPR->eventing = 0;
                return abs(es->eventCount - beginEventCount);
            }
        } 
        if (es->eventCount == eventCount) {
            lock(es);
            delay = getIdleTime(es, expires - es->now);
            if (delay > 0) {
                es->waiting = 1;
                es->willAwake = es->now + delay;
                unlock(es);
                if (mprIsStopping()) {
                    if (mprServicesAreIdle()) {
                        break;
                    }
                    delay = 10;
                }
                mprWaitForIO(MPR->waitService, delay);
            } else {
                unlock(es);
            }
        }
        es->now = mprGetTime();
        if (justOne) {
            break;
        }
    }
    MPR->eventing = 0;
    return abs(es->eventCount - beginEventCount);
}


/*
    Wait for an event to occur. Expect the event to signal the cond var.
    WARNING: this will enable GC while sleeping
    Return Return 0 if an event was signalled. Return MPR_ERR_TIMEOUT if no event was seen before the timeout.
 */
PUBLIC int mprWaitForEvent(MprDispatcher *dispatcher, MprTime timeout)
{
    MprEventService     *es;
    MprTime             expires, delay;
    MprOsThread         thread;
    int                 claimed, signalled, wasRunning, runEvents;

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!dispatcher->destroyed);

    es = MPR->eventService;
    es->now = mprGetTime();

    if (dispatcher == NULL) {
        dispatcher = MPR->dispatcher;
    }
    mprAssert(!dispatcher->waitingOnCond);
    if (dispatcher->waitingOnCond) {
        return MPR_ERR_BUSY;
    }
    thread = mprGetCurrentOsThread();
    expires = timeout < 0 ? (es->now + MPR_MAX_TIMEOUT) : (es->now + timeout);
    claimed = signalled = 0;

    lock(es);
    /*
        Acquire dedicates the dispatcher to this thread. If acquire fails, another thread is servicing this dispatcher.
        makeRunnable() prevents mprServiceEvents from servicing this dispatcher
     */
    wasRunning = isRunning(dispatcher);
    runEvents = (!wasRunning || dispatcher->owner == thread);
    if (runEvents) {
        if (!wasRunning) {
            makeRunnable(dispatcher);
        }
        dispatcher->owner = thread;
    }
    unlock(es);

    while (es->now < expires && !mprIsStoppingCore()) {
        mprAssert(!dispatcher->destroyed);
        if (runEvents) {
            makeRunnable(dispatcher);
            if (dispatchEvents(dispatcher)) {
                signalled++;
                break;
            }
        }
        lock(es);
        delay = getDispatcherIdleTime(dispatcher, expires - es->now);
        dispatcher->waitingOnCond = 1;
        mprAssert(!dispatcher->destroyed);
        unlock(es);
        
        mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
        mprYield(MPR_YIELD_STICKY);
        mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);

        if (mprWaitForCond(dispatcher->cond, delay) == 0) {
            mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
            mprResetYield();
            dispatcher->waitingOnCond = 0;
            if (runEvents) {
                makeRunnable(dispatcher);
                dispatchEvents(dispatcher);
            }
            mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
            signalled++;
            break;
        }
        mprResetYield();
        mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
        dispatcher->waitingOnCond = 0;
        es->now = mprGetTime();
    }
    if (!wasRunning) {
        scheduleDispatcher(dispatcher);
        if (claimed) {
            dispatcher->owner = 0;
        }
    }
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    return signalled ? 0 : MPR_ERR_TIMEOUT;
}


PUBLIC void mprWakeDispatchers()
{
    MprEventService     *es;
    MprDispatcher       *runQ, *dp;

    es = MPR->eventService;
    lock(es);
    runQ = es->runQ;
    for (dp = runQ->next; dp != runQ; dp = dp->next) {
        mprAssert(dp->magic == MPR_DISPATCHER_MAGIC);
        mprAssert(!dp->destroyed);
        mprSignalCond(dp->cond);
    }
    unlock(es);
}


PUBLIC int mprDispatchersAreIdle()
{
    MprEventService     *es;
    MprDispatcher       *runQ, *dispatcher;
    int                 idle;

    es = MPR->eventService;
    runQ = es->runQ;
    lock(es);
    dispatcher = runQ->next;
    if (dispatcher == runQ) {
        idle = 1;
    } else {
        idle = (dispatcher->eventQ == dispatcher->eventQ->next);
    }
    unlock(es);
    return idle;
}


/*
    Relay an event to a dispatcher. This invokes the callback proc as though it was invoked from the given dispatcher. 
 */
PUBLIC void mprRelayEvent(MprDispatcher *dispatcher, void *proc, void *data, MprEvent *event)
{
#if BIT_DEBUG
    MprThread   *tp = mprGetCurrentThread();
    mprNop(tp);
#endif
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!dispatcher->destroyed);

    if (isRunning(dispatcher) && dispatcher->owner != mprGetCurrentOsThread()) {
        mprError("Relay to a running dispatcher owned by another thread");
    }
    if (event) {
        event->timestamp = dispatcher->service->now;
    }
    dispatcher->enabled = 1;
    dispatcher->owner = mprGetCurrentOsThread();
    makeRunnable(dispatcher);
    ((MprEventProc) proc)(data, event);
    scheduleDispatcher(dispatcher);
    dispatcher->owner = 0;
}


/*
    Schedule the dispatcher. If the dispatcher is already running then it is not modified. If the event queue is empty, 
    the dispatcher is moved to the idleQ. If there is a past-due event, it is moved to the readyQ. If there is a future 
    event pending, it is put on the waitQ.
 */
PUBLIC void mprScheduleDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    int                 mustWakeWaitService, mustWakeCond;
   
    mprAssert(dispatcher);
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!dispatcher->destroyed);
    mprAssert(dispatcher->name);
    mprAssert(dispatcher->cond);
    es = dispatcher->service;

    lock(es);
    mprAssert(!dispatcher->destroyed);
    if (isRunning(dispatcher) || !dispatcher->enabled) {
        /* Wake up if waiting in mprWaitForIO */
        mustWakeWaitService = es->waiting;
        mustWakeCond = dispatcher->waitingOnCond;

    } else {
        if (isEmpty(dispatcher)) {
            queueDispatcher(es->idleQ, dispatcher);
            unlock(es);
            return;
        }
        event = dispatcher->eventQ->next;
        mprAssert(event->magic == MPR_EVENT_MAGIC);
        mustWakeWaitService = mustWakeCond = 0;
        if (event->due > es->now) {
            mprAssert(!dispatcher->destroyed);
            queueDispatcher(es->waitQ, dispatcher);
            if (event->due < es->willAwake) {
                mustWakeWaitService = 1;
                mustWakeCond = dispatcher->waitingOnCond;
            }
        } else {
            queueDispatcher(es->readyQ, dispatcher);
            mustWakeWaitService = es->waiting;
            mustWakeCond = dispatcher->waitingOnCond;
        }
    }
    unlock(es);
    if (mustWakeCond) {
        mprSignalDispatcher(dispatcher);
    }
    if (mustWakeWaitService) {
        mprWakeNotifier();
    }
}


/*
    Dispatch events for a dispatcher
 */
static int dispatchEvents(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    int                 count;

    mprAssert(dispatcher->enabled);
    mprAssert(dispatcher->cond);
    mprAssert(!dispatcher->destroyed);

    es = dispatcher->service;
    LOG(7, "dispatchEvents for %s", dispatcher->name);

    lock(es);
    for (count = 0; (event = mprGetNextEvent(dispatcher)) != 0; count++) {
        mprAssert(event->magic == MPR_EVENT_MAGIC);
        dispatcher->current = event;
        if (event->continuous) {
            /* Reschedule if continuous */
            event->timestamp = dispatcher->service->now;
            event->due = event->timestamp + (event->period ? event->period : 1);
            mprQueueEvent(dispatcher, event);
        }
        mprAssert(event->proc);
        unlock(es);
        LOG(7, "Call event %s", event->name);
        (event->proc)(event->data, event);
        dispatcher->current = 0;
        lock(es);
    }
    unlock(es);
    if (count && es->waiting) {
        es->eventCount += count;
        mprWakeNotifier();
    }
    return count;
}


static bool serviceDispatcher(MprDispatcher *dispatcher)
{
    mprAssert(isRunning(dispatcher));
    mprAssert(dispatcher->owner == 0);
    mprAssert(dispatcher->cond);
    mprAssert(!dispatcher->destroyed);
    
    dispatcher->owner = mprGetCurrentOsThread();

    if (dispatcher == MPR->nonBlock) {
        serviceDispatcherMain(dispatcher);

    } else if (dispatcher->requiredWorker) {
        mprActivateWorker(dispatcher->requiredWorker, (MprWorkerProc) serviceDispatcherMain, dispatcher);

    } else if (mprStartWorker((MprWorkerProc) serviceDispatcherMain, dispatcher) < 0) {
        return 0;
    }
    return 1;
}


static void serviceDispatcherMain(MprDispatcher *dispatcher)
{
    if (dispatcher->destroyed) {
        /* Dispatcher may have been destroyed after starting the worker */
        return;
    }
    mprAssert(isRunning(dispatcher));
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(dispatcher->cond);
    mprAssert(dispatcher->name);
    mprAssert(!dispatcher->destroyed);

    dispatcher->owner = mprGetCurrentOsThread();
    dispatchEvents(dispatcher);
    if (!dispatcher->destroyed) {
        dispatcher->owner = 0;
        scheduleDispatcher(dispatcher);
    }
}


PUBLIC void mprClaimDispatcher(MprDispatcher *dispatcher)
{
    mprAssert(isRunning(dispatcher));
    dispatcher->owner = mprGetCurrentOsThread();
}


PUBLIC void mprWakePendingDispatchers()
{
    mprWakeNotifier();
}


/*
    Get the next (ready) dispatcher off given runQ and move onto the runQ
 */
static MprDispatcher *getNextReadyDispatcher(MprEventService *es)
{
    MprDispatcher   *dp, *next, *pendingQ, *readyQ, *waitQ, *dispatcher;
    MprEvent        *event;

    waitQ = es->waitQ;
    readyQ = es->readyQ;
    pendingQ = es->pendingQ;
    dispatcher = 0;

    lock(es);
    if (pendingQ->next != pendingQ && mprAvailableWorkers()) {
        dispatcher = pendingQ->next;
        mprAssert(!dispatcher->destroyed);
        queueDispatcher(es->runQ, dispatcher);
        mprAssert(dispatcher->enabled);
        dispatcher->owner = 0;

    } else if (readyQ->next == readyQ) {
        /*
            ReadyQ is empty, try to transfer a dispatcher with due events onto the readyQ
         */
        for (dp = waitQ->next; dp != waitQ; dp = next) {
            mprAssert(dp->magic == MPR_DISPATCHER_MAGIC);
            mprAssert(!dp->destroyed);
            next = dp->next;
            event = dp->eventQ->next;
            mprAssert(event->magic == MPR_EVENT_MAGIC);
            if (event->due <= es->now && dp->enabled) {
                queueDispatcher(es->readyQ, dp);
                break;
            }
        }
    }
    if (!dispatcher && readyQ->next != readyQ) {
        dispatcher = readyQ->next;
        mprAssert(!dispatcher->destroyed);
        queueDispatcher(es->runQ, dispatcher);
        mprAssert(dispatcher->enabled);
        dispatcher->owner = 0;
    }
    unlock(es);
    mprAssert(dispatcher == NULL || isRunning(dispatcher));
    mprAssert(dispatcher == NULL || dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(dispatcher == NULL || !dispatcher->destroyed);
    mprAssert(dispatcher == NULL || dispatcher->cond);
    return dispatcher;
}


/*
    Get the time to sleep till the next pending event. Must be called locked.
 */
static MprTime getIdleTime(MprEventService *es, MprTime timeout)
{
    MprDispatcher   *readyQ, *waitQ, *dp;
    MprEvent        *event;
    MprTime         delay;

    waitQ = es->waitQ;
    readyQ = es->readyQ;

    if (readyQ->next != readyQ) {
        delay = 0;
    } else if (mprIsStopping()) {
        delay = 10;
    } else {
        delay = MPR_MAX_TIMEOUT;
        /*
            Examine all the dispatchers on the waitQ
         */
        for (dp = waitQ->next; dp != waitQ; dp = dp->next) {
            mprAssert(dp->magic == MPR_DISPATCHER_MAGIC);
            mprAssert(!dp->destroyed);
            event = dp->eventQ->next;
            mprAssert(event->magic == MPR_EVENT_MAGIC);
            if (event != dp->eventQ) {
                delay = min(delay, (event->due - es->now));
                if (delay <= 0) {
                    break;
                }
            }
        }
        delay = min(delay, timeout);
    }
    return delay;
}


static MprTime getDispatcherIdleTime(MprDispatcher *dispatcher, MprTime timeout)
{
    MprEvent    *next;
    MprTime     delay;

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);

    if (timeout < 0) {
        timeout = 0;
    } else {
        next = dispatcher->eventQ->next;
        delay = MPR_MAX_TIMEOUT;
        if (next != dispatcher->eventQ) {
            delay = (next->due - dispatcher->service->now);
            if (delay < 0) {
                delay = 0;
            }
        }
        timeout = min(delay, timeout);
    }
    return timeout;
}


static void initDispatcher(MprDispatcher *q)
{
    mprAssert(q->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!q->destroyed);
           
    q->next = q;
    q->prev = q;
    q->parent = q;
}


static void queueDispatcher(MprDispatcher *prior, MprDispatcher *dispatcher)
{
    mprAssert(dispatcher->service == MPR->eventService);
    lock(dispatcher->service);

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!dispatcher->destroyed);

    if (dispatcher->parent) {
        dequeueDispatcher(dispatcher);
    }
    dispatcher->parent = prior->parent;
    dispatcher->prev = prior;
    dispatcher->next = prior->next;
    prior->next->prev = dispatcher;
    prior->next = dispatcher;
    mprAssert(dispatcher->cond);
    unlock(dispatcher->service);
}


/*
    Remove an dispatcher
 */
static void dequeueDispatcher(MprDispatcher *dispatcher)
{
    mprAssert(dispatcher->service == MPR->eventService);
    lock(dispatcher->service);

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(!dispatcher->destroyed);
           
    if (dispatcher->next) {
        dispatcher->next->prev = dispatcher->prev;
        dispatcher->prev->next = dispatcher->next;
        dispatcher->next = dispatcher;
        dispatcher->prev = dispatcher;
        dispatcher->parent = dispatcher;
    } else {
        mprAssert(dispatcher->parent == dispatcher);
        mprAssert(dispatcher->next == dispatcher);
        mprAssert(dispatcher->prev == dispatcher);
    }
    mprAssert(dispatcher->cond);
    unlock(dispatcher->service);
}


static void scheduleDispatcher(MprDispatcher *dispatcher)
{
    MprEventService     *es;

    mprAssert(dispatcher->service == MPR->eventService);
    es = dispatcher->service;

    lock(es);
    mprAssert(dispatcher->cond);
    dequeueDispatcher(dispatcher);
    mprScheduleDispatcher(dispatcher);
    unlock(es);
}


static int makeRunnable(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    int                 wasRunning;

    es = dispatcher->service;

    lock(es);
    mprAssert(!dispatcher->destroyed);
    wasRunning = isRunning(dispatcher);
    if (!isRunning(dispatcher)) {
        queueDispatcher(es->runQ, dispatcher);
    }
    unlock(es);
    return wasRunning;
}


#if UNUSED && KEEP
/*
    Designate the required worker thread to run the event
 */
PUBLIC void mprDedicateWorkerToDispatcher(MprDispatcher *dispatcher, MprWorker *worker)
{
    dispatcher->requiredWorker = worker;
    mprDedicateWorker(worker);
}


PUBLIC void mprReleaseWorkerFromDispatcher(MprDispatcher *dispatcher, MprWorker *worker)
{
    dispatcher->requiredWorker = 0;
    mprReleaseWorker(worker);
}
#endif


PUBLIC void mprSignalDispatcher(MprDispatcher *dispatcher)
{
    if (dispatcher == NULL) {
        dispatcher = MPR->dispatcher;
    }
    mprSignalCond(dispatcher->cond);
}


PUBLIC bool mprDispatcherHasEvents(MprDispatcher *dispatcher)
{
    if (dispatcher == 0) {
        return 0;
    }
    return !isEmpty(dispatcher);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprEncode.c"
 */
/************************************************************************/

/*
    mprEncode.c - URI encode and decode routines
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/************************************ Locals **********************************/
/*
    Character escape/descape matching codes. Generated by charGen.
 */
static uchar charMatch[256] = {
    0x00,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3e,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x0c,0x3f,0x28,0x2a,0x3c,0x2b,0x0f,0x0e,0x0e,0x0e,0x28,0x28,0x00,0x00,0x28,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x28,0x2a,0x3f,0x28,0x3f,0x2a,
    0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3a,0x3e,0x3a,0x3e,0x00,
    0x3e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x3e,0x3e,0x3e,0x02,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,
    0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c,0x3c 
};

/*  
    Max size of the port specification in a URL
 */
#define MAX_PORT_LEN 8

#define MIME_HASH_SIZE 67

/************************************ Code ************************************/
/*  
    Uri encode by encoding special characters with hex equivalents. Return an allocated string.
 */
PUBLIC char *mprUriEncode(cchar *inbuf, int map)
{
    static cchar    hexTable[] = "0123456789ABCDEF";
    uchar           c;
    cchar           *ip;
    char            *result, *op;
    int             len;

    mprAssert(inbuf);
    mprAssert(inbuf);

    if (!inbuf) {
        return MPR->emptyString;
    }
    for (len = 1, ip = inbuf; *ip; ip++, len++) {
        if (charMatch[(int) (uchar) *ip] & map) {
            len += 2;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }
    op = result;

    while ((c = (uchar) (*inbuf++)) != 0) {
        if (c == ' ' && (map & MPR_ENCODE_URI_COMPONENT)) {
            *op++ = '+';
        } else if (charMatch[c] & map) {
            *op++ = '%';
            *op++ = hexTable[c >> 4];
            *op++ = hexTable[c & 0xf];
        } else {
            *op++ = c;
        }
    }
    mprAssert(op < &result[len]);
    *op = '\0';
    return result;
}


/*  
    Decode a string using URL encoding. Return an allocated string.
 */
PUBLIC char *mprUriDecode(cchar *inbuf)
{
    cchar   *ip;
    char    *result, *op;
    int     num, i, c;

    mprAssert(inbuf);

    if ((result = sclone(inbuf)) == 0) {
        return 0;
    }
    for (op = result, ip = inbuf; ip && *ip; ip++, op++) {
        if (*ip == '+') {
            *op = ' ';

        } else if (*ip == '%' && isxdigit((uchar) ip[1]) && isxdigit((uchar) ip[2])) {
            ip++;
            num = 0;
            for (i = 0; i < 2; i++, ip++) {
                c = tolower((uchar) *ip);
                if (c >= 'a' && c <= 'f') {
                    num = (num * 16) + 10 + c - 'a';
                } else if (c >= '0' && c <= '9') {
                    num = (num * 16) + c - '0';
                } else {
                    /* Bad chars in URL */
                    return 0;
                }
            }
            *op = (char) num;
            ip--;

        } else {
            *op = *ip;
        }
    }
    *op = '\0';
    return result;
}


/*  
    Escape a shell command. Not really Http, but useful anyway for CGI
 */
PUBLIC char *mprEscapeCmd(cchar *cmd, int escChar)
{
    uchar   c;
    cchar   *ip;
    char    *result, *op;
    int     len;

    mprAssert(cmd);

    if (!cmd) {
        return MPR->emptyString;
    }
    for (len = 1, ip = cmd; *ip; ip++, len++) {
        if (charMatch[(int) (uchar) *ip] & MPR_ENCODE_SHELL) {
            len++;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }

    if (escChar == 0) {
        escChar = '\\';
    }
    op = result;
    while ((c = (uchar) *cmd++) != 0) {
#if BIT_WIN_LIKE
        //  MOB - should use fs->newline
        if ((c == '\r' || c == '\n') && *cmd != '\0') {
            c = ' ';
            continue;
        }
#endif
        if (charMatch[c] & MPR_ENCODE_SHELL) {
            *op++ = escChar;
        }
        *op++ = c;
    }
    mprAssert(op < &result[len]);
    *op = '\0';
    return result;
}


/*  
    Escape HTML to escape defined characters (prevent cross-site scripting)
 */
PUBLIC char *mprEscapeHtml(cchar *html)
{
    cchar   *ip;
    char    *result, *op;
    int     len;

    if (!html) {
        return MPR->emptyString;
    }
    for (len = 1, ip = html; *ip; ip++, len++) {
        if (charMatch[(int) (uchar) *ip] & MPR_ENCODE_HTML) {
            len += 5;
        }
    }
    if ((result = mprAlloc(len)) == 0) {
        return 0;
    }

    /*  
        Leave room for the biggest expansion
     */
    op = result;
    while (*html != '\0') {
        if (charMatch[(uchar) *html] & MPR_ENCODE_HTML) {
            if (*html == '&') {
                strcpy(op, "&amp;");
                op += 5;
            } else if (*html == '<') {
                strcpy(op, "&lt;");
                op += 4;
            } else if (*html == '>') {
                strcpy(op, "&gt;");
                op += 4;
            } else if (*html == '#') {
                strcpy(op, "&#35;");
                op += 5;
            } else if (*html == '(') {
                strcpy(op, "&#40;");
                op += 5;
            } else if (*html == ')') {
                strcpy(op, "&#41;");
                op += 5;
            } else if (*html == '"') {
                strcpy(op, "&quot;");
                op += 6;
            } else if (*html == '\'') {
                strcpy(op, "&#39;");
                op += 5;
            } else {
                mprAssert(0);
            }
            html++;
        } else {
            *op++ = *html++;
        }
    }
    mprAssert(op < &result[len]);
    *op = '\0';
    return result;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprEpoll.c"
 */
/************************************************************************/

/**
    mprEpoll.c - Wait for I/O by using epoll on unix like systems.

    This module augments the mprWait wait services module by providing kqueue() based waiting support.
    Also see mprAsyncSelectWait and mprSelectWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if MPR_EVENT_EPOLL
/********************************** Forwards **********************************/

static int growEvents(MprWaitService *ws);
static void serviceIO(MprWaitService *ws, int count);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    struct epoll_event  ev;

    ws->eventsMax = MPR_EPOLL_SIZE;
    ws->handlerMax = MPR_FD_MIN;
    ws->events = mprAllocZeroed(sizeof(struct epoll_event) * ws->eventsMax);
    ws->handlerMap = mprAllocZeroed(sizeof(MprWaitHandler*) * ws->handlerMax);
    if (ws->events == 0 || ws->handlerMap == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    if ((ws->epoll = epoll_create(MPR_EPOLL_SIZE)) < 0) {
        mprError("Call to epoll() failed");
        return MPR_ERR_CANT_INITIALIZE;
    }
    /*
        Initialize the "wakeup" pipe. This is used to wakeup the service thread if other threads need 
     *  to wait for I/O.
     */
    if (pipe(ws->breakPipe) < 0) {
        mprError("Can't open breakout pipe");
        return MPR_ERR_CANT_INITIALIZE;
    }
    fcntl(ws->breakPipe[0], F_SETFL, fcntl(ws->breakPipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(ws->breakPipe[1], F_SETFL, fcntl(ws->breakPipe[1], F_GETFL) | O_NONBLOCK);

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.fd = ws->breakPipe[MPR_READ_PIPE];
    epoll_ctl(ws->epoll, EPOLL_CTL_ADD, ws->breakPipe[MPR_READ_PIPE], &ev);
    return 0;
}


PUBLIC void mprManageEpoll(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->events);
    
    } else if (flags & MPR_MANAGE_FREE) {
        if (ws->epoll) {
            close(ws->epoll);
            ws->epoll = 0;
        }
        if (ws->breakPipe[0] >= 0) {
            close(ws->breakPipe[0]);
        }
        if (ws->breakPipe[1] >= 0) {
            close(ws->breakPipe[1]);
        }
    }
}


static int growEvents(MprWaitService *ws)
{
    ws->eventsMax *= 2;
    if ((ws->events = mprRealloc(ws->events, sizeof(struct epoll_event) * ws->eventsMax)) == 0) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    return 0;
}


PUBLIC int mprNotifyOn(MprWaitService *ws, MprWaitHandler *wp, int mask)
{
    struct epoll_event  ev;
    int                 fd, rc;

    mprAssert(wp);
    fd = wp->fd;

    lock(ws);
    if (wp->desiredMask != mask) {
        memset(&ev, 0, sizeof(ev));
        ev.data.fd = fd;
        if (wp->desiredMask & MPR_READABLE) {
            ev.events |= (EPOLLIN | EPOLLHUP);
        }
        if (wp->desiredMask & MPR_WRITABLE) {
            ev.events |= EPOLLOUT;
        }
        if (ev.events) {
            rc = epoll_ctl(ws->epoll, EPOLL_CTL_DEL, fd, &ev);
#if UNUSED && KEEP
            if (rc != 0) {
                mprError("Epoll del error %d on fd %d\n", errno, fd);
            }
#endif
        }
        ev.events = 0;
        if (mask & MPR_READABLE) {
            ev.events |= (EPOLLIN | EPOLLHUP);
        }
        if (mask & MPR_WRITABLE) {
            ev.events |= EPOLLOUT;
        }
        if (ev.events) {
            rc = epoll_ctl(ws->epoll, EPOLL_CTL_ADD, fd, &ev);
            if (rc != 0) {
                mprError("Epoll add error %d on fd %d\n", errno, fd);
            }
        }
        if (mask && fd >= ws->handlerMax) {
            ws->handlerMax = fd + 32;
            if ((ws->handlerMap = mprRealloc(ws->handlerMap, sizeof(MprWaitHandler*) * ws->handlerMax)) == 0) {
                mprAssert(!MPR_ERR_MEMORY);
                return MPR_ERR_MEMORY;
            }
        }
        mprAssert(ws->handlerMap[fd] == 0 || ws->handlerMap[fd] == wp);
        wp->desiredMask = mask;
    }
    ws->handlerMap[fd] = (mask) ? wp : 0;
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single file descriptor. Return a mask of events found. Mask is the events of interest.
    timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTime timeout)
{
    struct epoll_event  ev, events[2];
    int                 epfd, rc;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
    memset(&ev, 0, sizeof(ev));
    memset(events, 0, sizeof(events));
    ev.data.fd = fd;
    if ((epfd = epoll_create(MPR_EPOLL_SIZE)) < 0) {
        mprError("Call to epoll() failed");
        return MPR_ERR_CANT_INITIALIZE;
    }
    if (mask & MPR_READABLE) {
        ev.events = (EPOLLIN | EPOLLHUP);
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    if (mask & MPR_WRITABLE) {
        ev.events = EPOLLOUT;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
    }
    mask = 0;
    rc = epoll_wait(epfd, events, sizeof(events) / sizeof(struct epoll_event), timeout);
    close(epfd);
    if (rc < 0) {
        mprLog(2, "Epoll returned %d, errno %d", rc, errno);
    } else if (rc > 0) {
        if (rc > 0) {
            if (events[0].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
                mask |= MPR_READABLE;
            }
            if (events[0].events & (EPOLLOUT)) {
                mask |= MPR_WRITABLE;
            }
        }
    }
    return mask;
}


/*
    Wait for I/O on all registered file descriptors. Timeout is in milliseconds. Return the number of events detected. 
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTime timeout)
{
    int     rc;

    if (timeout < 0 || timeout > MAXINT) {
        timeout = MAXINT;
    }
#if BIT_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    mprYield(MPR_YIELD_STICKY);
    rc = epoll_wait(ws->epoll, ws->events, ws->eventsMax, timeout);
    mprResetYield();

    if (rc < 0) {
        if (errno != EINTR) {
            mprLog(7, "epoll returned %d, errno %d", mprGetOsError());
        }
    } else if (rc > 0) {
        serviceIO(ws, rc);
        if (rc == ws->eventsMax) {
            growEvents(ws);
        }
    }
    ws->wakeRequested = 0;
}


static void serviceIO(MprWaitService *ws, int count)
{
    MprWaitHandler      *wp;
    struct epoll_event  *ev;
    int                 fd, i, mask;

    lock(ws);
    for (i = 0; i < count; i++) {
        ev = &ws->events[i];
        fd = ev->data.fd;
        mprAssert(fd < ws->handlerMax);
        if ((wp = ws->handlerMap[fd]) == 0) {
            char    buf[128];
            if ((ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP)) && (fd == ws->breakPipe[MPR_READ_PIPE])) {
                if (read(fd, buf, sizeof(buf)) < 0) {}
            }
            continue;
        }
        mask = 0;
        if (ev->events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
            mask |= MPR_READABLE;
        }
        if (ev->events & EPOLLOUT) {
            mask |= MPR_WRITABLE;
        }
        if (mask == 0) {
            mprAssert(mask);
            continue;
        }
        wp->presentMask = mask & wp->desiredMask;
        if (wp->presentMask) {
            struct epoll_event  ev;
            memset(&ev, 0, sizeof(ev));
            ev.data.fd = fd;
            wp->desiredMask = 0;
            ws->handlerMap[wp->fd] = 0;
            epoll_ctl(ws->epoll, EPOLL_CTL_DEL, wp->fd, &ev);
            mprQueueIOEvent(wp);
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
    Must be async-safe.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;
    int             c;

    ws = MPR->waitService;
    if (!ws->wakeRequested) {
        ws->wakeRequested = 1;
        c = 0;
        if (write(ws->breakPipe[MPR_WRITE_PIPE], (char*) &c, 1) < 0) {};
    }
}

#else
PUBLIC void stubMmprEpoll() {}
#endif /* MPR_EVENT_EPOLL */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprEvent.c"
 */
/************************************************************************/

/*
    mprEvent.c - Event and dispatch services

    This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/***************************** Forward Declarations ***************************/

static void dequeueEvent(MprEvent *event);
static void initEvent(MprDispatcher *dispatcher, MprEvent *event, cchar *name, MprTime period, void *proc, 
        void *data, int flgs);
static void initEventQ(MprEvent *q);
static void manageEvent(MprEvent *event, int flags);
static void queueEvent(MprEvent *prior, MprEvent *event);

/************************************* Code ***********************************/
/*
    Create and queue a new event for service. Period is used as the delay before running the event and as the period 
    between events for continuous events.
 */
PUBLIC MprEvent *mprCreateEventQueue()
{
    MprEvent    *queue;

    if ((queue = mprAllocObj(MprEvent, manageEvent)) == 0) {
        return 0;
    }
    initEventQ(queue);
    return queue;
}


/*
    Create and queue a new event for service. Period is used as the delay before running the event and as the period 
    between events for continuous events.
 */
PUBLIC MprEvent *mprCreateEvent(MprDispatcher *dispatcher, cchar *name, MprTime period, void *proc, void *data, int flags)
{
    MprEvent    *event;

    if ((event = mprAllocObj(MprEvent, manageEvent)) == 0) {
        return 0;
    }
    if (dispatcher == 0) {
        dispatcher = (flags & MPR_EVENT_QUICK) ? MPR->nonBlock : MPR->dispatcher;
    }
    initEvent(dispatcher, event, name, period, proc, data, flags);
    if (!(flags & MPR_EVENT_DONT_QUEUE)) {
        mprQueueEvent(dispatcher, event);
    }
    return event;
}


static void manageEvent(MprEvent *event, int flags)
{
    mprAssert(event->magic == MPR_EVENT_MAGIC);

    if (flags & MPR_MANAGE_MARK) {
        /*
            Events in dispatcher queues are marked by the dispatcher managers, not via event->next,prev
         */
        mprAssert(event->dispatcher == 0 || event->dispatcher->magic == MPR_DISPATCHER_MAGIC);
        mprMark(event->name);
        mprMark(event->dispatcher);
        mprMark(event->handler);
        if (!(event->flags & MPR_EVENT_STATIC_DATA)) {
            mprMark(event->data);
        }

    } else if (flags & MPR_MANAGE_FREE) {
        if (event->next) {
            mprAssert(event->dispatcher == 0 || event->dispatcher->magic == MPR_DISPATCHER_MAGIC);
            mprRemoveEvent(event);
            event->magic = 1;
        }
    }
}


static void initEvent(MprDispatcher *dispatcher, MprEvent *event, cchar *name, MprTime period, void *proc, void *data, 
    int flags)
{
    mprAssert(dispatcher);
    mprAssert(event);
    mprAssert(proc);
    mprAssert(event->next == 0);
    mprAssert(event->prev == 0);

    dispatcher->service->now = mprGetTime();
    event->name = sclone(name);
    event->timestamp = dispatcher->service->now;
    event->proc = proc;
    event->period = period;
    event->due = event->timestamp + period;
    event->data = data;
    event->dispatcher = dispatcher;
    event->next = event->prev = 0;
    event->flags = flags;
    event->continuous = (flags & MPR_EVENT_CONTINUOUS) ? 1 : 0;
    event->magic = MPR_EVENT_MAGIC;
}


/*
    Create an interval timer
 */
PUBLIC MprEvent *mprCreateTimerEvent(MprDispatcher *dispatcher, cchar *name, MprTime period, void *proc, 
    void *data, int flags)
{
    return mprCreateEvent(dispatcher, name, period, proc, data, MPR_EVENT_CONTINUOUS | flags);
}


PUBLIC void mprQueueEvent(MprDispatcher *dispatcher, MprEvent *event)
{
    MprEventService     *es;
    MprEvent            *prior, *q;

    mprAssert(dispatcher);
    mprAssert(event);
    mprAssert(event->timestamp);
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);
    mprAssert(event->magic == MPR_EVENT_MAGIC);

    es = dispatcher->service;

    lock(es);
    q = dispatcher->eventQ;
    for (prior = q->prev; prior != q; prior = prior->prev) {
        if (event->due > prior->due) {
            break;
        } else if (event->due == prior->due) {
            break;
        }
    }
    mprAssert(event->next == 0);
    mprAssert(event->prev == 0);
    mprAssert(prior->next);
    mprAssert(prior->prev);
    
    queueEvent(prior, event);
    es->eventCount++;
    if (dispatcher->enabled) {
        mprScheduleDispatcher(dispatcher);
    }
    unlock(es);
}


PUBLIC void mprRemoveEvent(MprEvent *event)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;

    dispatcher = event->dispatcher;
    if (dispatcher) {
        es = dispatcher->service;
        lock(es);
        if (event->next) {
            dequeueEvent(event);
        }
        if (dispatcher->enabled && event->due == es->willAwake && dispatcher->eventQ->next != dispatcher->eventQ) {
            mprScheduleDispatcher(dispatcher);
        }
        event->dispatcher = 0;
        unlock(es);
    }
}


PUBLIC void mprRescheduleEvent(MprEvent *event, MprTime period)
{
    MprEventService     *es;
    MprDispatcher       *dispatcher;

    mprAssert(event->magic == MPR_EVENT_MAGIC);
    dispatcher = event->dispatcher;
    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);

    es = dispatcher->service;

    lock(es);
    event->period = period;
    event->timestamp = es->now;
    event->due = event->timestamp + period;
    if (event->next) {
        mprRemoveEvent(event);
    }
    unlock(es);
    mprQueueEvent(dispatcher, event);
}


PUBLIC void mprStopContinuousEvent(MprEvent *event)
{
    event->continuous = 0;
}


PUBLIC void mprRestartContinuousEvent(MprEvent *event)
{
    event->continuous = 1;
    mprRescheduleEvent(event, event->period);
}


PUBLIC void mprEnableContinuousEvent(MprEvent *event, int enable)
{
    event->continuous = enable;
}


/*
    Get the next due event from the front of the event queue.
 */
PUBLIC MprEvent *mprGetNextEvent(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event, *next;

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);

    es = dispatcher->service;
    event = 0;

    lock(es);
    next = dispatcher->eventQ->next;
    if (next != dispatcher->eventQ) {
        if (next->due <= es->now) {
            event = next;
            dequeueEvent(event);
            mprAssert(event->magic == MPR_EVENT_MAGIC);
        }
    }
    unlock(es);
    return event;
}


PUBLIC int mprGetEventCount(MprDispatcher *dispatcher)
{
    MprEventService     *es;
    MprEvent            *event;
    int                 count;

    mprAssert(dispatcher->magic == MPR_DISPATCHER_MAGIC);

    es = dispatcher->service;

    lock(es);
	count = 0;
    for (event = dispatcher->eventQ->next; event != dispatcher->eventQ; event = event->next) {
        mprAssert(event->magic == MPR_EVENT_MAGIC);
        count++;
    }
    unlock(es);
    return count;
}


static void initEventQ(MprEvent *q)
{
    mprAssert(q);

    q->next = q;
    q->prev = q;
    q->magic = MPR_EVENT_MAGIC;
}


/*
    Append a new event. Must be locked when called.
 */
static void queueEvent(MprEvent *prior, MprEvent *event)
{
    mprAssert(prior);
    mprAssert(event);
    mprAssert(prior->next);
    mprAssert(event->magic == MPR_EVENT_MAGIC);
    mprAssert(event->dispatcher == 0 || event->dispatcher->magic == MPR_DISPATCHER_MAGIC);

    if (event->next) {
        dequeueEvent(event);
    }
    event->prev = prior;
    event->next = prior->next;
    prior->next->prev = event;
    prior->next = event;
}


/*
    Remove an event. Must be locked when called.
 */
static void dequeueEvent(MprEvent *event)
{
    mprAssert(event);
    mprAssert(event->next);
    mprAssert(event->magic == MPR_EVENT_MAGIC);
    mprAssert(event->dispatcher == 0 || event->dispatcher->magic == MPR_DISPATCHER_MAGIC);

    if (event->next) {
        event->next->prev = event->prev;
        event->prev->next = event->next;
        event->next = 0;
        event->prev = 0;
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprFile.c"
 */
/************************************************************************/

/**
    mprFile.c - File services.

    This modules provides a simple cross platform file I/O abstraction. It uses the MprFileSystem to provide I/O services.
    This module is not thread safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/****************************** Forward Declarations **************************/

static ssize fillBuf(MprFile *file);
static void manageFile(MprFile *file, int flags);

/************************************ Code ************************************/

PUBLIC MprFile *mprAttachFileFd(int fd, cchar *name, int omode)
{
    MprFileSystem   *fs;
    MprFile         *file;

    fs = mprLookupFileSystem("/");

    if ((file = mprAllocObj(MprFile, manageFile)) != 0) {
        file->fd = fd;
        file->fileSystem = fs;
        file->path = sclone(name);
        file->mode = omode;
        file->attached = 1;
    }
    return file;
}


static void manageFile(MprFile *file, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(file->buf);
        mprMark(file->path);

    } else if (flags & MPR_MANAGE_FREE) {
        if (!file->attached) {
            mprCloseFile(file);
        }
    }
}


PUBLIC int mprFlushFile(MprFile *file)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           len, rc;

    mprAssert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
    if (file->buf == 0) {
        return 0;
    }
    if (file->mode & (O_WRONLY | O_RDWR)) {
        fs = file->fileSystem;
        bp = file->buf;
        while (mprGetBufLength(bp) > 0) {
            len = mprGetBufLength(bp);
            rc = fs->writeFile(file, mprGetBufStart(bp), len);
            if (rc < 0) {
                return (int) rc;
            }
            mprAdjustBufStart(bp, rc);
        }
        mprFlushBuf(bp);
    }
    return 0;
}


PUBLIC MprOff mprGetFilePosition(MprFile *file)
{
    return file->pos;
}


PUBLIC MprOff mprGetFileSize(MprFile *file)
{
    return file->size;
}


PUBLIC MprFile *mprGetStderr()
{
    return MPR->stdError;
}


PUBLIC MprFile *mprGetStdin()
{
    return MPR->stdInput;
}


PUBLIC MprFile *mprGetStdout()
{
    return MPR->stdOutput;
}


/*
    Get a character from the file. This will put the file into buffered mode.
 */
PUBLIC int mprGetFileChar(MprFile *file)
{
    MprBuf      *bp;
    ssize     len;

    mprAssert(file);

    if (file == 0) {
        return MPR_ERR;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(MPR_BUFSIZE, MPR_BUFSIZE);
    }
    bp = file->buf;

    if (mprGetBufLength(bp) == 0) {
        len = fillBuf(file);
        if (len <= 0) {
            return -1;
        }
    }
    if (mprGetBufLength(bp) == 0) {
        return 0;
    }
    file->pos++;
    return mprGetCharFromBuf(bp);
}


static char *findNewline(cchar *str, cchar *newline, ssize len, ssize *nlen)
{
    char    *start, *best;
    ssize   newlines;
    int     i;

    mprAssert(str);
    mprAssert(newline);
    mprAssert(nlen);
    mprAssert(len > 0);

    if (str == NULL || newline == NULL) {
        return NULL;
    }
    newlines = slen(newline);
    mprAssert(newlines == 1 || newlines == 2);

    start = best = NULL;
    *nlen = 0;
    for (i = 0; i < newlines; i++) {
        if ((start = memchr(str, newline[i], len)) != 0) {
            if (best == NULL || start < best) {
                best = start;
                *nlen = 1;
                if (newlines == 2 && best[1] == newline[!i]) {
                    (*nlen)++;
                }
            }
        }
    }
    return best;
}


/*
    Get a string from the file. This will put the file into buffered mode.
    Return NULL on eof.
 */
PUBLIC char *mprReadLine(MprFile *file, ssize maxline, ssize *lenp)
{
    MprBuf          *bp;
    MprFileSystem   *fs;
    ssize           size, len, nlen, consumed;
    cchar           *eol, *newline, *start;
    char            *result;

    mprAssert(file);

    if (file == 0) {
        return NULL;
    }
    if (lenp) {
        *lenp = 0;
    }
    if (maxline <= 0) {
        maxline = MPR_BUFSIZE;
    }
    fs = file->fileSystem;
    newline = fs->newline;
    if (file->buf == 0) {
        file->buf = mprCreateBuf(maxline, maxline);
    }
    bp = file->buf;

    result = NULL;
    size = 0;
    do {
        if (mprGetBufLength(bp) == 0) {
            if (fillBuf(file) <= 0) {
                return result;
            }
        }
        start = mprGetBufStart(bp);
        len = mprGetBufLength(bp);
        if ((eol = findNewline(start, newline, len, &nlen)) != 0) {
            len = eol - start;
            consumed = len + nlen;
        } else {
            consumed = len;
        }
        file->pos += (MprOff) consumed;
        if (lenp) {
            *lenp += len;
        }
        if ((result = mprRealloc(result, size + len + 1)) == 0) {
            return NULL;
        }
        memcpy(&result[size], start, len);
        size += len;
        result[size] = '\0';
        mprAdjustBufStart(bp, consumed);
    } while (!eol);

    return result;
}


PUBLIC MprFile *mprOpenFile(cchar *path, int omode, int perms)
{
    MprFileSystem   *fs;
    MprFile         *file;
    MprPath         info;

    fs = mprLookupFileSystem(path);

    file = fs->openFile(fs, path, omode, perms);
    if (file) {
        file->fileSystem = fs;
        file->path = sclone(path);
        if (omode & (O_WRONLY | O_RDWR)) {
            /*
                OPT. Should compute this lazily.
             */
            fs->getPathInfo(fs, path, &info);
            file->size = (MprOff) info.size;
        }
        file->mode = omode;
        file->perms = perms;
    }
    return file;
}


PUBLIC int mprCloseFile(MprFile *file)
{
    MprFileSystem   *fs;

    if (file == 0) {
        return MPR_ERR_CANT_ACCESS;
    }
    fs = mprLookupFileSystem(file->path);
    return fs->closeFile(file);
}


/*
    Put a string to the file. This will put the file into buffered mode.
 */
PUBLIC ssize mprPutFileString(MprFile *file, cchar *str)
{
    MprBuf  *bp;
    ssize   total, bytes, count;
    char    *buf;

    mprAssert(file);
    count = slen(str);

    /*
        Buffer output and flush when full.
     */
    if (file->buf == 0) {
        file->buf = mprCreateBuf(MPR_BUFSIZE, 0);
        if (file->buf == 0) {
            return MPR_ERR_CANT_ALLOCATE;
        }
    }
    bp = file->buf;

    if (mprGetBufLength(bp) > 0 && mprGetBufSpace(bp) < count) {
        mprFlushFile(file);
    }
    total = 0;
    buf = (char*) str;

    while (count > 0) {
        bytes = mprPutBlockToBuf(bp, buf, count);
        if (bytes < 0) {
            return MPR_ERR_CANT_ALLOCATE;

        } else if (bytes == 0) {
            if (mprFlushFile(file) < 0) {
                return MPR_ERR_CANT_WRITE;
            }
            continue;
        }
        count -= bytes;
        buf += bytes;
        total += bytes;
        file->pos += (MprOff) bytes;
    }
    return total;
}


/*
    Peek at a character from the file without disturbing the read position. This will put the file into buffered mode.
 */
PUBLIC int mprPeekFileChar(MprFile *file)
{
    MprBuf      *bp;
    ssize       len;

    mprAssert(file);

    if (file == 0) {
        return MPR_ERR;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(MPR_BUFSIZE, MPR_BUFSIZE);
    }
    bp = file->buf;

    if (mprGetBufLength(bp) == 0) {
        len = fillBuf(file);
        if (len <= 0) {
            return -1;
        }
    }
    if (mprGetBufLength(bp) == 0) {
        return 0;
    }
    return ((uchar*) mprGetBufStart(bp))[0];
}


/*
    Put a character to the file. This will put the file into buffered mode.
 */
PUBLIC ssize mprPutFileChar(MprFile *file, int c)
{
    mprAssert(file);

    if (file == 0) {
        return -1;
    }
    if (file->buf) {
        if (mprPutCharToBuf(file->buf, c) != 1) {
            return MPR_ERR_CANT_WRITE;
        }
        file->pos++;
        return 1;

    }
    return mprWriteFile(file, &c, 1);
}


PUBLIC ssize mprReadFile(MprFile *file, void *buf, ssize size)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           bytes, totalRead;
    void            *bufStart;

    mprAssert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }
    fs = file->fileSystem;
    bp = file->buf;
    if (bp == 0) {
        totalRead = fs->readFile(file, buf, size);

    } else {
        bufStart = buf;
        while (size > 0) {
            if (mprGetBufLength(bp) == 0) {
                bytes = fillBuf(file);
                if (bytes <= 0) {
                    return -1;
                }
            }
            bytes = min(size, mprGetBufLength(bp));
            memcpy(buf, mprGetBufStart(bp), bytes);
            mprAdjustBufStart(bp, bytes);
            buf = (void*) (((char*) buf) + bytes);
            size -= bytes;
        }
        totalRead = ((char*) buf - (char*) bufStart);
    }
    file->pos += (MprOff) totalRead;
    return totalRead;
}


PUBLIC MprOff mprSeekFile(MprFile *file, int seekType, MprOff pos)
{
    MprFileSystem   *fs;

    mprAssert(file);
    fs = file->fileSystem;

    if (file->buf) {
        if (! (seekType == SEEK_CUR && pos == 0)) {
            /*
                Discard buffering as we may be seeking outside the buffer.
                OPT. Could be smarter about this and preserve the buffer.
             */
            if (file->mode & (O_WRONLY | O_RDWR)) {
                if (mprFlushFile(file) < 0) {
                    return MPR_ERR_CANT_WRITE;
                }
            }
            if (file->buf) {
                mprFlushBuf(file->buf);
            }
        }
    }
    if (seekType == SEEK_SET) {
        file->pos = pos;
    } else if (seekType == SEEK_CUR) {
        file->pos += pos;
    } else {
        file->pos = fs->seekFile(file, SEEK_END, 0);
    }
    if (fs->seekFile(file, SEEK_SET, (long) file->pos) != (long) file->pos) {
        return MPR_ERR;
    }
    if (file->mode & (O_WRONLY | O_RDWR)) {
        if (file->pos > file->size) {
            file->size = file->pos;
        }
    }
    return file->pos;
}


PUBLIC int mprTruncateFile(cchar *path, MprOff size)
{
    MprFileSystem   *fs;

    mprAssert(path && *path);

    if ((fs = mprLookupFileSystem(path)) == 0) {
        return MPR_ERR_CANT_OPEN;
    }
    return fs->truncateFile(fs, path, size);
}


PUBLIC ssize mprWriteFile(MprFile *file, cvoid *buf, ssize count)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           bytes, written;

    mprAssert(file);
    if (file == 0) {
        return MPR_ERR_BAD_HANDLE;
    }

    fs = file->fileSystem;
    bp = file->buf;
    if (bp == 0) {
        if ((written = fs->writeFile(file, buf, count)) < 0) {
            return written;
        }
    } else {
        written = 0;
        while (count > 0) {
            bytes = mprPutBlockToBuf(bp, buf, count);
            if (bytes < 0) {
                return bytes;
            } 
            if (bytes != count) {
                mprFlushFile(file);
            }
            count -= bytes;
            written += bytes;
            buf = (char*) buf + bytes;
        }
    }
    file->pos += (MprOff) written;
    if (file->pos > file->size) {
        file->size = file->pos;
    }
    return written;
}


PUBLIC ssize mprWriteFileString(MprFile *file, cchar *str)
{
    return mprWriteFile(file, str, slen(str));
}


PUBLIC ssize mprWriteFileFmt(MprFile *file, cchar *fmt, ...)
{
    va_list     ap;
    char        *buf;
    ssize       rc;

    rc = -1;
    va_start(ap, fmt);
    if ((buf = sfmtv(fmt, ap)) != NULL) {
        rc = mprWriteFileString(file, buf);
    }
    va_end(ap);
    return rc;
}


/*
    Fill the read buffer. Return the new buffer length. Only called when the buffer is empty.
 */
static ssize fillBuf(MprFile *file)
{
    MprFileSystem   *fs;
    MprBuf          *bp;
    ssize           len;

    bp = file->buf;
    fs = file->fileSystem;

    mprAssert(mprGetBufLength(bp) == 0);
    mprFlushBuf(bp);

    len = fs->readFile(file, mprGetBufStart(bp), mprGetBufSpace(bp));
    if (len <= 0) {
        return len;
    }
    mprAdjustBufEnd(bp, len);
    mprAddNullToBuf(bp);
    return len;
}


/*
    Enable and control file buffering
 */
PUBLIC int mprEnableFileBuffering(MprFile *file, ssize initialSize, ssize maxSize)
{
    mprAssert(file);

    if (file == 0) {
        return MPR_ERR_BAD_STATE;
    }
    if (initialSize <= 0) {
        initialSize = MPR_BUFSIZE;
    }
    if (maxSize <= 0) {
        maxSize = MPR_BUFSIZE;
    }
    if (maxSize <= initialSize) {
        maxSize = initialSize;
    }
    if (file->buf == 0) {
        file->buf = mprCreateBuf(initialSize, maxSize);
    }
    return 0;
}


PUBLIC void mprDisableFileBuffering(MprFile *file)
{
    mprFlushFile(file);
    file->buf = 0;
}


PUBLIC int mprGetFileFd(MprFile *file)
{
    return file->fd;
}

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprFileSystem.c"
 */
/************************************************************************/

/**
    mprFileSystem.c - File system services.

    This module provides a simple cross platform file system abstraction. File systems provide a file system switch and 
    underneath a file system provider that implements actual I/O.
    This module is not thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/************************************ Code ************************************/

PUBLIC MprFileSystem *mprCreateFileSystem(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    /*
        FUTURE: evolve this to support multiple file systems in a single system
     */
#if BIT_ROM
    fs = (MprFileSystem*) mprCreateRomFileSystem(path);
#else
    fs = (MprFileSystem*) mprCreateDiskFileSystem(path);
#endif

#if BIT_WIN_LIKE
    fs->separators = sclone("\\/");
    fs->newline = sclone("\r\n");
#elif CYGWIN
    fs->separators = sclone("/\\");
    fs->newline = sclone("\n");
#else
    fs->separators = sclone("/");
    fs->newline = sclone("\n");
#endif

#if BIT_WIN_LIKE || MACOSX || CYGWIN
    fs->caseSensitive = 0;
#else
    fs->caseSensitive = 1;
#endif

#if BIT_WIN_LIKE || VXWORKS || CYGWIN
    fs->hasDriveSpecs = 1;
#endif

    if (MPR->fileSystem == NULL) {
        MPR->fileSystem = fs;
    }
    fs->root = mprGetAbsPath(path);
    if ((cp = strpbrk(fs->root, fs->separators)) != 0) {
        *++cp = '\0';
    }
#if BIT_WIN_LIKE || CYGWIN
    fs->cygwin = mprReadRegistry("HKEY_LOCAL_MACHINE\\SOFTWARE\\Cygwin\\setup", "rootdir");
    fs->cygdrive = sclone("/cygdrive");
#endif
    return fs;
}


PUBLIC void mprAddFileSystem(MprFileSystem *fs)
{
    mprAssert(fs);
    
    /* NOTE: this does not currently add a file system. It merely replaces the existing file system. */
    MPR->fileSystem = fs;
}


/*
    Note: path can be null
 */
PUBLIC MprFileSystem *mprLookupFileSystem(cchar *path)
{
    return MPR->fileSystem;
}


PUBLIC cchar *mprGetPathNewline(cchar *path)
{
    MprFileSystem   *fs;

    mprAssert(path);

    fs = mprLookupFileSystem(path);
    return fs->newline;
}


PUBLIC cchar *mprGetPathSeparators(cchar *path)
{
    MprFileSystem   *fs;

    mprAssert(path);

    fs = mprLookupFileSystem(path);
    return fs->separators;
}


PUBLIC void mprSetPathSeparators(cchar *path, cchar *separators)
{
    MprFileSystem   *fs;

    mprAssert(path);
    mprAssert(separators);
    
    fs = mprLookupFileSystem(path);
    fs->separators = sclone(separators);
}


PUBLIC void mprSetPathNewline(cchar *path, cchar *newline)
{
    MprFileSystem   *fs;
    
    mprAssert(path);
    mprAssert(newline);
    
    fs = mprLookupFileSystem(path);
    fs->newline = sclone(newline);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprHash.c"
 */
/************************************************************************/

/*
    mprHash.c - Fast hashing hash lookup module

    This hash hash uses a fast key lookup mechanism. Keys may be C strings or unicode strings. The hash value entries 
    are arbitrary pointers. The keys are hashed into a series of buckets which then have a chain of hash entries.
    The chain in in collating sequence so search time through the chain is on average (N/hashSize)/2.

    This module is not thread-safe. It is the callers responsibility to perform all thread synchronization.
    There is locking solely for the purpose of synchronization with the GC marker()

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/**************************** Forward Declarations ****************************/

static void *dupKey(MprHash *hash, cvoid *key);
static MprKey *lookupHash(int *index, MprKey **prevSp, MprHash *hash, cvoid *key);
static void manageHashTable(MprHash *hash, int flags);

/*********************************** Code *************************************/
/*
    Create a new hash hash of a given size. Caller should provide a size that is a prime number for the greatest efficiency.
 */
PUBLIC MprHash *mprCreateHash(int hashSize, int flags)
{
    MprHash     *hash;

    if ((hash = mprAllocObj(MprHash, manageHashTable)) == 0) {
        return 0;
    }
    if (hashSize < MPR_DEFAULT_HASH_SIZE) {
        hashSize = MPR_DEFAULT_HASH_SIZE;
    }
    if ((hash->buckets = mprAllocZeroed(sizeof(MprKey*) * hashSize)) == 0) {
        return NULL;
    }
    hash->size = hashSize;
    hash->flags = flags | MPR_OBJ_HASH;
    hash->length = 0;
    if (!(flags & MPR_HASH_OWN)) {
        hash->mutex = mprCreateLock();
    }
#if BIT_CHAR_LEN > 1 && UNUSED && KEEP
    if (hash->flags & MPR_HASH_UNICODE) {
        if (hash->flags & MPR_HASH_CASELESS) {
            hash->fn = (MprHashProc) whashlower;
        } else {
            hash->fn = (MprHashProc) whash;
        }
    } else 
#endif
    {
        if (hash->flags & MPR_HASH_CASELESS) {
            hash->fn = (MprHashProc) shashlower;
        } else {
            hash->fn = (MprHashProc) shash;
        }
    }
    return hash;
}


static void manageHashTable(MprHash *hash, int flags)
{
    MprKey      *sp;
    int         i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(hash->mutex);
        mprMark(hash->buckets);
        lock(hash);
        for (i = 0; i < hash->size; i++) {
            for (sp = (MprKey*) hash->buckets[i]; sp; sp = sp->next) {
                mprAssert(mprIsValid(sp));
                mprMark(sp);
                if (!(hash->flags & MPR_HASH_STATIC_VALUES)) {
                    if (sp->data && !mprIsValid(sp->data)) {
                        mprLog(0, "Data in key %s is not valid", sp->key);
                    }
                    mprAssert(sp->data == 0 || mprIsValid(sp->data));
                    mprMark(sp->data);
                }
                if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
                    mprAssert(mprIsValid(sp->key));
                    mprMark(sp->key);
                }
            }
        }
        unlock(hash);
    }
}


/*
    Insert an entry into the hash hash. If the entry already exists, update its value. 
    Order of insertion is not preserved.
 */
PUBLIC MprKey *mprAddKey(MprHash *hash, cvoid *key, cvoid *ptr)
{
    MprKey      *sp, *prevSp;
    int         index;

    if (hash == 0) {
        mprAssert(hash);
        return 0;
    }
    lock(hash);
    if ((sp = lookupHash(&index, &prevSp, hash, key)) != 0) {
        if (hash->flags & MPR_HASH_UNIQUE) {
            unlock(hash);
            return 0;
        }
        /*
            Already exists. Just update the data.
         */
        sp->data = ptr;
        unlock(hash);
        return sp;
    }
    /*
        Hash entries are managed by manageHashTable
     */
    if ((sp = mprAllocStruct(MprKey)) == 0) {
        unlock(hash);
        return 0;
    }
    sp->data = ptr;
    if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
        sp->key = dupKey(hash, key);
    } else {
        sp->key = (void*) key;
    }
    sp->bucket = index;
    sp->next = hash->buckets[index];
    hash->buckets[index] = sp;
    hash->length++;
    unlock(hash);
    return sp;
}


PUBLIC MprKey *mprAddKeyFmt(MprHash *hash, cvoid *key, cchar *fmt, ...)
{
    va_list     ap;
    char        *value;

    va_start(ap, fmt);
    value = sfmtv(fmt, ap);
    va_end(ap);
    return mprAddKey(hash, key, value);
}


/*
    Multiple insertion. Insert an entry into the hash hash allowing for multiple entries with the same key.
    Order of insertion is not preserved. Lookup cannot be used to retrieve all duplicate keys, some will be shadowed. 
    Use enumeration to retrieve the keys.
 */
PUBLIC MprKey *mprAddDuplicateKey(MprHash *hash, cvoid *key, cvoid *ptr)
{
    MprKey      *sp;
    int         index;

    mprAssert(hash);
    mprAssert(key);

    if ((sp = mprAllocStruct(MprKey)) == 0) {
        return 0;
    }
    sp->data = ptr;
    if (!(hash->flags & MPR_HASH_STATIC_KEYS)) {
        sp->key = dupKey(hash, key);
    } else {
        sp->key = (void*) key;
    }
    lock(hash);
    index = hash->fn(key, slen(key)) % hash->size;
    sp->bucket = index;
    sp->next = hash->buckets[index];
    hash->buckets[index] = sp;
    hash->length++;
    unlock(hash);
    return sp;
}


PUBLIC int mprRemoveKey(MprHash *hash, cvoid *key)
{
    MprKey      *sp, *prevSp;
    int         index;

    mprAssert(hash);
    mprAssert(key);

    lock(hash);
    if ((sp = lookupHash(&index, &prevSp, hash, key)) == 0) {
        unlock(hash);
        return MPR_ERR_CANT_FIND;
    }
    if (prevSp) {
        prevSp->next = sp->next;
    } else {
        hash->buckets[index] = sp->next;
    }
    hash->length--;
    unlock(hash);
    return 0;
}


PUBLIC MprHash *mprBlendHash(MprHash *hash, MprHash *extra)
{
    MprKey      *kp;

    if (hash == 0 || extra == 0) {
        return hash;
    }
    for (ITERATE_KEYS(extra, kp)) {
        mprAddKey(hash, kp->key, kp->data);
    }
    return hash;
}


PUBLIC MprHash *mprCloneHash(MprHash *master)
{
    MprKey      *kp;
    MprHash     *hash;

    if ((hash = mprCreateHash(master->size, master->flags)) == 0) {
        return 0;
    }
    kp = mprGetFirstKey(master);
    while (kp) {
        mprAddKey(hash, kp->key, kp->data);
        kp = mprGetNextKey(master, kp);
    }
    return hash;
}


/*
    Lookup a key and return the hash entry
 */
PUBLIC MprKey *mprLookupKeyEntry(MprHash *hash, cvoid *key)
{
    return lookupHash(0, 0, hash, key);
}


/*
    Lookup a key and return the hash entry data
 */
PUBLIC void *mprLookupKey(MprHash *hash, cvoid *key)
{
    MprKey      *sp;

    if ((sp = lookupHash(0, 0, hash, key)) == 0) {
        return 0;
    }
    return (void*) sp->data;
}


/*
    Exponential primes
 */
static int hashSizes[] = {
     19, 29, 59, 79, 97, 193, 389, 769, 1543, 3079, 6151, 12289, 24593, 49157, 98317, 196613, 0
};


static int getHashSize(int numKeys)
{
    int     i;

    for (i = 0; hashSizes[i]; i++) {
        if (numKeys < hashSizes[i]) {
            return hashSizes[i];
        }
    }
    return hashSizes[i - 1];
}


/*
    This is unlocked because it is read-only
 */
static MprKey *lookupHash(int *bucketIndex, MprKey **prevSp, MprHash *hash, cvoid *key)
{
    MprKey      *sp, *prev, *next;
    MprKey      **buckets;
    int         hashSize, i, index, rc;

    if (key == 0 || hash == 0) {
        return 0;
    }
    if (hash->length > hash->size) {
        hashSize = getHashSize(hash->length * 4 / 3);
        if (hash->size < hashSize) {
            if ((buckets = mprAllocZeroed(sizeof(MprKey*) * hashSize)) != 0) {
                hash->length = 0;
                for (i = 0; i < hash->size; i++) {
                    for (sp = hash->buckets[i]; sp; sp = next) {
                        next = sp->next;
                        mprAssert(next != sp);
                        index = hash->fn(sp->key, slen(sp->key)) % hashSize;
                        if (buckets[index]) {
                            sp->next = buckets[index];
                        } else {
                            sp->next = 0;
                        }
                        buckets[index] = sp;
                        sp->bucket = index;
                        hash->length++;
                    }
                }
                hash->size = hashSize;
                hash->buckets = buckets;
            }
        }
    }
    index = hash->fn(key, slen(key)) % hash->size;
    if (bucketIndex) {
        *bucketIndex = index;
    }
    sp = hash->buckets[index];
    prev = 0;

    while (sp) {
#if BIT_CHAR_LEN > 1 && UNUSED && KEEP
        if (hash->flags & MPR_HASH_UNICODE) {
            wchar *u1, *u2;
            u1 = (wchar*) sp->key;
            u2 = (wchar*) key;
            rc = -1;
            if (hash->flags & MPR_HASH_CASELESS) {
                rc = wcasecmp(u1, u2);
            } else {
                rc = wcmp(u1, u2);
            }
        } else 
#endif
        if (hash->flags & MPR_HASH_CASELESS) {
            rc = scaselesscmp(sp->key, key);
        } else {
            rc = strcmp(sp->key, key);
        }
        if (rc == 0) {
            if (prevSp) {
                *prevSp = prev;
            }
            return sp;
        }
        prev = sp;
        mprAssert(sp != sp->next);
        sp = sp->next;
    }
    return 0;
}


PUBLIC int mprGetHashLength(MprHash *hash)
{
    return hash->length;
}


/*
    Return the first entry in the hash.
 */
PUBLIC MprKey *mprGetFirstKey(MprHash *hash)
{
    MprKey      *sp;
    int         i;

    mprAssert(hash);

    for (i = 0; i < hash->size; i++) {
        if ((sp = (MprKey*) hash->buckets[i]) != 0) {
            return sp;
        }
    }
    return 0;
}


/*
    Return the next entry in the hash
 */
PUBLIC MprKey *mprGetNextKey(MprHash *hash, MprKey *last)
{
    MprKey      *sp;
    int         i;

    if (hash == 0) {
        return 0;
    }
    if (last == 0) {
        return mprGetFirstKey(hash);
    }
    if (last->next) {
        return last->next;
    }
    for (i = last->bucket + 1; i < hash->size; i++) {
        if ((sp = (MprKey*) hash->buckets[i]) != 0) {
            return sp;
        }
    }
    return 0;
}


static void *dupKey(MprHash *hash, cvoid *key)
{
#if BIT_CHAR_LEN > 1 && UNUSED && KEEP
    if (hash->flags & MPR_HASH_UNICODE) {
        return wclone((wchar*) key);
    } else
#endif
        return sclone(key);
}


PUBLIC MprHash *mprCreateHashFromWords(cchar *str)
{
    MprHash     *hash;
    char        *word, *next;

    hash = mprCreateHash(0, 0);
    word = stok(sclone(str), ", \t\n\r", &next);
    while (word) {
        mprAddKey(hash, word, word);
        word = stok(NULL, " \t\n\r", &next);
    }
    return hash;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprJSON.c"
 */
/************************************************************************/

/**
    mprJSON.c - A JSON parser and serializer. 

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */
/********************************** Includes **********************************/



/****************************** Forward Declarations **************************/

static MprObj *deserialize(MprJson *jp);
static char advanceToken(MprJson *jp);
static cchar *findEndKeyword(MprJson *jp, cchar *str);
static cchar *findQuote(cchar *tok, int quote);
static MprObj *makeObj(MprJson *jp, bool list);
static cchar *parseComment(MprJson *jp);
static void jsonParseError(MprJson *jp, cchar *msg);
static cchar *parseName(MprJson *jp);
static cchar *parseValue(MprJson *jp);
static int setValue(MprJson *jp, MprObj *obj, int index, cchar *name, cchar *value, int type);

/************************************ Code ************************************/

PUBLIC MprObj *mprDeserializeCustom(cchar *str, MprJsonCallback callback, void *data)
{
    MprJson     jp;

    /*
        There is no need for GC management as this routine does not yield
     */
    memset(&jp, 0, sizeof(jp));
    jp.lineNumber = 1;
    jp.tok = str;
    jp.callback = callback;
    jp.data = data;
    return deserialize(&jp);
}


/*
    Deserialize a JSON string into an MprHash object. Objects and lists "[]" are stored in hashes. 
 */
PUBLIC MprObj *mprDeserialize(cchar *str)
{
    MprJsonCallback cb;

    cb.checkState = 0;
    cb.makeObj = makeObj;
    cb.parseError = jsonParseError;
    cb.setValue = setValue;
    return mprDeserializeCustom(str, cb, 0); 
}


static MprObj *deserialize(MprJson *jp)
{
    cvoid   *value;
    MprObj  *obj;
    cchar   *name;
    int     token, rc, index, valueType;

    if ((token = advanceToken(jp)) == '[') {
        obj = jp->callback.makeObj(jp, 1);
        index = 0;
    } else if (token == '{') {
        obj = jp->callback.makeObj(jp, 0);
        index = -1;
    } else {
        return (MprObj*) parseValue(jp);
    }
    jp->tok++;

    while (*jp->tok) {
        switch (advanceToken(jp)) {
        case '\0':
            break;

        case ',':
            if (index >= 0) {
                index++;
            }
            jp->tok++;
            continue;

        case '/':
            if (jp->tok[1] == '/' || jp->tok[1] == '*') {
                jp->tok = parseComment(jp);
            } else {
                mprJsonParseError(jp, "Unexpected character '%c'", *jp->tok);
                return 0;
            }
            continue;

        case '}':
        case ']':
            /* End of object or array */
            if (jp->callback.checkState && jp->callback.checkState(jp, NULL) < 0) {
                return 0;
            }
            jp->tok++;
            return obj;
            
        default:
            /*
                Value: String, "{" or "]"
             */
            value = 0;
            if (index < 0) {
                if ((name = parseName(jp)) == 0) {
                    return 0;
                }
                if ((token = advanceToken(jp)) != ':') {
                    if (token == ',' || token == '}' || token == ']') {
                        valueType = MPR_JSON_STRING;
                        value = name;
                    } else {
                        mprJsonParseError(jp, "Bad separator '%c'", *jp->tok);
                        return 0;
                    }
                }
                jp->tok++;
            } else {
                name = 0;
            }
            if (!value) {
                advanceToken(jp);
                if (jp->callback.checkState && jp->callback.checkState(jp, name) < 0) {
                    return 0;
                }
                if (*jp->tok == '{') {
                    value = deserialize(jp);
                    valueType = MPR_JSON_OBJ;

                } else if (*jp->tok == '[') {
                    value = deserialize(jp);
                    valueType = MPR_JSON_ARRAY;

                } else {
                    value = parseValue(jp);
                    valueType = MPR_JSON_STRING;
                }
                if (!value) {
                    /* Error already reported */
                    return 0;
                }
            }
            if ((rc = jp->callback.setValue(jp, obj, index, name, value, valueType)) < 0) {
                return 0;
            }
        }
    }
    return obj;
}


static cchar *parseComment(MprJson *jp)
{
    cchar   *tok;

    tok = jp->tok;
    if (*tok == '/') {
        for (tok++; *tok && *tok != '\n'; tok++) ;

    } else if (*jp->tok == '*') {
        tok++;
        for (tok++; tok[0] && (tok[0] != '*' || tok[1] != '/'); tok++) {
            if (*tok == '\n') {
                jp->lineNumber++;
            }
        }
    }
    return tok - 1;
}


static cchar *parseQuotedName(MprJson *jp)
{
    cchar    *etok, *name;
    int      quote;

    quote = *jp->tok;
    if ((etok = findQuote(++jp->tok, quote)) == 0) {
        mprJsonParseError(jp, "Missing closing quote");
        return 0;
    }
    name = snclone(jp->tok, etok - jp->tok);
    jp->tok = ++etok;
    return name;
}


static cchar *parseUnquotedName(MprJson *jp)
{
    cchar    *etok, *name;

    etok = findEndKeyword(jp, jp->tok);
    name = snclone(jp->tok, etok - jp->tok);
    jp->tok = etok;
    return name;
}


static cchar *parseName(MprJson *jp)
{
    char    token;

    token = advanceToken(jp);
    if (token == '"' || token == '\'') {
        return parseQuotedName(jp);
    } else {
        return parseUnquotedName(jp);
    }
}


static cchar *parseValue(MprJson *jp)
{
    cchar   *etok, *value;
    int     quote;

    value = 0;
    if (*jp->tok == '"' || *jp->tok == '\'') {
        quote = *jp->tok;
        if ((etok = findQuote(++jp->tok, quote)) == 0) {
            mprJsonParseError(jp, "Missing closing quote");
            return 0;
        }
        value = snclone(jp->tok, etok - jp->tok);
        jp->tok = etok + 1;

    } else {
        etok = findEndKeyword(jp, jp->tok);
        value = snclone(jp->tok, etok - jp->tok);
        jp->tok = etok;
    }
    return value;
}


static int setValue(MprJson *jp, MprObj *obj, int index, cchar *key, cchar *value, int type)
{
    MprKey  *kp;
    char    keybuf[32];

    if (index >= 0) {
        itosbuf(keybuf, sizeof(keybuf), index, 10);
        key = keybuf;
    }
    if ((kp = mprAddKey(obj, key, value)) == 0) {
        return MPR_ERR_MEMORY;
    }
    kp->type = type;
    return 0;
}


static MprObj *makeObj(MprJson *jp, bool list)
{
    MprHash     *hash;

    if ((hash = mprCreateHash(0, 0)) == 0) {
        return 0;
    }
    if (list) {
        hash->flags |= MPR_HASH_LIST;
    }
    return hash;
}


static void quoteValue(MprBuf *buf, cchar *str)
{
    cchar   *cp;

    mprPutCharToBuf(buf, '\'');
    for (cp = str; *cp; cp++) {
        if (*cp == '\'') {
            mprPutCharToBuf(buf, '\\');
        }
        mprPutCharToBuf(buf, *cp);
    }
    mprPutCharToBuf(buf, '\'');
}


/*
    Supports hashes where properties are strings or hashes of strings. N-level nest is supported.
 */
static cchar *objToString(MprBuf *buf, MprObj *obj, int type, int pretty)
{
    MprKey  *kp;
    char    numbuf[32];
    int     i, len;

    if (type == MPR_JSON_ARRAY) {
        mprPutCharToBuf(buf, '[');
        if (pretty) mprPutCharToBuf(buf, '\n');
        len = mprGetHashLength(obj);
        for (i = 0; i < len; i++) {
            itosbuf(numbuf, sizeof(numbuf), i, 10);
            if (pretty) mprPutStringToBuf(buf, "    ");
            if ((kp = mprLookupKeyEntry(obj, numbuf)) == 0) {
                mprAssert(kp);
                continue;
            }
            if (kp->type == MPR_JSON_ARRAY || kp->type == MPR_JSON_OBJ) {
                objToString(buf, (MprObj*) kp->data, kp->type, pretty);
            } else {
                quoteValue(buf, kp->data);
            }
            mprPutCharToBuf(buf, ',');
            if (pretty) mprPutCharToBuf(buf, '\n');
        }
        mprPutCharToBuf(buf, ']');

    } else if (type == MPR_JSON_OBJ) {
        mprPutCharToBuf(buf, '{');
        if (pretty) mprPutCharToBuf(buf, '\n');
        for (ITERATE_KEYS(obj, kp)) {
            if (kp->key == 0 || kp->data == 0) continue;
            if (pretty) mprPutStringToBuf(buf, "    ");
            mprPutStringToBuf(buf, kp->key);
            mprPutStringToBuf(buf, ": ");
            if (kp->type == MPR_JSON_ARRAY || kp->type == MPR_JSON_OBJ) {
                objToString(buf, (MprObj*) kp->data, kp->type, pretty);
            } else {
                quoteValue(buf, kp->data);
            }
            mprPutCharToBuf(buf, ',');
            if (pretty) mprPutCharToBuf(buf, '\n');
        }
        mprPutCharToBuf(buf, '}');
    }
    if (pretty) mprPutCharToBuf(buf, '\n');
    return sclone(mprGetBufStart(buf));
}


/*
    Serialize into JSON format.
 */
PUBLIC cchar *mprSerialize(MprObj *obj, int flags)
{
    MprBuf  *buf;
    int     pretty;

    pretty = (flags & MPR_JSON_PRETTY);
    if ((buf = mprCreateBuf(0, 0)) == 0) {
        return 0;
    }
    objToString(buf, obj, MPR_JSON_OBJ, pretty);
    return mprGetBuf(buf);
}


static char advanceToken(MprJson *jp)
{
    while (isspace((uchar) *jp->tok)) {
        if (*jp->tok == '\n') {
            jp->lineNumber++;
        }
        jp->tok++;
    }
    return *jp->tok;
}


static cchar *findQuote(cchar *tok, int quote)
{
    cchar   *cp;

    mprAssert(tok);
    for (cp = tok; *cp; cp++) {
        if (*cp == quote && (cp == tok || *cp != '\\')) {
            return cp;
        }
    }
    return 0;
}


static cchar *findEndKeyword(MprJson *jp, cchar *str)
{
    cchar   *cp, *etok;

    mprAssert(str);
    for (cp = jp->tok; *cp; cp++) {
        if ((etok = strpbrk(cp, " \t\n\r:,}]")) != 0) {
            if (etok == jp->tok || *etok != '\\') {
                return etok;
            }
        }
    }
    return &str[strlen(str)];
}


static void jsonParseError(MprJson *jp, cchar *msg)
{
    if (jp->path) {
        mprLog(4, "%s\nIn file '%s' at line %d", msg, jp->path, jp->lineNumber);
    } else {
        mprLog(4, "%s\nAt line %d", msg, jp->lineNumber);
    }
}


PUBLIC void mprJsonParseError(MprJson *jp, cchar *fmt, ...)
{
    va_list     args;
    cchar       *msg;

    va_start(args, fmt);
    msg = sfmtv(fmt, args);
    (jp->callback.parseError)(jp, msg);
    va_end(args);
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprKqueue.c"
 */
/************************************************************************/

/**
    mprKevent.c - Wait for I/O by using kevent on BSD based Unix systems.

    This module augments the mprWait wait services module by providing kqueue() based waiting support.
    Also see mprAsyncSelectWait and mprSelectWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if MPR_EVENT_KQUEUE
/********************************** Forwards **********************************/

static int growEvents(MprWaitService *ws);
static void serviceIO(MprWaitService *ws, int count);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    ws->interestMax = MPR_FD_MIN;
    ws->eventsMax = MPR_FD_MIN;
    ws->handlerMax = MPR_FD_MIN;
    ws->interest = mprAllocZeroed(sizeof(struct kevent) * ws->interestMax);
    ws->events = mprAllocZeroed(sizeof(struct kevent) * ws->eventsMax);
    ws->handlerMap = mprAllocZeroed(sizeof(MprWaitHandler*) * ws->handlerMax);
    if (ws->interest == 0 || ws->events == 0 || ws->handlerMap == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    if ((ws->kq = kqueue()) < 0) {
        mprError("Call to kqueue() failed");
        return MPR_ERR_CANT_INITIALIZE;
    }
    /*
        Initialize the "wakeup" pipe. This is used to wakeup the service thread if other threads need to wait for I/O.
     */
    if (pipe(ws->breakPipe) < 0) {
        mprError("Can't open breakout pipe");
        return MPR_ERR_CANT_INITIALIZE;
    }
    fcntl(ws->breakPipe[0], F_SETFL, fcntl(ws->breakPipe[0], F_GETFL) | O_NONBLOCK);
    fcntl(ws->breakPipe[1], F_SETFL, fcntl(ws->breakPipe[1], F_GETFL) | O_NONBLOCK);
    EV_SET(&ws->interest[ws->interestCount], ws->breakPipe[MPR_READ_PIPE], EVFILT_READ, EV_ADD, 0, 0, 0);
    ws->interestCount++;
    return 0;
}


PUBLIC void mprManageKqueue(MprWaitService *ws, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ws->events);
        mprMark(ws->interest);
        mprMark(ws->stableInterest);

    } else if (flags & MPR_MANAGE_FREE) {
        if (ws->kq) {
            close(ws->kq);
        }
        if (ws->breakPipe[0] >= 0) {
            close(ws->breakPipe[0]);
        }
        if (ws->breakPipe[1] >= 0) {
            close(ws->breakPipe[1]);
        }
    }
}


static int growEvents(MprWaitService *ws)
{
    ws->interestMax *= 2;
    ws->eventsMax = ws->interestMax;
    ws->interest = mprRealloc(ws->interest, sizeof(struct kevent) * ws->interestMax);
    ws->events = mprRealloc(ws->events, sizeof(struct kevent) * ws->eventsMax);
    if (ws->interest == 0 || ws->events == 0) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    return 0;
}


PUBLIC int mprNotifyOn(MprWaitService *ws, MprWaitHandler *wp, int mask)
{
    struct kevent   *kp, *start;
    int             fd;

    mprAssert(wp);
    fd = wp->fd;

    lock(ws);
    mprLog(7, "mprNotifyOn: fd %d, mask %x, old mask %x", wp->fd, mask, wp->desiredMask);
    if (wp->desiredMask != mask) {
        mprAssert(fd >= 0);
        while ((ws->interestCount + 4) >= ws->interestMax) {
            growEvents(ws);
        }
        start = kp = &ws->interest[ws->interestCount];
        if (wp->desiredMask & MPR_READABLE && !(mask & MPR_READABLE)) {
            EV_SET(kp, fd, EVFILT_READ, EV_DELETE, 0, 0, 0);
            kp++;
        }
        if (wp->desiredMask & MPR_WRITABLE && !(mask & MPR_WRITABLE)) {
            EV_SET(kp, fd, EVFILT_WRITE, EV_DELETE, 0, 0, 0);
            kp++;
        }
        if (mask & MPR_READABLE) {
            EV_SET(kp, fd, EVFILT_READ, EV_ADD, 0, 0, 0);
            kp++;
        }
        if (mask & MPR_WRITABLE) {
            EV_SET(kp, fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
            kp++;
        }
        ws->interestCount += (int) (kp - start);
        if (fd >= ws->handlerMax) {
            ws->handlerMax = fd + 32;
            if ((ws->handlerMap = mprRealloc(ws->handlerMap, sizeof(MprWaitHandler*) * ws->handlerMax)) == 0) {
                mprAssert(!MPR_ERR_MEMORY);
                return MPR_ERR_MEMORY;
            }
        }
        mprAssert(ws->handlerMap[fd] == 0 || ws->handlerMap[fd] == wp);
        wp->desiredMask = mask;
    }
    ws->handlerMap[fd] = (mask) ? wp : 0;
    unlock(ws);
    return 0;
}


/*
    Wait for I/O on a single file descriptor. Return a mask of events found. Mask is the events of interest.
    timeout is in milliseconds.
 */
PUBLIC int mprWaitForSingleIO(int fd, int mask, MprTime timeout)
{
    struct timespec ts;
    struct kevent   interest[2], events[1];
    int             kq, interestCount, rc;

    if (timeout < 0) {
        timeout = MAXINT;
    }
    interestCount = 0; 
    if (mask & MPR_READABLE) {
        EV_SET(&interest[interestCount++], fd, EVFILT_READ, EV_ADD, 0, 0, 0);
    }
    if (mask & MPR_WRITABLE) {
        EV_SET(&interest[interestCount++], fd, EVFILT_WRITE, EV_ADD, 0, 0, 0);
    }
    kq = kqueue();
    ts.tv_sec = ((int) (timeout / 1000));
    ts.tv_nsec = ((int) (timeout % 1000)) * 1000 * 1000;

    mask = 0;
    rc = kevent(kq, interest, interestCount, events, 1, &ts);
    close(kq);
    if (rc < 0) {
        mprLog(7, "Kevent returned %d, errno %d", rc, errno);
    } else if (rc > 0) {
        if (rc > 0) {
            if (events[0].filter == EVFILT_READ) {
                mask |= MPR_READABLE;
            }
            if (events[0].filter == EVFILT_WRITE) {
                mask |= MPR_WRITABLE;
            }
        }
    }
    return mask;
}


/*
    Wait for I/O on all registered file descriptors. Timeout is in milliseconds. Return the number of events detected.
 */
PUBLIC void mprWaitForIO(MprWaitService *ws, MprTime timeout)
{
    struct timespec ts;
    int             rc;

    mprAssert(timeout > 0);

    if (timeout < 0) {
        timeout = MAXINT;
    }
#if BIT_DEBUG
    if (mprGetDebugMode() && timeout > 30000) {
        timeout = 30000;
    }
#endif
    ts.tv_sec = ((int) (timeout / 1000));
    ts.tv_nsec = ((int) ((timeout % 1000) * 1000 * 1000));

    if (ws->needRecall) {
        mprDoWaitRecall(ws);
        return;
    }
    lock(ws);
    ws->stableInterest = mprMemdup(ws->interest, sizeof(struct kevent) * ws->interestCount);
    ws->stableInterestCount = ws->interestCount;
    /* Preserve the wakeup pipe fd */
    ws->interestCount = 1;
    unlock(ws);

    LOG(8, "kevent sleep for %d", timeout);
    mprYield(MPR_YIELD_STICKY);
    rc = kevent(ws->kq, ws->stableInterest, ws->stableInterestCount, ws->events, ws->eventsMax, &ts);
    mprResetYield();
    LOG(8, "kevent wakes rc %d", rc);

    if (rc < 0) {
        mprLog(7, "Kevent returned %d, errno %d", rc, mprGetOsError());
    } else if (rc > 0) {
        serviceIO(ws, rc);
    }
    ws->wakeRequested = 0;
}


static void serviceIO(MprWaitService *ws, int count)
{
    MprWaitHandler      *wp;
    struct kevent       *kev;
    char                buf[128];
    int                 fd, i, mask, err;

    lock(ws);
    for (i = 0; i < count; i++) {
        kev = &ws->events[i];
        fd = (int) kev->ident;
        mprAssert(fd < ws->handlerMax);
        if ((wp = ws->handlerMap[fd]) == 0) {
            if (kev->filter == EVFILT_READ && fd == ws->breakPipe[MPR_READ_PIPE]) {
                (void) read(fd, buf, sizeof(buf));
            }
            continue;
        }
        if (kev->flags & EV_ERROR) {
            err = (int) kev->data;
            if (err == ENOENT) {
                /* File descriptor was closed and re-opened */
                mask = wp->desiredMask;
                mprNotifyOn(ws, wp, 0);
                wp->desiredMask = 0;
                mprNotifyOn(ws, wp, mask);
                mprLog(7, "kqueue: file descriptor closed and reopened, fd %d", wp->fd);

            } else if (err == EBADF) {
                /* File descriptor was closed */
                mask = wp->desiredMask;
                mprNotifyOn(ws, wp, 0);
                wp->desiredMask = 0;
                mprNotifyOn(ws, wp, mask);
                mprLog(7, "kqueue: invalid file descriptor %d, fd %d", wp->fd);
            }
            continue;
        }
        mask = 0;
        if (kev->filter == EVFILT_READ) {
            mask |= MPR_READABLE;
        }
        if (kev->filter == EVFILT_WRITE) {
            mask |= MPR_WRITABLE;
        }
        wp->presentMask = mask & wp->desiredMask;
        LOG(7, "Got I/O event mask %x", wp->presentMask);
        if (wp->presentMask) {
            LOG(7, "ServiceIO for wp %p", wp);
            /* Suppress further events while this event is being serviced. User must re-enable */
            mprNotifyOn(ws, wp, 0);            
            mprQueueIOEvent(wp);
        }
    }
    unlock(ws);
}


/*
    Wake the wait service. WARNING: This routine must not require locking. MprEvents in scheduleDispatcher depends on this.
    Must be async-safe.
 */
PUBLIC void mprWakeNotifier()
{
    MprWaitService  *ws;
    int             c;

    ws = MPR->waitService;
    if (!ws->wakeRequested) {
        ws->wakeRequested = 1;
        c = 0;
        (void) write(ws->breakPipe[MPR_WRITE_PIPE], (char*) &c, 1);
    }
}

#else
PUBLIC void stubMprKqueue() {}
#endif /* MPR_EVENT_KQUEUE */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprList.c"
 */
/************************************************************************/

/**
    mprList.c - Simple list type.

    The list supports two modes of operation. Compact mode where the list is compacted after removing list items, 
    and no-compact mode where removed items are zeroed. No-compact mode implies that all valid list entries must 
    be non-zero.

    This module is not thread-safe. It is the callers responsibility to perform all thread synchronization.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/****************************** Forward Declarations **************************/

static int growList(MprList *lp, int incr);
static void manageList(MprList *lp, int flags);

/************************************ Code ************************************/
/*
    Create a general growable list structure
 */
PUBLIC MprList *mprCreateList(int size, int flags)
{
    MprList     *lp;

    if ((lp = mprAllocObj(MprList, manageList)) == 0) {
        return 0;
    }
    lp->maxSize = MAXINT;
    lp->flags = flags | MPR_OBJ_LIST;
    if (!(flags & MPR_LIST_OWN)) {
        lp->mutex = mprCreateLock();
    }
    if (size != 0) {
        mprSetListLimits(lp, size, -1);
    }
    return lp;
}


static void manageList(MprList *lp, int flags)
{
    int     i;

    if (flags & MPR_MANAGE_MARK) {
        mprMark(lp->mutex);
        lock(lp);
        mprMark(lp->items);
        if (!(lp->flags & MPR_LIST_STATIC_VALUES)) {
            for (i = 0; i < lp->length; i++) {
                mprAssert(lp->items[i] == 0 || mprIsValid(lp->items[i]));
                mprMark(lp->items[i]);
            }
        }
        unlock(lp);
    }
}

/*
    Initialize a list which may not be a memory context.
 */
PUBLIC void mprInitList(MprList *lp, int flags)
{
    lp->flags = 0;
    lp->size = 0;
    lp->length = 0;
    lp->maxSize = MAXINT;
    lp->items = 0;
    lp->mutex = (flags & MPR_LIST_OWN) ? 0 : mprCreateLock();
}


/*
    Define the list maximum size. If the list has not yet been written to, the initialSize will be observed.
 */
PUBLIC int mprSetListLimits(MprList *lp, int initialSize, int maxSize)
{
    ssize   size;

    if (initialSize <= 0) {
        initialSize = MPR_LIST_INCR;
    }
    if (maxSize <= 0) {
        maxSize = MAXINT;
    }
    size = initialSize * sizeof(void*);

    lock(lp);
    if (lp->items == 0) {
        if ((lp->items = mprAlloc(size)) == 0) {
            mprAssert(!MPR_ERR_MEMORY);
            unlock(lp);
            return MPR_ERR_MEMORY;
        }
        memset(lp->items, 0, size);
        lp->size = initialSize;
    }
    lp->maxSize = maxSize;
    unlock(lp);
    return 0;
}


PUBLIC int mprCopyListContents(MprList *dest, MprList *src)
{
    void        *item;
    int         next;

    mprClearList(dest);

    lock(src);
    if (mprSetListLimits(dest, src->size, src->maxSize) < 0) {
        mprAssert(!MPR_ERR_MEMORY);
        unlock(src);
        return MPR_ERR_MEMORY;
    }
    for (next = 0; (item = mprGetNextItem(src, &next)) != 0; ) {
        if (mprAddItem(dest, item) < 0) {
            mprAssert(!MPR_ERR_MEMORY);
            unlock(src);
            return MPR_ERR_MEMORY;
        }
    }
    unlock(src);
    return 0;
}


PUBLIC MprList *mprCloneList(MprList *src)
{
    MprList     *lp;

    if ((lp = mprCreateList(src->size, src->flags)) == 0) {
        return 0;
    }
    if (mprCopyListContents(lp, src) < 0) {
        return 0;
    }
    return lp;
}


PUBLIC MprList *mprAppendList(MprList *lp, MprList *add)
{
    void        *item;
    int         next;

    mprAssert(lp);

    for (next = 0; ((item = mprGetNextItem(add, &next)) != 0); ) {
        if (mprAddItem(lp, item) < 0) {
            return 0;
        }
    }
    return lp;
}


/*
    Change the item in the list at index. Return the old item.
 */
PUBLIC void *mprSetItem(MprList *lp, int index, cvoid *item)
{
    void    *old;
    int     length;

    mprAssert(lp);
    mprAssert(lp->size >= 0);
    mprAssert(lp->length >= 0);
    mprAssert(index >= 0);

    length = lp->length;

    if (index >= length) {
        length = index + 1;
    }
    lock(lp);
    if (length > lp->size) {
        if (growList(lp, length - lp->size) < 0) {
            unlock(lp);
            return 0;
        }
    }
    old = lp->items[index];
    lp->items[index] = (void*) item;
    lp->length = length;
    unlock(lp);
    return old;
}



/*
    Add an item to the list and return the item index.
 */
PUBLIC int mprAddItem(MprList *lp, cvoid *item)
{
    int     index;

    mprAssert(lp);
    mprAssert(lp->size >= 0);
    mprAssert(lp->length >= 0);

    lock(lp);
    if (lp->length >= lp->size) {
        if (growList(lp, 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }
    }
    index = lp->length++;
    lp->items[index] = (void*) item;
    unlock(lp);
    return index;
}


PUBLIC int mprAddNullItem(MprList *lp)
{
    int     index;

    mprAssert(lp);
    mprAssert(lp->size >= 0);
    mprAssert(lp->length >= 0);

    lock(lp);
    if (lp->length != 0 && lp->items[lp->length - 1] == 0) {
        index = lp->length - 1;
    } else {
        if (lp->length >= lp->size) {
            if (growList(lp, 1) < 0) {
                unlock(lp);
                return MPR_ERR_TOO_MANY;
            }
        }
        index = lp->length;
        lp->items[index] = 0;
    }
    unlock(lp);
    return index;
}


/*
    Insert an item to the list at a specified position. We insert before the item at "index".
    ie. The inserted item will go into the "index" location and the other elements will be moved up.
 */
PUBLIC int mprInsertItemAtPos(MprList *lp, int index, cvoid *item)
{
    void    **items;
    int     i;

    mprAssert(lp);
    mprAssert(lp->size >= 0);
    mprAssert(lp->length >= 0);
    mprAssert(index >= 0);

    if (index < 0) {
        index = 0;
    }
    lock(lp);
    if (index >= lp->size) {
        if (growList(lp, index - lp->size + 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }

    } else if (lp->length >= lp->size) {
        if (growList(lp, 1) < 0) {
            unlock(lp);
            return MPR_ERR_TOO_MANY;
        }
    }
    if (index >= lp->length) {
        lp->length = index + 1;
    } else {
        /*
            Copy up items to make room to insert
         */
        items = lp->items;
        for (i = lp->length; i > index; i--) {
            items[i] = items[i - 1];
        }
        lp->length++;
    }
    lp->items[index] = (void*) item;
    unlock(lp);
    return index;
}


/*
    Remove an item from the list. Return the index where the item resided.
 */
PUBLIC int mprRemoveItem(MprList *lp, cvoid *item)
{
    int     index;

    mprAssert(lp);

    lock(lp);
    index = mprLookupItem(lp, item);
    if (index < 0) {
        unlock(lp);
        return index;
    }
    index = mprRemoveItemAtPos(lp, index);
    mprAssert(index >= 0);
    unlock(lp);
    return index;
}


PUBLIC int mprRemoveLastItem(MprList *lp)
{
    mprAssert(lp);
    mprAssert(lp->size > 0);
    mprAssert(lp->length > 0);

    if (lp->length <= 0) {
        return MPR_ERR_CANT_FIND;
    }
    return mprRemoveItemAtPos(lp, lp->length - 1);
}


/*
    Remove an index from the list. Return the index where the item resided.
    The list is compacted.
 */
PUBLIC int mprRemoveItemAtPos(MprList *lp, int index)
{
    void    **items;

    mprAssert(lp);
    mprAssert(lp->size > 0);
    mprAssert(index >= 0 && index < lp->size);
    mprAssert(lp->length > 0);

    if (index < 0 || index >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    lock(lp);
    items = lp->items;
#if FUTURE
    void    **ip;
    if (index == (lp->length - 1)) {
        /* Scan backwards to find last non-null item */
        for (ip = &items[index - 1]; ip >= items && *ip == 0; ip--) ;
        lp->length = ++ip - items;
        mprAssert(lp->length >= 0);
    } else {
        /* Copy down following items */
        for (ip = &items[index]; ip < &items[lp->length]; ip++) {
            *ip = ip[1];
        }
        lp->length--;
    }
#else
    memmove(&items[index], &items[index + 1], (lp->length - index - 1) * sizeof(void*));
    lp->length--;
#endif
    lp->items[lp->length] = 0;
    mprAssert(lp->length >= 0);
    unlock(lp);
    return index;
}


/*
    Remove a set of items. Return 0 if successful.
 */
PUBLIC int mprRemoveRangeOfItems(MprList *lp, int start, int end)
{
    void    **items;
    int     i, count;

    mprAssert(lp);
    mprAssert(lp->size > 0);
    mprAssert(lp->length > 0);
    mprAssert(start > end);

    if (start < 0 || start >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    if (end < 0 || end >= lp->length) {
        return MPR_ERR_CANT_FIND;
    }
    if (start > end) {
        return MPR_ERR_BAD_ARGS;
    }

    /*
        Copy down to compress
     */
    items = lp->items;
    count = end - start;
    lock(lp);
    for (i = start; i < (lp->length - count); i++) {
        items[i] = items[i + count];
    }
    lp->length -= count;
    for (i = lp->length; i < lp->size; i++) {
        items[i] = 0;
    }
    unlock(lp);
    return 0;
}


/*
    Remove a string item from the list. Return the index where the item resided.
 */
PUBLIC int mprRemoveStringItem(MprList *lp, cchar *str)
{
    int     index;

    mprAssert(lp);

    lock(lp);
    index = mprLookupStringItem(lp, str);
    if (index < 0) {
        unlock(lp);
        return index;
    }
    index = mprRemoveItemAtPos(lp, index);
    mprAssert(index >= 0);
    unlock(lp);
    return index;
}


PUBLIC void *mprGetItem(MprList *lp, int index)
{
    mprAssert(lp);

    if (index < 0 || index >= lp->length) {
        return 0;
    }
    return lp->items[index];
}


PUBLIC void *mprGetFirstItem(MprList *lp)
{
    mprAssert(lp);

    if (lp == 0) {
        return 0;
    }
    if (lp->length == 0) {
        return 0;
    }
    return lp->items[0];
}


PUBLIC void *mprGetLastItem(MprList *lp)
{
    mprAssert(lp);

    if (lp == 0) {
        return 0;
    }
    if (lp->length == 0) {
        return 0;
    }
    return lp->items[lp->length - 1];
}


PUBLIC void *mprGetNextItem(MprList *lp, int *next)
{
    void    *item;
    int     index;

    mprAssert(next);
    mprAssert(*next >= 0);

    if (lp == 0) {
        return 0;
    }
    lock(lp);
    index = *next;
    if (index < lp->length) {
        item = lp->items[index];
        *next = ++index;
        unlock(lp);
        return item;
    }
    unlock(lp);
    return 0;
}


PUBLIC void *mprGetPrevItem(MprList *lp, int *next)
{
    void    *item;
    int     index;

    mprAssert(next);

    if (lp == 0) {
        return 0;
    }
    lock(lp);
    if (*next < 0) {
        *next = lp->length;
    }
    index = *next;
    if (--index < lp->length && index >= 0) {
        *next = index;
        item = lp->items[index];
        unlock(lp);
        return item;
    }
    unlock(lp);
    return 0;
}


PUBLIC int mprPushItem(MprList *lp, cvoid *item)
{
    return mprAddItem(lp, item);
}


PUBLIC void *mprPopItem(MprList *lp)
{
    void    *item;
    int     index;

    item = NULL;
    if (lp->length > 0) {
        lock(lp);
        index = lp->length - 1;
        item = mprGetItem(lp, index);
        mprRemoveItemAtPos(lp, index);
        unlock(lp);
    }
    return item;
}


#ifndef mprGetListLength
PUBLIC int mprGetListLength(MprList *lp)
{
    if (lp == 0) {
        return 0;
    }
    return lp->length;
}
#endif


PUBLIC int mprGetListCapacity(MprList *lp)
{
    mprAssert(lp);

    if (lp == 0) {
        return 0;
    }
    return lp->size;
}


PUBLIC void mprClearList(MprList *lp)
{
    int     i;

    mprAssert(lp);

    lock(lp);
    for (i = 0; i < lp->length; i++) {
        lp->items[i] = 0;
    }
    lp->length = 0;
    unlock(lp);
}


PUBLIC int mprLookupItem(MprList *lp, cvoid *item)
{
    int     i;

    mprAssert(lp);
    
    lock(lp);
    for (i = 0; i < lp->length; i++) {
        if (lp->items[i] == item) {
            unlock(lp);
            return i;
        }
    }
    unlock(lp);
    return MPR_ERR_CANT_FIND;
}


PUBLIC int mprLookupStringItem(MprList *lp, cchar *str)
{
    int     i;

    mprAssert(lp);
    
    lock(lp);
    for (i = 0; i < lp->length; i++) {
        if (smatch(lp->items[i], str)) {
            unlock(lp);
            return i;
        }
    }
    unlock(lp);
    return MPR_ERR_CANT_FIND;
}


/*
    Grow the list by the requried increment
 */
static int growList(MprList *lp, int incr)
{
    ssize       memsize;
    int         len;

    if (lp->maxSize <= 0) {
        lp->maxSize = MAXINT;
    }
    /*
        Need to grow the list
     */
    if (lp->size >= lp->maxSize) {
        mprAssert(lp->size < lp->maxSize);
        return MPR_ERR_TOO_MANY;
    }
    /*
        If growing by 1, then use the default increment which exponentially grows. Otherwise, assume the caller knows exactly
        how much the list needs to grow.
     */
    if (incr <= 1) {
        len = MPR_LIST_INCR + (lp->size * 2);
    } else {
        len = lp->size + incr;
    }
    memsize = len * sizeof(void*);

    /*
        Lock free realloc. Old list will be intact via lp->items until mprRealloc returns.
     */
    if ((lp->items = mprRealloc(lp->items, memsize)) == NULL) {
        mprAssert(!MPR_ERR_MEMORY);
        return MPR_ERR_MEMORY;
    }
    lp->size = len;
    return 0;
}


static int defaultSort(char **q1, char **q2, void *ctx)
{
    return scmp(*q1, *q2);
}


PUBLIC MprList *mprSortList(MprList *lp, MprSortProc compare, void *ctx)
{
    lock(lp);
    if (!compare) {
        compare = (MprSortProc) defaultSort;
    }
    mprSort(lp->items, lp->length, sizeof(void*), compare, ctx);
    unlock(lp);
    return lp;
}


static void manageKeyValue(MprKeyValue *pair, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(pair->key);
        mprMark(pair->value);
    }
}


PUBLIC MprKeyValue *mprCreateKeyPair(cchar *key, cchar *value)
{
    MprKeyValue     *pair;
    
    if ((pair = mprAllocObj(MprKeyValue, manageKeyValue)) == 0) {
        return 0;
    }
    pair->key = sclone(key);
    pair->value = sclone(value);
    return pair;
}


static void swapElt(char *a, char *b, ssize width)
{
    char tmp;

    if (a != b) {
        while (width--) {
            tmp = *a;
            *a++ = *b;
            *b++ = tmp;
        }
    }
}


static void shortsort(char *lo, char *hi, ssize width, MprSortProc comp, void *ctx)
{
    char    *p, *max;

    while (hi > lo) {
        max = lo;
        for (p = lo + width; p <= hi; p += width) {
            if (comp(p, max, ctx) > 0) {
                max = p;
            }
        }
        swapElt(max, hi, width);
        hi -= width;
    }
}

PUBLIC void mprSort(void *base, ssize num, ssize width, MprSortProc comp, void *ctx) 
{
    char    *lo, *hi, *mid, *l, *h, *lostk[30], *histk[30];
    ssize   size;
    int     stkptr;

    if (num < 2 || width == 0) {
        return;
    }
    stkptr = 0;
    lo = base;
    hi = (char *) base + width * (num - 1);

recurse:
    size = (int) (hi - lo) / width + 1;
    if (size <= 8) {
        shortsort(lo, hi, width, comp, ctx);
    } else {
        mid = lo + (size / 2) * width;
        swapElt(mid, lo, width);
        l = lo;
        h = hi + width;

        for (;;) {
            do { l += width; } while (l <= hi && comp(l, lo, ctx) <= 0);
            do { h -= width; } while (h > lo && comp(h, lo, ctx) >= 0);
            if (h < l) break;
            swapElt(l, h, width);
        }
        swapElt(lo, h, width);

        if (h - 1 - lo >= hi - l) {
            if (lo + width < h) {
                lostk[stkptr] = lo;
                histk[stkptr] = h - width;
                ++stkptr;
            }
            if (l < hi) {
                lo = l;
                goto recurse;
            }
        } else {
            if (l < hi) {
                lostk[stkptr] = l;
                histk[stkptr] = hi;
                ++stkptr;
            }
            if (lo + width < h) {
                hi = h - width;
                goto recurse;
            }
        }
    }
    --stkptr;
    if (stkptr >= 0) {
        lo = lostk[stkptr];
        hi = histk[stkptr];
        goto recurse;
    } else {
        return;
    }
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprLock.c"
 */
/************************************************************************/

/**
    mprLock.c - Thread Locking Support

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/*********************************** Includes *********************************/



/***************************** Forward Declarations ***************************/

static void manageLock(MprMutex *lock, int flags);

/************************************ Code ************************************/

PUBLIC MprMutex *mprCreateLock()
{
    MprMutex    *lock;
#if BIT_UNIX_LIKE
    pthread_mutexattr_t attr;
#endif
    if ((lock = mprAllocObj(MprMutex, manageLock)) == 0) {
        return 0;
    }
#if BIT_UNIX_LIKE
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&lock->cs, &attr);
    pthread_mutexattr_destroy(&attr);
#elif WINCE
    InitializeCriticalSection(&lock->cs);
#elif BIT_WIN_LIKE
    InitializeCriticalSectionAndSpinCount(&lock->cs, 5000);
#elif VXWORKS
    /* Removed SEM_INVERSION_SAFE */
    lock->cs = semMCreate(SEM_Q_PRIORITY | SEM_DELETE_SAFE);
    if (lock->cs == 0) {
        mprAssert(0);
        return 0;
    }
#endif
    return lock;
}


static void manageLock(MprMutex *lock, int flags)
{
    if (flags & MPR_MANAGE_FREE) {
        mprAssert(lock);
#if BIT_UNIX_LIKE
        pthread_mutex_destroy(&lock->cs);
#elif BIT_WIN_LIKE
        DeleteCriticalSection(&lock->cs);
        lock->cs.SpinCount = 0;
#elif VXWORKS
        semDelete(lock->cs);
#endif
    }
}


PUBLIC MprMutex *mprInitLock(MprMutex *lock)
{
#if BIT_UNIX_LIKE
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&lock->cs, &attr);
    pthread_mutexattr_destroy(&attr);

#elif WINCE
    InitializeCriticalSection(&lock->cs);

#elif BIT_WIN_LIKE
    InitializeCriticalSectionAndSpinCount(&lock->cs, 5000);

#elif VXWORKS
    /* Removed SEM_INVERSION_SAFE */
    lock->cs = semMCreate(SEM_Q_PRIORITY | SEM_DELETE_SAFE);
    if (lock->cs == 0) {
        mprAssert(0);
        return 0;
    }
#endif
    return lock;
}


/*
    Try to attain a lock. Do not block! Returns true if the lock was attained.
 */
PUBLIC bool mprTryLock(MprMutex *lock)
{
    int     rc;

    if (lock == 0) return 0;

#if BIT_UNIX_LIKE
    rc = pthread_mutex_trylock(&lock->cs) != 0;
#elif BIT_WIN_LIKE
    /* Rely on SpinCount being non-zero */
    if (lock->cs.SpinCount) {
        rc = TryEnterCriticalSection(&lock->cs) == 0;
    } else {
        rc = 0;
    }
#elif VXWORKS
    rc = semTake(lock->cs, NO_WAIT) != OK;
#endif
#if BIT_DEBUG
    lock->owner = mprGetCurrentOsThread();
#endif
    return (rc) ? 0 : 1;
}


PUBLIC MprSpin *mprCreateSpinLock()
{
    MprSpin    *lock;

    if ((lock = mprAllocObj(MprSpin, mprManageSpinLock)) == 0) {
        return 0;
    }
    return mprInitSpinLock(lock);
}


PUBLIC void mprManageSpinLock(MprSpin *lock, int flags)
{
    if (flags & MPR_MANAGE_FREE) {
        mprAssert(lock);
#if USE_MPR_LOCK || MACOSX
        ;
#elif BIT_UNIX_LIKE && BIT_HAS_SPINLOCK
        pthread_spin_destroy(&lock->cs);
#elif BIT_UNIX_LIKE
        pthread_mutex_destroy(&lock->cs);
#elif BIT_WIN_LIKE
        DeleteCriticalSection(&lock->cs);
        lock->cs.SpinCount = 0;
#elif VXWORKS
        semDelete(lock->cs);
#endif
    }
}


/*
    Static version just for mprAlloc which needs locks that don't allocate memory.
 */
PUBLIC MprSpin *mprInitSpinLock(MprSpin *lock)
{
#if BIT_UNIX_LIKE && !BIT_HAS_SPINLOCK && !MACOSX
    pthread_mutexattr_t attr;
#endif

#if USE_MPR_LOCK
    mprInitLock(&lock->cs);
#elif MACOSX
    lock->cs = OS_SPINLOCK_INIT;
#elif BIT_UNIX_LIKE && BIT_HAS_SPINLOCK
    pthread_spin_init(&lock->cs, 0);
#elif BIT_UNIX_LIKE
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
    pthread_mutex_init(&lock->cs, &attr);
    pthread_mutexattr_destroy(&attr);
#elif WINCE
    InitializeCriticalSection(&lock->cs);
#elif BIT_WIN_LIKE
    InitializeCriticalSectionAndSpinCount(&lock->cs, 5000);
#elif VXWORKS
    #if FUTURE
        spinLockTaskInit(&lock->cs, 0);
    #else
        /* Removed SEM_INVERSION_SAFE */
        lock->cs = semMCreate(SEM_Q_PRIORITY | SEM_DELETE_SAFE);
        if (lock->cs == 0) {
            mprAssert(0);
            return 0;
        }
    #endif
#endif /* VXWORKS */
#if BIT_DEBUG
    lock->owner = 0;
#endif
    return lock;
}


/*
    Try to attain a lock. Do not block! Returns true if the lock was attained.
 */
PUBLIC bool mprTrySpinLock(MprSpin *lock)
{
    int     rc;

    if (lock == 0) return 0;

#if USE_MPR_LOCK
    mprTryLock(&lock->cs);
#elif MACOSX
    rc = !OSSpinLockTry(&lock->cs);
#elif BIT_UNIX_LIKE && BIT_HAS_SPINLOCK
    rc = pthread_spin_trylock(&lock->cs) != 0;
#elif BIT_UNIX_LIKE
    rc = pthread_mutex_trylock(&lock->cs) != 0;
#elif BIT_WIN_LIKE
    /* Rely on SpinCount being non-zero */
    if (lock->cs.SpinCount) {
        rc = TryEnterCriticalSection(&lock->cs) == 0;
    } else {
        rc = 0;
    }
#elif VXWORKS
    rc = semTake(lock->cs, NO_WAIT) != OK;
#endif
#if BIT_DEBUG
    if (rc == 0) {
        mprAssert(lock->owner != mprGetCurrentOsThread());
        lock->owner = mprGetCurrentOsThread();
    }
#endif
    return (rc) ? 0 : 1;
}


/*
    Big global lock. Avoid using this.
 */
PUBLIC void mprGlobalLock()
{
    if (MPR && MPR->mutex) {
        mprLock(MPR->mutex);
    }
}


PUBLIC void mprGlobalUnlock()
{
    if (MPR && MPR->mutex) {
        mprUnlock(MPR->mutex);
    }
}


#if BIT_USE_LOCK_MACROS
/*
    Still define these even if using macros to make linking with *.def export files easier
 */
#undef mprLock
#undef mprUnlock
#undef mprSpinLock
#undef mprSpinUnlock
#endif

/*
    Lock a mutex
 */
PUBLIC void mprLock(MprMutex *lock)
{
    if (lock == 0) return;
#if BIT_UNIX_LIKE
    pthread_mutex_lock(&lock->cs);
#elif BIT_WIN_LIKE
    /* Rely on SpinCount being non-zero */
    if (lock->cs.SpinCount) {
        EnterCriticalSection(&lock->cs);
    }
#elif VXWORKS
    semTake(lock->cs, WAIT_FOREVER);
#endif
#if BIT_DEBUG
    /* Store last locker only */ 
    lock->owner = mprGetCurrentOsThread();
#endif
}


PUBLIC void mprUnlock(MprMutex *lock)
{
    if (lock == 0) return;
#if BIT_UNIX_LIKE
    pthread_mutex_unlock(&lock->cs);
#elif BIT_WIN_LIKE
    LeaveCriticalSection(&lock->cs);
#elif VXWORKS
    semGive(lock->cs);
#endif
}


/*
    Use functions for debug mode. Production release uses macros
 */
/*
    Lock a mutex
 */
PUBLIC void mprSpinLock(MprSpin *lock)
{
    if (lock == 0) return;

#if BIT_DEBUG
    /*
        Spin locks don't support recursive locking on all operating systems.
     */
    mprAssert(lock->owner != mprGetCurrentOsThread());
#endif

#if USE_MPR_LOCK
    mprLock(&lock->cs);
#elif MACOSX
    OSSpinLockLock(&lock->cs);
#elif BIT_UNIX_LIKE && BIT_HAS_SPINLOCK
    pthread_spin_lock(&lock->cs);
#elif BIT_UNIX_LIKE
    pthread_mutex_lock(&lock->cs);
#elif BIT_WIN_LIKE
    if (lock->cs.SpinCount) {
        EnterCriticalSection(&lock->cs);
    }
#elif VXWORKS
    semTake(lock->cs, WAIT_FOREVER);
#endif
#if BIT_DEBUG
    mprAssert(lock->owner != mprGetCurrentOsThread());
    lock->owner = mprGetCurrentOsThread();
#endif
}


PUBLIC void mprSpinUnlock(MprSpin *lock)
{
    if (lock == 0) return;

#if BIT_DEBUG
    lock->owner = 0;
#endif

#if USE_MPR_LOCK
    mprUnlock(&lock->cs);
#elif MACOSX
    OSSpinLockUnlock(&lock->cs);
#elif BIT_UNIX_LIKE && BIT_HAS_SPINLOCK
    pthread_spin_unlock(&lock->cs);
#elif BIT_UNIX_LIKE
    pthread_mutex_unlock(&lock->cs);
#elif BIT_WIN_LIKE
    LeaveCriticalSection(&lock->cs);
#elif VXWORKS
    semGive(lock->cs);
#endif
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprLog.c"
 */
/************************************************************************/

/**
    mprLog.c - Multithreaded Portable Runtime (MPR) Logging and error reporting.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/****************************** Forward Declarations **************************/

static void defaultLogHandler(int flags, int level, cchar *msg);
static void logOutput(int flags, int level, cchar *msg);

/************************************ Code ************************************/
/*
    Put first in file so it is easy to locate in a debugger
 */
PUBLIC void mprBreakpoint()
{
#if DEBUG_PAUSE
    {
        static int  paused = 1;
        int         i;
        printf("Paused to permit debugger to attach - will awake in 2 minutes\n");
        fflush(stdout);
        for (i = 0; i < 120 && paused; i++) {
            mprNap(1000);
        }
    }
#endif
}


PUBLIC void mprCreateLogService() 
{
    MPR->logFile = MPR->stdError;
}


PUBLIC int mprStartLogging(cchar *logSpec, int showConfig)
{
    MprFile     *file;
    MprPath     info;
    char        *levelSpec, *path;
    int         level, mode;

    level = -1;
    if (logSpec == 0) {
        logSpec = "stderr:0";
    }
    if (*logSpec && strcmp(logSpec, "none") != 0) {
        MPR->logPath = path = sclone(logSpec);
        if ((levelSpec = strrchr(path, ':')) != 0 && isdigit((uchar) levelSpec[1])) {
            *levelSpec++ = '\0';
            level = atoi(levelSpec);
        }
        if (strcmp(path, "stdout") == 0) {
            file = MPR->stdOutput;
        } else if (strcmp(path, "stderr") == 0) {
            file = MPR->stdError;
        } else {
            mode = (MPR->flags & MPR_LOG_APPEND)  ? O_APPEND : O_TRUNC;
            mode |= O_CREAT | O_WRONLY | O_TEXT;
            if (MPR->logBackup > 0) {
                mprGetPathInfo(path, &info);
                if (MPR->logSize <= 0 || (info.valid && info.size > MPR->logSize) || (MPR->flags & MPR_LOG_ANEW)) {
                    mprBackupLog(path, MPR->logBackup);
                }
            }
            if ((file = mprOpenFile(path, mode, 0664)) == 0) {
                mprError("Can't open log file %s", path);
                return -1;
            }
        }
        if (level >= 0) {
            mprSetLogLevel(level);
        }
        mprSetLogFile(file);

        if (showConfig) {
            mprLogHeader();
        }
    }
    return 0;
}


PUBLIC void mprLogHeader()
{
    mprLog(MPR_CONFIG, "Configuration for %s", mprGetAppTitle());
    mprLog(MPR_CONFIG, "---------------------------------------------");
    mprLog(MPR_CONFIG, "Version:            %s-%s", BIT_VERSION, BIT_BUILD_NUMBER);
    mprLog(MPR_CONFIG, "BuildType:          %s", BIT_DEBUG ? "Debug" : "Release");
    mprLog(MPR_CONFIG, "CPU:                %s", BIT_CPU);
    mprLog(MPR_CONFIG, "OS:                 %s", BIT_OS);
    mprLog(MPR_CONFIG, "Host:               %s", mprGetHostName());
    mprLog(MPR_CONFIG, "Directory:          %s", mprGetCurrentPath());
    mprLog(MPR_CONFIG, "Configure:          %s", BIT_CONFIG_CMD);
    mprLog(MPR_CONFIG, "---------------------------------------------");
}


PUBLIC int mprBackupLog(cchar *path, int count)
{
    char    *from, *to;
    int     i;

    for (i = count - 1; i > 0; i--) {
        from = sfmt("%s.%d", path, i - 1);
        to = sfmt("%s.%d", path, i);
        unlink(to);
        rename(from, to);
    }
    from = sfmt("%s", path);
    to = sfmt("%s.0", path);
    unlink(to);
    if (rename(from, to) < 0) {
        return MPR_ERR_CANT_CREATE;
    }
    return 0;
}


PUBLIC void mprSetLogBackup(ssize size, int backup, int flags)
{
    MPR->logBackup = backup;
    MPR->logSize = size;
    MPR->flags |= (flags & (MPR_LOG_APPEND | MPR_LOG_ANEW));
}


PUBLIC void mprLog(int level, cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    if (level > mprGetLogLevel()) {
        return;
    }
    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
    logOutput(MPR_LOG_SRC, level, buf);
}


/*
    RawLog will call alloc. 
 */
PUBLIC void mprRawLog(int level, cchar *fmt, ...)
{
    va_list     args;
    char        *buf;

    if (level > mprGetLogLevel()) {
        return;
    }
    va_start(args, fmt);
    buf = sfmtv(fmt, args);
    va_end(args);

    logOutput(MPR_RAW, 0, buf);
}


PUBLIC void mprError(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
    logOutput(MPR_ERROR_MSG | MPR_ERROR_SRC, 0, buf);
    mprBreakpoint();
}


PUBLIC void mprWarn(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
    logOutput(MPR_ERROR_MSG | MPR_WARN_SRC, 0, buf);
    mprBreakpoint();
}


PUBLIC void mprMemoryError(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    if (fmt == 0) {
        logOutput(MPR_ERROR_MSG | MPR_ERROR_SRC, 0, "Memory allocation error");
    } else {
        va_start(args, fmt);
        fmtv(buf, sizeof(buf), fmt, args);
        va_end(args);
        logOutput(MPR_ERROR_MSG | MPR_ERROR_SRC, 0, buf);
    }
}


PUBLIC void mprUserError(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
    logOutput(MPR_USER_MSG | MPR_ERROR_SRC, 0, buf);
}


PUBLIC void mprFatalError(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
    logOutput(MPR_USER_MSG | MPR_FATAL_SRC, 0, buf);
    exit(2);
}


/*
    Handle an error without allocating memory. Bypasses the logging mechanism.
 */
PUBLIC void mprStaticError(cchar *fmt, ...)
{
    va_list     args;
    char        buf[MPR_MAX_LOG];

    va_start(args, fmt);
    fmtv(buf, sizeof(buf), fmt, args);
    va_end(args);
#if BIT_UNIX_LIKE || VXWORKS
    if (write(2, (char*) buf, slen(buf)) < 0) {}
    if (write(2, (char*) "\n", 1) < 0) {}
#elif BIT_WIN_LIKE
    if (fprintf(stderr, "%s\n", buf) < 0) {}
    fflush(stderr);
#endif
    mprBreakpoint();
}


PUBLIC void mprAssureError(cchar *loc, cchar *msg)
{
#if BIT_ASSERT
    char    buf[MPR_MAX_LOG];

    if (loc) {
#if BIT_UNIX_LIKE
        snprintf(buf, sizeof(buf), "Assertion %s, failed at %s", msg, loc);
#else
        sprintf(buf, "Assertion %s, failed at %s", msg, loc);
#endif
        msg = buf;
    }
    mprLog(0, "%s", buf);
    mprBreakpoint();
#endif
}


/*
    Output a log message to the log handler
 */
static void logOutput(int flags, int level, cchar *msg)
{
    MprLogHandler   handler;

    handler = MPR->logHandler;
    if (handler != 0) {
        (handler)(flags, level, msg);
        return;
    }
    defaultLogHandler(flags, level, msg);
}


static void defaultLogHandler(int flags, int level, cchar *msg)
{
    MprFile     *file;
    MprPath     info;
    char        *prefix, buf[MPR_MAX_LOG];
    int         mode;

    lock(MPR);
    if ((file = MPR->logFile) == 0) {
        unlock(MPR);
        return;
    }
    prefix = MPR->name;

    if (MPR->logBackup > 0 && MPR->logSize) {
        //  OPT - slow. Should not check every time
        mprGetPathInfo(MPR->logPath, &info);
        if (info.valid && info.size > MPR->logSize) {
            mprSetLogFile(0);
            mprBackupLog(MPR->logPath, MPR->logBackup);
            mode = O_CREAT | O_WRONLY | O_TEXT;
            if ((file = mprOpenFile(MPR->logPath, mode, 0664)) == 0) {
                mprError("Can't open log file %s", MPR->logPath);
                unlock(MPR);
                return;
            }
            mprSetLogFile(file);
        }
    }
    while (*msg == '\n') {
        mprWriteFile(file, "\n", 1);
        msg++;
    }
    if (flags & MPR_LOG_SRC) {
        fmt(buf, sizeof(buf), "%s: %d: %s\n", prefix, level, msg);
        mprWriteFileString(file, buf);

    } else if (flags & (MPR_WARN_SRC | MPR_ERROR_SRC)) {
        if (flags & MPR_WARN_SRC) {
            fmt(buf, sizeof(buf), "%s: Warning: %s\n", prefix, msg);
        } else {
            fmt(buf, sizeof(buf), "%s: Error: %s\n", prefix, msg);
        }
#if BIT_WIN_LIKE || BIT_UNIX_LIKE
        mprWriteToOsLog(buf, flags, level);
#endif
        fmt(buf, sizeof(buf), "%s: Error: %s\n", prefix, msg);
        mprWriteFileString(file, buf);

    } else if (flags & MPR_FATAL_SRC) {
        fmt(buf, sizeof(buf), "%s: Fatal: %s\n", prefix, msg);
        mprWriteToOsLog(buf, flags, level);
        mprWriteFileString(file, buf);
        
    } else if (flags & MPR_RAW) {
        mprWriteFileString(file, msg);
    }
    unlock(MPR);
}


/*
    Return the raw O/S error code
 */
PUBLIC int mprGetOsError()
{
#if BIT_WIN_LIKE
    int     rc;
    rc = GetLastError();

    /*
        Client has closed the pipe
     */
    if (rc == ERROR_NO_DATA) {
        return EPIPE;
    }
    return rc;
#elif BIT_UNIX_LIKE || VXWORKS
    return errno;
#else
    return 0;
#endif
}


/*
    Return the mapped (portable, Posix) error code
 */
PUBLIC int mprGetError()
{
#if !BIT_WIN_LIKE
    return mprGetOsError();
#else
    int     err;

    err = mprGetOsError();

    switch (err) {
    case ERROR_SUCCESS:
        return 0;
    case ERROR_FILE_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
        return EPERM;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
        return ENOMEM;
    case ERROR_PATH_BUSY:
    case ERROR_BUSY_DRIVE:
    case ERROR_NETWORK_BUSY:
    case ERROR_PIPE_BUSY:
    case ERROR_BUSY:
        return EBUSY;
    case ERROR_FILE_EXISTS:
        return EEXIST;
    case ERROR_BAD_PATHNAME:
    case ERROR_BAD_ARGUMENTS:
        return EINVAL;
    case WSAENOTSOCK:
        return ENOENT;
    case WSAEINTR:
        return EINTR;
    case WSAEBADF:
        return EBADF;
    case WSAEACCES:
        return EACCES;
    case WSAEINPROGRESS:
        return EINPROGRESS;
    case WSAEALREADY:
        return EALREADY;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAENETDOWN:
        return ENETDOWN;
    case WSAENETUNREACH:
        return ENETUNREACH;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAEWOULDBLOCK:
        return EAGAIN;
    }
    return MPR_ERR;
#endif
}


PUBLIC int mprGetLogLevel()
{
    Mpr     *mpr;

    /* Leave the code like this so debuggers can patch logLevel before returning */
    mpr = MPR;
    return mpr->logLevel;
}


PUBLIC MprLogHandler mprGetLogHandler()
{
    return MPR->logHandler;
}


PUBLIC int mprUsingDefaultLogHandler()
{
    return MPR->logHandler == defaultLogHandler;
}


PUBLIC MprFile *mprGetLogFile()
{
    return MPR->logFile;
}


PUBLIC void mprSetLogHandler(MprLogHandler handler)
{
    MPR->logHandler = handler;
}


PUBLIC void mprSetLogFile(MprFile *file)
{
    if (file != MPR->logFile && MPR->logFile != MPR->stdOutput && MPR->logFile != MPR->stdError) {
        mprCloseFile(MPR->logFile);
    }
    MPR->logFile = file;
}


PUBLIC void mprSetLogLevel(int level)
{
    MPR->logLevel = level;
}


PUBLIC bool mprSetCmdlineLogging(bool on)
{
    bool    wasLogging;

    wasLogging = MPR->cmdlineLogging;
    MPR->cmdlineLogging = on;
    return wasLogging;
}


PUBLIC bool mprGetCmdlineLogging()
{
    return MPR->cmdlineLogging;
}


#if MACOSX
/*
    Just for conditional breakpoints when debugging in Xcode
 */
PUBLIC int _cmp(char *s1, char *s2)
{
    return !strcmp(s1, s2);
}
#endif

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprMime.c"
 */
/************************************************************************/

/* 
    mprMime.c - Mime type handling

    Copyright (c) All Rights Reserved. See copyright notice at the bottom of the file.
 */

/********************************* Includes ***********************************/



/*********************************** Code *************************************/
/*  
    Inbuilt mime type support
 */
static char *standardMimeTypes[] = {
    "ai",    "application/postscript",
    "asc",   "text/plain",
    "au",    "audio/basic",
    "avi",   "video/x-msvideo",
    "bin",   "application/octet-stream",
    "bmp",   "image/bmp",
    "class", "application/octet-stream",
    "css",   "text/css",
    "deb",   "application/octet-stream",
    "dll",   "application/octet-stream",
    "dmg",   "application/octet-stream",
    "doc",   "application/msword",
    "eps",   "application/postscript",
    "es",    "application/x-javascript",
    "exe",   "application/octet-stream",
    "gif",   "image/gif",
    "gz",    "application/x-gzip",
    "htm",   "text/html",
    "html",  "text/html",
    "ico",   "image/x-icon",
    "jar",   "application/octet-stream",
    "jpeg",  "image/jpeg",
    "jpg",   "image/jpeg",
    "js",    "application/javascript",
    "json",  "application/json",
    "mp3",   "audio/mpeg",
    "mpg",   "video/mpeg",
    "mpeg",  "video/mpeg",
    "pdf",   "application/pdf",
    "php",   "application/x-php",
    "pl",    "application/x-perl",
    "png",   "image/png",
    "ppt",   "application/vnd.ms-powerpoint",
    "ps",    "application/postscript",
    "py",    "application/x-python",
    "py",    "application/x-python",
    "ra",    "audio/x-realaudio",
    "ram",   "audio/x-pn-realaudio",
    "rmm",   "audio/x-pn-realaudio",
    "rtf",   "text/rtf",
    "rv",    "video/vnd.rn-realvideo",
    "so",    "application/octet-stream",
    "swf",   "application/x-shockwave-flash",
    "tar",   "application/x-tar",
    "tgz",   "application/x-gzip",
    "tiff",  "image/tiff",
    "txt",   "text/plain",
    "wav",   "audio/x-wav",
    "xls",   "application/vnd.ms-excel",
    "zip",   "application/zip",
    0,       0,
};

/********************************** Forward ***********************************/

static void addStandardMimeTypes(MprHash *table);
static void manageMimeType(MprMime *mt, int flags);

/*********************************** Code *************************************/

PUBLIC MprHash *mprCreateMimeTypes(cchar *path)
{
    MprHash     *table;
    MprFile     *file;
    char        *buf, *tok, *ext, *type;
    int         line;

    if (path) {
        if ((file = mprOpenFile(path, O_RDONLY | O_TEXT, 0)) == 0) {
            return 0;
        }
        if ((table = mprCreateHash(MPR_DEFAULT_HASH_SIZE, 0)) == 0) {
            mprCloseFile(file);
            return 0;
        }
        line = 0;
        while ((buf = mprReadLine(file, 0, NULL)) != 0) {
            line++;
            if (buf[0] == '#' || isspace((uchar) buf[0])) {
                continue;
            }
            type = stok(buf, " \t\n\r", &tok);
            ext = stok(0, " \t\n\r", &tok);
            if (type == 0 || ext == 0) {
                mprError("Bad mime type in %s at line %d", path, line);
                continue;
            }
            while (ext) {
                mprAddMime(table, ext, type);
                ext = stok(0, " \t\n\r", &tok);
            }
        }
        mprCloseFile(file);

    } else {
        if ((table = mprCreateHash(59, 0)) == 0) {
            return 0;
        }
        addStandardMimeTypes(table);
    }
    return table;
}


static void addStandardMimeTypes(MprHash *table)
{
    char    **cp;

    for (cp = standardMimeTypes; cp[0]; cp += 2) {
        mprAddMime(table, cp[0], cp[1]);
    }
}


static void manageMimeType(MprMime *mt, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mt->type);
        mprMark(mt->program);
    }
}


PUBLIC MprMime *mprAddMime(MprHash *table, cchar *ext, cchar *mimeType)
{
    MprMime  *mt;

    if ((mt = mprAllocObj(MprMime, manageMimeType)) == 0) {
        return 0;
    }
    mt->type = sclone(mimeType);
    if (*ext == '.') {
        ext++;
    }
    mprAddKey(table, ext, mt);
    return mt;
}


PUBLIC int mprSetMimeProgram(MprHash *table, cchar *mimeType, cchar *program)
{
    MprKey      *kp;
    MprMime     *mt;
    
    kp = 0;
    mt = 0;
    while ((kp = mprGetNextKey(table, kp)) != 0) {
        mt = (MprMime*) kp->data;
        if (mt->type[0] == mimeType[0] && strcmp(mt->type, mimeType) == 0) {
            break;
        }
    }
    if (mt == 0) {
        mprError("Can't find mime type %s for action program %s", mimeType, program);
        return MPR_ERR_CANT_FIND;
    }
    mt->program = sclone(program);
    return 0;
}


PUBLIC cchar *mprGetMimeProgram(MprHash *table, cchar *mimeType)
{
    MprMime      *mt;

    if (mimeType == 0 || *mimeType == '\0') {
        return 0;
    }
    if ((mt = mprLookupKey(table, mimeType)) == 0) {
        return 0;
    }
    return mt->program;
}


PUBLIC cchar *mprLookupMime(MprHash *table, cchar *ext)
{
    MprMime     *mt;
    cchar       *ep;

    if (ext == 0 || *ext == '\0') {
        return "";
    }
    if ((ep = strrchr(ext, '.')) != 0) {
        ext = &ep[1];
    }
    if (table == 0) {
        table = MPR->mimeTypes;
    }
    if ((mt = mprLookupKey(table, ext)) == 0) {
        return 0;
    }
    return mt->type;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprMixed.c"
 */
/************************************************************************/

/**
    mprMixed.c - Mixed mode strings. Unicode results with ascii args.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if BIT_CHAR_LEN > 1 && UNUSED && KEEP
/********************************** Forwards **********************************/

PUBLIC int mcaselesscmp(wchar *str1, cchar *str2)
{
    return mncaselesscmp(str1, str2, -1);
}


PUBLIC int mcmp(wchar *s1, cchar *s2)
{
    return mncmp(s1, s2, -1);
}


PUBLIC wchar *mncontains(wchar *str, cchar *pattern, ssize limit)
{
    wchar   *cp, *s1;
    cchar   *s2;
    ssize   lim;

    mprAssert(0 <= limit && limit < MAXSSIZE);

    if (limit < 0) {
        limit = MAXINT;
    }
    if (str == 0) {
        return 0;
    }
    if (pattern == 0 || *pattern == '\0') {
        return (wchar*) str;
    }
    for (cp = str; *cp && limit > 0; cp++, limit--) {
        s1 = cp;
        s2 = pattern;
        for (lim = limit; *s1 && *s2 && (*s1 == (uchar) *s2) && lim > 0; lim--) {
            s1++;
            s2++;
        }
        if (*s2 == '\0') {
            return cp;
        }
    }
    return 0;
}


PUBLIC wchar *mcontains(wchar *str, cchar *pattern)
{
    return mncontains(str, pattern, -1);
}


/*
    destMax and len are character counts, not sizes in bytes
 */
PUBLIC ssize mcopy(wchar *dest, ssize destMax, cchar *src)
{
    ssize       len;

    mprAssert(src);
    mprAssert(dest);
    mprAssert(0 < destMax && destMax < MAXINT);

    len = slen(src);
    if (destMax <= len) {
        mprAssert(!MPR_ERR_WONT_FIT);
        return MPR_ERR_WONT_FIT;
    }
    return mtow(dest, len + 1, src, len);
}


PUBLIC int mends(wchar *str, cchar *suffix)
{
    wchar   *cp;
    cchar   *sp;

    if (str == NULL || suffix == NULL) {
        return 0;
    }
    cp = &str[wlen(str) - 1];
    sp = &suffix[slen(suffix)];
    for (; cp > str && sp > suffix; ) {
        if (*cp-- != *sp--) {
            return 0;
        }
    }
    if (sp > suffix) {
        return 0;
    }
    return 1;
}


PUBLIC wchar *mfmt(cchar *fmt, ...)
{
    va_list     ap;
    char        *mresult;

    mprAssert(fmt);

    va_start(ap, fmt);
    mresult = sfmtv(fmt, ap);
    va_end(ap);
    return amtow(mresult, NULL);
}


PUBLIC wchar *mfmtv(cchar *fmt, va_list arg)
{
    char    *mresult;

    mprAssert(fmt);
    mresult = sfmtv(fmt, arg);
    return amtow(mresult, NULL);
}


/*
    Sep is ascii, args are wchar
 */
PUBLIC wchar *mjoin(wchar *str, ...)
{
    wchar       *result;
    va_list     ap;

    mprAssert(str);

    va_start(ap, str);
    result = mjoinv(str, ap);
    va_end(ap);
    return result;
}


/*
    MOB - comment required. What does this do?
 */
PUBLIC wchar *mjoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp;
    int         required, len;

    mprAssert(buf);

    va_copy(ap, args);
    required = 1;
    if (buf) {
        required += wlen(buf);
    }
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprAlloc(required)) == 0) {
        return 0;
    }
    dp = dest;
    if (buf) {
        wcopy(dp, -1, buf);
        dp += wlen(buf);
    }
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        len = wlen(str);
        dp += len;
        required -= len;
        str = va_arg(ap, wchar*);
    }
    *dp = '\0';
    return dest;
}


/*
    Case insensitive string comparison. Limited by length
 */
PUBLIC int mncaselesscmp(wchar *s1, cchar *s2, ssize n)
{
    int     rc;

    mprAssert(0 <= n && n < MAXSSIZE);

    if (s1 == 0 || s2 == 0) {
        return -1;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = tolower(*s1) - tolower(*s2);
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}



PUBLIC int mncmp(wchar *s1, cchar *s2, ssize n)
{
    mprAssert(0 <= n && n < MAXSSIZE);

    if (s1 == 0 && s2 == 0) {
        return 0;
    } else if (s1 == 0) {
        return -1;
    } else if (s2 == 0) {
        return 1;
    }
    for (rc = 0; n > 0 && *s1 && rc == 0; s1++, s2++, n--) {
        rc = *s1 - (uchar) *s2;
    }
    if (rc) {
        return (rc > 0) ? 1 : -1;
    } else if (n == 0) {
        return 0;
    } else if (*s1 == '\0' && *s2 == '\0') {
        return 0;
    } else if (*s1 == '\0') {
        return -1;
    } else if (*s2 == '\0') {
        return 1;
    }
    return 0;
}


PUBLIC ssize mncopy(wchar *dest, ssize destMax, cchar *src, ssize len)
{
    mprAssert(0 <= len && len < MAXSSIZE);
    mprAssert(0 < destMax && destMax < MAXSSIZE);

    return mtow(dest, destMax, src, len);
}


PUBLIC wchar *mpbrk(wchar *str, cchar *set)
{
    cchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return str;
            }
        }
    }
    return 0;
}


/*
    Sep is ascii, args are wchar
 */
PUBLIC wchar *mrejoin(wchar *buf, ...)
{
    va_list     ap;
    wchar       *result;

    va_start(ap, buf);
    result = mrejoinv(buf, ap);
    va_end(ap);
    return result;
}


PUBLIC wchar *mrejoinv(wchar *buf, va_list args)
{
    va_list     ap;
    wchar       *dest, *str, *dp;
    int         required, len;

    va_copy(ap, args);
    required = 1;
    if (buf) {
        required += wlen(buf);
    }
    str = va_arg(ap, wchar*);
    while (str) {
        required += wlen(str);
        str = va_arg(ap, wchar*);
    }
    if ((dest = mprRealloc(buf, required)) == 0) {
        return 0;
    }
    dp = dest;
    va_copy(ap, args);
    str = va_arg(ap, wchar*);
    while (str) {
        wcopy(dp, required, str);
        len = wlen(str);
        dp += len;
        required -= len;
        str = va_arg(ap, wchar*);
    }
    *dp = '\0';
    return dest;
}


PUBLIC ssize mspn(wchar *str, cchar *set)
{
    cchar   *sp;
    int     count;

    if (str == NULL || set == NULL) {
        return 0;
    }
    for (count = 0; *str; count++, str++) {
        for (sp = set; *sp; sp++) {
            if (*str == *sp) {
                return break;
            }
        }
        if (*str != *sp) {
            break;
        }
    }
    return count;
}
 

PUBLIC int mstarts(wchar *str, cchar *prefix)
{
    if (str == NULL || prefix == NULL) {
        return 0;
    }
    if (mncmp(str, prefix, slen(prefix)) == 0) {
        return 1;
    }
    return 0;
}


PUBLIC wchar *mtok(wchar *str, cchar *delim, wchar **last)
{
    wchar   *start, *end;
    ssize   i;

    start = str ? str : *last;

    if (start == 0) {
        *last = 0;
        return 0;
    }
    i = mspn(start, delim);
    start += i;
    if (*start == '\0') {
        *last = 0;
        return 0;
    }
    end = mpbrk(start, delim);
    if (end) {
        *end++ = '\0';
        i = mspn(end, delim);
        end += i;
    }
    *last = end;
    return start;
}


PUBLIC wchar *mtrim(wchar *str, cchar *set, int where)
{
    wchar   s;
    ssize   len, i;

    if (str == NULL || set == NULL) {
        return str;
    }
    s = wclone(str);
    if (where & MPR_TRIM_START) {
        i = mspn(s, set);
    } else {
        i = 0;
    }
    s += i;
    if (where & MPR_TRIM_END) {
        len = wlen(s);
        while (len > 0 && mspn(&s[len - 1], set) > 0) {
            s[len - 1] = '\0';
            len--;
        }
    }
    return s;
}

#else
PUBLIC void dummyWide() {}
#endif /* BIT_CHAR_LEN > 1 */

/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprModule.c"
 */
/************************************************************************/

/**
    mprModule.c - Dynamic module loading support.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



/********************************** Forwards **********************************/

static void manageModule(MprModule *mp, int flags);
static void manageModuleService(MprModuleService *ms, int flags);

/************************************* Code ***********************************/
/*
    Open the module service
 */
PUBLIC MprModuleService *mprCreateModuleService()
{
    MprModuleService    *ms;

    if ((ms = mprAllocObj(MprModuleService, manageModuleService)) == 0) {
        return 0;
    }
    ms->modules = mprCreateList(-1, 0);
    ms->mutex = mprCreateLock();
    MPR->moduleService = ms;
    mprSetModuleSearchPath(NULL);
    return ms;
}


static void manageModuleService(MprModuleService *ms, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(ms->modules);
        mprMark(ms->searchPath);
        mprMark(ms->mutex);
    }
}


/*
    Call the start routine for each module
 */
PUBLIC int mprStartModuleService()
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    ms = MPR->moduleService;
    mprAssert(ms);

    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        if (mprStartModule(mp) < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
#if VXWORKS && BIT_DEBUG && SYM_SYNC_INCLUDED
    symSyncLibInit();
#endif
    return 0;
}


PUBLIC void mprStopModuleService()
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    ms = MPR->moduleService;
    mprAssert(ms);
    mprLock(ms->mutex);
    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        mprStopModule(mp);
    }
    mprUnlock(ms->mutex);
}


PUBLIC MprModule *mprCreateModule(cchar *name, cchar *path, cchar *entry, void *data)
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 index;

    ms = MPR->moduleService;
    mprAssert(ms);

    if ((mp = mprAllocObj(MprModule, manageModule)) == 0) {
        return 0;
    }
    mp->name = sclone(name);
    mp->path = sclone(path);
    if (entry && *entry) {
        mp->entry = sclone(entry);
    }
    mp->moduleData = data;
    mp->lastActivity = mprGetTime();
    index = mprAddItem(ms->modules, mp);
    if (index < 0 || mp->name == 0) {
        return 0;
    }
    return mp;
}


static void manageModule(MprModule *mp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(mp->entry);
        mprMark(mp->name);
        mprMark(mp->path);
        mprMark(mp->moduleData);
    }
}


PUBLIC int mprStartModule(MprModule *mp)
{
    mprAssert(mp);

    if (mp->start && !(mp->flags & MPR_MODULE_STARTED)) {
        if (mp->start(mp) < 0) {
            return MPR_ERR_CANT_INITIALIZE;
        }
    }
    mp->flags |= MPR_MODULE_STARTED;
    return 0;
}


PUBLIC int mprStopModule(MprModule *mp)
{
    mprAssert(mp);

    if (mp->stop && (mp->flags & MPR_MODULE_STARTED) && !(mp->flags & MPR_MODULE_STOPPED)) {
        if (mp->stop(mp) < 0) {
            return MPR_ERR_NOT_READY;
        }
        mp->flags |= MPR_MODULE_STOPPED;
    }
    return 0;
}


/*
    See if a module is already loaded
 */
PUBLIC MprModule *mprLookupModule(cchar *name)
{
    MprModuleService    *ms;
    MprModule           *mp;
    int                 next;

    mprAssert(name && name);

    ms = MPR->moduleService;
    mprAssert(ms);

    for (next = 0; (mp = mprGetNextItem(ms->modules, &next)) != 0; ) {
        mprAssert(mp->name);
        if (mp && strcmp(mp->name, name) == 0) {
            return mp;
        }
    }
    return 0;
}


PUBLIC void *mprLookupModuleData(cchar *name)
{
    MprModule   *module;

    if ((module = mprLookupModule(name)) == NULL) {
        return NULL;
    }
    return module->moduleData;
}


PUBLIC void mprSetModuleTimeout(MprModule *module, MprTime timeout)
{
    assure(module);
    if (module) {
        module->timeout = timeout;
    }
}


PUBLIC void mprSetModuleFinalizer(MprModule *module, MprModuleProc stop)
{
    assure(module);
    if (module) {
        module->stop = stop;
    }
}


PUBLIC void mprSetModuleSearchPath(char *searchPath)
{
    MprModuleService    *ms;

    ms = MPR->moduleService;
    if (searchPath == 0) {
        ms->searchPath = sjoin(mprGetAppDir(), MPR_SEARCH_SEP, mprGetAppDir(), MPR_SEARCH_SEP, BIT_BIN_PREFIX, NULL);
    } else {
        ms->searchPath = sclone(searchPath);
    }
}


PUBLIC cchar *mprGetModuleSearchPath()
{
    return MPR->moduleService->searchPath;
}


/*
    Load a module. The module is located by searching for the filename by optionally using the module search path.
 */
PUBLIC int mprLoadModule(MprModule *mp)
{
#if BIT_HAS_DYN_LOAD
    mprAssert(mp);

    if (mprLoadNativeModule(mp) < 0) {
        return MPR_ERR_CANT_READ;
    }
    mprStartModule(mp);
    return 0;
#else
    mprError("Product built without the ability to load modules dynamically");
    return MPR_ERR_BAD_STATE;
#endif
}


PUBLIC int mprUnloadModule(MprModule *mp)
{
    mprLog(6, "Unloading native module %s from %s", mp->name, mp->path);
    if (mprStopModule(mp) < 0) {
        return MPR_ERR_NOT_READY;
    }
#if BIT_HAS_DYN_LOAD
    if (mp->handle) {
        if (mprUnloadNativeModule(mp) != 0) {
            mprError("Can't unload module %s", mp->name);
        }
        mp->handle = 0;
    }
#endif
    mprRemoveItem(MPR->moduleService->modules, mp);
    return 0;
}


#if BIT_HAS_DYN_LOAD
/*
    Return true if the shared library in "file" can be found. Return the actual path in *path. The filename
    may not have a shared library extension which is typical so calling code can be cross platform.
 */
static char *probe(cchar *filename)
{
    char    *path;

    mprAssert(filename && *filename);

    mprLog(7, "Probe for native module %s", filename);
    if (mprPathExists(filename, R_OK)) {
        return sclone(filename);
    }

    if (strstr(filename, BIT_SHOBJ) == 0) {
        path = sjoin(filename, BIT_SHOBJ, NULL);
        mprLog(7, "Probe for native module %s", path);
        if (mprPathExists(path, R_OK)) {
            return path;
        }
    }
    return 0;
}
#endif


/*
    Search for a module "filename" in the modulePath. Return the result in "result"
 */
PUBLIC char *mprSearchForModule(cchar *filename)
{
#if BIT_HAS_DYN_LOAD
    char    *path, *f, *searchPath, *dir, *tok;

    filename = mprNormalizePath(filename);

    /*
        Search for the path directly
     */
    if ((path = probe(filename)) != 0) {
        mprLog(6, "Found native module %s at %s", filename, path);
        return path;
    }

    /*
        Search in the searchPath
     */
    searchPath = sclone(mprGetModuleSearchPath());
    tok = 0;
    dir = stok(searchPath, MPR_SEARCH_SEP, &tok);
    while (dir && *dir) {
        f = mprJoinPath(dir, filename);
        if ((path = probe(f)) != 0) {
            mprLog(6, "Found native module %s at %s", filename, path);
            return path;
        }
        dir = stok(0, MPR_SEARCH_SEP, &tok);
    }
#endif /* BIT_HAS_DYN_LOAD */
    return 0;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprPath.c"
 */
/************************************************************************/

/**
    mprPath.c - Path (filename) services.

    This modules provides cross platform path name services.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************** Includes **********************************/



/********************************** Defines ***********************************/
/*
    Find the first separator in the path
 */
#if BIT_UNIX_LIKE
    #define firstSep(fs, path)  strchr(path, fs->separators[0])
#else
    #define firstSep(fs, path)  strpbrk(path, fs->separators)
#endif

#define defaultSep(fs)          (fs->separators[0])

/************************************* Code ***********************************/

static MPR_INLINE bool isSep(MprFileSystem *fs, int c) 
{
    char    *separators;

    mprAssert(fs);
    for (separators = fs->separators; *separators; separators++) {
        if (*separators == c)
            return 1;
    }
    return 0;
}


static MPR_INLINE bool hasDrive(MprFileSystem *fs, cchar *path) 
{
    char    *cp, *endDrive;

    mprAssert(fs);
    mprAssert(path);

    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && (cp == NULL || endDrive < cp)) {
            return 1;
        }
    }
    return 0;
}


/*
    Return true if the path is absolute.
    This means the path portion after an optional drive specifier must begin with a directory speparator charcter.
    Cygwin returns true for "/abc" and "C:/abc".
 */
static MPR_INLINE bool isAbsPath(MprFileSystem *fs, cchar *path) 
{
    char    *cp, *endDrive;

    mprAssert(fs);
    mprAssert(path);

    if (path == NULL || *path == '\0') {
        return 0;
    }
    if (fs->hasDriveSpecs) {
        if ((cp = firstSep(fs, path)) != 0) {
            if ((endDrive = strchr(path, ':')) != 0) {
                if (&endDrive[1] == cp) {
                    return 1;
                }
            }
            if (cp == path) {
                return 1;
            }
        }
    } else {
        if (isSep(fs, path[0])) {
            return 1;
        }
    }
    return 0;
}


/*
    Return true if the path is a fully qualified absolute path.
    On windows, this means it must have a drive specifier.
    On cygwin, this means it must not have a drive specifier.
 */
static MPR_INLINE bool isFullPath(MprFileSystem *fs, cchar *path) 
{
    mprAssert(fs);
    mprAssert(path);

#if (BIT_WIN_LIKE || VXWORKS) && !WINCE
{
    char    *cp, *endDrive;

    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && cp && &endDrive[1] == cp) {
            return 1;
        }
        return 0;
    }
}
#endif
    if (isSep(fs, path[0])) {
        return 1;
    }
    return 0;
}


/*
    Return true if the directory is the root directory on a file system
 */
static MPR_INLINE bool isRoot(MprFileSystem *fs, cchar *path) 
{
    char    *cp;

    if (isAbsPath(fs, path)) {
        cp = firstSep(fs, path);
        if (cp && cp[1] == '\0') {
            return 1;
        }
    }
    return 0;
}


static MPR_INLINE char *lastSep(MprFileSystem *fs, cchar *path) 
{
    char    *cp;

    for (cp = (char*) &path[slen(path)] - 1; cp >= path; cp--) {
        if (isSep(fs, *cp)) {
            return cp;
        }
    }
    return 0;
}

/************************************ Code ************************************/
/*
    This copies a file.
 */
PUBLIC int mprCopyPath(cchar *fromName, cchar *toName, int mode)
{
    MprFile     *from, *to;
    ssize       count;
    char        buf[MPR_BUFSIZE];

    if ((from = mprOpenFile(fromName, O_RDONLY | O_BINARY, 0)) == 0) {
        mprError("Can't open %s", fromName);
        return MPR_ERR_CANT_OPEN;
    }
    if ((to = mprOpenFile(toName, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, mode)) == 0) {
        mprError("Can't open %s", toName);
        return MPR_ERR_CANT_OPEN;
    }
    while ((count = mprReadFile(from, buf, sizeof(buf))) > 0) {
        mprWriteFile(to, buf, count);
    }
    mprCloseFile(from);
    mprCloseFile(to);
    return 0;
}


PUBLIC int mprDeletePath(cchar *path)
{
    MprFileSystem   *fs;

    if (path == NULL || *path == '\0') {
        return MPR_ERR_CANT_ACCESS;
    }
    fs = mprLookupFileSystem(path);
    return fs->deletePath(fs, path);
}


/*
    Return an absolute (normalized) path.
    On CYGWIN, this is a cygwin path with forward-slashes and without drive specs. 
    Use mprGetWinPath for a windows style path with a drive specifier and back-slashes.
 */
PUBLIC char *mprGetAbsPath(cchar *path)
{
    MprFileSystem   *fs;
    char            *result;

    if (path == 0 || *path == '\0') {
        path = ".";
    }
#if BIT_ROM
    return mprNormalizePath(path);
#elif CYGWIN
    {
        ssize   len;
        /*
            cygwin_conf_path has a bug for paths that attempt to address a directory above the root. ie. "../../../.."
            So must convert to a windows path first.
         */
        if (strncmp(path, "../", 3) == 0) {
            path = mprGetWinPath(path);
        }
        if ((len = cygwin_conv_path(CCP_WIN_A_TO_POSIX | CCP_ABSOLUTE, path, NULL, 0)) >= 0) {
            /* Len includes room for the null */
            if ((result = mprAlloc(len)) == 0) {
                return 0;
            }
            cygwin_conv_path(CCP_WIN_A_TO_POSIX | CCP_ABSOLUTE, path, result, len);
            if (len > 3 && result[len - 2] == '/' && result[len - 3] != ':') {
                /* Trim trailing "/" */
                result[len - 2] = '\0';
            }
            return result;
        }
    }
#endif
    fs = mprLookupFileSystem(path);
    if (isFullPath(fs, path)) {
        /* Already absolute. On windows, must contain a drive specifier */
        return mprNormalizePath(path);
    }

#if BIT_WIN_LIKE && !WINCE
{
    wchar    buf[MPR_MAX_PATH];
    GetFullPathName(wide(path), sizeof(buf) - 1, buf, NULL);
    buf[TSZ(buf) - 1] = '\0';
    result = mprNormalizePath(multi(buf));
}
#elif VXWORKS
{
    char    *dir;
    if (hasDrive(fs, path)) {
        dir = mprGetCurrentPath();
        result = mprJoinPath(dir, &strchr(path, ':')[1]);

    } else {
        if (isAbsPath(fs, path)) {
            /*
                Path is absolute, but without a drive. Use the current drive.
             */
            dir = mprGetCurrentPath();
            result = mprJoinPath(dir, path);
        } else {
            dir = mprGetCurrentPath();
            result = mprJoinPath(dir, path);
        }
    }
}
#else
{
    char   *dir;
    dir = mprGetCurrentPath();
    result = mprJoinPath(dir, path);
}
#endif
    return result;
}


/*
    Get the directory containing the application executable. Tries to return an absolute path.
 */
PUBLIC char *mprGetAppDir()
{ 
    if (MPR->appDir == 0) {
        MPR->appDir = mprGetPathDir(mprGetAppPath());
    }
    return sclone(MPR->appDir); 
} 


/*
    Get the path for the application executable. Tries to return an absolute path.
 */
PUBLIC char *mprGetAppPath()
{ 
    if (MPR->appPath) {
        return sclone(MPR->appPath);
    }

#if MACOSX
{
    char    path[MPR_MAX_PATH], pbuf[MPR_MAX_PATH];
    uint    size;
    ssize   len;

    size = sizeof(path) - 1;
    if (_NSGetExecutablePath(path, &size) < 0) {
        return mprGetAbsPath(".");
    }
    path[size] = '\0';
    len = readlink(path, pbuf, sizeof(pbuf) - 1);
    if (len < 0) {
        return mprGetAbsPath(path);
    }
    pbuf[len] = '\0';
    MPR->appPath = mprGetAbsPath(pbuf);
}
#elif FREEBSD 
{
    char    pbuf[MPR_MAX_STRING];
    int     len;

    len = readlink("/proc/curproc/file", pbuf, sizeof(pbuf) - 1);
    if (len < 0) {
        return mprGetAbsPath(".");
     }
     pbuf[len] = '\0';
     MPR->appPath = mprGetAbsPath(pbuf);
}
#elif BIT_UNIX_LIKE 
{
    char    pbuf[MPR_MAX_STRING], *path;
    int     len;
#if SOLARIS
    path = sfmt("/proc/%i/path/a.out", getpid()); 
#else
    path = sfmt("/proc/%i/exe", getpid()); 
#endif
    len = readlink(path, pbuf, sizeof(pbuf) - 1);
    if (len < 0) {
        return mprGetAbsPath(".");
    }
    pbuf[len] = '\0';
    MPR->appPath = mprGetAbsPath(pbuf);
}
#elif BIT_WIN_LIKE
{
    wchar    pbuf[MPR_MAX_PATH];

    if (GetModuleFileName(0, pbuf, sizeof(pbuf) - 1) <= 0) {
        return 0;
    }
    MPR->appPath = mprGetAbsPath(multi(pbuf));
}
#else
    if (mprIsPathAbs(MPR->argv[0])) {
        MPR->appPath = sclone(MPR->argv[0]);
    } else {
        MPR->appPath = mprGetCurrentPath();
    }
#endif
    return sclone(MPR->appPath);
}

 
/*
    This will return a fully qualified absolute path for the current working directory.
 */
PUBLIC char *mprGetCurrentPath()
{
    char    dir[MPR_MAX_PATH];

    if (getcwd(dir, sizeof(dir)) == 0) {
        return mprGetAbsPath("/");
    }

#if VXWORKS
{
    MprFileSystem   *fs;
    char            sep[2];

    fs = mprLookupFileSystem(dir);

    /*
        Vx will sometimes just return a drive with no path.
     */
    if (firstSep(fs, dir) == NULL) {
        sep[0] = defaultSep(fs);
        sep[1] = '\0';
        return sjoin(dir, sep, NULL);
    }
}
#elif BIT_WIN_LIKE || CYGWIN
{
    MprFileSystem   *fs;
    fs = mprLookupFileSystem(dir);
    mprMapSeparators(dir, fs->separators[0]);
}
#endif
    return sclone(dir);
}


PUBLIC cchar *mprGetFirstPathSeparator(cchar *path) 
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return firstSep(fs, path);
}


/*
    Return a pointer into the path at the last path separator or null if none found
 */
PUBLIC cchar *mprGetLastPathSeparator(cchar *path) 
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return lastSep(fs, path);
}


/*
    Return a path with native separators. This means "\\" on windows and cygwin
 */
PUBLIC char *mprGetNativePath(cchar *path)
{
    return mprTransformPath(path, MPR_PATH_NATIVE_SEP);
}


/*
    Return the last portion of a pathname. The separators are not mapped and the path is not cleaned.
 */
PUBLIC char *mprGetPathBase(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    if (path == 0) {
        return sclone("");
    }
    fs = mprLookupFileSystem(path);
    cp = (char*) lastSep(fs, path);
    if (cp == 0) {
        return sclone(path);
    } 
    if (cp == path) {
        if (cp[1] == '\0') {
            return sclone(path);
        }
    } else if (cp[1] == '\0') {
        return sclone("");
    }
    return sclone(&cp[1]);
}


/*
    Return the last portion of a pathname. The separators are not mapped and the path is not cleaned.
    This returns a reference into the original string
 */
PUBLIC cchar *mprGetPathBaseRef(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp;

    if (path == 0) {
        return sclone("");
    }
    fs = mprLookupFileSystem(path);
    if ((cp = (char*) lastSep(fs, path)) == 0) {
        return path;
    } 
    if (cp == path) {
        if (cp[1] == '\0') {
            return path;
        }
    } else if (cp[1] == '\0') {
        return "";
    }
    return &cp[1];
}


/*
    Return the directory portion of a pathname.
 */
PUBLIC char *mprGetPathDir(cchar *path)
{
    MprFileSystem   *fs;
    cchar           *cp, *start;
    char            *result;
    ssize          len;

    mprAssert(path);

    if (path == 0 || *path == '\0') {
        return sclone(path);
    }

    fs = mprLookupFileSystem(path);
    len = slen(path);
    cp = &path[len - 1];
    start = hasDrive(fs, path) ? strchr(path, ':') + 1 : path;

    /*
        Step back over trailing slashes
     */
    while (cp > start && isSep(fs, *cp)) {
        cp--;
    }
    for (; cp > start && !isSep(fs, *cp); cp--) { }

    if (cp == start) {
        if (!isSep(fs, *cp)) {
            /* No slashes found, parent is current dir */
            return sclone(".");
        }
        cp++;
    }
    len = (cp - path);
    result = mprAlloc(len + 1);
    mprMemcpy(result, len + 1, path, len);
    result[len] = '\0';
    return result;
}


/*
    Return the extension portion of a pathname.
    Return the extension without the "."
 */
PUBLIC char *mprGetPathExt(cchar *path)
{
    MprFileSystem  *fs;
    char            *cp;

    if ((cp = srchr(path, '.')) != NULL) {
        fs = mprLookupFileSystem(path);
        /*
            If there is no separator ("/") after the extension, then use it.
         */
        if (firstSep(fs, cp) == 0) {
            return sclone(++cp);
        }
    } 
    return 0;
}


/*
    This returns a list of MprDirEntry objects
 */
#if BIT_WIN_LIKE
static MprList *getDirFiles(cchar *dir, int flags)
{
    HANDLE          h;
    MprDirEntry     *dp;
    MprPath         fileInfo;
    MprList         *list;
    cchar           *seps;
    char            *path, pbuf[MPR_MAX_PATH];
#if WINCE
    WIN32_FIND_DATAA findData;
#else
    WIN32_FIND_DATA findData;
#endif

    list = 0;
    dp = 0;

    if ((path = mprJoinPath(dir, "*.*")) == 0) {
        return 0;
    }
    seps = mprGetPathSeparators(dir);

    h = FindFirstFile(wide(path), &findData);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    list = mprCreateList(-1, 0);

    do {
        if (findData.cFileName[0] == '.' && (findData.cFileName[1] == '\0' || findData.cFileName[1] == '.')) {
            continue;
        }
        if ((dp = mprAlloc(sizeof(MprDirEntry))) == 0) {
            return 0;
        }
        dp->name = awtom(findData.cFileName, 0);
        if (dp->name == 0) {
            return 0;
        }
        /* dp->lastModified = (uint) findData.ftLastWriteTime.dwLowDateTime; */

        if (fmt(pbuf, sizeof(pbuf), "%s%c%s", dir, seps[0], dp->name) < 0) {
            dp->lastModified = 0;
        } else {
            mprGetPathInfo(pbuf, &fileInfo);
            dp->lastModified = fileInfo.mtime;
        }
        dp->isDir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        dp->isLink = 0;

#if FUTURE_64_BIT
        if (findData.nFileSizeLow < 0) {
            dp->size = (((uint64) findData.nFileSizeHigh) * INT64(4294967296)) + (4294967296L - 
                (uint64) findData.nFileSizeLow);
        } else {
            dp->size = (((uint64) findData.nFileSizeHigh * INT64(4294967296))) + (uint64) findData.nFileSizeLow;
        }
#else
        dp->size = (uint) findData.nFileSizeLow;
#endif
        mprAddItem(list, dp);
    } while (FindNextFile(h, &findData) != 0);

    FindClose(h);
    return list;
}
#endif /* WIN */


static void manageDirEntry(MprDirEntry *dp, int flags)
{
    if (flags & MPR_MANAGE_MARK) {
        mprMark(dp->name);
    }
}


#if BIT_UNIX_LIKE || VXWORKS || CYGWIN
static MprList *getDirFiles(cchar *path, int flags)
{
    DIR             *dir;
    MprPath         fileInfo;
    MprList         *list;
    struct dirent   *dirent;
    MprDirEntry     *dp;
    char            *fileName;
    int             rc;

    if ((dir = opendir((char*) path)) == 0) {
        return 0;
    }
    list = mprCreateList(256, 0);

    while ((dirent = readdir(dir)) != 0) {
        if (dirent->d_name[0] == '.' && (dirent->d_name[1] == '\0' || dirent->d_name[1] == '.')) {
            continue;
        }
        fileName = mprJoinPath(path, dirent->d_name);
        /* workaround for if target of symlink does not exist */
        fileInfo.isLink = 0;
        fileInfo.isDir = 0;
        rc = mprGetPathInfo(fileName, &fileInfo);
        if ((dp = mprAllocObj(MprDirEntry, manageDirEntry)) == 0) {
            return 0;
        }
        dp->name = sclone(dirent->d_name);
        if (dp->name == 0) {
            return 0;
        }
        if (rc == 0 || fileInfo.isLink) {
            dp->lastModified = fileInfo.mtime;
            dp->size = fileInfo.size;
            dp->isDir = fileInfo.isDir;
            dp->isLink = fileInfo.isLink;
        } else {
            dp->lastModified = 0;
            dp->size = 0;
            dp->isDir = 0;
            dp->isLink = 0;
        }
        mprAddItem(list, dp);
    }
    closedir(dir);
    return list;
}
#endif


/*
    Find files in the directory "dir". If base is set, use that as the prefix for returned files.
    Returns a list of MprDirEntry objects.
 */
static MprList *findFiles(MprList *list, cchar *dir, cchar *base, int flags)
{
    MprDirEntry     *dp;
    MprList         *files;
    char            *name;
    int             next;

    if ((files = getDirFiles(dir, flags)) == 0) {
        return 0;
    }
    for (next = 0; (dp = mprGetNextItem(files, &next)) != 0; ) {
        if (dp->name[0] == '.') {
            if (dp->name[1] == '\0' || (dp->name[1] == '.' && dp->name[2] == '\0')) {
                continue;
            }
            if (!(flags & MPR_PATH_INC_HIDDEN)) {
                continue;
            }
        }
        name = dp->name;
        dp->name = mprJoinPath(base, name);

        if (!(flags & MPR_PATH_DEPTH_FIRST) && !(dp->isDir && flags & MPR_PATH_NODIRS)) {
            mprAddItem(list, dp);
        }
        if (dp->isDir) {
            if (flags & MPR_PATH_DESCEND) {
                findFiles(list, mprJoinPath(dir, name), mprJoinPath(base, name), flags);
            } 
        }
        if ((flags & MPR_PATH_DEPTH_FIRST) && (!(dp->isDir && flags & MPR_PATH_NODIRS))) {
            mprAddItem(list, dp);
        }
    }
    return list;
}


#if LINUX
static int sortFiles(MprDirEntry **dp1, MprDirEntry **dp2)
{
    return strcmp((*dp1)->name, (*dp2)->name);
}
#endif


/*
    Get the files in a directory. Returns a list of MprDirEntry objects.

    MPR_PATH_DESCEND        to traverse subdirectories
    MPR_PATH_DEPTH_FIRST    to do a depth-first traversal
    MPR_PATH_INC_HIDDEN     to include hidden files
    MPR_PATH_NODIRS         to exclude subdirectories
    MPR_PATH_RELATIVE       to return paths relative to the initial directory
 */
PUBLIC MprList *mprGetPathFiles(cchar *dir, int flags)
{
    MprList *list;
    cchar   *base;

    if (dir == 0 || *dir == '\0') {
        dir = ".";
    }
    base = (flags & MPR_PATH_RELATIVE) ? 0 : dir;
    if ((list = findFiles(mprCreateList(-1, 0), dir, base, flags)) == 0) {
        return 0;
    }
#if LINUX
    /* Linux returns directories not sorted */
    mprSortList(list, (MprSortProc) sortFiles, 0);
#endif
    return list;
}


/*
    Return the first directory of a pathname
 */
PUBLIC char *mprGetPathFirstDir(cchar *path)
{
    MprFileSystem   *fs;
    cchar           *cp;
    int             len;

    mprAssert(path);

    fs = mprLookupFileSystem(path);
    if (isAbsPath(fs, path)) {
        len = hasDrive(fs, path) ? 2 : 1;
        return snclone(path, len);
    } else {
        if ((cp = firstSep(fs, path)) != 0) {
            return snclone(path, cp - path);
        }
        return sclone(path);
    }
}


PUBLIC int mprGetPathInfo(cchar *path, MprPath *info)
{
    MprFileSystem  *fs;

    fs = mprLookupFileSystem(path);
    return fs->getPathInfo(fs, path, info);
}


PUBLIC char *mprGetPathLink(cchar *path)
{
    MprFileSystem  *fs;

    fs = mprLookupFileSystem(path);
    return fs->getPathLink(fs, path);
}


/*
    GetPathParent is smarter than GetPathDir which operates purely textually on the path. GetPathParent will convert
    relative paths to absolute to determine the parent directory.
 */
PUBLIC char *mprGetPathParent(cchar *path)
{
    MprFileSystem   *fs;
    char            *dir;

    fs = mprLookupFileSystem(path);

    if (path == 0 || path[0] == '\0') {
        return mprGetAbsPath(".");
    }
    if (firstSep(fs, path) == NULL) {
        /*
            No parents in the path, so convert to absolute
         */
        dir = mprGetAbsPath(path);
        return mprGetPathDir(dir);
    }
    return mprGetPathDir(path);
}


PUBLIC char *mprGetPortablePath(cchar *path)
{
    char    *result, *cp;

    result = mprTransformPath(path, 0);
    for (cp = result; *cp; cp++) {
        if (*cp == '\\') {
            *cp = '/';
        }
    }
    return result;
}


/*
    This returns a path relative to the current working directory for the given path
 */
PUBLIC char *mprGetRelPath(cchar *destArg, cchar *originArg)
{
    MprFileSystem   *fs;
    char            originBuf[MPR_MAX_FNAME], *cp, *result, *dest, *lastcp, *origin, *op, *lastop;
    int             originSegments, i, commonSegments, sep;

    fs = mprLookupFileSystem(destArg);
    
    if (destArg == 0 || *destArg == '\0') {
        return sclone(".");
    }
    dest = mprNormalizePath(destArg);

    if (!isAbsPath(fs, dest) && (originArg == 0 || *originArg == '\0')) {
        return dest;
    }
    sep = (cp = firstSep(fs, dest)) ? *cp : defaultSep(fs);
    
    if (originArg == 0 || *originArg == '\0') {
        /*
            Get the working directory. Ensure it is null terminated and leave room to append a trailing separators
            On cygwin, this will be a cygwin style path (starts with "/" and no drive specifier).
         */
        if (getcwd(originBuf, sizeof(originBuf)) == 0) {
            strcpy(originBuf, ".");
        }
        originBuf[sizeof(originBuf) - 2] = '\0';
        origin = originBuf;
    } else {
        origin = mprGetAbsPath(originArg);
    }
    dest = mprGetAbsPath(dest);

    /*
        Count segments in origin working directory. Ignore trailing separators.
     */
    for (originSegments = 0, cp = origin; *cp; cp++) {
        if (isSep(fs, *cp) && cp[1]) {
            originSegments++;
        }
    }

    /*
        Find portion of dest that matches the origin directory, if any. Start at -1 because matching root doesn't count.
     */
    commonSegments = -1;
    for (lastop = op = origin, lastcp = cp = dest; *op && *cp; op++, cp++) {
        if (isSep(fs, *op)) {
            lastop = op + 1;
            if (isSep(fs, *cp)) {
                lastcp = cp + 1;
                commonSegments++;
            }
        } else if (fs->caseSensitive) {
            if (*op != *cp) {
                break;
            }
        } else if (*op != *cp && tolower((uchar) *op) != tolower((uchar) *cp)) {
            break;
        }
    }
    mprAssert(commonSegments >= 0);

    if (*cp && *op) {
        op = lastop;
        cp = lastcp;
    }

    /*
        Add one more segment if the last segment matches. Handle trailing separators
     */
    if ((isSep(fs, *op) || *op == '\0') && (isSep(fs, *cp) || *cp == '\0')) {
        commonSegments++;
    }
    if (isSep(fs, *cp)) {
        cp++;
    }
    
    op = result = mprAlloc(originSegments * 3 + slen(dest) + 2);
    for (i = commonSegments; i < originSegments; i++) {
        *op++ = '.';
        *op++ = '.';
        *op++ = defaultSep(fs);
    }
    if (*cp) {
        strcpy(op, cp);
    } else if (op > result) {
        /*
            Cleanup trailing separators ("../" is the end of the new path)
         */
        op[-1] = '\0';
    } else {
        strcpy(result, ".");
    }
    mprMapSeparators(result, sep);
    return result;
}


/*
    Get a temporary file name. The file is created in the system temp location.
 */
PUBLIC char *mprGetTempPath(cchar *tempDir)
{
    MprFile         *file;
    char            *dir, *path;
    int             i, now;
    static int      tempSeed = 0;

    if (tempDir == 0 || *tempDir == '\0') {
#if WINCE
        dir = sclone("/Temp");
#elif BIT_WIN_LIKE
{
        MprFileSystem   *fs;
        fs = mprLookupFileSystem(tempDir ? tempDir : (cchar*) "/");
        dir = sclone(getenv("TEMP"));
        mprMapSeparators(dir, defaultSep(fs));
}
#elif VXWORKS
        dir = sclone(".");
#else
        dir = sclone("/tmp");
#endif
    } else {
        dir = sclone(tempDir);
    }
    now = ((int) mprGetTime() & 0xFFFF) % 64000;
    file = 0;
    path = 0;
    for (i = 0; i < 128; i++) {
        path = sfmt("%s/MPR_%d_%d_%d.tmp", dir, getpid(), now, ++tempSeed);
        file = mprOpenFile(path, O_CREAT | O_EXCL | O_BINARY, 0664);
        if (file) {
            mprCloseFile(file);
            break;
        }
    }
    if (file == 0) {
        return 0;
    }
    return path;
}


/*
    Return a windows path.
    On CYGWIN, this is a cygwin path without drive specs.
 */
PUBLIC char *mprGetWinPath(cchar *path)
{
    char            *result;

    if (path == 0 || *path == '\0') {
        path = ".";
    }
#if BIT_ROM
    result = mprNormalizePath(path);
#elif CYGWIN
{
    ssize   len;
    if ((len = cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, path, NULL, 0)) >= 0) {
        if ((result = mprAlloc(len)) == 0) {
            return 0;
        }
        cygwin_conv_path(CCP_POSIX_TO_WIN_A | CCP_ABSOLUTE, path, result, len);
        return result;
    } else {
        result = mprGetAbsPath(path);
    }
}
#else
    result = mprNormalizePath(path);
    mprMapSeparators(result, '\\');
#endif
    return result;
}


PUBLIC bool mprIsPathAbs(cchar *path)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return isAbsPath(fs, path);
}


PUBLIC bool mprIsPathDir(cchar *path)
{
    MprPath     info;

    return (mprGetPathInfo(path, &info) == 0 && info.isDir);
}


PUBLIC bool mprIsPathRel(cchar *path)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return !isAbsPath(fs, path);
}


PUBLIC bool mprIsPathSeparator(cchar *path, cchar c)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    return isSep(fs, c);
}


/*
    Join paths. Returns a joined (normalized) path.
    If other is absolute, then return other. If other is null, empty or "." then return path.
    The separator is chosen to match the first separator found in either path. If none, it uses the default separator.
 */
PUBLIC char *mprJoinPath(cchar *path, cchar *other)
{
    MprFileSystem   *fs;
    char            *join, *drive, *cp;
    int             sep;

    fs = mprLookupFileSystem(path);
    if (other == NULL || *other == '\0' || strcmp(other, ".") == 0) {
        return sclone(path);
    }
    if (isAbsPath(fs, other)) {
        if (fs->hasDriveSpecs && !isFullPath(fs, other) && isFullPath(fs, path)) {
            /*
                Other is absolute, but without a drive. Use the drive from path.
             */
            drive = sclone(path);
            if ((cp = strchr(drive, ':')) != 0) {
                *++cp = '\0';
            }
            return sjoin(drive, other, NULL);
        } else {
            return mprNormalizePath(other);
        }
    }
    if (path == NULL || *path == '\0') {
        return mprNormalizePath(other);
    }
    if ((cp = firstSep(fs, path)) != 0) {
        sep = *cp;
    } else if ((cp = firstSep(fs, other)) != 0) {
        sep = *cp;
    } else {
        sep = defaultSep(fs);
    }
    if ((join = sfmt("%s%c%s", path, sep, other)) == 0) {
        return 0;
    }
    return mprNormalizePath(join);
}


/*
    Join an extension to a path. If path already has an extension, this call does nothing.
    The extension should not have a ".", but this routine is tolerant if it does.
 */
PUBLIC char *mprJoinPathExt(cchar *path, cchar *ext)
{
    MprFileSystem   *fs;
    char            *cp;

    fs = mprLookupFileSystem(path);
    if (ext == NULL || *ext == '\0') {
        return sclone(path);
    }
    cp = srchr(path, '.');
    if (cp && firstSep(fs, cp) == 0) {
        return sclone(path);
    }
    if (ext[0] == '.') {
        return sjoin(path, ext, NULL);
    } else {
        return sjoin(path, ".", ext, NULL);
    }
}


/*
    Make a directory with all necessary intervening directories.
 */
PUBLIC int mprMakeDir(cchar *path, int perms, int owner, int group, bool makeMissing)
{
    MprFileSystem   *fs;
    char            *parent;
    int             rc;

    fs = mprLookupFileSystem(path);

    if (mprPathExists(path, X_OK)) {
        return 0;
    }
    if (fs->makeDir(fs, path, perms, owner, group) == 0) {
        return 0;
    }
    if (makeMissing && !isRoot(fs, path)) {
        parent = mprGetPathParent(path);
        if ((rc = mprMakeDir(parent, perms, owner, group, makeMissing)) < 0) {
            return rc;
        }
        return fs->makeDir(fs, path, perms, owner, group);
    }
    return MPR_ERR_CANT_CREATE;
}


PUBLIC int mprMakeLink(cchar *path, cchar *target, bool hard)
{
    MprFileSystem   *fs;

    fs = mprLookupFileSystem(path);
    if (mprPathExists(path, X_OK)) {
        return 0;
    }
    return fs->makeLink(fs, path, target, hard);
}


/*
    Normalize a path to remove redundant "./" and cleanup "../" and make separator uniform. Does not make an abs path.
    It does not map separators, change case, nor add drive specifiers.
 */
PUBLIC char *mprNormalizePath(cchar *pathArg)
{
    MprFileSystem   *fs;
    char            *path, *sp, *dp, *mark, **segments;
    ssize           len;
    int             addSep, i, segmentCount, hasDot, last, sep;

    if (pathArg == 0 || *pathArg == '\0') {
        return sclone("");
    }
    fs = mprLookupFileSystem(pathArg);

    /*
        Allocate one spare byte incase we need to break into segments. If so, will add a trailing "/" to make 
        parsing easier later.
     */
    len = slen(pathArg);
    if ((path = mprAlloc(len + 2)) == 0) {
        return NULL;
    }
    strcpy(path, pathArg);
    sep = (sp = firstSep(fs, path)) ? *sp : defaultSep(fs);

    /*
        Remove multiple path separators. Check if we have any "." characters and count the number of path segments
        Map separators to the first separator found
     */
    hasDot = segmentCount = 0;
    for (sp = dp = path; *sp; ) {
        if (isSep(fs, *sp)) {
            *sp = sep;
            segmentCount++;
            while (isSep(fs, sp[1])) {
                sp++;
            }
        } 
        if (*sp == '.') {
            hasDot++;
        }
        *dp++ = *sp++;
    }
    *dp = '\0';
    if (!sep) {
        sep = defaultSep(fs);
    }
    if (!hasDot && segmentCount == 0) {
        if (fs->hasDriveSpecs) {
            last = path[slen(path) - 1];
            if (last == ':') {
                path = sjoin(path, ".", NULL);
            }
        }
        return path;
    }

    if (dp > path && !isSep(fs, dp[-1])) {
        *dp++ = sep;
        *dp = '\0';
        segmentCount++;
    }

    /*
        Have dots to process so break into path segments. Add one incase we need have an absolute path with a drive-spec.
     */
    mprAssert(segmentCount > 0);
    if ((segments = mprAlloc(sizeof(char*) * (segmentCount + 1))) == 0) {
        return NULL;
    }

    /*
        NOTE: The root "/" for absolute paths will be stored as empty.
     */
    len = 0;
    for (i = 0, mark = sp = path; *sp; sp++) {
        if (isSep(fs, *sp)) {
            *sp = '\0';
            if (*mark == '.' && mark[1] == '\0' && segmentCount > 1) {
                /* Remove "."  However, preserve lone "." */
                mark = sp + 1;
                segmentCount--;
                continue;
            }
            if (*mark == '.' && mark[1] == '.' && mark[2] == '\0' && i > 0 && strcmp(segments[i-1], "..") != 0) {
                /* Erase ".." and previous segment */
                if (*segments[i - 1] == '\0' ) {
                    mprAssert(i == 1);
                    /* Previous segement is "/". Prevent escape from root */
                    segmentCount--;
                } else {
                    i--;
                    segmentCount -= 2;
                }
                mprAssert(segmentCount >= 0);
                mark = sp + 1;
                continue;
            }
            segments[i++] = mark;
            len += (sp - mark);
#if KEEP
            if (i == 1 && segmentCount == 1 && fs->hasDriveSpecs && strchr(mark, ':') != 0) {
                /*
                    Normally we truncate a trailing "/", but this is an absolute path with a drive spec (c:/). 
                 */
                segments[i++] = "";
                segmentCount++;
            }
#endif
            mark = sp + 1;
        }
    }

    if (--sp > mark) {
        segments[i++] = mark;
        len += (sp - mark);
    }
    mprAssert(i <= segmentCount);
    segmentCount = i;

    if (segmentCount <= 0) {
        return sclone(".");
    }

    addSep = 0;
    sp = segments[0];
    if (fs->hasDriveSpecs && *sp != '\0') {
        last = sp[slen(sp) - 1];
        if (last == ':') {
            /* This matches an original path of: "c:/" but not "c:filename" */
            addSep++;
        }
    }
#if BIT_WIN_LIKE
    if (strcmp(segments[segmentCount - 1], " ") == 0) {
        segmentCount--;
    }
#endif
    if ((path = mprAlloc(len + segmentCount + 1)) == 0) {
        return NULL;
    }
    mprAssert(segmentCount > 0);

    /*
        First segment requires special treatment due to drive specs
     */
    dp = path;
    strcpy(dp, segments[0]);
    dp += slen(segments[0]);

    if (segmentCount == 1 && (addSep || (*segments[0] == '\0'))) {
        *dp++ = sep;
    }

    for (i = 1; i < segmentCount; i++) {
        *dp++ = sep;
        strcpy(dp, segments[i]);
        dp += slen(segments[i]);
    }
    *dp = '\0';
    return path;
}


PUBLIC void mprMapSeparators(char *path, int separator)
{
    MprFileSystem   *fs;
    char            *cp;

    fs = mprLookupFileSystem(path);
    for (cp = path; *cp; cp++) {
        if (isSep(fs, *cp)) {
            *cp = separator;
        }
    }
}


PUBLIC bool mprPathExists(cchar *path, int omode)
{
    MprFileSystem  *fs;

    if (path == 0 || *path == '\0') {
        return 0;
    }
    fs = mprLookupFileSystem(path);
    return fs->accessPath(fs, path, omode);
}


PUBLIC char *mprReadPathContents(cchar *path, ssize *lenp)
{
    MprFile     *file;
    MprPath     info;
    ssize       len;
    char        *buf;

    if ((file = mprOpenFile(path, O_RDONLY | O_BINARY, 0)) == 0) {
        mprError("Can't open %s", path);
        return 0;
    }
    if (mprGetPathInfo(path, &info) < 0) {
        mprCloseFile(file);
        return 0;
    }
    len = (ssize) info.size;
    if ((buf = mprAlloc(len + 1)) == 0) {
        mprCloseFile(file);
        return 0;
    }
    if (mprReadFile(file, buf, len) != len) {
        mprCloseFile(file);
        return 0;
    }
    buf[len] = '\0';
    if (lenp) {
        *lenp = len;
    }
    mprCloseFile(file);
    return buf;
}


PUBLIC int mprRenamePath(cchar *from, cchar *to)
{
    return rename(from, to);
}


PUBLIC char *mprReplacePathExt(cchar *path, cchar *ext)
{
    return mprJoinPathExt(mprTrimPathExt(path), ext);
}


/*
    Resolve paths in the neighborhood of this path. Resolve operates like join, except that it joins the 
    given paths to the directory portion of the current ("this") path. For example: 
    Path("/usr/bin/ejs/bin").resolve("lib") will return "/usr/lib/ejs/lib". i.e. it will return the
    sibling directory "lib".

    Resolve operates by determining a virtual current directory for this Path object. It then successively 
    joins the given paths to the directory portion of the current result. If the next path is an absolute path, 
    it is used unmodified.  The effect is to find the given paths with a virtual current directory set to the 
    directory containing the prior path.

    Resolve is useful for creating paths in the region of the current path and gracefully handles both 
    absolute and relative path segments.

    Returns a joined (normalized) path.
    If path is absolute, then return path. If path is null, empty or "." then return path.
 */
PUBLIC char *mprResolvePath(cchar *base, cchar *path)
{
    MprFileSystem   *fs;
    char            *join, *drive, *cp, *dir;

    fs = mprLookupFileSystem(base);
    if (path == NULL || *path == '\0' || strcmp(path, ".") == 0) {
        return sclone(base);
    }
    if (isAbsPath(fs, path)) {
        if (fs->hasDriveSpecs && !isFullPath(fs, path) && isFullPath(fs, base)) {
            /*
                Other is absolute, but without a drive. Use the drive from base.
             */
            drive = sclone(base);
            if ((cp = strchr(drive, ':')) != 0) {
                *++cp = '\0';
            }
            return sjoin(drive, path, NULL);
        }
        return mprNormalizePath(path);
    }
    if (base == NULL || *base == '\0') {
        return mprNormalizePath(path);
    }
    dir = mprGetPathDir(base);
    if ((join = sfmt("%s/%s", dir, path)) == 0) {
        return 0;
    }
    return mprNormalizePath(join);
}


/*
    Compare two file path to determine if they point to the same file.
 */
PUBLIC int mprSamePath(cchar *path1, cchar *path2)
{
    MprFileSystem   *fs;
    cchar           *p1, *p2;

    fs = mprLookupFileSystem(path1);

    /*
        Convert to absolute (normalized) paths to compare. 
        MOB - resolve symlinks.
     */
    if (!isFullPath(fs, path1)) {
        path1 = mprGetAbsPath(path1);
    } else {
        path1 = mprNormalizePath(path1);
    }
    if (!isFullPath(fs, path2)) {
        path2 = mprGetAbsPath(path2);
    } else {
        path2 = mprNormalizePath(path2);
    }
    if (fs->caseSensitive) {
        for (p1 = path1, p2 = path2; *p1 && *p2; p1++, p2++) {
            if (*p1 != *p2 && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    } else {
        for (p1 = path1, p2 = path2; *p1 && *p2; p1++, p2++) {
            if (tolower((uchar) *p1) != tolower((uchar) *p2) && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    }
    return (*p1 == *p2);
}


/*
    Compare two file path to determine if they point to the same file.
 */
PUBLIC int mprSamePathCount(cchar *path1, cchar *path2, ssize len)
{
    MprFileSystem   *fs;
    cchar           *p1, *p2;

    fs = mprLookupFileSystem(path1);

    /*
        Convert to absolute paths to compare. 
        MOB - resolve symlinks.
     */
    if (!isFullPath(fs, path1)) {
        path1 = mprGetAbsPath(path1);
    }
    if (!isFullPath(fs, path2)) {
        path2 = mprGetAbsPath(path2);
    }
    if (fs->caseSensitive) {
        for (p1 = path1, p2 = path2; *p1 && *p2 && len > 0; p1++, p2++, len--) {
            if (*p1 != *p2 && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    } else {
        for (p1 = path1, p2 = path2; *p1 && *p2 && len > 0; p1++, p2++, len--) {
            if (tolower((uchar) *p1) != tolower((uchar) *p2) && !(isSep(fs, *p1) && isSep(fs, *p2))) {
                break;
            }
        }
    }
    return len == 0;
}


PUBLIC void mprSetAppPath(cchar *path)
{ 
    MPR->appPath = sclone(path);
    MPR->appDir = mprGetPathDir(MPR->appPath);
}


static char* checkPath(cchar *path, int flags) 
{
    MprPath     info;
    int         access;

    access = (flags & (MPR_SEARCH_EXE | MPR_SEARCH_DIR)) ? X_OK : R_OK;

    if (mprPathExists(path, access)) {
        mprGetPathInfo(path, &info);
        if (flags & MPR_SEARCH_DIR && info.isDir) {
            mprLog(4, "mprSearchForFile: found %s", path);
            return sclone(path);
        }
        if (info.isReg) {
            mprLog(4, "mprSearchForFile: found %s", path);
            return sclone(path);
        }
    }
    return 0;
}


PUBLIC char *mprSearchPath(cchar *file, int flags, cchar *search, ...)
{
    va_list     args;
    char        *result, *path, *dir, *nextDir, *tok;

    va_start(args, search);

    mprLog(5, "mprSearchForFile: %s", file);

    if ((result = checkPath(file, flags)) != 0) {
        return result;
    }
    if ((flags & MPR_SEARCH_EXE) && *BIT_EXE) {
        if ((result = checkPath(mprJoinPathExt(file, BIT_EXE), flags)) != 0) {
            return result;
        }
    }
    for (nextDir = (char*) search; nextDir; nextDir = va_arg(args, char*)) {
        tok = NULL;
        nextDir = sclone(nextDir);
        dir = stok(nextDir, MPR_SEARCH_SEP, &tok);
        while (dir && *dir) {
            mprLog(5, "mprSearchForFile: %s in search path %s", file, dir);
            path = mprJoinPath(dir, file);
            if ((result = checkPath(path, flags)) != 0) {
                return mprNormalizePath(result);
            }
            if ((flags & MPR_SEARCH_EXE) && *BIT_EXE) {
                if ((result = checkPath(mprJoinPathExt(path, BIT_EXE), flags)) != 0) {
                    return mprNormalizePath(result);
                }
            }
            dir = stok(0, MPR_SEARCH_SEP, &tok);
        }
    }
    va_end(args);
    return 0;
}


/*
    This normalizes a path. Returns a normalized path according to flags. Default is absolute. 
    if MPR_PATH_NATIVE_SEP is specified in the flags, map separators to the native format.
 */
PUBLIC char *mprTransformPath(cchar *path, int flags)
{
    char    *result;

#if CYGWIN
    if (flags & MPR_PATH_ABS) {
        if (flags & MPR_PATH_WIN) {
            result = mprGetWinPath(path);
        } else {
            result = mprGetAbsPath(path);
        }
#else
    if (flags & MPR_PATH_ABS) {
        result = mprGetAbsPath(path);

#endif
    } else if (flags & MPR_PATH_REL) {
        result = mprGetRelPath(path, 0);

    } else {
        result = mprNormalizePath(path);
    }
    if (flags & MPR_PATH_NATIVE_SEP) {
#if BIT_WIN_LIKE
        mprMapSeparators(result, '\\');
#elif CYGWIN
        mprMapSeparators(result, '/');
#endif
    }
    return result;
}


PUBLIC char *mprTrimPathExt(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp, *result;

    fs = mprLookupFileSystem(path);
    result = sclone(path);
    if ((cp = srchr(result, '.')) != NULL) {
        if (firstSep(fs, cp) == 0) {
            *cp = '\0';
        }
    } 
    return result;
}


PUBLIC char *mprTrimPathDrive(cchar *path)
{
    MprFileSystem   *fs;
    char            *cp, *endDrive;

    fs = mprLookupFileSystem(path);
    if (fs->hasDriveSpecs) {
        cp = firstSep(fs, path);
        endDrive = strchr(path, ':');
        if (endDrive && (cp == NULL || endDrive < cp)) {
            return sclone(++endDrive);
        }
    }
    return sclone(path);
}


PUBLIC ssize mprWritePathContents(cchar *path, cchar *buf, ssize len, int mode)
{
    MprFile     *file;

    if (mode == 0) {
        mode = 0644;
    }
    if (len < 0) {
        len = slen(buf);
    }
    if ((file = mprOpenFile(path, O_WRONLY | O_TRUNC | O_CREAT | O_BINARY, mode)) == 0) {
        mprError("Can't open %s", path);
        return MPR_ERR_CANT_OPEN;
    }
    if (mprWriteFile(file, buf, len) != len) {
        mprError("Can't write %s", path);
        mprCloseFile(file);
        return MPR_ERR_CANT_WRITE;
    }
    mprCloseFile(file);
    return len;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2012. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */

/************************************************************************/
/*
    Start of file "src/mprPoll.c"
 */
/************************************************************************/

/**
    mprPoll.c - Wait for I/O by using poll on unix like systems.

    This module augments the mprWait wait services module by providing poll() based waiting support.
    Also see mprAsyncSelectWait and mprSelectWait. This module is thread-safe.

    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/



#if MPR_EVENT_POLL
/********************************** Forwards **********************************/

static void serviceIO(MprWaitService *ws, struct pollfd *fds, int count);

/************************************ Code ************************************/

PUBLIC int mprCreateNotifierService(MprWaitService *ws)
{
    struct pollfd   *pollfd;
    int             fd;

    ws->fdsCount = 0;
    ws->fdMax = MPR_FD_MIN;
    ws->handlerMax = MPR_FD_MIN;

    ws->fds = mprAllocZeroed(sizeof(struct pollfd) * ws->fdMax);
    ws->handlerMap = mprAllocZeroed(sizeof(MprWaitHandler*) * ws->handlerMax);
    if (ws->fds == 0 || ws->handlerMap == 0) {
        return MPR_ERR_CANT_INITIALIZE;
    }
    /*
        Initialize the "wakeup" pipe