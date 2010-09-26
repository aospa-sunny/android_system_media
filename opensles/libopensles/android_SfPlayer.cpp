/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "android_SfPlayer.h"

#include <stdio.h>
#include "SLES/OpenSLES.h"
#include "SLES/OpenSLES_Android.h"
#include "sllog.h"
#include <stdlib.h>

namespace android {

SfPlayer::SfPlayer()
    : mAudioTrack(NULL),
      mFlags(0),
      mBitrate(-1),
      mNumChannels(1),
      mSampleRateHz(0),
      mTimeDelta(-1),
      mDurationUsec(-1),
      mCacheStatus(kStatusEmpty),
      mSeekTimeMsec(0),
      mLastDecodedPositionUs(-1),
      mCacheFill(0),
      mLastNotifiedCacheFill(0),
      mCacheFillNotifThreshold(100),
      mDataLocatorType(kDataLocatorNone),
      mNotifyClient(NULL),
      mNotifyUser(NULL),
      mDecodeBuffer(NULL) {

      mRenderLooper = new android::ALooper();
}


SfPlayer::~SfPlayer() {
    SL_LOGV("SfPlayer::~SfPlayer()");

    mRenderLooper->stop();
    mRenderLooper->unregisterHandler(this->id());
    mRenderLooper.clear();

    if (mAudioSource != NULL) {
        {
            // don't even think about stopping the media source without releasing the decode buffer
            Mutex::Autolock _l(mDecodeBufferLock);
            if (NULL != mDecodeBuffer) {
                mDecodeBuffer->release();
                mDecodeBuffer = NULL;
            }
        }
        mAudioSource->stop();
    }

    resetDataLocator();
}

void SfPlayer::armLooper() {
    mRenderLooper->registerHandler(this);
    mRenderLooper->start(false /*runOnCallingThread*/, false /*canCallJava*/,
            ANDROID_PRIORITY_AUDIO);
}

void SfPlayer::useAudioTrack(AudioTrack* pTrack) {
    mAudioTrack = pTrack;
}

void SfPlayer::setNotifListener(const notif_client_t cbf, void* notifUser) {
    mNotifyClient = cbf;
    mNotifyUser = notifUser;
}


void SfPlayer::notifyStatus() {
    sp<AMessage> msg = new AMessage(kWhatNotif, id());
    msg->setInt32(EVENT_PREFETCHSTATUSCHANGE, (int32_t)mCacheStatus);
    notify(msg, true /*async*/);
}


void SfPlayer::notifyCacheFill() {
    sp<AMessage> msg = new AMessage(kWhatNotif, id());
    mLastNotifiedCacheFill = mCacheFill;
    msg->setInt32(EVENT_PREFETCHFILLLEVELUPDATE, (int32_t)mLastNotifiedCacheFill);
    notify(msg, true /*async*/);
}


void SfPlayer::notify(const sp<AMessage> &msg, bool async) {
    if (async) {
        msg->post();
    } else {
        onNotify(msg);
    }
}


void SfPlayer::setDataSource(const char *uri) {
    resetDataLocator();

    size_t len = strlen((const char *) uri);
    char* newUri = (char*) malloc(len + 1);
    if (NULL == newUri) {
        // mem issue
        SL_LOGE("SfPlayer::setDataSource: not enough memory to allocator URI string");
        return;
    }
    memcpy(newUri, uri, len + 1);
    mDataLocator.uri = newUri;

    mDataLocatorType = kDataLocatorUri;
}

void SfPlayer::setDataSource(const int fd, const int64_t offset, const int64_t length) {
    resetDataLocator();

    mDataLocator.fdi.fd = fd;

    struct stat sb;
    int ret = fstat(fd, &sb);
    if (ret != 0) {
        // sockets are not supported
        SL_LOGE("SfPlayer::setDataSource: fstat(%d) failed: %d, %s", fd, ret, strerror(errno));
        return;
    }

    if (offset >= sb.st_size) {
        SL_LOGE("SfPlayer::setDataSource: invalid offset");
        return;
    }
    mDataLocator.fdi.offset = offset;

    if (SFPLAYER_FD_FIND_FILE_SIZE == length) {
        mDataLocator.fdi.length = sb.st_size;
    } else if (offset + length > sb.st_size) {
        mDataLocator.fdi.length = sb.st_size - offset;
    } else {
        mDataLocator.fdi.length = length;
    }

    mDataLocatorType = kDataLocatorFd;
}

void SfPlayer::prepare_async() {
    //SL_LOGV("SfPlayer::prepare_async()");
    sp<AMessage> msg = new AMessage(kWhatPrepare, id());
    msg->post();
}

int SfPlayer::prepare_sync() {
    //SL_LOGV("SfPlayer::prepare_sync()");
    sp<AMessage> msg = new AMessage(kWhatPrepare, id());
    return onPrepare(msg);
}

int SfPlayer::onPrepare(const sp<AMessage> &msg) {
    //SL_LOGV("SfPlayer::onPrepare");
    sp<DataSource> dataSource;

    switch (mDataLocatorType) {

        case kDataLocatorNone:
            SL_LOGE("SfPlayer::onPrepare: no data locator set");
            return MEDIA_ERROR_BASE;
            break;

        case kDataLocatorUri:
            if (!strncasecmp(mDataLocator.uri, "http://", 7)) {
                sp<NuHTTPDataSource> http = new NuHTTPDataSource;
                if (http->connect(mDataLocator.uri) == OK) {
                    dataSource =
                        new NuCachedSource2(
                                new ThrottledSource(
                                        http, 50 * 1024 /* bytes/sec */));
                }
            } else {
                dataSource = DataSource::CreateFromURI(mDataLocator.uri);
            }
            break;

        case kDataLocatorFd: {
            dataSource = new FileSource(
                    mDataLocator.fdi.fd, mDataLocator.fdi.offset, mDataLocator.fdi.length);
            status_t err = dataSource->initCheck();
            if (err != OK) {
                return err;
            }
            }
            break;

        default:
            TRESPASS();
    }

    if (dataSource == NULL) {
        SL_LOGE("SfPlayer::onPrepare: Could not create data source.");
        return ERROR_UNSUPPORTED;
    }

    sp<MediaExtractor> extractor = MediaExtractor::Create(dataSource);
    if (extractor == NULL) {
        SL_LOGE("SfPlayer::onPrepare: Could not instantiate extractor.");
        return ERROR_UNSUPPORTED;
    }

    ssize_t audioTrackIndex = -1;
    bool isRawAudio = false;
    for (size_t i = 0; i < extractor->countTracks(); ++i) {
        sp<MetaData> meta = extractor->getTrackMetaData(i);

        const char *mime;
        CHECK(meta->findCString(kKeyMIMEType, &mime));

        if (!strncasecmp("audio/", mime, 6)) {
            audioTrackIndex = i;

            if (!strcasecmp(MEDIA_MIMETYPE_AUDIO_RAW, mime)) {
                isRawAudio = true;
            }
            break;
        }
    }

    if (audioTrackIndex < 0) {
        SL_LOGE("SfPlayer::onPrepare: Could not find an audio track.");
        return ERROR_UNSUPPORTED;
    }

    sp<MediaSource> source = extractor->getTrack(audioTrackIndex);
    sp<MetaData> meta = source->getFormat();

    off_t size;
    int64_t durationUs;
    if (dataSource->getSize(&size) == OK
            && meta->findInt64(kKeyDuration, &durationUs)) {
        mBitrate = size * 8000000ll / durationUs;  // in bits/sec
        mDurationUsec = durationUs;
    } else {
        mBitrate = -1;
        mDurationUsec = -1;
    }

    if (!isRawAudio) {
        OMXClient client;
        CHECK_EQ(client.connect(), (status_t)OK);

        source = OMXCodec::Create(
                client.interface(), meta, false /* createEncoder */,
                source);

        if (source == NULL) {
            SL_LOGE("SfPlayer::onPrepare: Could not instantiate decoder.");
            return ERROR_UNSUPPORTED;
        }

        meta = source->getFormat();
    }

    if (source->start() != OK) {
        SL_LOGE("SfPlayer::onPrepare: Failed to start source/decoder.");
        return MEDIA_ERROR_BASE;
    }

    mDataSource = dataSource;
    mAudioSource = source;

    CHECK(meta->findInt32(kKeyChannelCount, &mNumChannels));
    CHECK(meta->findInt32(kKeySampleRate, &mSampleRateHz));

    if (!wantPrefetch()) {
        SL_LOGV("SfPlayer::onPrepare: no need to prefetch");
        // doesn't need prefetching, notify good to go
        mCacheStatus = kStatusHigh;
        mCacheFill = 1000;
        notifyStatus();
        notifyCacheFill();
    }

    //SL_LOGV("SfPlayer::onPrepare: end");
    return SFPLAYER_SUCCESS;

}


bool SfPlayer::wantPrefetch() {
    return (mDataSource->flags() & DataSource::kWantsPrefetching);
}


void SfPlayer::startPrefetch_async() {
    //SL_LOGV("SfPlayer::startPrefetch_async()");
    if (wantPrefetch()) {
        //SL_LOGV("SfPlayer::startPrefetch_async(): sending check cache msg");

        mFlags |= kFlagPreparing;
        mFlags |= kFlagBuffering;

        (new AMessage(kWhatCheckCache, id()))->post(100000);
    }
}


void SfPlayer::play() {
    SL_LOGV("SfPlayer::play");
    if (NULL == mAudioTrack) {
        return;
    }

    mAudioTrack->start();

    (new AMessage(kWhatPlay, id()))->post();
    (new AMessage(kWhatDecode, id()))->post();
}


void SfPlayer::stop() {
    SL_LOGV("SfPlayer::stop");

    if (NULL == mAudioTrack) {
        return;
    }
    mAudioTrack->stop();

    (new AMessage(kWhatPause, id()))->post();

    // after a stop, playback should resume from the start.
    seek(0);
}

void SfPlayer::pause() {
    SL_LOGV("SfPlayer::pause");
    if (NULL == mAudioTrack) {
        return;
    }
    (new AMessage(kWhatPause, id()))->post();
    mAudioTrack->pause();
}

void SfPlayer::seek(int64_t timeMsec) {
    SL_LOGV("SfPlayer::seek %lld", timeMsec);
    sp<AMessage> msg = new AMessage(kWhatSeek, id());
    msg->setInt64("seek", timeMsec);
    msg->post();
}


uint32_t SfPlayer::getPositionMsec() {
    Mutex::Autolock _l(mSeekLock);
    if (mFlags & kFlagSeeking) {
        return (uint32_t) mSeekTimeMsec;
    } else {
        if (mLastDecodedPositionUs < 0) {
            return 0;
        } else {
            return (uint32_t) (mLastDecodedPositionUs / 1000);
        }
    }
}


int64_t SfPlayer::getPositionUsec() {
    Mutex::Autolock _l(mSeekLock);
    if (mFlags & kFlagSeeking) {
        return mSeekTimeMsec * 1000;
    } else {
        if (mLastDecodedPositionUs < 0) {
            return 0;
        } else {
            return mLastDecodedPositionUs;
        }
    }
}

/*
 * Message handlers
 */

void SfPlayer::onPlay() {
    SL_LOGV("SfPlayer::onPlay");
    mFlags |= kFlagPlaying;
}

void SfPlayer::onPause() {
    SL_LOGV("SfPlayer::onPause");
    mFlags &= ~kFlagPlaying;
}

void SfPlayer::onSeek(const sp<AMessage> &msg) {
    SL_LOGV("SfPlayer::onSeek");
    int64_t timeMsec;
    CHECK(msg->findInt64("seek", &timeMsec));

    Mutex::Autolock _l(mSeekLock);
    mFlags |= kFlagSeeking;
    mSeekTimeMsec = timeMsec;
    mTimeDelta = -1;
    mLastDecodedPositionUs = -1;
}


void SfPlayer::onDecode() {
    //SL_LOGV("SfPlayer::onDecode");
    bool eos;
    if ((mDataSource->flags() & DataSource::kWantsPrefetching)
            && (getCacheRemaining(&eos) == kStatusLow)
            && !eos) {
        SL_LOGV("buffering more.");

        if (mFlags & kFlagPlaying) {
            mAudioTrack->pause();
        }
        mFlags |= kFlagBuffering;
        (new AMessage(kWhatCheckCache, id()))->post(100000);
        return;
    }

    if (!(mFlags & (kFlagPlaying | kFlagBuffering | kFlagPreparing))) {
        // don't decode if we're not buffering, prefetching or playing
        //SL_LOGV("don't decode: not buffering, prefetching or playing");
        return;
    }

    status_t err;
    MediaSource::ReadOptions readOptions;
    if (mFlags & kFlagSeeking) {
        readOptions.setSeekTo(mSeekTimeMsec * 1000);
    }

    {
        Mutex::Autolock _l(mDecodeBufferLock);
        if (NULL != mDecodeBuffer) {
            // the current decoded buffer hasn't been rendered, drop it
            mDecodeBuffer->release();
            mDecodeBuffer = NULL;
        }
        err = mAudioSource->read(&mDecodeBuffer, &readOptions);
        if (err == OK) {
            CHECK(mDecodeBuffer->meta_data()->findInt64(kKeyTime, &mLastDecodedPositionUs));
        }
    }

    if (err != OK) {
        if (err != ERROR_END_OF_STREAM) {
            SL_LOGE("MediaSource::read returned error %d", err);
            // FIXME handle error
        } else {
            //SL_LOGV("SfPlayer::onDecode hit ERROR_END_OF_STREAM");
            if (mFlags & kFlagPlaying) {
                //SL_LOGV("SfPlayer::onDecode hit ERROR_END_OF_STREAM while playing");
                // async notification of end of stream reached during playback
                sp<AMessage> msg = new AMessage(kWhatNotif, id());
                msg->setInt32(EVENT_ENDOFSTREAM, 1);
                notify(msg, true /*async*/);
            }
        }
        return;
    }

    {
        Mutex::Autolock _l(mSeekLock);
        if (mFlags & kFlagSeeking) {
            mFlags &= ~kFlagSeeking;
        }
    }

    sp<AMessage> msg = new AMessage(kWhatRender, id());

    if (mTimeDelta < 0) {
        mTimeDelta = ALooper::GetNowUs() - mLastDecodedPositionUs;
    }

    int64_t delayUs = mLastDecodedPositionUs + mTimeDelta - ALooper::GetNowUs();

    msg->post(delayUs); // negative delays are ignored
    //SL_LOGV("timeUs=%lld, mTimeDelta=%lld, delayUs=%lld",
    //mLastDecodedPositionUs, mTimeDelta, delayUs);
}


void SfPlayer::onRender(const sp<AMessage> &msg) {
    //SL_LOGV("SfPlayer::onRender");

    Mutex::Autolock _l(mDecodeBufferLock);

    if (NULL == mDecodeBuffer) {
        // nothing to render, move along
        return;
    }

    if (mFlags & kFlagPlaying) {
        mAudioTrack->write( (const uint8_t *)mDecodeBuffer->data() + mDecodeBuffer->range_offset(),
                mDecodeBuffer->range_length());
        (new AMessage(kWhatDecode, id()))->post();
    }

    mDecodeBuffer->release();
    mDecodeBuffer = NULL;

}


void SfPlayer::onCheckCache(const sp<AMessage> &msg) {
    //SL_LOGV("SfPlayer::onCheckCache");
    bool eos;
    CacheStatus status = getCacheRemaining(&eos);

    if (eos || status == kStatusHigh
            || ((mFlags & kFlagPreparing) && (status >= kStatusEnough))) {
        if (mFlags & kFlagPlaying) {
            mAudioTrack->start();
        }
        mFlags &= ~kFlagBuffering;

        SL_LOGV("SfPlayer::onCheckCache: buffering done.");

        if (mFlags & kFlagPreparing) {
            //SL_LOGV("SfPlayer::onCheckCache: preparation done.");
            mFlags &= ~kFlagPreparing;
        }

        mTimeDelta = -1;
        if (mFlags & kFlagPlaying) {
            (new AMessage(kWhatDecode, id()))->post();
        }
        return;
    }

    msg->post(100000);
}

void SfPlayer::onNotify(const sp<AMessage> &msg) {
    if (NULL == mNotifyClient) {
        return;
    }
    int32_t cacheInfo;
    if (msg->findInt32(EVENT_PREFETCHSTATUSCHANGE, &cacheInfo)) {
        SL_LOGV("\tSfPlayer notifying %s = %d", EVENT_PREFETCHSTATUSCHANGE, cacheInfo);
        mNotifyClient(kEventPrefetchStatusChange, cacheInfo, mNotifyUser);
    }
    if (msg->findInt32(EVENT_PREFETCHFILLLEVELUPDATE, &cacheInfo)) {
        SL_LOGV("\tSfPlayer notifying %s = %d", EVENT_PREFETCHFILLLEVELUPDATE, cacheInfo);
        mNotifyClient(kEventPrefetchFillLevelUpdate, cacheInfo, mNotifyUser);
    }
    if (msg->findInt32(EVENT_ENDOFSTREAM, &cacheInfo)) {
        SL_LOGV("\tSfPlayer notifying %s = %d", EVENT_ENDOFSTREAM, cacheInfo);
        mNotifyClient(kEventEndOfStream, cacheInfo, mNotifyUser);
    }
}

SfPlayer::CacheStatus SfPlayer::getCacheRemaining(bool *eos) {
    sp<NuCachedSource2> cachedSource =
        static_cast<NuCachedSource2 *>(mDataSource.get());

    CacheStatus oldStatus = mCacheStatus;

    size_t dataRemaining = cachedSource->approxDataRemaining(eos);

    CHECK_GE(mBitrate, 0);

    int64_t dataRemainingUs = dataRemaining * 8000000ll / mBitrate;

    //SL_LOGV("SfPlayer::getCacheRemaining: approx %.2f secs remaining (eos=%d)",
    //       dataRemainingUs / 1E6, *eos);

    if (*eos) {
        // data is buffered up to the end of the stream, it can't get any better than this
        mCacheStatus = kStatusHigh;
        mCacheFill = 1000;

    } else {
        if (mDurationUsec > 0) {
            // known duration:

            //   fill level is ratio of how much has been played + how much is
            //   cached, divided by total duration
            uint32_t currentPositionUsec = getPositionUsec();
            mCacheFill = (int16_t) ((1000.0
                    * (double)(currentPositionUsec + dataRemainingUs) / mDurationUsec));
            //SL_LOGV("cacheFill = %d", mCacheFill);

            //   cache status is evaluated against duration thresholds
            if (dataRemainingUs > DURATION_CACHED_HIGH_US) {
                mCacheStatus = kStatusHigh;
                //LOGV("high");
            } else if (dataRemainingUs > DURATION_CACHED_MED_US) {
                //LOGV("enough");
                mCacheStatus = kStatusEnough;
            } else if (dataRemainingUs < DURATION_CACHED_LOW_US) {
                //LOGV("low");
                mCacheStatus = kStatusLow;
            } else {
                mCacheStatus = kStatusIntermediate;
            }

        } else {
            // unknown duration:

            //   cache status is evaluated against cache amount thresholds
            //   (no duration so we don't have the bitrate either, could be derived from format?)
            if (dataRemaining > SIZE_CACHED_HIGH_BYTES) {
                mCacheStatus = kStatusHigh;
            } else if (dataRemaining > SIZE_CACHED_MED_BYTES) {
                mCacheStatus = kStatusEnough;
            } else if (dataRemaining < SIZE_CACHED_LOW_BYTES) {
                mCacheStatus = kStatusLow;
            } else {
                mCacheStatus = kStatusIntermediate;
            }
        }

    }

    if (oldStatus != mCacheStatus) {
        notifyStatus();
    }

    if (abs(mCacheFill - mLastNotifiedCacheFill) > mCacheFillNotifThreshold) {
        notifyCacheFill();
    }

    return mCacheStatus;
}


/*
 * post-condition: mDataLocatorType == kDataLocatorNone
 *
 */
void SfPlayer::resetDataLocator() {
    if (kDataLocatorUri == mDataLocatorType) {
        if (NULL != mDataLocator.uri) {
            free(mDataLocator.uri);
            mDataLocator.uri = NULL;
        }
    }
    mDataLocatorType = kDataLocatorNone;
}


void SfPlayer::onMessageReceived(const sp<AMessage> &msg) {
    switch (msg->what()) {
        case kWhatPrepare:
            onPrepare(msg);
            break;

        case kWhatDecode:
            onDecode();
            break;

        case kWhatRender:
            onRender(msg);
            break;

        case kWhatCheckCache:
            onCheckCache(msg);
            break;

        case kWhatNotif:
            onNotify(msg);
            break;

        case kWhatPlay:
            onPlay();
            break;

        case kWhatPause:
            onPause();
            break;

        case kWhatSeek:
            onSeek(msg);
            break;

        default:
            TRESPASS();
    }
}

}  // namespace android
