//
//  MausiRxPool.hpp
//  IntelMausiEthernet
//
//  Created by Laura Müller on 02.12.25.
//  Copyright © 2025 Laura Müller. All rights reserved.
//

#ifndef MausiRxPool_hpp
#define MausiRxPool_hpp

#define kRefillDelayTime  5000UL

class MausiRxPool : public OSObject
{
    OSDeclareDefaultStructors(MausiRxPool);

public:
    virtual bool init() APPLE_KEXT_OVERRIDE;
    
    virtual void free() APPLE_KEXT_OVERRIDE;
    
    virtual bool initWithCapacity(UInt32 mbufCapacity,
                                  UInt32 clustCapacity);

    static MausiRxPool * withCapacity(UInt32 mbufCapacity,
                                        UInt32 clustCapacity);

    virtual mbuf_t getPacket(UInt32 size);

    
    mbuf_t replaceOrCopyPacket(mbuf_t *mp,
                               UInt32 len,
                               bool * replaced);
    
protected:
    void refillPool();

    static void refillThread(thread_call_param_t param0);

    thread_call_t refillCE;
    UInt64 refillDelay;
    mbuf_t cPktHead;
    mbuf_t cPktTail;
    mbuf_t mPktHead;
    mbuf_t mPktTail;
    UInt32 cCapacity;
    UInt32 cRefillTresh;
    SInt32 cPktNum;
    UInt32 mCapacity;
    UInt32 mRefillTresh;
    SInt32 mPktNum;
    UInt32 maxCopySize;
    bool refillScheduled;
};

#endif /* MausiRxPool_hpp */
