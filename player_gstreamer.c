#include "player.h"
#include <gst/gst.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <alsa/asoundlib.h>

static GstElement *pipeline = NULL;
static volatile int playing = 0;
static volatile int paused = 0;
static gint64 duration_ns = 0;
static gint64 position_ns = 0;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static GMainLoop *main_loop = NULL;
static int g_running = 0;
static pthread_t progress_thread;
static int g_volume_changed_by_controller = 0;//音量改变标致，同步到对应硬件

typedef struct {
    const char* card;//声卡设备
    const char* selem_name;//ALSA元素名
    int buffer_time;//缓冲时间
    int latency_time;//延迟时间
    int initial_volume;//初始音量(未设置的话会读取默认硬件音量)
} PlayerOptions;

static PlayerOptions g_player_options = {
    .card = "hw:0",
    .selem_name = "DAC volume",
    .buffer_time = 200000,
    .latency_time = 10000,
    .initial_volume = 0
};

static GOptionEntry player_option_entries[] = {
    { "card", 'c', 0, G_OPTION_ARG_STRING, &g_player_options.card,
      "ALSA sound card (e.g., hw:0)", "CARD" },
    { "selem-name", 's', 0, G_OPTION_ARG_STRING, &g_player_options.selem_name,
      "ALSA element name (e.g., DAC volume)", "SELEM" },
    { "buffer-time", 'B', 0, G_OPTION_ARG_INT, &g_player_options.buffer_time,
      "GStreamer buffer time in microseconds (default: 200000)", "TIME" },
    { "latency-time", 'L', 0, G_OPTION_ARG_INT, &g_player_options.latency_time,
      "GStreamer latency time in microseconds (default: 10000)", "TIME" },
    { "volume", 'V', 0, G_OPTION_ARG_INT, &g_player_options.initial_volume,
      "Initial volume level (0-100, default: 0)", "VOLUME" },
    { NULL }
};

// 获取播放器选项组
GOptionGroup* player_get_option_group(void) {
    GOptionGroup *group = g_option_group_new(
        "player",
        "Player Options",
        "Show player configuration options",
        NULL,
        NULL
    );
    
    g_option_group_add_entries(group, player_option_entries);
    return group;
}

// 实时获取播放进度线程
static void* update_track_time_thread(void* arg) {
    GstFormat fmt = GST_FORMAT_TIME;
    gint64 pos = 0;
    LOG_DEBUG("-----[%s] starting-----",__func__);

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
    LOG_DEBUG("-----[%s] end-----",__func__);

    return NULL;
}

static void query_audio_stream_info(GstElement *pipeline) {
    GstElement *audio_sink = NULL;
    g_object_get(pipeline, "audio-sink", &audio_sink, NULL);
    if (!audio_sink) {
        LOG_ERROR("audio-sink not available");
        return;
    }

    GstPad *sinkpad = gst_element_get_static_pad(audio_sink, "sink");
    if (!sinkpad) {
        LOG_ERROR("audio-sink pad not found");
        gst_object_unref(audio_sink);
        return;
    }

    GstCaps *caps = gst_pad_get_current_caps(sinkpad);
    if (!caps) {
        LOG_ERROR("No caps available on pad (likely not negotiated yet)");
        gst_object_unref(sinkpad);
        gst_object_unref(audio_sink);
        return;
    }

    GstStructure *str = gst_caps_get_structure(caps, 0);
    if (!str) {
        LOG_ERROR("Failed to get structure from caps");
        gst_caps_unref(caps);
        gst_object_unref(sinkpad);
        gst_object_unref(audio_sink);
        return;
    }

    const gchar *media_type = gst_structure_get_name(str);
    const gchar *format = gst_structure_get_string(str, "format");

    gint rate = 0, channels = 0;
    gst_structure_get_int(str, "rate", &rate);
    gst_structure_get_int(str, "channels", &channels);

    const gchar *layout = NULL;
    if (gst_structure_has_field(str, "layout"))
        layout = gst_structure_get_string(str, "layout");

    gint width = 0, depth = 0, signed_flag = -1, endianness = -1;
    gboolean is_float = FALSE;

    gst_structure_get_int(str, "width", &width);       // 每通道位宽
    gst_structure_get_int(str, "depth", &depth);       // 实际使用位数
    gst_structure_get_int(str, "endianness", &endianness); // 1234 = LE
    gst_structure_get_boolean(str, "signed", &signed_flag);
    gst_structure_get_boolean(str, "floating", &is_float);

    LOG_INFO("Media Type: %s", media_type);
    LOG_INFO("Format:     %s", format ? format : "N/A");
    LOG_INFO("SampleRate: %d Hz", rate);
    LOG_INFO("Channels:   %d", channels);
    LOG_INFO("Layout:     %s", layout ? layout : "N/A");
    //LOG_INFO("Width:      %d bits per sample (declared)", width);
    //LOG_INFO("Depth:      %d bits used per sample", depth);
    //LOG_INFO("Endian:     %s", (endianness == G_LITTLE_ENDIAN) ? "Little Endian" :
    //                             (endianness == G_BIG_ENDIAN) ? "Big Endian" : "Unknown");
    //LOG_INFO("Signed:     %s", (signed_flag == 1) ? "Yes" : "No");
    //LOG_INFO("Floating:   %s", is_float ? "Yes" : "No");

    gst_caps_unref(caps);
    gst_object_unref(sinkpad);
    gst_object_unref(audio_sink);
}

// 总线回调
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer data) {
    (void)bus; (void)data;

    switch (GST_MESSAGE_TYPE(msg)) {
    	case GST_MESSAGE_EOS:
    	    LOG_DEBUG("[%s] End of stream reached",__func__);
    	    pthread_mutex_lock(&lock);
    	    playing = 0;
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
		    query_audio_stream_info(pipeline);
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

    	//    // 处理网络流缓冲
    	//    if (percent < 10) {
    	//        gst_element_set_state(pipeline, GST_STATE_PAUSED);
    	//    } else {
    	//        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    	//    }
    	//    break;
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

static GstState get_current_player_state() {
    GstState state = GST_STATE_PLAYING;
    GstState pending = GST_STATE_NULL;
    gst_element_get_state(pipeline, &state, &pending, 0);
    return state;
}

int player_play(const char* uri) {

    LOG_DEBUG("-----[%s] starting-----",__func__);

    if (get_current_player_state() != GST_STATE_PAUSED) {
        if (gst_element_set_state(pipeline, GST_STATE_READY) ==
            GST_STATE_CHANGE_FAILURE) {
            LOG_ERROR("setting play state failed (1)");
            // Error, but continue; can't get worse :)
        }
        g_object_set(G_OBJECT(pipeline), "uri", uri, NULL);
    }
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("setting play state failed (2)");
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
    } else {
        LOG_DEBUG("No active pipeline to stop");
    }

    playing = 0;
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
    //LOG_DEBUG("-----[%s] starting-----",__func__);
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
        //LOG_DEBUG("Duration: %d sec", *total_sec);
    } else if (total_sec) {
        *total_sec = -1;
    }

    if (position_success && current_sec) {
        *current_sec = (int)(position_ns / GST_SECOND);
        //LOG_DEBUG("Position: %d sec", *current_sec);
    } else if (current_sec) {
        *current_sec = -1;
    }

    pthread_mutex_unlock(&lock);
    //LOG_DEBUG("-----[%s] end-----",__func__);
    return (duration_success && position_success) ? 0 : -1;
}

int player_get_volume(void) {
    LOG_DEBUG("Getting volume: %d%%", g_player_options.initial_volume);
    return g_player_options.initial_volume;
}

int player_set_volume(int volume) {
    LOG_DEBUG("Setting volume: %d%%", volume);

    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    pthread_mutex_lock(&lock);
    g_player_options.initial_volume = volume;
    g_object_set(pipeline, "volume", (double)volume/100.0, NULL);
    player_set_mute(g_player_options.initial_volume == 0);
    g_volume_changed_by_controller = 1;
    pthread_mutex_unlock(&lock);

    return 0;
}

int player_get_mute(int *mute){
    gboolean val;
    g_object_get(pipeline, "mute", &val, NULL);
    *mute = val ? 1 : 0;
    return 0;
}

int player_set_mute(int mute){
    LOG_INFO("Set mute to %s", mute ? "on" : "off");
    g_object_set(pipeline, "mute", mute, NULL);
    return 0;
}

int player_is_playing(void) {
    pthread_mutex_lock(&lock);
    int status = playing && !paused;
    pthread_mutex_unlock(&lock);

    LOG_DEBUG("Playing status: %s", status ? "PLAYING" : "NOT PLAYING");
    return status;
}

static void exit_loop_sighandler(int sig) {
    if (main_loop) {
        // TODO(hzeller): revisit - this is not safe to do.
	LOG_DEBUG("Quit main_loop");
        g_main_loop_quit(main_loop);
    }
}

int run_main_loop(void){
   LOG_DEBUG("Starting GLib main loop");
   main_loop = g_main_loop_new(NULL, FALSE);

   signal(SIGINT, &exit_loop_sighandler);
   signal(SIGTERM, &exit_loop_sighandler);

   g_main_loop_run(main_loop);
   g_main_loop_unref(main_loop);
   main_loop = NULL;

   return 0;
}

void list_mixer_controls(const char *card) {
    snd_mixer_t *handle;
    snd_mixer_elem_t *elem;

    if (snd_mixer_open(&handle, 0) < 0) return;
    if (snd_mixer_attach(handle, card) < 0) {
        snd_mixer_close(handle);
        return;
    }
    snd_mixer_selem_register(handle, NULL, NULL);
    snd_mixer_load(handle);

    for (elem = snd_mixer_first_elem(handle); elem; elem = snd_mixer_elem_next(elem)) {
        if (snd_mixer_elem_get_type(elem) == SND_MIXER_ELEM_SIMPLE) {
            const char *name = snd_mixer_selem_get_name(elem);
            LOG_DEBUG("Found mixer control: '%s'", name);
        }
    }

    snd_mixer_close(handle);
}

int get_hw_volume(long *out_vol, long *min_out, long *max_out) {
    snd_mixer_t *handle = NULL;
    snd_mixer_selem_id_t *sid = NULL;
    int err = 0;
    list_mixer_controls(g_player_options.card);
    if ((err = snd_mixer_open(&handle, 0)) < 0) {
        LOG_ERROR("snd_mixer_open failed: %s", snd_strerror(err));
        return err;
    }

    if ((err = snd_mixer_attach(handle, g_player_options.card)) < 0) {
        LOG_ERROR("snd_mixer_attach('%s') failed: %s", g_player_options.card, snd_strerror(err));
        goto error;
    }

    if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
        LOG_ERROR("snd_mixer_selem_register failed: %s", snd_strerror(err));
        goto error;
    }

    if ((err = snd_mixer_load(handle)) < 0) {
        LOG_ERROR("snd_mixer_load failed: %s", snd_strerror(err));
        goto error;
    }

    snd_mixer_selem_id_malloc(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, g_player_options.selem_name);

    snd_mixer_elem_t *elem = snd_mixer_find_selem(handle, sid);
    if (!elem) {
        LOG_ERROR("snd_mixer_find_selem('%s') failed",g_player_options.selem_name);
        err = -1;
        goto error;
    }

    long min = 0, max = 0, vol = 0;
    snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
    snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &vol);

    if (out_vol) *out_vol = vol;
    if (min_out) *min_out = min;
    if (max_out) *max_out = max;

    snd_mixer_close(handle);
    snd_mixer_selem_id_free(sid);
    return 0;

error:
    if (handle) snd_mixer_close(handle);
    if (sid) snd_mixer_selem_id_free(sid);
    return err;
}

// 同步软件音量到硬件音量,参数 volume 范围为 0.0 到 1.0
int set_hw_volume_from_gst(double volume, const char *card, const char *selem_name) {
    LOG_DEBUG("[%s] volume: %f",__func__,volume);
    if (volume < 0.0) volume = 0.0;
    if (volume > 1.0) volume = 1.0;

    snd_mixer_t *handle = NULL;
    snd_mixer_selem_id_t *sid = NULL;
    snd_mixer_elem_t *elem = NULL;
    long minv = 0, maxv = 0;
    long hw_vol = 0;
    int err;

    // 打开 mixer
    if ((err = snd_mixer_open(&handle, 0)) < 0) {
        LOG_ERROR("snd_mixer_open failed: %s\n", snd_strerror(err));
        return err;
    }
    if ((err = snd_mixer_attach(handle, card)) < 0) {
        LOG_ERROR("snd_mixer_attach('%s') failed: %s\n", card, snd_strerror(err));
        goto fail;
    }
    if ((err = snd_mixer_selem_register(handle, NULL, NULL)) < 0) {
        LOG_ERROR("snd_mixer_selem_register failed: %s\n", snd_strerror(err));
        goto fail;
    }
    if ((err = snd_mixer_load(handle)) < 0) {
        LOG_ERROR("snd_mixer_load failed: %s\n", snd_strerror(err));
        goto fail;
    }
    snd_mixer_selem_id_malloc(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, selem_name);
    elem = snd_mixer_find_selem(handle, sid);
    if (!elem) {
        LOG_ERROR("snd_mixer_find_selem('%s') failed\n", selem_name);
        err = -1;
        goto fail;
    }
    snd_mixer_selem_get_playback_volume_range(elem, &minv, &maxv);
    hw_vol = minv + volume * (maxv - minv);
    LOG_DEBUG("[%s] hw_vol: %ld",__func__,hw_vol);

    if ((err = snd_mixer_selem_set_playback_volume_all(elem, hw_vol)) < 0) {
        LOG_ERROR("set_playback_volume_all failed: %s\n", snd_strerror(err));
        goto fail;
    }
    //snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, hw_vol);
    //snd_mixer_selem_set_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, hw_vol);
    snd_mixer_selem_id_free(sid);
    snd_mixer_close(handle);
    return 0;

fail:
    if (sid) snd_mixer_selem_id_free(sid);
    if (handle) snd_mixer_close(handle);
    return err;
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
    //创建一个名为player的playbin元素
    pipeline = gst_element_factory_make("playbin", "player");
    if (!pipeline) {
        LOG_ERROR("Failed to create playbin pipeline");
        return -1;
    }
    // 配置音频输出
    GstElement *audio_sink = gst_element_factory_make("alsasink", "audio-output");
    if (audio_sink) {
        g_object_set(audio_sink,
            "device", g_player_options.card,
            "buffer-time", g_player_options.buffer_time,
            "latency-time", g_player_options.latency_time,
            NULL);
	gst_object_ref(audio_sink);  // 增加引用给 playbin 使用
        g_object_set(pipeline, "audio-sink", audio_sink, NULL);//将alsasink绑定到管道，playbin会自动连接音频流到我们指定的sink
	gst_object_unref(audio_sink);  // 释放引用
	audio_sink = NULL;
    }

    // 忽略视频
    g_object_set(pipeline, "video-sink", gst_element_factory_make("fakesink", NULL), NULL);

    if(!g_player_options.initial_volume){ //没有指定音量，就根据硬件音量设置软件音量
        long hw_vol = 0, vol_min = 0, vol_max = 0;
        if (get_hw_volume(&hw_vol, &vol_min, &vol_max) == 0) {
            g_player_options.initial_volume = (double)(hw_vol - vol_min) / (vol_max - vol_min)*100.0;
            LOG_DEBUG("Current hardware volume: %ld (range: %ld ~ %ld), software volume: %d%%", hw_vol, vol_min, vol_max, g_player_options.initial_volume);
            g_object_set(pipeline, "volume", (double)g_player_options.initial_volume/100.0, NULL);	
        } else {
            LOG_ERROR("Failed to get hardware volume");
        }
    }else if(g_player_options.initial_volume){
	    LOG_DEBUG("Current software volume: %d%%",g_player_options.initial_volume);
            g_object_set(pipeline, "volume", (double)g_player_options.initial_volume/100.0, NULL);	
	    g_volume_changed_by_controller = 1; //保存当前音量到硬件
    }

    // 设置总线监听
    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    gst_bus_add_watch(bus, bus_callback, NULL);//非阻塞，消息作为事件源挂到GLib主循环
    gst_object_unref(bus);

    //创建获取播放信息线程,循环读取播放进度
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
    if (pipeline) {
        GstElement *audio_sink = NULL;
        g_object_get(pipeline, "audio-sink", &audio_sink, NULL);
    	if (audio_sink) {
    	    if (GST_IS_ELEMENT(audio_sink)) {
    	        gst_element_set_state(audio_sink, GST_STATE_NULL);
    	    }
    	    gst_object_unref(audio_sink);
    	    audio_sink = NULL;
    	}

        GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_remove_watch(bus);  // 移除监视器
        gst_object_unref(bus);
        gst_object_unref(pipeline);
	LOG_DEBUG("remove bus,unref pipeline");
    }
    gst_deinit();
    pthread_mutex_lock(&lock);  // 先获取锁
    if (g_volume_changed_by_controller) {
        set_hw_volume_from_gst((double)g_player_options.initial_volume / 100.0, g_player_options.card, g_player_options.selem_name);
    }
    pthread_mutex_unlock(&lock);  // 再释放
    pthread_mutex_destroy(&lock);//销毁锁

    return 0;
}
