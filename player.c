#include "player.h"
#include <mpg123.h>
#include <ao/ao.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <curl/curl.h>

static mpg123_handle *mh = NULL;
static ao_device *dev = NULL;
static pthread_t play_thread;
static volatile int playing = 0, paused = 0, stop_flag = 0;
static long rate;
static int channels, encoding;
static off_t current_sample = 0, total_sample = 0;

static snd_mixer_t *mixer_handle = NULL;
static snd_mixer_elem_t *mixer_elem = NULL;
static const char *mixer_name = "Master";  // 默认控制主音量
static long int volume_min = 0;
static long int volume_max = 100;
static int current_volume = 50;  // 默认音量50%

int init_output_device() {

    ao_sample_format format;
    format.bits = mpg123_encsize(encoding) * 8;
    format.rate = rate;
    format.channels = channels;
    format.byte_format = AO_FMT_NATIVE;
    format.matrix = NULL;

    ao_info **driver_info_list;
    int driver_count = 0;
    int alsa_driver = -1;
    int default_driver = -1;
    ao_device *device = NULL;

    driver_info_list = ao_driver_info_list(&driver_count);
    if (!driver_info_list) {
        fprintf(stderr, "Failed to get AO driver info list.\n");
        return -1;
    }

    // 输出所有可用驱动
    printf("Available libao drivers:\n");
    for (int i = 0; i < driver_count; i++) {
        ao_info *info = driver_info_list[i];
        printf("  [%d] %s (%s)\n", i, info->short_name, info->name);
        if (strcmp(info->short_name, "alsa") == 0) {
            alsa_driver = i;
        }
    }

    // 如果找到alsa，则尝试用alsa打开
    if (alsa_driver >= 0) {
        printf("Trying to use ALSA driver...\n");
        device = ao_open_live(alsa_driver, &format, NULL);
	printf("Format: rate=%d, bits=%d, channels=%d\n", format.rate, format.bits, format.channels);
        if (device) {
            printf("Audio output initialized with ALSA.\n");
            return 0;
        }else {
	     fprintf(stderr, "ALSA error: %s\n", strerror(errno));
	 }
    }

    // 如果alsa失败，则尝试默认驱动
    default_driver = ao_default_driver_id();
    if (default_driver < 0) {
        fprintf(stderr, "No default AO driver available. %s\n",strerror(errno));
        return -1;
    }

    printf("Trying to use default driver: %s\n", driver_info_list[default_driver]->short_name);
    device = ao_open_live(default_driver, &format, NULL);
    if (device) {
        printf("Audio output initialized with default driver.\n");
        return 0;
    } else {
        fprintf(stderr, "Failed to open audio output device with default driver.\n");
        return -1;
    }
}

// 新增全局变量，用于curl下载数据回调时使用
static int curl_running = 0;
static pthread_t curl_thread;
static void* playback_thread(void* arg);

static size_t my_curl_write_callback(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t bytes = size * nmemb;

    // 送数据给mpg123解码器
    int err = mpg123_feed(mh, ptr, bytes);
    if (err != MPG123_OK) {
        fprintf(stderr, "mpg123_feed error: %s\n", mpg123_strerror(mh));
        return 0; // 停止curl传输
    }

    // 尝试从mpg123读取解码PCM数据并播放
    unsigned char buffer[8192];
    size_t done;
    while (mpg123_read(mh, buffer, sizeof(buffer), &done) == MPG123_OK) {
        ao_play(dev, (char*)buffer, done);
        current_sample += done / (channels * mpg123_encsize(encoding));
    }
    return bytes;
}

static void* curl_download_thread(void* arg) {
    const char* url = (const char*)arg;
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "Failed to init curl\n");
        return NULL;
    }

    curl_running = 1;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, my_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    curl_running = 0;
    return NULL;
}

int player_play(const char* uri) {
    stop_flag = 0;
    paused = 0;
    current_sample = 0;

    // 判断是否是http网络流
    if (strncmp(uri, "http://", 7) == 0 || strncmp(uri, "https://", 8) == 0) {
        // 初始化mpg123 feed模式
        mpg123_open_feed(mh);

        // 初始化音频输出格式，固定为mp3常见格式（可根据需求更灵活）
        rate = 48000;
        channels = 2;
        encoding = MPG123_ENC_SIGNED_16;
        if (init_output_device() < 0) {
            fprintf(stderr, "[%s] https Failed to open audio output device\n",__func__);
            return -1;
        }

        playing = 1;

        // 创建curl下载线程
        if (pthread_create(&curl_thread, NULL, curl_download_thread, (void*)uri) != 0) {
            fprintf(stderr, "Failed to create curl thread\n");
            return -1;
        }
        return 0;
    } else {
        // 本地文件播放，维持原逻辑
        if (mpg123_open(mh, uri) != MPG123_OK) {
            fprintf(stderr, "Failed to open URI: %s\n", uri);
            return -1;
        }
        mpg123_getformat(mh, &rate, &channels, &encoding);
        mpg123_format_none(mh);
        mpg123_format(mh, rate, channels, encoding);

        total_sample = mpg123_length(mh);

        if (init_output_device() < 0) {
            fprintf(stderr, "[%s] local Failed to open audio output device\n",__func__);
            return -1;
        }

        playing = 1;
        pthread_create(&play_thread, NULL, playback_thread, NULL);
        return 0;
    }
}

int player_stop(void) {
    if (!playing) return -1;

    stop_flag = 1;

    if (curl_running) {
        // 等待curl下载线程结束
        pthread_join(curl_thread, NULL);
    } else {
        // 等待本地文件播放线程结束
        pthread_join(play_thread, NULL);
    }

    if (mh) {
        mpg123_close(mh);
    }
    if (dev) {
        ao_close(dev);
        dev = NULL;
    }
    playing = 0;
    return 0;
}

static void* playback_thread(void* arg) {
    unsigned char *buffer;
    size_t buffer_size;
    size_t done;

    buffer_size = mpg123_outblock(mh);
    buffer = malloc(buffer_size);
    if (!buffer) {
        perror("malloc");
        return NULL;
    }

    if (init_output_device() < 0) {
        fprintf(stderr, "Failed to open audio output.\n");
        free(buffer);
        return NULL;
    }

    while (!stop_flag) {
        if (paused) {
            usleep(10000);
            continue;
        }

        int err = mpg123_read(mh, buffer, buffer_size, &done);
        if (err == MPG123_OK) {
            ao_play(dev, (char *)buffer, done);
            current_sample += done / (channels * mpg123_encsize(encoding));
        } else if (err == MPG123_DONE) {
            break;
        } else {
            fprintf(stderr, "mpg123_read() error: %s\n", mpg123_strerror(mh));
            break;
        }
    }

    free(buffer);
    ao_close(dev);
    ao_shutdown();
    return NULL;
}

int player_init(void) {
    ao_initialize();
    if (mpg123_init() != MPG123_OK) {
        fprintf(stderr, "Failed to initialize mpg123\n");
        return -1;
    }

    mh = mpg123_new(NULL, NULL);
    if (!mh) {
        fprintf(stderr, "Failed to create mpg123 handle\n");
        return -1;
    }
    // 初始化ALSA混音器
    if (snd_mixer_open(&mixer_handle, 0) < 0) {
        fprintf(stderr, "Failed to open ALSA mixer\n");
        mixer_handle = NULL;
    } else if (snd_mixer_attach(mixer_handle, "default") < 0) {
        fprintf(stderr, "Failed to attach mixer to default device\n");
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
    } else if (snd_mixer_selem_register(mixer_handle, NULL, NULL) < 0) {
        fprintf(stderr, "Failed to register mixer simple element\n");
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
    } else if (snd_mixer_load(mixer_handle) < 0) {
        fprintf(stderr, "Failed to load mixer controls\n");
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
    } else {
        // 查找主音量控制
        snd_mixer_selem_id_t *sid;
        snd_mixer_selem_id_alloca(&sid);
        snd_mixer_selem_id_set_index(sid, 0);
        snd_mixer_selem_id_set_name(sid, mixer_name);
        mixer_elem = snd_mixer_find_selem(mixer_handle, sid);
        
        if (mixer_elem) {
            snd_mixer_selem_get_playback_volume_range(mixer_elem, &volume_min, &volume_max);
        } else {
            fprintf(stderr, "Failed to find mixer control '%s'\n", mixer_name);
        }
    }

    return 0;
}

int player_pause(void) {
    if (playing) {
        paused = 1;
        return 0;
    }
    return -1;
}

int player_resume(void) {
    if (playing && paused) {
        paused = 0;
        return 0;
    }
    return -1;
}

int player_seek(int seconds) {
    if (!playing) return -1;
    off_t target_sample = (off_t)(seconds * rate);
    if (mpg123_seek(mh, target_sample, SEEK_SET) >= 0) {
        current_sample = target_sample;
        return 0;
    }
    return -1;
}

int player_get_position(int* current_sec, int* total_sec) {
    if (current_sec) *current_sec = current_sample / rate;
    if (total_sec) *total_sec = total_sample / rate;
    return 0;
}

int player_get_volume(void) {
    if (!mixer_handle || !mixer_elem) {
        return current_volume;  // 返回软件保存的音量值
    }

    long alsa_vol;
    if (snd_mixer_selem_get_playback_volume(mixer_elem, SND_MIXER_SCHN_FRONT_LEFT, &alsa_vol) < 0) {
        return current_volume;
    }

    // 将ALSA音量值转换为百分比
    current_volume = (int)(100 * (alsa_vol - volume_min) / (volume_max - volume_min));
    return current_volume;
}

int player_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    
    current_volume = volume;

    if (!mixer_handle || !mixer_elem) {
        return 0;  // 没有硬件控制，只保存软件值
    }

    // 将百分比转换为ALSA音量值
    long alsa_vol = volume_min + (volume * (volume_max - volume_min) / 100);
    
    // 设置左右声道音量
    if (snd_mixer_selem_set_playback_volume_all(mixer_elem, alsa_vol) < 0) {
        fprintf(stderr, "Failed to set playback volume\n");
        return -1;
    }

    return 0;
}

int player_is_playing(void) {
    return playing && !paused;
}

int player_deinit(void) {
    mpg123_delete(mh);
    mpg123_exit();
    ao_shutdown();
    if (mixer_handle) {
        snd_mixer_close(mixer_handle);
        mixer_handle = NULL;
        mixer_elem = NULL;
    }
    return 0;
}
