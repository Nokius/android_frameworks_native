/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#include "VirtualDisplaySurface.h"
#include "HWComposer.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------

#define VDS_LOGE(msg, ...) ALOGE("[%s] "msg, \
        mDisplayName.string(), ##__VA_ARGS__)
#define VDS_LOGW_IF(cond, msg, ...) ALOGW_IF(cond, "[%s] "msg, \
        mDisplayName.string(), ##__VA_ARGS__)
#define VDS_LOGV(msg, ...) ALOGV("[%s] "msg, \
        mDisplayName.string(), ##__VA_ARGS__)

static const char* dbgCompositionTypeStr(DisplaySurface::CompositionType type) {
    switch (type) {
        case DisplaySurface::COMPOSITION_UNKNOWN: return "UNKNOWN";
        case DisplaySurface::COMPOSITION_GLES:    return "GLES";
        case DisplaySurface::COMPOSITION_HWC:     return "HWC";
        case DisplaySurface::COMPOSITION_MIXED:   return "MIXED";
        default:                                  return "<INVALID>";
    }
}

VirtualDisplaySurface::VirtualDisplaySurface(HWComposer& hwc, int32_t dispId,
        const sp<IGraphicBufferProducer>& sink, const String8& name)
:   ConsumerBase(new BufferQueue(true)),
    mHwc(hwc),
    mDisplayId(dispId),
    mDisplayName(name),
    mProducerUsage(GRALLOC_USAGE_HW_COMPOSER),
    mProducerSlotSource(0),
    mDbgState(DBG_STATE_IDLE),
    mDbgLastCompositionType(COMPOSITION_UNKNOWN)
{
    mSource[SOURCE_SINK] = sink;
    mSource[SOURCE_SCRATCH] = mBufferQueue;

    resetPerFrameState();

    int sinkWidth, sinkHeight;
    mSource[SOURCE_SINK]->query(NATIVE_WINDOW_WIDTH, &sinkWidth);
    mSource[SOURCE_SINK]->query(NATIVE_WINDOW_HEIGHT, &sinkHeight);

    ConsumerBase::mName = String8::format("VDS: %s", mDisplayName.string());
    mBufferQueue->setConsumerName(ConsumerBase::mName);
    mBufferQueue->setConsumerUsageBits(GRALLOC_USAGE_HW_COMPOSER);
    mBufferQueue->setDefaultBufferSize(sinkWidth, sinkHeight);
    mBufferQueue->setDefaultMaxBufferCount(2);
}

VirtualDisplaySurface::~VirtualDisplaySurface() {
}

sp<IGraphicBufferProducer> VirtualDisplaySurface::getIGraphicBufferProducer() const {
    if (mDisplayId >= 0) {
        return static_cast<IGraphicBufferProducer*>(
                const_cast<VirtualDisplaySurface*>(this));
    } else {
        // There won't be any interaction with HWC for this virtual display,
        // so the GLES driver can pass buffers directly to the sink.
        return mSource[SOURCE_SINK];
    }
}

status_t VirtualDisplaySurface::prepareFrame(CompositionType compositionType) {
    if (mDisplayId < 0)
        return NO_ERROR;

    VDS_LOGW_IF(mDbgState != DBG_STATE_IDLE,
            "Unexpected prepareFrame() in %s state", dbgStateStr());
    mDbgState = DBG_STATE_PREPARED;

    mCompositionType = compositionType;

    if (mCompositionType != mDbgLastCompositionType) {
        VDS_LOGV("prepareFrame: composition type changed to %s",
                dbgCompositionTypeStr(mCompositionType));
        mDbgLastCompositionType = mCompositionType;
    }

    return NO_ERROR;
}

status_t VirtualDisplaySurface::compositionComplete() {
    return NO_ERROR;
}

status_t VirtualDisplaySurface::advanceFrame() {
    if (mDisplayId < 0)
        return NO_ERROR;

    if (mCompositionType == COMPOSITION_HWC) {
        VDS_LOGW_IF(mDbgState != DBG_STATE_PREPARED,
                "Unexpected advanceFrame() in %s state on HWC frame",
                dbgStateStr());
    } else {
        VDS_LOGW_IF(mDbgState != DBG_STATE_GLES_DONE,
                "Unexpected advanceFrame() in %s state on GLES/MIXED frame",
                dbgStateStr());
    }
    mDbgState = DBG_STATE_HWC;

    status_t result;
    sp<Fence> outFence;
    if (mCompositionType != COMPOSITION_GLES) {
        // Dequeue an output buffer from the sink
        uint32_t transformHint, numPendingBuffers;
        mQueueBufferOutput.deflate(&mSinkBufferWidth, &mSinkBufferHeight,
                &transformHint, &numPendingBuffers);
        int sslot;
        result = dequeueBuffer(SOURCE_SINK, 0, &sslot, &outFence);
        if (result < 0)
            return result;
        mOutputProducerSlot = mapSource2ProducerSlot(SOURCE_SINK, sslot);
    }

    if (mCompositionType == COMPOSITION_HWC) {
        // We just dequeued the output buffer, use it for FB as well
        mFbProducerSlot = mOutputProducerSlot;
        mFbFence = outFence;
    } else if (mCompositionType == COMPOSITION_GLES) {
        mOutputProducerSlot = mFbProducerSlot;
        outFence = mFbFence;
    } else {
        // mFbFence and mFbProducerSlot were set in queueBuffer,
        // and mOutputProducerSlot and outFence were set above when dequeueing
        // the sink buffer.
    }

    if (mFbProducerSlot < 0 || mOutputProducerSlot < 0) {
        // Last chance bailout if something bad happened earlier. For example,
        // in a GLES configuration, if the sink disappears then dequeueBuffer
        // will fail, the GLES driver won't queue a buffer, but SurfaceFlinger
        // will soldier on. So we end up here without a buffer. There should
        // be lots of scary messages in the log just before this.
        VDS_LOGE("advanceFrame: no buffer, bailing out");
        return NO_MEMORY;
    }

    sp<GraphicBuffer> fbBuffer = mProducerBuffers[mFbProducerSlot];
    sp<GraphicBuffer> outBuffer = mProducerBuffers[mOutputProducerSlot];
    VDS_LOGV("advanceFrame: fb=%d(%p) out=%d(%p)",
            mFbProducerSlot, fbBuffer.get(),
            mOutputProducerSlot, outBuffer.get());

    result = mHwc.fbPost(mDisplayId, mFbFence, fbBuffer);
    if (result == NO_ERROR) {
        result = mHwc.setOutputBuffer(mDisplayId, outFence, outBuffer);
    }

    return result;
}

void VirtualDisplaySurface::onFrameCommitted() {
    if (mDisplayId < 0)
        return;

    VDS_LOGW_IF(mDbgState != DBG_STATE_HWC,
            "Unexpected onFrameCommitted() in %s state", dbgStateStr());
    mDbgState = DBG_STATE_IDLE;

    sp<Fence> fbFence = mHwc.getAndResetReleaseFence(mDisplayId);
    if (mCompositionType == COMPOSITION_MIXED && mFbProducerSlot >= 0) {
        // release the scratch buffer back to the pool
        Mutex::Autolock lock(mMutex);
        int sslot = mapProducer2SourceSlot(SOURCE_SCRATCH, mFbProducerSlot);
        VDS_LOGV("onFrameCommitted: release scratch sslot=%d", sslot);
        addReleaseFenceLocked(sslot, mProducerBuffers[mFbProducerSlot], fbFence);
        releaseBufferLocked(sslot, mProducerBuffers[mFbProducerSlot],
                EGL_NO_DISPLAY, EGL_NO_SYNC_KHR);
    }

    if (mOutputProducerSlot >= 0) {
        int sslot = mapProducer2SourceSlot(SOURCE_SINK, mOutputProducerSlot);
        QueueBufferOutput qbo;
        sp<Fence> outFence = mHwc.getLastRetireFence(mDisplayId);
        VDS_LOGV("onFrameCommitted: queue sink sslot=%d", sslot);
        status_t result = mSource[SOURCE_SINK]->queueBuffer(sslot,
                QueueBufferInput(systemTime(),
                    Rect(mSinkBufferWidth, mSinkBufferHeight),
                    NATIVE_WINDOW_SCALING_MODE_FREEZE, 0, outFence),
                &qbo);
        if (result == NO_ERROR) {
            updateQueueBufferOutput(qbo);
        }
    }

    resetPerFrameState();
}

void VirtualDisplaySurface::dump(String8& result) const {
}

status_t VirtualDisplaySurface::requestBuffer(int pslot,
        sp<GraphicBuffer>* outBuf) {
    VDS_LOGW_IF(mDbgState != DBG_STATE_GLES,
            "Unexpected requestBuffer pslot=%d in %s state",
            pslot, dbgStateStr());

    *outBuf = mProducerBuffers[pslot];
    return NO_ERROR;
}

status_t VirtualDisplaySurface::setBufferCount(int bufferCount) {
    return mSource[SOURCE_SINK]->setBufferCount(bufferCount);
}

status_t VirtualDisplaySurface::dequeueBuffer(Source source,
        uint32_t format, int* sslot, sp<Fence>* fence) {
    status_t result = mSource[source]->dequeueBuffer(sslot, fence,
            mSinkBufferWidth, mSinkBufferHeight, format, mProducerUsage);
    if (result < 0)
        return result;
    int pslot = mapSource2ProducerSlot(source, *sslot);
    VDS_LOGV("dequeueBuffer(%s): sslot=%d pslot=%d result=%d",
            dbgSourceStr(source), *sslot, pslot, result);
    uint32_t sourceBit = static_cast<uint32_t>(source) << pslot;

    if ((mProducerSlotSource & (1u << pslot)) != sourceBit) {
        // This slot was previously dequeued from the other source; must
        // re-request the buffer.
        result |= BUFFER_NEEDS_REALLOCATION;
        mProducerSlotSource &= ~(1u << pslot);
        mProducerSlotSource |= sourceBit;
    }

    if (result & RELEASE_ALL_BUFFERS) {
        for (uint32_t i = 0; i < BufferQueue::NUM_BUFFER_SLOTS; i++) {
            if ((mProducerSlotSource & (1u << i)) == sourceBit)
                mProducerBuffers[i].clear();
        }
    }
    if (result & BUFFER_NEEDS_REALLOCATION) {
        mSource[source]->requestBuffer(*sslot, &mProducerBuffers[pslot]);
        VDS_LOGV("dequeueBuffer(%s): buffers[%d]=%p",
                dbgSourceStr(source), pslot, mProducerBuffers[pslot].get());
    }

    return result;
}

status_t VirtualDisplaySurface::dequeueBuffer(int* pslot, sp<Fence>* fence,
        uint32_t w, uint32_t h, uint32_t format, uint32_t usage) {
    VDS_LOGW_IF(mDbgState != DBG_STATE_PREPARED,
            "Unexpected dequeueBuffer() in %s state", dbgStateStr());
    mDbgState = DBG_STATE_GLES;

    VDS_LOGV("dequeueBuffer %dx%d fmt=%d usage=%#x", w, h, format, usage);

    mProducerUsage = usage | GRALLOC_USAGE_HW_COMPOSER;
    Source source = fbSourceForCompositionType(mCompositionType);
    if (source == SOURCE_SINK) {
        mSinkBufferWidth = w;
        mSinkBufferHeight = h;
    }

    int sslot;
    status_t result = dequeueBuffer(source, format, &sslot, fence);
    if (result >= 0) {
        *pslot = mapSource2ProducerSlot(source, sslot);
    }
    return result;
}

status_t VirtualDisplaySurface::queueBuffer(int pslot,
        const QueueBufferInput& input, QueueBufferOutput* output) {
    VDS_LOGW_IF(mDbgState != DBG_STATE_GLES,
            "Unexpected queueBuffer(pslot=%d) in %s state", pslot,
            dbgStateStr());
    mDbgState = DBG_STATE_GLES_DONE;

    VDS_LOGV("queueBuffer pslot=%d", pslot);

    status_t result;
    if (mCompositionType == COMPOSITION_MIXED) {
        // Queue the buffer back into the scratch pool
        QueueBufferOutput scratchQBO;
        int sslot = mapProducer2SourceSlot(SOURCE_SCRATCH, pslot);
        result = mBufferQueue->queueBuffer(sslot, input, &scratchQBO);
        if (result != NO_ERROR)
            return result;

        // Now acquire the buffer from the scratch pool -- should be the same
        // slot and fence as we just queued.
        Mutex::Autolock lock(mMutex);
        BufferQueue::BufferItem item;
        result = acquireBufferLocked(&item, 0);
        if (result != NO_ERROR)
            return result;
        VDS_LOGW_IF(item.mBuf != sslot,
                "queueBuffer: acquired sslot %d from SCRATCH after queueing sslot %d",
                item.mBuf, sslot);
        mFbProducerSlot = mapSource2ProducerSlot(SOURCE_SCRATCH, item.mBuf);
        mFbFence = mSlots[item.mBuf].mFence;

    } else {
        LOG_FATAL_IF(mCompositionType != COMPOSITION_GLES,
                "Unexpected queueBuffer in state %s for compositionType %s",
                dbgStateStr(), dbgCompositionTypeStr(mCompositionType));

        // Extract the GLES release fence for HWC to acquire
        int64_t timestamp;
        Rect crop;
        int scalingMode;
        uint32_t transform;
        input.deflate(&timestamp, &crop, &scalingMode, &transform,
                &mFbFence);

        mFbProducerSlot = pslot;
    }

    *output = mQueueBufferOutput;
    return NO_ERROR;
}

void VirtualDisplaySurface::cancelBuffer(int pslot, const sp<Fence>& fence) {
    VDS_LOGW_IF(mDbgState != DBG_STATE_GLES,
            "Unexpected cancelBuffer(pslot=%d) in %s state", pslot,
            dbgStateStr());
    VDS_LOGV("cancelBuffer pslot=%d", pslot);
    Source source = fbSourceForCompositionType(mCompositionType);
    return mSource[source]->cancelBuffer(
            mapProducer2SourceSlot(source, pslot), fence);
}

int VirtualDisplaySurface::query(int what, int* value) {
    return mSource[SOURCE_SINK]->query(what, value);
}

status_t VirtualDisplaySurface::setSynchronousMode(bool enabled) {
    return mSource[SOURCE_SINK]->setSynchronousMode(enabled);
}

status_t VirtualDisplaySurface::connect(int api, QueueBufferOutput* output) {
    QueueBufferOutput qbo;
    status_t result = mSource[SOURCE_SINK]->connect(api, &qbo);
    if (result == NO_ERROR) {
        updateQueueBufferOutput(qbo);
        *output = mQueueBufferOutput;
    }
    return result;
}

status_t VirtualDisplaySurface::disconnect(int api) {
    return mSource[SOURCE_SINK]->disconnect(api);
}

void VirtualDisplaySurface::updateQueueBufferOutput(
        const QueueBufferOutput& qbo) {
    uint32_t w, h, transformHint, numPendingBuffers;
    qbo.deflate(&w, &h, &transformHint, &numPendingBuffers);
    mQueueBufferOutput.inflate(w, h, 0, numPendingBuffers);
}

void VirtualDisplaySurface::resetPerFrameState() {
    mCompositionType = COMPOSITION_UNKNOWN;
    mSinkBufferWidth = 0;
    mSinkBufferHeight = 0;
    mFbFence = Fence::NO_FENCE;
    mFbProducerSlot = -1;
    mOutputProducerSlot = -1;
}

// This slot mapping function is its own inverse, so two copies are unnecessary.
// Both are kept to make the intent clear where the function is called, and for
// the (unlikely) chance that we switch to a different mapping function.
int VirtualDisplaySurface::mapSource2ProducerSlot(Source source, int sslot) {
    if (source == SOURCE_SCRATCH) {
        return BufferQueue::NUM_BUFFER_SLOTS - sslot - 1;
    } else {
        return sslot;
    }
}
int VirtualDisplaySurface::mapProducer2SourceSlot(Source source, int pslot) {
    return mapSource2ProducerSlot(source, pslot);
}

VirtualDisplaySurface::Source
VirtualDisplaySurface::fbSourceForCompositionType(CompositionType type) {
    return type == COMPOSITION_MIXED ? SOURCE_SCRATCH : SOURCE_SINK;
}

const char* VirtualDisplaySurface::dbgStateStr() const {
    switch (mDbgState) {
        case DBG_STATE_IDLE:      return "IDLE";
        case DBG_STATE_PREPARED:  return "PREPARED";
        case DBG_STATE_GLES:      return "GLES";
        case DBG_STATE_GLES_DONE: return "GLES_DONE";
        case DBG_STATE_HWC:       return "HWC";
        default:                  return "INVALID";
    }
}

const char* VirtualDisplaySurface::dbgSourceStr(Source s) {
    switch (s) {
        case SOURCE_SINK:    return "SINK";
        case SOURCE_SCRATCH: return "SCRATCH";
        default:             return "INVALID";
    }
}

// ---------------------------------------------------------------------------
} // namespace android
// ---------------------------------------------------------------------------