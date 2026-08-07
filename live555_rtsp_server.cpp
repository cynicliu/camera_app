#include <BasicUsageEnvironment.hh>
#include <GroupsockHelper.hh>
#include <H264VideoRTPSink.hh>
#include <H264VideoStreamFramer.hh>
#include <H265VideoRTPSink.hh>
#include <H265VideoStreamFramer.hh>
#include <MPEG4GenericRTPSink.hh>
#include <MPEG1or2AudioRTPSink.hh>
#include <OnDemandServerMediaSubsession.hh>
#include <RTSPServer.hh>

#include <algorithm>
#include <cstring>
#include <deque>
#include <new>
#include <string>
#include <vector>

#include <pthread.h>

#include "live555_rtsp_server.h"

struct QueuedFrame {
    std::vector<unsigned char> data;
    uint64_t pts_us;
    unsigned duration_us;
};

class QueueFramedSource;

class FrameQueue {
public:
    FrameQueue() : scheduler_(NULL), trigger_id_(0), source_(NULL),
                   max_frames_(1), duration_us_(0) {
        pthread_mutex_init(&mutex_, NULL);
    }

    ~FrameQueue() {
        shutdown();
        pthread_mutex_destroy(&mutex_);
    }

    void shutdown() {
        if (scheduler_ && trigger_id_)
            scheduler_->deleteEventTrigger(trigger_id_);
        trigger_id_ = 0;
        scheduler_ = NULL;
    }

    void initialize(TaskScheduler *scheduler, size_t max_frames,
                    unsigned duration_us) {
        scheduler_ = scheduler;
        max_frames_ = max_frames;
        duration_us_ = duration_us;
        trigger_id_ = scheduler_->createEventTrigger(deliverEvent);
    }

    void attach(QueueFramedSource *source) {
        pthread_mutex_lock(&mutex_);
        source_ = source;
        pthread_mutex_unlock(&mutex_);
    }

    void detach(QueueFramedSource *source) {
        pthread_mutex_lock(&mutex_);
        if (source_ == source) source_ = NULL;
        pthread_mutex_unlock(&mutex_);
    }

    int push(const void *data, size_t size, uint64_t pts_us) {
        QueuedFrame frame;
        if (!data || !size || !scheduler_ || !trigger_id_) return -1;
        try {
            const unsigned char *bytes = static_cast<const unsigned char *>(data);
            frame.data.assign(bytes, bytes + size);
            frame.pts_us = pts_us;
            frame.duration_us = duration_us_;
        } catch (...) {
            return -1;
        }
        pthread_mutex_lock(&mutex_);
        try {
            while (frames_.size() >= max_frames_) frames_.pop_front();
            frames_.push_back(frame);
        } catch (...) {
            pthread_mutex_unlock(&mutex_);
            return -1;
        }
        pthread_mutex_unlock(&mutex_);
        scheduler_->triggerEvent(trigger_id_, this);
        return 0;
    }

    bool pop(QueuedFrame& frame) {
        pthread_mutex_lock(&mutex_);
        if (frames_.empty()) {
            pthread_mutex_unlock(&mutex_);
            return false;
        }
        frame.data.swap(frames_.front().data);
        frame.pts_us = frames_.front().pts_us;
        frame.duration_us = frames_.front().duration_us;
        frames_.pop_front();
        pthread_mutex_unlock(&mutex_);
        return true;
    }

private:
    static void deliverEvent(void *client_data);

    TaskScheduler *scheduler_;
    EventTriggerId trigger_id_;
    QueueFramedSource *source_;
    size_t max_frames_;
    unsigned duration_us_;
    std::deque<QueuedFrame> frames_;
    pthread_mutex_t mutex_;
};

class QueueFramedSource : public FramedSource {
public:
    static QueueFramedSource *createNew(UsageEnvironment& env, FrameQueue& queue) {
        return new QueueFramedSource(env, queue);
    }

    void deliverFrame() {
        QueuedFrame frame;
        if (!isCurrentlyAwaitingData() || !queue_.pop(frame)) return;

        fFrameSize = std::min(static_cast<size_t>(fMaxSize), frame.data.size());
        fNumTruncatedBytes = frame.data.size() - fFrameSize;
        memmove(fTo, &frame.data[0], fFrameSize);
        fPresentationTime.tv_sec = frame.pts_us / 1000000;
        fPresentationTime.tv_usec = frame.pts_us % 1000000;
        fDurationInMicroseconds = frame.duration_us;
        FramedSource::afterGetting(this);
    }

protected:
    QueueFramedSource(UsageEnvironment& env, FrameQueue& queue)
        : FramedSource(env), queue_(queue) {
        queue_.attach(this);
    }

    virtual ~QueueFramedSource() {
        queue_.detach(this);
    }

private:
    virtual void doGetNextFrame() {
        deliverFrame();
    }

    FrameQueue& queue_;
};

void FrameQueue::deliverEvent(void *client_data) {
    FrameQueue *queue = static_cast<FrameQueue *>(client_data);
    pthread_mutex_lock(&queue->mutex_);
    QueueFramedSource *source = queue->source_;
    pthread_mutex_unlock(&queue->mutex_);
    if (source) source->deliverFrame();
}

class VideoQueueSubsession : public OnDemandServerMediaSubsession {
public:
    static VideoQueueSubsession *createNew(UsageEnvironment& env,
                                           FrameQueue& queue,
                                           live555_video_codec_t codec) {
        return new VideoQueueSubsession(env, queue, codec);
    }

protected:
    VideoQueueSubsession(UsageEnvironment& env, FrameQueue& queue,
                         live555_video_codec_t codec)
        : OnDemandServerMediaSubsession(env, True), queue_(queue), codec_(codec),
          aux_sdp_line_(NULL), done_flag_(0), dummy_sink_(NULL) {}

    virtual ~VideoQueueSubsession() {
        delete[] aux_sdp_line_;
    }

    virtual FramedSource *createNewStreamSource(unsigned, unsigned& bitrate) {
        bitrate = 2048;
        FramedSource *source = QueueFramedSource::createNew(envir(), queue_);
        if (codec_ == LIVE555_VIDEO_H264)
            return H264VideoStreamFramer::createNew(envir(), source);
        return H265VideoStreamFramer::createNew(envir(), source);
    }

    virtual RTPSink *createNewRTPSink(Groupsock *groupsock,
                                      unsigned char payload_type,
                                      FramedSource *) {
        if (codec_ == LIVE555_VIDEO_H264)
            return H264VideoRTPSink::createNew(envir(), groupsock, payload_type);
        return H265VideoRTPSink::createNew(envir(), groupsock, payload_type);
    }

    virtual char const *getAuxSDPLine(RTPSink *sink, FramedSource *source) {
        if (aux_sdp_line_) return aux_sdp_line_;
        if (!dummy_sink_) {
            dummy_sink_ = sink;
            dummy_sink_->startPlaying(*source, afterPlayingDummy, this);
            checkForAuxSDPLine(this);
        }
        envir().taskScheduler().doEventLoop(&done_flag_);
        return aux_sdp_line_;
    }

private:
    static void afterPlayingDummy(void *client_data) {
        VideoQueueSubsession *self = static_cast<VideoQueueSubsession *>(client_data);
        self->envir().taskScheduler().unscheduleDelayedTask(self->nextTask());
        self->done_flag_ = ~0;
    }

    static void checkForAuxSDPLine(void *client_data) {
        VideoQueueSubsession *self = static_cast<VideoQueueSubsession *>(client_data);
        self->nextTask() = NULL;
        const char *line;
        if (self->aux_sdp_line_) {
            self->done_flag_ = ~0;
        } else if (self->dummy_sink_ &&
                   (line = self->dummy_sink_->auxSDPLine()) != NULL) {
            self->aux_sdp_line_ = strDup(line);
            self->dummy_sink_ = NULL;
            self->done_flag_ = ~0;
        } else if (!self->done_flag_) {
            self->nextTask() = self->envir().taskScheduler().scheduleDelayedTask(
                100000, checkForAuxSDPLine, self);
        }
    }

    FrameQueue& queue_;
    live555_video_codec_t codec_;
    char *aux_sdp_line_;
    char done_flag_;
    RTPSink *dummy_sink_;
};

class AudioQueueSubsession : public OnDemandServerMediaSubsession {
public:
    static AudioQueueSubsession *createNew(UsageEnvironment& env,
                                         FrameQueue& queue,
                                         live555_audio_codec_t codec,
                                         unsigned sample_rate,
                                         unsigned channels,
                                         const char *config) {
        return new AudioQueueSubsession(env, queue, codec, sample_rate, channels,
                                      config);
    }

protected:
    AudioQueueSubsession(UsageEnvironment& env, FrameQueue& queue,
                       live555_audio_codec_t codec,
                       unsigned sample_rate, unsigned channels,
                       const char *config)
        : OnDemandServerMediaSubsession(env, True), queue_(queue), codec_(codec),
          sample_rate_(sample_rate), channels_(channels), config_(config) {}

    virtual FramedSource *createNewStreamSource(unsigned, unsigned& bitrate) {
        bitrate = 32;
        return QueueFramedSource::createNew(envir(), queue_);
    }

    virtual RTPSink *createNewRTPSink(Groupsock *groupsock,
                                      unsigned char payload_type,
                                      FramedSource *) {
        if (codec_ == LIVE555_AUDIO_MP3)
            return MPEG1or2AudioRTPSink::createNew(envir(), groupsock);
        return MPEG4GenericRTPSink::createNew(
            envir(), groupsock, payload_type, sample_rate_, "audio", "AAC-hbr",
            config_.c_str(), channels_);
    }

private:
    FrameQueue& queue_;
    live555_audio_codec_t codec_;
    unsigned sample_rate_;
    unsigned channels_;
    std::string config_;
};

struct live555_rtsp_server {
    int port;
    std::string path;
    live555_video_codec_t video_codec;
    live555_audio_codec_t audio_codec;
    unsigned audio_sample_rate;
    unsigned audio_channels;
    std::string audio_config;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool initialized;
    bool init_ok;
    char stop_flag;
    TaskScheduler *scheduler;
    UsageEnvironment *environment;
    RTSPServer *rtsp_server;
    EventTriggerId stop_trigger;
    FrameQueue video_queue;
    FrameQueue audio_queue;
};

static void stopEvent(void *client_data) {
    live555_rtsp_server *server = static_cast<live555_rtsp_server *>(client_data);
    server->stop_flag = 1;
}

static void *serverThread(void *opaque) {
    live555_rtsp_server *server = static_cast<live555_rtsp_server *>(opaque);
    server->scheduler = BasicTaskScheduler::createNew();
    server->environment = BasicUsageEnvironment::createNew(*server->scheduler);
    OutPacketBuffer::maxSize = 2 * 1024 * 1024;
    server->rtsp_server = RTSPServer::createNew(*server->environment,
                                                server->port, NULL);
    if (server->rtsp_server) {
        std::string stream_name = server->path;
        while (!stream_name.empty() && stream_name[0] == '/')
            stream_name.erase(0, 1);
        if (stream_name.empty()) stream_name = "live/0";

        server->video_queue.initialize(server->scheduler, 8, 1000000 / 25);
        server->audio_queue.initialize(server->scheduler, 32,
                                       (server->audio_codec == LIVE555_AUDIO_AAC
                                            ? 1024 : 576) * 1000000 /
                                       server->audio_sample_rate);
        ServerMediaSession *session = ServerMediaSession::createNew(
            *server->environment, stream_name.c_str(), stream_name.c_str(),
            "H.264/H.265 video with AAC/MP3 audio");
        session->addSubsession(VideoQueueSubsession::createNew(
            *server->environment, server->video_queue, server->video_codec));
        session->addSubsession(AudioQueueSubsession::createNew(
            *server->environment, server->audio_queue, server->audio_codec,
            server->audio_sample_rate, server->audio_channels,
            server->audio_config.c_str()));
        server->rtsp_server->addServerMediaSession(session);
        server->stop_trigger = server->scheduler->createEventTrigger(stopEvent);
    }

    pthread_mutex_lock(&server->mutex);
    server->init_ok = server->rtsp_server != NULL && server->stop_trigger != 0;
    server->initialized = true;
    pthread_cond_signal(&server->condition);
    pthread_mutex_unlock(&server->mutex);

    if (server->init_ok)
        server->scheduler->doEventLoop(&server->stop_flag);

    if (server->stop_trigger)
        server->scheduler->deleteEventTrigger(server->stop_trigger);
    if (server->rtsp_server) Medium::close(server->rtsp_server);
    server->video_queue.shutdown();
    server->audio_queue.shutdown();
    if (server->environment) server->environment->reclaim();
    delete server->scheduler;
    server->scheduler = NULL;
    server->environment = NULL;
    server->rtsp_server = NULL;
    return NULL;
}

extern "C" int live555_rtsp_server_start(live555_rtsp_server_t **result,
                                          int port, const char *path,
                                          live555_video_codec_t video_codec,
                                          live555_audio_codec_t audio_codec,
                                          unsigned audio_sample_rate,
                                          unsigned audio_channels,
                                          const char *audio_config) {
    if (!result || !path || !audio_config || port < 1 || port > 65535 ||
        (video_codec != LIVE555_VIDEO_H264 &&
         video_codec != LIVE555_VIDEO_H265) ||
        (audio_codec != LIVE555_AUDIO_AAC &&
         audio_codec != LIVE555_AUDIO_MP3) ||
        !audio_sample_rate || !audio_channels)
        return -1;

    live555_rtsp_server *server = new (std::nothrow) live555_rtsp_server;
    if (!server) return -1;
    server->port = port;
    server->path = path;
    server->video_codec = video_codec;
    server->audio_codec = audio_codec;
    server->audio_sample_rate = audio_sample_rate;
    server->audio_channels = audio_channels;
    server->audio_config = audio_config;
    server->initialized = false;
    server->init_ok = false;
    server->stop_flag = 0;
    server->scheduler = NULL;
    server->environment = NULL;
    server->rtsp_server = NULL;
    server->stop_trigger = 0;
    pthread_mutex_init(&server->mutex, NULL);
    pthread_cond_init(&server->condition, NULL);

    if (pthread_create(&server->thread, NULL, serverThread, server) != 0) {
        pthread_cond_destroy(&server->condition);
        pthread_mutex_destroy(&server->mutex);
        delete server;
        return -1;
    }
    pthread_mutex_lock(&server->mutex);
    while (!server->initialized)
        pthread_cond_wait(&server->condition, &server->mutex);
    bool ok = server->init_ok;
    pthread_mutex_unlock(&server->mutex);
    if (!ok) {
        pthread_join(server->thread, NULL);
        pthread_cond_destroy(&server->condition);
        pthread_mutex_destroy(&server->mutex);
        delete server;
        return -1;
    }
    *result = server;
    return 0;
}

extern "C" void live555_rtsp_server_stop(live555_rtsp_server_t *server) {
    if (!server) return;
    server->scheduler->triggerEvent(server->stop_trigger, server);
    pthread_join(server->thread, NULL);
    pthread_cond_destroy(&server->condition);
    pthread_mutex_destroy(&server->mutex);
    delete server;
}

extern "C" int live555_rtsp_server_push_video(live555_rtsp_server_t *server,
                                               const void *data, size_t size,
                                               uint64_t pts_us) {
    return server ? server->video_queue.push(data, size, pts_us) : -1;
}

extern "C" int live555_rtsp_server_push_audio(live555_rtsp_server_t *server,
                                               const void *data, size_t size,
                                               uint64_t pts_us) {
    return server ? server->audio_queue.push(data, size, pts_us) : -1;
}
