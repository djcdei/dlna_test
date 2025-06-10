#include "player.h"
#include <gst/gst.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

#define LOG_PREFIX "[PLAYER] "
#define LOG_INFO(fmt, ...) printf(LOG_PREFIX "[INFO] " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, LOG_PREFIX "[ERROR] " fmt "\n", ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) printf(LOG_PREFIX "[DEBUG] " fmt "\n", ##__VA_ARGS__)

static GstElement *pipeline = NULL;
static GstElement *audio_sink = NULL;
static volatile int playing = 0;
static volatile int paused = 0;
static gint64 duration_ns = 0;
static gint64 position_ns = 0;
static int current_volume = 50;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;
    
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        LOG_INFO("End of stream reached");
        playing = 0;
        break;
    case GST_MESSAGE_ERROR: {
        gchar *debug;
        GError *err;
        gst_message_parse_error(msg, &err, &debug);
        LOG_ERROR("GStreamer error: %s", err->message);
        LOG_ERROR("Debug details: %s", debug);
        g_error_free(err);
        g_free(debug);
        playing = 0;
        break;
    }
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
            LOG_DEBUG("State changed: %s -> %s (pending: %s)",
                      gst_element_state_get_name(old_state),
                      gst_element_state_get_name(new_state),
                      gst_element_state_get_name(pending));
                      
            if (new_state == GST_STATE_PLAYING) {
                LOG_INFO("Playback started");
                playing = 1;
                paused = 0;
            } else if (new_state == GST_STATE_PAUSED) {
                LOG_INFO("Playback paused");
                paused = 1;
            } else if (new_state == GST_STATE_READY) {
                LOG_INFO("Pipeline ready");
            } else if (new_state == GST_STATE_NULL) {
                LOG_INFO("Pipeline null");
            }
        }
        break;
    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering(msg, &percent);
        LOG_DEBUG("Buffering: %d%%", percent);
        break;
    }
    case GST_MESSAGE_NEW_CLOCK:
        LOG_DEBUG("New clock created");
        break;
    case GST_MESSAGE_STREAM_STATUS:
        LOG_DEBUG("Stream status update");
        break;
    default:
        LOG_DEBUG("Received %s message", GST_MESSAGE_TYPE_NAME(msg));
        break;
    }
    return TRUE;
}

// 添加pad-added回调函数（类似gstreamer_test.c）
static void on_pad_added(GstElement *src, GstPad *new_pad, gpointer data) {
    GstElement *convert = (GstElement *)data;
    GstPad *sink_pad = gst_element_get_static_pad(convert, "sink");

    printf("[pad-added] Received new pad '%s' from '%s'\n",
           GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

    if (gst_pad_is_linked(sink_pad)) {
	printf("[pad-added] Sink pad already linked. Ignoring.\n");
        gst_object_unref(sink_pad);
        return;
    }
    
    GstCaps *new_pad_caps = gst_pad_get_current_caps(new_pad);
    GstStructure *new_pad_struct = gst_caps_get_structure(new_pad_caps, 0);
    const gchar *new_pad_type = gst_structure_get_name(new_pad_struct);
    
    if (g_str_has_prefix(new_pad_type, "audio/x-raw")) {
        if (gst_pad_link(new_pad, sink_pad) != GST_PAD_LINK_OK) {
            fprintf(stderr, "Linking dynamic pad failed\n");
        }
    }
    
    if (new_pad_caps) gst_caps_unref(new_pad_caps);
    gst_object_unref(sink_pad);
}

// 修改player_play函数
int player_play(const char* uri) {
    if (pipeline) {
        player_stop();
    }

    // 手动创建元素
    pipeline = gst_pipeline_new("audio-player");
    GstElement *source = gst_element_factory_make("uridecodebin", "source");
    GstElement *convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *vol = gst_element_factory_make("volume", "vol");
    GstElement *queue = gst_element_factory_make("queue", "queue");
    GstElement *sink = gst_element_factory_make("autoaudiosink", "sink");
    
    if (!pipeline || !source || !convert || !resample || !vol || !queue || !sink) {
        fprintf(stderr, "Failed to create elements\n");
        return -1;
    }

    // 添加元素到管道
    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, vol, queue, sink, NULL);
    
    // 链接静态部分：convert->resample->vol->queue->sink
    if (!gst_element_link_many(convert, resample, vol, queue, sink, NULL)) {
        fprintf(stderr, "Failed to link elements\n");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }

    // 设置URI
    g_object_set(source, "uri", uri, NULL);
    
    // 设置初始音量
    g_object_set(vol, "volume", (double)current_volume/100.0, NULL);
    
    // 连接pad-added信号
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), convert);
    
    // 设置总线监听
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_callback, NULL);
    gst_object_unref(bus);
    
    // 启动播放
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "Failed to start playback\n");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }

    return 0;
}

int player_stop(void) {
    LOG_INFO("Stopping playback");
    
    if (pipeline) {
        LOG_DEBUG("Setting pipeline to NULL state");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    } else {
        LOG_DEBUG("No active pipeline to stop");
    }
    
    playing = 0;
    return 0;
}

int player_pause(void) {
    LOG_INFO("Pausing playback");
    
    if (pipeline && playing) {
        LOG_DEBUG("Setting pipeline to PAUSED state");
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        return 0;
    }
    
    LOG_ERROR("Cannot pause - no active pipeline or not playing");
    return -1;
}

int player_resume(void) {
    LOG_INFO("Resuming playback");
    
    if (pipeline && paused) {
        LOG_DEBUG("Setting pipeline to PLAYING state");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        return 0;
    }
    
    LOG_ERROR("Cannot resume - no active pipeline or not paused");
    return -1;
}

int player_seek(int seconds) {
    LOG_INFO("Seeking to %d seconds", seconds);
    
    if (!pipeline || !playing) {
        LOG_ERROR("Cannot seek - no active pipeline or not playing");
        return -1;
    }
    
    gint64 seek_pos = seconds * GST_SECOND;
    LOG_DEBUG("Seeking to position: %" GST_TIME_FORMAT, GST_TIME_ARGS(seek_pos));
    
    if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH, seek_pos)) {
        LOG_ERROR("Seek failed");
        return -1;
    }
    
    LOG_DEBUG("Seek successful");
    return 0;
}

int player_get_position(int* current_sec, int* total_sec) {
    if (!pipeline) {
        LOG_DEBUG("Position query - no active pipeline");
        return -1;
    }
    
    gboolean duration_success = gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration_ns);
    gboolean position_success = gst_element_query_position(pipeline, GST_FORMAT_TIME, &position_ns);
    
    if (duration_success) {
        LOG_DEBUG("Duration: %" GST_TIME_FORMAT, GST_TIME_ARGS(duration_ns));
        if (total_sec) *total_sec = duration_ns / GST_SECOND;
    } else {
        LOG_DEBUG("Duration query failed");
        if (total_sec) *total_sec = -1;
    }
    
    if (position_success) {
        LOG_DEBUG("Position: %" GST_TIME_FORMAT, GST_TIME_ARGS(position_ns));
        if (current_sec) *current_sec = position_ns / GST_SECOND;
    } else {
        LOG_DEBUG("Position query failed");
        if (current_sec) *current_sec = -1;
    }
    
    return (duration_success && position_success) ? 0 : -1;
}

int player_get_volume(void) {
    LOG_DEBUG("Getting volume: %d%%", current_volume);
    return current_volume;
}

int player_set_volume(int volume) {
    LOG_INFO("Setting volume: %d%%", volume);
    
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    current_volume = volume;
    
    if (pipeline) {
        GstElement *vol = gst_bin_get_by_name(GST_BIN(pipeline), "vol");
        if (vol) {
            LOG_DEBUG("Updating volume element to %d%%", volume);
            g_object_set(vol, "volume", (double)volume/100.0, NULL);
            gst_object_unref(vol);
            return 0;
        }
        LOG_ERROR("Volume element not found in pipeline");
    } else {
        LOG_DEBUG("No active pipeline, storing volume for next playback");
    }
    return -1;
}

int player_is_playing(void) {
    int status = playing && !paused;
    LOG_DEBUG("Playing status: %s", status ? "PLAYING" : "NOT PLAYING");
    return status;
}

int player_init(void) {
    LOG_INFO("Initializing player");
    
    if (!gst_is_initialized()) {
        LOG_DEBUG("Initializing GStreamer");
        gst_init(NULL, NULL);
        
        // 打印GStreamer版本信息
        guint major, minor, micro, nano;
        gst_version(&major, &minor, &micro, &nano);
        LOG_INFO("Using GStreamer version: %u.%u.%u.%u", major, minor, micro, nano);
    }
    return 0;
}

int player_deinit(void) {
    LOG_INFO("Deinitializing player");
    player_stop();
    return 0;
}
