#include "frame_queue.h"

void frame_queue_init(FrameQueue *q) {
    memset(q, 0, sizeof(*q));       //将 q 指向的整个 FrameQueue 结构体的所有成员变量强制初始化为 0；
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);     //条件变量初始化
    int fps = VIDEO_FPS;
    if (fps <= 0 || fps > 60) fps = 25;
    q->drop_until_keyframe = 0;
    q->push_delay_us = 5000;
}

int frame_queue_get_count(FrameQueue *q) {
    pthread_mutex_lock(&q->lock);
    int c = q->count;
    pthread_mutex_unlock(&q->lock);
    return c;
}

void frame_queue_clean(FrameQueue *q) {
    pthread_mutex_lock(&q->lock);
    for (int i=0;i<MAX_FRAME_QUEUE;i++) {
        if (q->frames[i].data) { free(q->frames[i].data); q->frames[i].data=NULL; }
    }
    q->front = q->rear = q->count = 0;
    q->drop_count = 0;
    q->drop_until_keyframe = 0;
    int fps = VIDEO_FPS; if (fps<=0 || fps>60) fps=25;
    q->drop_until_keyframe = 0;
    q->push_delay_us = 5000;
    pthread_mutex_unlock(&q->lock);
}

int frame_queue_push(FrameQueue *q, const uint8_t *data, int size, int64_t pts, int is_keyframe) {
    if (!data || size<=0) return -1;

    int need_throttle = 0;
    int drop_frame = 0;

    pthread_mutex_lock(&q->lock);

    // If we are recovering from congestion, only accept next keyframe
    if (q->drop_until_keyframe && !is_keyframe) {
        q->drop_count++;
        pthread_mutex_unlock(&q->lock);
        return 1;
    }

    if (q->count >= MAX_FRAME_QUEUE*9/10) {
        // Prefer dropping non-keyframes instead of throttling producer, to avoid accumulating latency
        if (!is_keyframe) {
            q->drop_count++;
            pthread_mutex_unlock(&q->lock);
            return 1;
        }
        need_throttle = 1;
        if (q->push_delay_us < 100000) q->push_delay_us += 2000;
    } else if (q->count <= MAX_FRAME_QUEUE/4) {
        int base = 1000000 / (VIDEO_FPS<=0?25:VIDEO_FPS);
        if (q->push_delay_us > base) q->push_delay_us = base;
    }

    if (q->count >= MAX_FRAME_QUEUE) {
        // Hard congestion: flush queue to keep realtime, then wait for next keyframe
        for (int i=0;i<MAX_FRAME_QUEUE;i++) {
            if (q->frames[i].data) { free(q->frames[i].data); q->frames[i].data=NULL; }
            memset(&q->frames[i], 0, sizeof(q->frames[i]));
        }
        q->front = q->rear = q->count = 0;
        q->drop_until_keyframe = 1;
        q->drop_count++;
        if (!is_keyframe) {
            pthread_mutex_unlock(&q->lock);
            return 1;
        }
        // This frame is a keyframe: accept it and exit recovery mode
        q->drop_until_keyframe = 0;
        drop_frame = 0;
    }

    FrameData *f = &q->frames[q->rear];
    f->data = (uint8_t*)malloc((size_t)size);
    if (!f->data) { pthread_mutex_unlock(&q->lock); return -1; }
    memcpy(f->data, data, (size_t)size);
    f->size = size;
    f->pts = pts;
    f->is_keyframe = is_keyframe;

    q->rear = (q->rear + 1) % MAX_FRAME_QUEUE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);

    if (need_throttle && !drop_frame) usleep((useconds_t)q->push_delay_us);
    return drop_frame ? 1 : 0;
}

int frame_queue_pop(FrameQueue *q, FrameData *out, int timeout_ms) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t total_nsec = (uint64_t)tv.tv_sec * 1000000000ULL +
                          (uint64_t)tv.tv_usec * 1000ULL +
                          (uint64_t)timeout_ms * 1000000ULL;
    struct timespec ts;
    ts.tv_sec = (time_t)(total_nsec / 1000000000ULL);
    ts.tv_nsec = (long)(total_nsec % 1000000000ULL);

    pthread_mutex_lock(&q->lock);
    while (q->count == 0) {
        int ret = pthread_cond_timedwait(&q->not_empty, &q->lock, &ts);
        if (ret == ETIMEDOUT || !atomic_load(&g_state.running)) {
            pthread_mutex_unlock(&q->lock);
            return -1;
        }
    }

    FrameData *src = &q->frames[q->front];
    if (out->data) free(out->data);
    *out = *src;
    memset(src, 0, sizeof(*src));

    q->front = (q->front + 1) % MAX_FRAME_QUEUE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

void frame_queue_clear(FrameQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->lock);
    q->front = 0;
    q->rear = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->lock);
}
