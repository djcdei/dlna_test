#include "player.h"
#include <gst/gst.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>

static GstElement *pipeline = NULL;
static volatile int playing = 0;
static volatile int paused = 0;
static gint64 duration_ns = 0;
static gint64 position_ns = 0;
static int current_volume = 50;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static GMainLoop *main_loop = NULL;
static int g_running = 0;
static pthread_t progress_thread;

// 改进的pad-added回调
static void on_pad_added(GstElement *src, GstPad *new_pad, gpointer data) {
    GstElement *convert = (GstElement *)data;
    GstPad *sink_pad = gst_element_get_static_pad(convert, "sink");

    LOG_DEBUG("[pad-added] Received new pad '%s' from '%s'",
           GST_PAD_NAME(new_pad), GST_ELEMENT_NAME(src));

    // 检查是否已连接
    if (gst_pad_is_linked(sink_pad)) {
        LOG_DEBUG("[pad-added] Sink pad already linked. Ignoring.");
        gst_object_unref(sink_pad);
        return;
    }

    // 尝试直接连接而不检查caps
    GstPadLinkReturn ret = gst_pad_link(new_pad, sink_pad);
    if (ret == GST_PAD_LINK_OK) {
        LOG_DEBUG("[pad-added] Successfully linked pad");
    } else {
        LOG_ERROR("[pad-added] Linking failed: %s", gst_pad_link_get_name(ret));
    }

    gst_object_unref(sink_pad);
}

static void* update_track_time_thread(void* arg) {
    GstFormat fmt = GST_FORMAT_TIME;
    gint64 pos = 0;
    LOG_DEBUG("[%s] start....",__func__);

    while (g_running) {
	if(paused){
	   g_usleep(1000000);
	   continue;
	};
	if(playing){
           if (gst_element_query_position(pipeline, fmt, &pos)) {
               LOG_INFO("Current position: %" GST_TIME_FORMAT, GST_TIME_ARGS(pos));
           }
	}
        g_usleep(1000000); // 1秒刷新一次
    }
    LOG_DEBUG("[%s] end !!!",__func__);

    return NULL;
}

// 总线回调（添加缓冲处理）
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
        LOG_DEBUG("[%s] End of stream reached",__func__);
        pthread_mutex_lock(&lock);
        playing = 0;
        g_main_loop_quit(main_loop);
        pthread_mutex_unlock(&lock);
        break;

    case GST_MESSAGE_ERROR: {
        gchar *debug;
        GError *err;
        gst_message_parse_error(msg, &err, &debug);

        // 添加详细的错误诊断
        LOG_ERROR("GStreamer error: %s (domain: %d, code: %d)",
                  err->message, err->domain, err->code);

        // 检查特定错误类型
        if (err->domain == GST_RESOURCE_ERROR) {
            LOG_ERROR("Resource error details:");

            // 获取更多资源错误信息
            gchar *uri = NULL;
            GstElement *src = gst_element_factory_make("uridecodebin", NULL);
            if (src) {
                g_object_get(src, "uri", &uri, NULL);
                if (uri) {
                    LOG_ERROR("URI: %s", uri);
                    g_free(uri);
                }
                gst_object_unref(src);
            }
        }

        LOG_ERROR("Debug details: %s", debug);
        g_error_free(err);
        g_free(debug);

        pthread_mutex_lock(&lock);
        playing = 0;
        g_main_loop_quit(main_loop);
        pthread_mutex_unlock(&lock);
        break;
    }
    //pipeline状态变化
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
            GstState old_state, new_state, pending;
            gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
            LOG_DEBUG("State changed: %s -> %s (pending: %s)",
                      gst_element_state_get_name(old_state),
                      gst_element_state_get_name(new_state),
                      gst_element_state_get_name(pending));

            pthread_mutex_lock(&lock);
            if (new_state == GST_STATE_PLAYING) {
                playing = 1;
                paused = 0;
            } else if (new_state == GST_STATE_PAUSED) {
                paused = 1;
            } else if (new_state == GST_STATE_READY) {
            } else if (new_state == GST_STATE_NULL) {
                playing = 0;
            }
            pthread_mutex_unlock(&lock);
        }
        break;

    case GST_MESSAGE_BUFFERING: {
        gint percent = 0;
        gst_message_parse_buffering(msg, &percent);
        LOG_DEBUG("Buffering: %d%%", percent);

        // 处理网络流缓冲
        if (percent < 100) {
            gst_element_set_state(pipeline, GST_STATE_PAUSED);
        } else {
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        }
        break;
    }

    case GST_MESSAGE_STREAM_START:
        LOG_DEBUG("Stream started");
        break;

    default:
        //LOG_DEBUG("Received %s message", GST_MESSAGE_TYPE_NAME(msg));
        break;
    }
    return TRUE;
}

// 运行GLib主循环
static void* run_main_loop(void* data) {
    (void)data;
    LOG_DEBUG("Starting GLib main loop");

    pthread_mutex_lock(&lock);
    main_loop = g_main_loop_new(NULL, FALSE);
    pthread_mutex_unlock(&lock);

    if (!main_loop) {
        LOG_ERROR("Failed to create GLib main loop");
        return NULL;
    }

    g_main_loop_run(main_loop); //阻塞不断轮询所有事件源，包括总线事件，这里主要获取pipeline管道中的事件bus

    LOG_DEBUG("GLib main loop exited");

    pthread_mutex_lock(&lock);
    g_main_loop_unref(main_loop);
    main_loop = NULL;
    pthread_mutex_unlock(&lock);

    return NULL;
}

int player_play(const char* uri) {

    LOG_DEBUG("-----[%s] starting-----",__func__);
    if (pipeline) {
        player_stop();
    }

    // 创建元素
    pipeline = gst_pipeline_new("audio-player");
    GstElement *source = gst_element_factory_make("uridecodebin", "source");
    GstElement *convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *vol = gst_element_factory_make("volume", "vol");
    GstElement *sink = gst_element_factory_make("alsasink", "sink");

    if (!pipeline || !source || !convert || !resample || !vol || !sink) {
        LOG_ERROR("Failed to create GStreamer elements");
        if (pipeline) gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }
    g_object_set(sink,
        "device", "hw:0,0",          // 明确指定设备
        "buffer-time", 50000,      // 缓冲区大小（微秒）
        "latency-time", 10000,     // 延迟时间
        NULL);
    // 添加元素到管道
    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, vol, sink, NULL);

    // 链接静态部分
    if (!gst_element_link_many(convert, resample, vol, sink, NULL)) {
        LOG_ERROR("Failed to link elements");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }

    // 设置URI
    g_object_set(source, "uri", uri, NULL);

    // 设置初始音量
    g_object_set(vol, "volume", (double)current_volume/100.0, NULL);
    LOG_DEBUG("Initial volume set: %d%%", current_volume);

    // 连接动态pad-added信号
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), convert);

    // 设置总线监听
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_callback, NULL);//非阻塞，消息作为事件源挂到GLib主循环
    gst_object_unref(bus);

    // 启动GLib主循环线程
    pthread_t loop_thread;
    if (pthread_create(&loop_thread, NULL, run_main_loop, NULL) != 0) {
        LOG_ERROR("Failed to create event loop thread");
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }
    pthread_detach(loop_thread);//自动资源回收，避免阻塞主线程

    // 给事件循环时间启动
    g_usleep(10000);

    // 启动播放
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("Failed to start playback");
        g_main_loop_quit(main_loop);
        gst_object_unref(pipeline);
        pipeline = NULL;
        return -1;
    }

    LOG_DEBUG("-----[%s] end-----",__func__);
    return 0;
}

int player_stop(void) {

    LOG_DEBUG("-----[%s] starting-----",__func__);
    pthread_mutex_lock(&lock);
    if (pipeline) {
        LOG_DEBUG("Setting pipeline to NULL state");
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        pipeline = NULL;
    } else {
        LOG_DEBUG("No active pipeline to stop");
    }

    playing = 0;
    if (main_loop) {
	LOG_DEBUG("Quit main_loop");
        g_main_loop_quit(main_loop);
    }
    pthread_mutex_unlock(&lock);

    LOG_DEBUG("-----[%s] end-----",__func__);
    return 0;
}

int player_pause(void) {

    LOG_DEBUG("-----[%s] starting-----",__func__);

    pthread_mutex_lock(&lock);
    if (pipeline && playing) {
        LOG_DEBUG("Setting pipeline to PAUSED state");
        gst_element_set_state(pipeline, GST_STATE_PAUSED);
        pthread_mutex_unlock(&lock);
    	LOG_DEBUG("-----[%s] end-----",__func__);
        return 0;
    }
    pthread_mutex_unlock(&lock);

    LOG_ERROR("Cannot pause - no active pipeline or not playing");
    return -1;
}

int player_resume(void) {
    LOG_DEBUG("-----[%s] starting-----",__func__);
    pthread_mutex_lock(&lock);

    if (pipeline && paused) {
        LOG_DEBUG("Setting pipeline to PLAYING state");
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        pthread_mutex_unlock(&lock);
    	LOG_DEBUG("-----[%s] end-----",__func__);
        return 0;
    }

    pthread_mutex_unlock(&lock);
    LOG_ERROR("Cannot resume - no active pipeline or not paused");
    return -1;
}

int player_seek(int seconds) {
    pthread_mutex_lock(&lock);

    if (!pipeline || !playing) {
        pthread_mutex_unlock(&lock);
        LOG_ERROR("Cannot seek - no active pipeline or not playing");
        return -1;
    }

    gint64 seek_pos = seconds * GST_SECOND;

    gboolean seek_result = gst_element_seek_simple(
        pipeline,
        GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
        seek_pos
    );
    pthread_mutex_unlock(&lock);

    if (!seek_result) {
        LOG_ERROR("Seek failed");
        return -1;
    }

    LOG_DEBUG("Seeking to position: %" GST_TIME_FORMAT, GST_TIME_ARGS(seek_pos));

    return 0;
}

int player_get_position(int* current_sec, int* total_sec) {
    LOG_DEBUG("-----[%s] starting-----",__func__);
    pthread_mutex_lock(&lock);

    if (!pipeline) {
        pthread_mutex_unlock(&lock);
        LOG_DEBUG("Position query - no active pipeline");
        return -1;
    }

    gboolean duration_success = gst_element_query_duration(
        pipeline, GST_FORMAT_TIME, &duration_ns);
    gboolean position_success = gst_element_query_position(
        pipeline, GST_FORMAT_TIME, &position_ns);

    if (duration_success && total_sec) {
        *total_sec = (int)(duration_ns / GST_SECOND);
        LOG_DEBUG("Duration: %d sec", *total_sec);
    } else if (total_sec) {
        *total_sec = -1;
    }

    if (position_success && current_sec) {
        *current_sec = (int)(position_ns / GST_SECOND);
        LOG_DEBUG("Position: %d sec", *current_sec);
    } else if (current_sec) {
        *current_sec = -1;
    }

    pthread_mutex_unlock(&lock);
    LOG_DEBUG("-----[%s] end-----",__func__);
    return (duration_success && position_success) ? 0 : -1;
}

int player_get_volume(void) {
    LOG_DEBUG("Getting volume: %d%%", current_volume);
    return current_volume;
}

int player_set_volume(int volume) {
    LOG_DEBUG("Setting volume: %d%%", volume);

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    current_volume = volume;

    pthread_mutex_lock(&lock);
    if (pipeline) {
        GstElement *vol = gst_bin_get_by_name(GST_BIN(pipeline), "vol");
        if (vol) {
            LOG_DEBUG("Updating volume element");
            g_object_set(vol, "volume", (double)volume/100.0, NULL);
            gst_object_unref(vol);
       	    player_set_mute(current_volume == 0);
            pthread_mutex_unlock(&lock);
            return 0;
        }
        LOG_ERROR("Volume element not found");
    }
    pthread_mutex_unlock(&lock);
    return -1;
}

int player_get_mute(int *mute){
    gboolean val;
    GstElement *vol = gst_element_factory_make("volume", "vol");
    if(vol){
   	g_object_set(G_OBJECT(vol), "mute", &val, NULL);
	gst_object_unref(vol);
    }
    *mute = val;
    return 0;
}

int player_set_mute(int mute){
    LOG_INFO("Set mute to %s", mute ? "on" : "off");
    GstElement *vol = gst_element_factory_make("volume", "vol");
    if(vol){
   	g_object_set(G_OBJECT(vol), "mute", mute, NULL);
	gst_object_unref(vol);
    }
    return 0;
}

int player_is_playing(void) {
    pthread_mutex_lock(&lock);
    int status = playing && !paused;
    pthread_mutex_unlock(&lock);

    LOG_DEBUG("Playing status: %s", status ? "PLAYING" : "NOT PLAYING");
    return status;
}

int player_init(void) {
    LOG_INFO("Initializing player");

    if (!gst_is_initialized()) {
        LOG_DEBUG("Initializing GStreamer");
        gst_init(NULL, NULL);

        // 打印版本信息
        guint major, minor, micro, nano;
        gst_version(&major, &minor, &micro, &nano);
        LOG_INFO("GStreamer version: %u.%u.%u.%u", major, minor, micro, nano);
    }
    //创建获取播放信息线程
    g_running = 1;
    pthread_create(&progress_thread, NULL, update_track_time_thread, NULL);
    return 0;
}

int player_deinit(void) {
    LOG_INFO("Deinitializing player");
    player_stop();
    g_running = 0;
    if (pthread_join(progress_thread, NULL) != 0) {
        perror("pthread_join failed");
        return -1;
    }
    pthread_mutex_destroy(&lock);

    return 0;
}
