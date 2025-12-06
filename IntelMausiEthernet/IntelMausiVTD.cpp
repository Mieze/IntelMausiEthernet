/* IntelMausiVTD.cpp -- IntelMausi driver data structure setup.
 *
 * Copyright (c) 2025 Laura Müller <laura-mueller@uni-duesseldorf.de>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * Driver for Intel PCIe gigabit ethernet controllers.
 *
 * This driver is based on Intel's E1000e driver for Linux.
 */

#include "IntelMausiEthernet.h"

#ifndef MIN
#define MIN(a,b) (((a)<(b))?(a):(b))
#endif /* MIN */

#define next_page(x) trunc_page(x + PAGE_SIZE)

#pragma mark --- initialisation methods for AppleVTD support ---

bool IntelMausi::setupRxMap()
{
    IOMemoryDescriptor *md;
    IOPhysicalAddress pa;
    IOByteCount offset;
    UInt32 end;
    UInt32 i, n, idx;
    bool result = false;

    /* Alloc ixgbeRxBufferInfo. */
    rxMapMem = IOMallocZero(kRxMapMemSize);
    
    if (!rxMapMem) {
        IOLog("Couldn't alloc rx map.\n");
        goto done;
    }
    rxMapInfo = (intelRxMapInfo *)rxMapMem;
    
    /* Setup Ranges for IOMemoryDescriptors. */
    for (i = 0; i < kNumRxDesc; i++) {
        rxMapInfo->rxMemRange[i].address = (IOVirtualAddress)mbuf_datastart(rxBufArray[i].mbuf);
        rxMapInfo->rxMemRange[i].length = PAGE_SIZE;
    }

    /* Alloc IOMemoryDescriptors. */
    for (i = 0, idx = 0; i < kNumRxMemDesc; i++, idx += kRxMemBatchSize) {
        md = IOMemoryDescriptor::withOptions(&rxMapInfo->rxMemRange[idx], kRxMemBatchSize, 0, kernel_task, (kIOMemoryTypeVirtual | kIODirectionIn | kIOMemoryAsReference), mapper);
        
        if (!md) {
            IOLog("Couldn't alloc IOMemoryDescriptor.\n");
            goto error_rx_desc;
        }
        if (md->prepare() != kIOReturnSuccess) {
            IOLog("IOMemoryDescriptor::prepare() failed.\n");
            goto error_prep;
        }
        rxMapInfo->rxMemIO[i] = md;
        offset = 0;
        end = idx + kRxMemBatchSize;

        for (n = idx; n < end; n++) {
            pa = md->getPhysicalSegment(offset, NULL);
            rxBufArray[n].phyAddr = pa;
            
            rxDescArray[i].read.buffer_addr = OSSwapHostToLittleInt64(pa);
            rxDescArray[i].read.reserved = 0;

            offset += PAGE_SIZE;
        }
    }
    result = true;
    
done:
    return result;
            
error_prep:
    md->complete();
    RELEASE(md);

error_rx_desc:
    if (rxMapMem) {
        for (i = 0; i < kNumRxMemDesc; i++) {
            md = rxMapInfo->rxMemIO[i];
                            
            if (md) {
                md->complete();
                md->release();
            }
            rxMapInfo->rxMemIO[i] = NULL;
        }
        IOFree(rxMapMem, kRxMapMemSize);
        rxMapMem = NULL;
    }
    goto done;
}

void IntelMausi::freeRxMap()
{
    IOMemoryDescriptor *md;
    UInt32 i;

    if (rxMapMem) {
        for (i = 0; i < kNumRxMemDesc; i++) {
            md = rxMapInfo->rxMemIO[i];
                            
            if (md) {
                md->complete();
                md->release();
            }
            rxMapInfo->rxMemIO[i] = NULL;
        }
        IOFree(rxMapMem, kRxMapMemSize);
        rxMapMem = NULL;
    }
}

bool IntelMausi::setupTxMap()
{
    bool result = false;

    txMapMem = IOMallocZero(kTxMapMemSize);
    
    if (!txMapMem) {
        IOLog("Couldn't alloc memory for tx map.\n");
        goto done;
    }
    txMapInfo = (intelTxMapInfo *)txMapMem;
    
    txMapInfo->txNextMem2Use = 0;
    txMapInfo->txNextMem2Free = 0;
    txMapInfo->txNumFreeMem = kNumTxMemDesc;

    result = true;
    
done:
    return result;
}

void IntelMausi::freeTxMap()
{
    UInt32 i;

    if (txMapMem) {
        for (i = 0; i < kNumTxMemDesc; i++) {
            if (txMapInfo->txMemIO[i]) {
                txMapInfo->txMemIO[i]->complete();
                txMapInfo->txMemIO[i]->release();
                txMapInfo->txMemIO[i] = NULL;
            }
        }
        IOFree(txMapMem, kTxMapMemSize);
        txMapMem = NULL;
    }
}

#pragma mark --- interrupt methods for AppleVTD support ---

void IntelMausi::interruptOccurredVTD(OSObject *client, IOInterruptEventSource *src, int count)
{
    struct e1000_hw *hw = &adapterData.hw;
    UInt32 icr = intelReadMem32(E1000_ICR); /* read ICR disables interrupts using IAM */

    UInt32 packets;

    if (!polling) {
        if (icr & (E1000_ICR_TXDW | E1000_ICR_TXQ0)) {
            txInterrupt();
            etherStats->dot3TxExtraEntry.interrupts++;
        }

        if (icr & (E1000_ICR_RXQ0 | E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
            packets = rxInterruptVTD(netif, kNumRxDesc, NULL, NULL);
            etherStats->dot3RxExtraEntry.interrupts++;

            if (packets)
                netif->flushInputQueue();
        }
    }
    /* Reset on uncorrectable ECC error */
    if ((icr & E1000_ICR_ECCER) && (hw->mac.type >= e1000_pch_lpt)) {
        UInt32 pbeccsts = intelReadMem32(E1000_PBECCSTS);

        etherStats->dot3StatsEntry.internalMacReceiveErrors += (pbeccsts & E1000_PBECCSTS_UNCORR_ERR_CNT_MASK) >>E1000_PBECCSTS_UNCORR_ERR_CNT_SHIFT;
        etherStats->dot3TxExtraEntry.resets++;

        IOLog("Uncorrectable ECC error. Reseting chip.\n");
        intelRestart();
        return;
    }
    if (icr & (E1000_ICR_LSC | E1000_IMS_RXSEQ)) {
        checkLinkStatus();
    }
    /* Reenable interrupts by setting the bits in the mask register. */
    intelWriteMem32(E1000_IMS, icr);
}

#pragma mark --- tx methods for AppleVTD support ---

/*
 * Map a tx packet for read DMA access by the NIC.
 * The packet is split up into physical contiguous segments
 * and an IOMemoryDescriptor is used to map all segments for
 * DMA access.
 */
UInt32 IntelMausi::txMapPacket(mbuf_t packet,
                            IOPhysicalSegment *vector,
                            UInt32 maxSegs)
{
    IOMemoryDescriptor *md = NULL;
    IOAddressRange *srcRange;
    IOAddressRange *dstRange;
    mbuf_t m;
    IOVirtualAddress d;
    IOByteCount offset;
    UInt64 len, l;
    UInt32 segIndex = 0;
    UInt32 i;
    UInt16 saveMem;
    bool result = false;

    if (packet && vector && maxSegs) {
        srcRange = txMapInfo->txSCRange;
        m = packet;
        
        /*
         * Split up the packet into virtual contiguos segments.
         */
        if (mbuf_next(m) == 0) {
            d = (IOVirtualAddress)mbuf_data(m);
            len = mbuf_len(m);
            
            if ( trunc_page(d) == trunc_page(d + len - 1) ) {
                srcRange[0].address = d;
                srcRange[0].length = len;
                segIndex = 1;
                goto map;
            }
        }
        do {
            d = (IOVirtualAddress)mbuf_data(m);
            
            for (len = mbuf_len(m); len; d += l, len -= l) {
                l = MIN(len, PAGE_SIZE);
                l = MIN(next_page(d), d + l) - d;
                
                if (segIndex < maxSegs) {
                    srcRange[segIndex].address = d;
                    srcRange[segIndex].length = l;
                } else {
                    segIndex = 0;
                    goto done;
                }
                segIndex++;
            }
            m = mbuf_next(m);
        } while (m);
map:
        /*
         * Get IORanges, fill in the virtual segments and grab
         * an IOMemoryDescriptor to map the packet.
         */
        if (txMapInfo->txNumFreeMem > 1) {
            dstRange = &txMapInfo->txMemRange[txNextDescIndex];
            
            for (i = 0; i < segIndex; i++) {
                dstRange[i].address = (srcRange[i].address & ~PAGE_MASK);
                dstRange[i].length = PAGE_SIZE;
                srcRange[i].address &= PAGE_MASK;
            }
            OSAddAtomic16(-1, &txMapInfo->txNumFreeMem);
            saveMem = txMapInfo->txNextMem2Use++;
            txMapInfo->txNextMem2Use &= kTxMemDescMask;
            md = txMapInfo->txMemIO[saveMem];
            
            if (md) {
                result = md->initWithOptions(dstRange, segIndex, 0, kernel_task, (kIOMemoryTypeVirtual | kIODirectionOut | kIOMemoryAsReference), mapper);
            } else {
                md = IOMemoryDescriptor::withAddressRanges(dstRange, segIndex, (kIOMemoryTypeVirtual | kIODirectionOut | kIOMemoryAsReference), kernel_task);
                
                if (!md) {
                    DebugLog("Couldn't alloc IOMemoryDescriptor for tx packet.");
                    goto error_map;
                }
                txMapInfo->txMemIO[saveMem] = md;
                result = true;
            }
            if (!result) {
                DebugLog("Failed to init IOMemoryDescriptor for tx packet.");
                goto error_map;
            }
            if (md->prepare() != kIOReturnSuccess) {
                DebugLog("Failed to prepare() tx packet.");
                goto error_map;
            }
            md->setTag(kIOMemoryActive);
            offset = 0;

            /*
             * Get the physical segments and fill in the vector.
             */
            for (i = 0; i < segIndex; i++) {
                vector[i].location = md->getPhysicalSegment(offset, NULL) + srcRange[i].address;
                vector[i].length = srcRange[i].length;

                //DebugLog("Phy. Segment %u addr: %llx, len: %llu\n", i, vector[i].location, vector[i].length);
                offset += PAGE_SIZE;
            }
        }
    }
    
done:
    return segIndex;

error_map:
    txMapInfo->txNextMem2Use = saveMem;
    OSAddAtomic16(1, &txMapInfo->txNumFreeMem);

    segIndex = 0;
    goto done;
}

/*
 * Unmap a tx packet. Complete the IOMemoryDecriptor and free it
 * for reuse.
 */
void IntelMausi::txUnmapPacket()
{
    IOMemoryDescriptor *md = txMapInfo->txMemIO[txMapInfo->txNextMem2Free];
    
    md->complete();
    md->setTag(kIOMemoryInactive);
    
    ++(txMapInfo->txNextMem2Free) &= kTxMemDescMask;
    OSAddAtomic16(1, &txMapInfo->txNumFreeMem);
}

#pragma mark --- rx methods for AppleVTD support ---

/*
 * Unmap a batch of rx buffers, replace them with new ones and map them.
 * @ring        The ring to map for
 * @index       The index of the first buffer in a batch to map.
 * @count       Number of batches to map.
 * @result      The index of the next batch to map.
 */
UInt16 IntelMausi::rxMapBuffers(UInt16 index, UInt16 count, bool update)
{
    IOPhysicalAddress pa;
    IOMemoryDescriptor *md;
    IOByteCount offset;
    UInt32 batch = count;
    UInt32 rdt = 0;
    UInt16 end, i;
    bool result;
    
    while (batch--) {
        /*
         * Get the coresponding IOMemoryDescriptor and complete
         * the mapping;
         */
        md = rxMapInfo->rxMemIO[index >> kRxMemBaseShift];
        md->complete();
        
        /*
         * Update IORanges with the addresses of the replaced buffers.
         */
        for (i = index, end = index + kRxMemBatchSize; i < end; i++) {
            if (rxBufArray[i].phyAddr == 0) {
                rxMapInfo->rxMemRange[i].address = (IOVirtualAddress)mbuf_datastart(rxBufArray[i].mbuf);
            }
        }
        /*
         * Prepare IOMemoryDescriptor with updated buffer addresses.
         */
        result = md->initWithOptions(&rxMapInfo->rxMemRange[index], kRxMemBatchSize, 0, kernel_task, kIOMemoryTypeVirtual | kIODirectionIn | kIOMemoryAsReference, mapper);

        if (!result) {
            IOLog("Failed to reinit rx IOMemoryDescriptor.\n");
            goto done;
        }
        if (md->prepare() != kIOReturnSuccess) {
            IOLog("Failed to prepare rx IOMemoryDescriptor.\n");
            goto done;
        }
        /*
         * Get physical addresses of the buffers and update buffer info,
         * as well as the descriptor ring with new addresses.
         */
        offset = 0;

        for (i = index, end = index + kRxMemBatchSize; i < end; i++) {            
            pa = md->getPhysicalSegment(offset, NULL);
            rxBufArray[i].phyAddr = pa;
            
            rxDescArray[i].read.buffer_addr = OSSwapHostToLittleInt64(pa);
            rxDescArray[i].read.reserved = 0;

            //DebugLog("rxDescArray[%u]: 0x%x %llu\n", i, (unsigned int)length, pa);
            offset += PAGE_SIZE;
        }
        wmb();
        
next_batch:
        rdt = index + kRxMemDescMask;
        index = (index + kRxMemBatchSize) & kRxDescMask;
        rxMapNextIndex = index;

        if (update) {
            /*
             * Prevent the tail from reaching the head in order to avoid a false
             * buffer queue full condition.
             */
            if (adapterData.flags2 & FLAG2_PCIM2PCI_ARBITER_WA)
                intelUpdateRxDescTail(rdt);
            else
                intelWriteMem32(E1000_RDT(0), rdt);
        }
    }
    
done:
    return index;
}

UInt32 IntelMausi::rxInterruptVTD(IONetworkInterface *interface, uint32_t maxCount, IOMbufQueue *pollQueue, void *context)
{
    union e1000_rx_desc_extended *desc = &rxDescArray[rxNextDescIndex];
    mbuf_t bufPkt, newPkt;
    UInt64 addr;
    UInt32 status;
    UInt32 goodPkts = 0;
    UInt32 pktSize;
    UInt16 vlanTag;
    bool replaced;
    
    while (((status = OSSwapLittleToHostInt32(desc->wb.upper.status_error)) & E1000_RXD_STAT_DD) && (goodPkts < maxCount)) {
        addr = rxBufArray[rxNextDescIndex].phyAddr;
        bufPkt = rxBufArray[rxNextDescIndex].mbuf;
        pktSize = OSSwapLittleToHostInt16(desc->wb.upper.length);
        vlanTag = (status & E1000_RXD_STAT_VP) ? (OSSwapLittleToHostInt16(desc->wb.upper.vlan) & E1000_RXD_SPC_VLAN_MASK) : 0;
        
        /* Skip bad packet. */
        if (status & E1000_RXDEXT_ERR_FRAME_ERR_MASK) {
            DebugLog("Bad packet.\n");
            etherStats->dot3StatsEntry.internalMacReceiveErrors++;
            discardPacketFragment();
            goto nextDesc;
        }
        newPkt = rxPool->replaceOrCopyPacket(&bufPkt, pktSize, &replaced);
        
        if (!newPkt) {
            /* Allocation of a new packet failed so that we must leave the original packet in place. */
            //DebugLog("replaceOrCopyPacket() failed.\n");
            etherStats->dot3RxExtraEntry.resourceErrors++;
            discardPacketFragment();
            goto nextDesc;
        }
        
        /* If the packet was replaced we have to update the descriptor's buffer address. */
        if (replaced) {
            if (mbuf_next(bufPkt) != NULL) {
                DebugLog("getPhysicalSegments() failed.\n");
                etherStats->dot3RxExtraEntry.resourceErrors++;
                mbuf_freem_list(bufPkt);
                discardPacketFragment();
                goto nextDesc;
            }
            rxBufArray[rxNextDescIndex].mbuf = bufPkt;
            rxBufArray[rxNextDescIndex].phyAddr = 0;
        }
        /* Set the length of the buffer. */
        mbuf_setlen(newPkt, pktSize);

        if (status & E1000_RXD_STAT_EOP) {
            if (rxPacketHead) {
                /* This is the last buffer of a jumbo frame. */
                mbuf_setflags_mask(newPkt, 0, MBUF_PKTHDR);
                mbuf_setnext(rxPacketTail, newPkt);
                
                rxPacketSize += pktSize;
                rxPacketTail = newPkt;
            } else {
                /*
                 * We've got a complete packet in one buffer.
                 * It can be enqueued directly.
                 */
                rxPacketHead = newPkt;
                rxPacketSize = pktSize;
            }
            intelGetChecksumResult(rxPacketHead, status);

            /* Also get the VLAN tag if there is any. */
            if (vlanTag)
                setVlanTag(rxPacketHead, vlanTag);

            mbuf_pkthdr_setlen(rxPacketHead, rxPacketSize);
            interface->enqueueInputPacket(rxPacketHead, pollQueue);
            
            rxPacketHead = rxPacketTail = NULL;
            rxPacketSize = 0;

            goodPkts++;
        } else {
            if (rxPacketHead) {
                /* We are in the middle of a jumbo frame. */
                mbuf_setflags_mask(newPkt, 0, MBUF_PKTHDR);
                mbuf_setnext(rxPacketTail, newPkt);

                rxPacketTail = newPkt;
                rxPacketSize += pktSize;
            } else {
                /* This is the first buffer of a jumbo frame. */
                rxPacketHead = rxPacketTail = newPkt;
                rxPacketSize = pktSize;
            }
        }
        
        /* Finally update the descriptor and get the next one to examine. */
    nextDesc:
        /*
         * If a batch has been completed, increment the number of
         * batches, which need to be mapped.
         */
        if ((rxNextDescIndex & kRxMemDescMask) == kRxMemDescMask)
            rxCleanedCount++;
        
        ++rxNextDescIndex &= kRxDescMask;
        desc = &rxDescArray[rxNextDescIndex];
    }
    if (rxCleanedCount) {
        rxMapBuffers(rxMapNextIndex, rxCleanedCount, true);
        rxCleanedCount = 0;
    }
    return goodPkts;
}
