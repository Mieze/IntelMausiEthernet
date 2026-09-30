//
//  MausiRxPool.cpp
//  IntelMausiEthernet
//
//  Created by Laura Müller on 02.12.25.
//  Copyright © 2025 Laura Müller. All rights reserved.
//

#include "MausiRxPool.hpp"

OSDefineMetaClassAndStructors(MausiRxPool, OSObject);

#define super OSObject

bool MausiRxPool::init()
{
    return true;
}

void MausiRxPool::free()
{
    if (refillCE) {
        thread_call_cancel(refillCE);
        IOSleep(1);
        thread_call_free(refillCE);
        refillCE = NULL;
    }
    if (cPktHead) {
        mbuf_freem_list(cPktHead);
        cPktHead = cPktTail = NULL;
        cPktNum = 0;
    }
    if (mPktHead) {
        mbuf_freem_list(mPktHead);
        mPktHead = mPktTail = NULL;
        mPktNum = 0;
    }
    if (listLock) {
        IOSimpleLockFree(listLock);
        listLock = NULL;
    }
    super::free();
}

bool MausiRxPool::initWithCapacity(UInt32 mbufCapacity,
                                     UInt32 clustCapacity)
{
    mbuf_t m;
    void *data;
    UInt32 i;
    errno_t err;
    unsigned int chunks;
    bool result = false;
    
    if ((mbufCapacity > 0) && (clustCapacity > 0)) {
        listLock = IOSimpleLockAlloc();
        if (!listLock)
            goto done;

        cPktHead = cPktTail = NULL;
        mPktHead = mPktTail = NULL;
        cCapacity = clustCapacity;
        cRefillTresh = cCapacity - (cCapacity >> 1);
        mCapacity = mbufCapacity;
        mRefillTresh = mCapacity - (mCapacity >> 1);
        cPktNum = 0;
        mPktNum = 0;
        maxCopySize = mbuf_get_mhlen();
        refillScheduled = false;

        nanoseconds_to_absolutetime(kRefillDelayTime, &refillDelay);

        refillCE = thread_call_allocate_with_options((thread_call_func_t) &refillThread, (void *) this, THREAD_CALL_PRIORITY_KERNEL, 0);

        if (!refillCE) {
            goto done;
        }
        for (i = 0; i < mbufCapacity; i++) {
            chunks = 1;
            err = mbuf_allocpacket(MBUF_WAITOK, maxCopySize, &chunks, &m);

            if (err)
                goto fail_mbuf;

            if (mPktHead) {
                mbuf_setnextpkt(mPktTail, m);
                mPktTail = m;
                mPktNum++;
            } else {
                mPktHead = mPktTail = m;
                mPktNum = 1;
            }
            data = mbuf_datastart(m);
            mbuf_setdata(m, data, 0);
        }
        for (i = 0; i < clustCapacity; i++) {
            chunks = 1;
            err = mbuf_allocpacket(MBUF_WAITOK, PAGE_SIZE, &chunks, &m);

            if (err)
                goto fail_cluster;

            if (cPktHead) {
                mbuf_setnextpkt(cPktTail, m);
                cPktTail = m;
                cPktNum++;
            } else {
                cPktHead = cPktTail = m;
                cPktNum = 1;
            }
            data = mbuf_datastart(m);
            mbuf_setdata(m, data, 0);
        }
        result = true;
    }
done:
    return result;
    
fail_cluster:
    if (cPktHead) {
        mbuf_freem_list(cPktHead);
        cPktHead = cPktTail = NULL;
        cPktNum = 0;
    }

fail_mbuf:
    if (mPktHead) {
        mbuf_freem_list(mPktHead);
        mPktHead = mPktTail = NULL;
        mPktNum = 0;
    }
    goto done;
}

MausiRxPool *
MausiRxPool::withCapacity(UInt32 mbufCapacity,
                              UInt32 clustCapacity)
{
    MausiRxPool *pool = new MausiRxPool;
    
    if (pool && !pool->initWithCapacity(mbufCapacity,
                                        clustCapacity)) {
        pool->release();
        pool = NULL;
    }
    return pool;
}

mbuf_t MausiRxPool::getPacket(UInt32 size, mbuf_how_t how)
{
    mbuf_t m = NULL;
    void * data;
    errno_t err;
    unsigned int chunks = 1;
    bool bigPacket = (size > maxCopySize);
    bool scheduleRefill = false;

    /*
     * Try the packet allocator first. This runs unlocked on purpose: it can
     * block when called with MBUF_WAITOK and must not be done under a spin
     * lock. The pool is only a fallback for when it fails.
     */
    err = mbuf_allocpacket(how, bigPacket ? PAGE_SIZE : maxCopySize, &chunks, &m);

    if (!err) {
        data = mbuf_datastart(m);
        mbuf_setdata(m, data, 0);
        return m;
    }

    /*
     * Pop from the pool. Taking the head, advancing it and dropping the count
     * has to be a single atomic step against refillPool(), otherwise a refill
     * running on its own thread can append behind the mbuf we are about to hand
     * out and the same mbuf ends up both owned by the driver and still on the
     * free list.
     */
    IOSimpleLockLock(listLock);

    if (bigPacket) {
        if (cPktNum > 1) {
            m = cPktHead;
            cPktHead = mbuf_nextpkt(cPktHead);
            mbuf_setnextpkt(m, NULL);
            cPktNum--;

            /*
             * If the list held two entries we just removed the tail as well,
             * so the tail now points at a mbuf that has been handed out. Drop
             * it, otherwise a later refill would link the new mbuf behind it
             * and that mbuf would end up on two lists at once.
             */
            if (cPktHead == NULL)
                cPktTail = NULL;

            if ((cPktNum < cRefillTresh) && !refillScheduled) {
                refillScheduled = true;
                scheduleRefill = true;
            }
        }
    } else {
        if (mPktNum > 1) {
            m = mPktHead;
            mPktHead = mbuf_nextpkt(mPktHead);
            mbuf_setnextpkt(m, NULL);
            mPktNum--;

            if (mPktHead == NULL)
                mPktTail = NULL;

            if ((mPktNum < mRefillTresh) && !refillScheduled) {
                refillScheduled = true;
                scheduleRefill = true;
            }
        }
    }

    IOSimpleLockUnlock(listLock);

    if (scheduleRefill)
        thread_call_enter_delayed(refillCE, refillDelay);

    return m;
}

void MausiRxPool::refillPool()
{
    refillList(&mPktHead, &mPktTail, &mPktNum, mCapacity, maxCopySize);
    refillList(&cPktHead, &cPktTail, &cPktNum, cCapacity, PAGE_SIZE);

    IOSimpleLockLock(listLock);
    refillScheduled = false;
    IOSimpleLockUnlock(listLock);
}

/*
 * Top up one of the two free lists. Allocating happens outside the lock,
 * because mbuf_allocpacket() may block and a spin lock may not be held across
 * a blocking call. The newly allocated mbuf is only published under the lock,
 * so a concurrent getPacket() can never observe a half linked list.
 */
void MausiRxPool::refillList(mbuf_t *head, mbuf_t *tail, SInt32 *num,
                             UInt32 capacity, UInt32 dataSize)
{
    mbuf_t m;
    void * data;
    errno_t err;
    unsigned int chunks;

    while (true) {
        IOSimpleLockLock(listLock);
        if (*num >= (SInt32)capacity) {
            IOSimpleLockUnlock(listLock);
            break;
        }
        IOSimpleLockUnlock(listLock);

        chunks = 1;
        err = mbuf_allocpacket(MBUF_DONTWAIT, dataSize, &chunks, &m);

        if (err)
            break;

        data = mbuf_datastart(m);
        mbuf_setdata(m, data, 0);

        IOSimpleLockLock(listLock);

        /*
         * The list may have been drained completely by getPacket() while we
         * were allocating. In that case the old tail has already been handed
         * out and must not be written to, so start a new list instead.
         */
        if (*head == NULL) {
            *head = *tail = m;
        } else {
            mbuf_setnextpkt(*tail, m);
            *tail = m;
        }
        (*num)++;

        IOSimpleLockUnlock(listLock);
    }
}

void MausiRxPool::refillThread(thread_call_param_t param0)
{
    ((MausiRxPool *) param0)->refillPool();
}

/*
 * This is an exact copy of IONetworkController's method
 * replaceOrCopyPacket(), except that it tries to get new
 * packets form one of our pool.
 */
mbuf_t MausiRxPool::replaceOrCopyPacket(mbuf_t *mp,
                                            UInt32 len,
                                            bool * replaced)
{
    mbuf_t m = NULL;
    
    if ((mp != NULL) && (replaced != NULL)) {
        /*
         * Packet needs to be replaced. Try to alloc one
         * get or get one from the cluster buffer pool.
         */
        if (len > maxCopySize) {
            m = *mp;
            
            if ((*mp = getPacket(len, MBUF_DONTWAIT)) == NULL) {
                *mp = m;
                m = NULL;
            }
            *replaced = true;
        } else {
            /*
             * Packet should be copied. Try to get
             * one from the mbuf buffer pool.
             */
            m = getPacket(len, MBUF_DONTWAIT);
            
            if (m) {
                mbuf_copy_pkthdr(m, *mp);
                mbuf_pkthdr_setheader(m, NULL);
                bcopy(mbuf_data(*mp), mbuf_data(m), len);
            }
            *replaced = false;
        }
    }
    return m;
}
