/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#ifndef SRC_DCP_STREAM_H_
#define SRC_DCP_STREAM_H_ 1

#include "config.h"

#include "vbucket.h"
#include "ext_meta_parser.h"
#include "response.h"

#include <atomic>
#include <queue>

class EventuallyPersistentEngine;
class MutationResponse;
class SetVBucketState;
class SnapshotMarker;
class DcpConsumer;
class DcpProducer;
class DcpResponse;

enum stream_state_t {
    STREAM_PENDING,
    STREAM_BACKFILLING,
    STREAM_IN_MEMORY,
    STREAM_TAKEOVER_SEND,
    STREAM_TAKEOVER_WAIT,
    STREAM_READING,
    STREAM_DEAD
};

enum end_stream_status_t {
    //! The stream ended due to all items being streamed
    END_STREAM_OK,
    //! The stream closed early due to a close stream message
    END_STREAM_CLOSED,
    //! The stream closed early because the vbucket state changed
    END_STREAM_STATE,
    //! The stream closed early because the connection was disconnected
    END_STREAM_DISCONNECTED,
    //! The stream was closed early because it was too slow
    END_STREAM_SLOW
};

enum stream_type_t {
    STREAM_ACTIVE,
    STREAM_NOTIFIER,
    STREAM_PASSIVE
};

enum snapshot_type_t {
    none,
    disk,
    memory
};

enum process_items_error_t {
    all_processed,
    more_to_process,
    cannot_process
};

enum backfill_source_t {
    BACKFILL_FROM_MEMORY,
    BACKFILL_FROM_DISK
};

class Stream : public RCValue {
public:
    Stream(const std::string &name, uint32_t flags, uint32_t opaque,
           uint16_t vb, uint64_t start_seqno, uint64_t end_seqno,
           uint64_t vb_uuid, uint64_t snap_start_seqno,
           uint64_t snap_end_seqno);

    virtual ~Stream() {}

    uint32_t getFlags() { return flags_; }

    uint16_t getVBucket() { return vb_; }

    uint32_t getOpaque() { return opaque_; }

    uint64_t getStartSeqno() { return start_seqno_; }

    uint64_t getEndSeqno() { return end_seqno_; }

    uint64_t getVBucketUUID() { return vb_uuid_; }

    uint64_t getSnapStartSeqno() { return snap_start_seqno_; }

    uint64_t getSnapEndSeqno() { return snap_end_seqno_; }

    stream_state_t getState() { return state_; }

    stream_type_t getType() { return type_; }

    virtual void addStats(ADD_STAT add_stat, const void *c);

    virtual DcpResponse* next() = 0;

    virtual uint32_t setDead(end_stream_status_t status) = 0;

    virtual void notifySeqnoAvailable(uint64_t seqno) {}

    const std::string& getName() {
        return name_;
    }

    bool isActive() {
        return state_ != STREAM_DEAD;
    }

    void clear() {
        LockHolder lh(streamMutex);
        clear_UNLOCKED();
    }

protected:

    const char* stateName(stream_state_t st) const;

    void clear_UNLOCKED();

    /* To be called after getting streamMutex lock */
    void pushToReadyQ(DcpResponse* resp);

    /* To be called after getting streamMutex lock */
    void popFromReadyQ(void);

    uint64_t getReadyQueueMemory(void);

    const std::string &name_;
    uint32_t flags_;
    uint32_t opaque_;
    uint16_t vb_;
    uint64_t start_seqno_;
    uint64_t end_seqno_;
    uint64_t vb_uuid_;
    uint64_t snap_start_seqno_;
    uint64_t snap_end_seqno_;
    AtomicValue<stream_state_t> state_;
    stream_type_t type_;

    AtomicValue<bool> itemsReady;
    Mutex streamMutex;
    std::queue<DcpResponse*> readyQ;

    const static uint64_t dcpMaxSeqno;

private:
    /* readyQueueMemory tracks the memory occupied by elements
     * in the readyQ.  It is an atomic because otherwise
       getReadyQueueMemory would need to acquire streamMutex.
     */
    AtomicValue <uint64_t> readyQueueMemory;
};

class ActiveStream : public Stream {
public:
    ActiveStream(EventuallyPersistentEngine* e, DcpProducer* p,
                 const std::string &name, uint32_t flags, uint32_t opaque,
                 uint16_t vb, uint64_t st_seqno, uint64_t en_seqno,
                 uint64_t vb_uuid, uint64_t snap_start_seqno,
                 uint64_t snap_end_seqno);

    ~ActiveStream();

    DcpResponse* next();

    void setActive() {
        LockHolder lh(streamMutex);
        if (state_ == STREAM_PENDING) {
            transitionState(STREAM_BACKFILLING);
        }
    }

    uint32_t setDead(end_stream_status_t status);

    void notifySeqnoAvailable(uint64_t seqno);

    void snapshotMarkerAckReceived();

    void setVBucketStateAckRecieved();

    void incrBackfillRemaining(size_t by) {
        backfillRemaining.fetch_add(by, std::memory_order_relaxed);
    }

    void markDiskSnapshot(uint64_t startSeqno, uint64_t endSeqno);

    bool backfillReceived(Item* itm, backfill_source_t backfill_source);

    void completeBackfill();

    bool isCompressionEnabled();

    void addStats(ADD_STAT add_stat, const void *c);

    void addTakeoverStats(ADD_STAT add_stat, const void *c);

    size_t getItemsRemaining();

    uint64_t getLastSentSeqno();

    const char* logHeader();

    bool isSendMutationKeyOnlyEnabled() const;

private:

    void transitionState(stream_state_t newState);

    DcpResponse* backfillPhase();

    DcpResponse* inMemoryPhase();

    DcpResponse* takeoverSendPhase();

    DcpResponse* takeoverWaitPhase();

    DcpResponse* deadPhase();

    DcpResponse* nextQueuedItem();

    void nextCheckpointItem();

    void snapshot(std::list<MutationResponse*>& snapshot, bool mark);

    void endStream(end_stream_status_t reason);

    void scheduleBackfill();

    const char* getEndStreamStatusStr(end_stream_status_t status);

    ExtendedMetaData* prepareExtendedMetaData(uint16_t vBucketId,
                                              uint8_t conflictResMode);

    //! The last sequence number queued from disk or memory
    AtomicValue<uint64_t> lastReadSeqno;

    //! The last sequence number sent to the network layer
    AtomicValue<uint64_t> lastSentSeqno;

    //! The last known seqno pointed to by the checkpoint cursor
    uint64_t curChkSeqno;

    //! The current vbucket state to send in the takeover stream
    vbucket_state_t takeoverState;

    /* backfillRemaining is a stat recording the amount of
     * items remaining to be read from disk.  It is an atomic
     * because otherwise the function incrBackfillRemaining
     * must acquire the streamMutex lock.
     */
    AtomicValue <size_t> backfillRemaining;

    //! Stats to track items read and sent from the backfill phase
    struct {
        AtomicValue<size_t> memory;
        AtomicValue<size_t> disk;
        AtomicValue<size_t> sent;
    } backfillItems;

    //! The amount of items that have been sent during the memory phase
    AtomicValue<size_t> itemsFromMemoryPhase;

    //! Whether ot not this is the first snapshot marker sent
    bool firstMarkerSent;

    int waitForSnapshot;

    EventuallyPersistentEngine* engine;
    DcpProducer* producer;
    bool isBackfillTaskRunning;

    struct {
        AtomicValue<uint32_t> bytes;
        AtomicValue<uint32_t> items;
    } bufferedBackfill;

    rel_time_t takeoverStart;
    size_t takeoverSendMaxTime;

    /* Enum indicating whether the stream mutations should contain key only or
       both key and value */
    MutationPayload payloadType;
};

class NotifierStream : public Stream {
public:
    NotifierStream(EventuallyPersistentEngine* e, DcpProducer* producer,
                   const std::string &name, uint32_t flags, uint32_t opaque,
                   uint16_t vb, uint64_t start_seqno, uint64_t end_seqno,
                   uint64_t vb_uuid, uint64_t snap_start_seqno,
                   uint64_t snap_end_seqno);

    ~NotifierStream() {
        LockHolder lh(streamMutex);
        transitionState(STREAM_DEAD);
        clear_UNLOCKED();
    }

    DcpResponse* next();

    uint32_t setDead(end_stream_status_t status);

    void notifySeqnoAvailable(uint64_t seqno);

private:

    void transitionState(stream_state_t newState);

    DcpProducer* producer;
};

class PassiveStream : public Stream {
public:
    PassiveStream(EventuallyPersistentEngine* e, DcpConsumer* consumer,
                  const std::string &name, uint32_t flags, uint32_t opaque,
                  uint16_t vb, uint64_t start_seqno, uint64_t end_seqno,
                  uint64_t vb_uuid, uint64_t snap_start_seqno,
                  uint64_t snap_end_seqno, uint64_t vb_high_seqno);

    ~PassiveStream();

    process_items_error_t processBufferedMessages(uint32_t &processed_bytes);

    DcpResponse* next();

    uint32_t setDead(end_stream_status_t status);

    void acceptStream(uint16_t status, uint32_t add_opaque);

    void reconnectStream(RCPtr<VBucket> &vb, uint32_t new_opaque,
                         uint64_t start_seqno);

    ENGINE_ERROR_CODE messageReceived(DcpResponse* response);

    void addStats(ADD_STAT add_stat, const void *c);

    static const size_t batchSize;

private:

    ENGINE_ERROR_CODE processMutation(MutationResponse* mutation);

    ENGINE_ERROR_CODE processDeletion(MutationResponse* deletion);

    void handleSnapshotEnd(RCPtr<VBucket>& vb, uint64_t byseqno);

    void processMarker(SnapshotMarker* marker);

    void processSetVBucketState(SetVBucketState* state);

    bool transitionState(stream_state_t newState);

    uint32_t clearBuffer();

    uint32_t setDead_UNLOCKED(end_stream_status_t status,
                              LockHolder *slh);

    const char* getEndStreamStatusStr(end_stream_status_t status);

    EventuallyPersistentEngine* engine;
    DcpConsumer* consumer;

    AtomicValue<uint64_t> last_seqno;

    AtomicValue<uint64_t> cur_snapshot_start;
    AtomicValue<uint64_t> cur_snapshot_end;
    AtomicValue<snapshot_type_t> cur_snapshot_type;
    bool cur_snapshot_ack;

    struct Buffer {
        Buffer() : bytes(0), items(0) {}
        size_t bytes;
        size_t items;
        Mutex bufMutex;
        std::queue<DcpResponse*> messages;
    } buffer;
};

typedef SingleThreadedRCPtr<Stream> stream_t;
typedef RCPtr<PassiveStream> passive_stream_t;

#endif  // SRC_DCP_STREAM_H_
