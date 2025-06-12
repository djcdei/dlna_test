#include <upnp/upnp.h>
#include <upnp/upnptools.h>
#include <upnp/upnpdebug.h>
#include <upnp/ixml.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <uuid/uuid.h>
#include <netdb.h>
#include <pthread.h>
#include "player.h"

#define VIRTUAL_DIR "/virtual"
#define UPNP_DEVICE_TYPE "urn:schemas-upnp-org:device:MediaRenderer:1"
#define AVTRANSPORT_SERVICE "urn:schemas-upnp-org:service:AVTransport:1"
#define RENDERING_SERVICE "urn:schemas-upnp-org:service:RenderingControl:1"

int CURRENT_LOG_LEVEL = LOG_LEVEL_DEBUG;

volatile sig_atomic_t running = 1;
static pthread_mutex_t renderer_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    const char *real_path;
    const char *virtual_path;
    const char *content_type;
} VirtualFileEntry;

typedef struct {
    off_t pos;
    const char *data;
    size_t len;
} WebServerFile;

struct virtual_file {
    char *virtual_fname;
    char *content_type;
    char *data;
    size_t len;
    struct virtual_file *next;
}*virtual_files = NULL;

typedef struct {
    char current_uri[1024];
    volatile int playing;
    volatile int paused;
} renderer_context_t;

static renderer_context_t g_renderer_ctx = {{0}, 0, 0};

void generate_uuid(char *uuid_str) {
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
}

void handle_sigint(int sig) {
    LOG_INFO("Caught SIGINT (Ctrl+C), exiting...");
    running = 0;
}

static char *read_file_to_memory(const char *filepath, size_t *length) {
    FILE *file = fopen(filepath, "rb");
    if (!file) {
        perror("fopen failed");
        return NULL;
    }

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (size <= 0) {
        fclose(file);
        return NULL;
    }

    char *buf = malloc(size);
    if (!buf) {
        fclose(file);
        return NULL;
    }

    if (fread(buf, 1, size, file) != (size_t)size) {
        free(buf);
        fclose(file);
        return NULL;
    }

    fclose(file);
    *length = size;
    return buf;
}

int create_virtual_file(const char *real_path, const char *virtual_path, const char *content_type) {
    size_t size;
    char *data = read_file_to_memory(real_path, &size);
    if (!data) {
        LOG_ERROR( "Failed to read file: %s", real_path);
        return -1;
    }

    struct virtual_file *vf = malloc(sizeof(struct virtual_file));
    if (!vf) {
        free(data);
        return -1;
    }

    vf->virtual_fname = strdup(virtual_path);
    vf->content_type = strdup(content_type);
    vf->data = data;
    vf->len = size;
    vf->next = virtual_files;
    virtual_files = vf;

    LOG_DEBUG("Loaded virtual file: %s -> %s (%zu bytes)",
           real_path, virtual_path, size);
    return 0;
}
int load_virtual_files(const VirtualFileEntry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (create_virtual_file(entries[i].real_path,
                              entries[i].virtual_path,
                              entries[i].content_type) != 0) {
            LOG_ERROR( "Failed to load: %s", entries[i].real_path);
            return -1;
        }
    }
    return 0;
}
void free_virtual_files(void) {
    struct virtual_file *current = virtual_files;
    while (current) {
        struct virtual_file *next = current->next;
        free(current->virtual_fname);
        free(current->content_type);
        free((void*)current->data);
        free(current);
        current = next;
    }
    virtual_files = NULL;
    LOG_DEBUG("Freed all virtual files");
}

static struct virtual_file *get_file_by_name(const char *filename) {
    struct virtual_file *virtfile = virtual_files;
    while (virtfile) {
        if (strcmp(filename, virtfile->virtual_fname) == 0) {
            return virtfile;
        }
        virtfile = virtfile->next;
    }
    return NULL;
}

int my_get_info(const char *filename, UpnpFileInfo *info) {
    struct virtual_file *virtfile = get_file_by_name(filename);

    if (virtfile) {
        UpnpFileInfo_set_FileLength(info, virtfile->len);
        UpnpFileInfo_set_LastModified(info, time(NULL));
        UpnpFileInfo_set_IsDirectory(info, 0);
        UpnpFileInfo_set_IsReadable(info, 1);

        // 克隆字符串以避免内存问题
        char *content_type = ixmlCloneDOMString(virtfile->content_type);
        UpnpFileInfo_set_ContentType(info, content_type);
        return 0;
    }
    return -1;
}

UpnpWebFileHandle my_open(const char *filename, enum UpnpOpenFileMode mode) {
    if (mode != UPNP_READ) return NULL;

    struct virtual_file *virtfile = get_file_by_name(filename);
    if (!virtfile) return NULL;

    WebServerFile *file = malloc(sizeof(WebServerFile));
    if (!file) return NULL;

    file->pos = 0;
    file->len = virtfile->len;
    file->data = virtfile->data;

    return (UpnpWebFileHandle)file;
}

int my_read(UpnpWebFileHandle fileHnd, char *buf, size_t buflen) {
    WebServerFile *file = (WebServerFile *)fileHnd;
    if (!file) return -1;

    size_t remaining = file->len - file->pos;
    if (remaining == 0) return 0;

    size_t to_read = (buflen < remaining) ? buflen : remaining;
    memcpy(buf, file->data + file->pos, to_read);
    file->pos += to_read;

    return to_read;
}

int my_close(UpnpWebFileHandle fileHnd) {
    WebServerFile *file = (WebServerFile *)fileHnd;
    if (file) free(file);
    return 0;
}

int my_write(UpnpWebFileHandle fileHnd, char *buf, size_t buflen) {
    return -1;
}

int my_seek(UpnpWebFileHandle fileHnd, off_t offset, int origin) {
    WebServerFile *file = (WebServerFile *)fileHnd;
    if (!file) return -1;

    off_t new_pos;
    switch (origin) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = file->pos + offset; break;
        case SEEK_END: new_pos = file->len + offset; break;
        default: return -1;
    }

    if (new_pos < 0 || new_pos > (off_t)file->len) return -1;

    file->pos = new_pos;
    return 0;
}

struct UpnpVirtualDirCallbacks virtual_dir_callbacks = {
    .get_info = my_get_info,
    .open = my_open,
    .read = my_read,
    .close = my_close,
    .write = my_write,
    .seek = my_seek,
};

int webserver_register_callbacks(void) {
    int rc = UpnpSetVirtualDirCallbacks(&virtual_dir_callbacks);
    if (rc != UPNP_E_SUCCESS) {
        LOG_ERROR( "UpnpSetVirtualDirCallbacks failed: %s (%d)",
                UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    rc = UpnpAddVirtualDir(VIRTUAL_DIR);
    if (rc != UPNP_E_SUCCESS) {
        LOG_ERROR( "UpnpAddVirtualDir failed: %s (%d)",
                UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    LOG_DEBUG("Registered virtual directory callbacks success");
    return 0;
}

const char* get_action_argument(struct Upnp_Action_Request* request, const char* arg_name) {
    if (!request || !request->ActionRequest) return NULL;

    IXML_NodeList* nodeList = ixmlDocument_getElementsByTagName(request->ActionRequest, arg_name);
    if (!nodeList) return NULL;

    IXML_Node* node = ixmlNodeList_item(nodeList, 0);
    if (!node) {
        ixmlNodeList_free(nodeList);
        return NULL;
    }

    IXML_Node* textNode = ixmlNode_getFirstChild(node);
    const char* value = textNode ? ixmlNode_getNodeValue(textNode) : NULL;

    ixmlNodeList_free(nodeList);
    return value;
}

int set_error_response(struct Upnp_Action_Request* request, int error_code, const char* error_msg) {
    UpnpActionRequest_set_ErrCode(request, error_code);
    snprintf(request->ErrStr, sizeof(request->ErrStr), "%s", error_msg);
    LOG_ERROR( "Action error [%d]: %s", error_code, error_msg);
    return UPNP_E_SUCCESS;
}

static char* escape_xml(const char *input) {
    if (!input) return NULL;

    size_t len = strlen(input);
    size_t escaped_len = len * 6 + 1; // 最坏情况下每个字符变成6个字符(&quot;)
    char *output = malloc(escaped_len);
    if (!output) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (input[i]) {
            case '<': strcpy(output + j, "&lt;"); j += 4; break;
            case '>': strcpy(output + j, "&gt;"); j += 4; break;
            case '&': strcpy(output + j, "&amp;"); j += 5; break;
            case '\"': strcpy(output + j, "&quot;"); j += 6; break;
            case '\'': strcpy(output + j, "&apos;"); j += 6; break;
            default: output[j++] = input[i]; break;
        }

        // 防止缓冲区溢出
        if (j >= escaped_len - 6) {
            output[j] = '\0';
            break;
        }
    }
    output[j] = '\0';
    return output;
}

IXML_Document* create_response_document(const char* action_name, const char* service_type, const char* content) {
    char resp_buf[2048];
    snprintf(resp_buf, sizeof(resp_buf),
        "<u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>",
        action_name, service_type, content, action_name);

    IXML_Document *doc = ixmlParseBuffer(resp_buf);
    if (!doc) {
        LOG_ERROR( "Failed to parse response XML");
    }
    return doc;
}

int action_handler(Upnp_EventType event_type, void* event, void* cookie) {
    if (event_type != UPNP_CONTROL_ACTION_REQUEST) {
        return UPNP_E_SUCCESS;
    }

    struct Upnp_Action_Request* request = (struct Upnp_Action_Request*)event;
    LOG_DEBUG("Action request: %s for service: %s",
           request->ActionName, request->ServiceID);

    const char* service_type = NULL;

    if (strcmp(request->ServiceID, "urn:upnp-org:serviceId:AVTransport") == 0) {
        service_type = AVTRANSPORT_SERVICE;
    } else if (strcmp(request->ServiceID, "urn:upnp-org:serviceId:RenderingControl") == 0) {
        service_type = RENDERING_SERVICE;
    } else {
        LOG_ERROR( "Unknown service ID: %s", request->ServiceID);
        return set_error_response(request, 700, "Unknown service");
    }

    pthread_mutex_lock(&renderer_mutex);

    // 处理具体动作
    if (strcmp(request->ActionName, "SetAVTransportURI") == 0) {
        const char* uri = get_action_argument(request, "CurrentURI");
        if (!uri || *uri == '\0') {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 701, "Invalid URI");
        }

        // 停止当前播放
        if (player_is_playing() || g_renderer_ctx.paused) {
            player_stop();
        }

        strncpy(g_renderer_ctx.current_uri, uri, sizeof(g_renderer_ctx.current_uri) - 1);
        g_renderer_ctx.current_uri[sizeof(g_renderer_ctx.current_uri)-1] = '\0';
        g_renderer_ctx.playing = 0;
        g_renderer_ctx.paused = 0;

        LOG_DEBUG("Set URI: %s", g_renderer_ctx.current_uri);

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "Play") == 0) {
        if (strlen(g_renderer_ctx.current_uri) == 0) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 702, "URI not set");
        }

        int ret = 0;
        if (g_renderer_ctx.paused) {
            ret = player_resume();
            if (ret == 0) {
                g_renderer_ctx.playing = 1;
                g_renderer_ctx.paused = 0;
            }
        } else {
            ret = player_play(g_renderer_ctx.current_uri);
            if (ret == 0) {
                g_renderer_ctx.playing = 1;
                g_renderer_ctx.paused = 0;
            }
        }

        if (ret != 0) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 703, "Playback failed");
        }

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "<Speed>1</Speed>");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "Stop") == 0) {
        if (player_stop() == 0) {
            g_renderer_ctx.playing = 0;
            g_renderer_ctx.paused = 0;
        } else {
            LOG_ERROR("Stop failed (not playing?)");
        }

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "Pause") == 0) {
        if (!player_is_playing()) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 704, "Not playing");
        }

        if (player_pause() == 0) {
            g_renderer_ctx.playing = 0;
            g_renderer_ctx.paused = 1;
        }

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "Seek") == 0) {
        const char* unit = get_action_argument(request, "Unit");
        const char* target = get_action_argument(request, "Target");

        if (!unit || strcmp(unit, "REL_TIME") != 0) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 705, "Unsupported seek unit");
        }

        if (!target) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 706, "Missing target");
        }

        // 解析时间格式 (HH:MM:SS)
        int hours = 0, minutes = 0, seconds = 0;
        if (sscanf(target, "%d:%d:%d", &hours, &minutes, &seconds) < 1) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 707, "Invalid time format");
        }

        int total_seconds = hours * 3600 + minutes * 60 + seconds;

        if (player_seek(total_seconds) != 0) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 708, "Seek failed");
        }

        LOG_DEBUG("Seek to %s (%d seconds)", target, total_seconds);

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "GetPositionInfo") == 0) {
        int curr_sec = 0, total_sec = 0;
        player_get_position(&curr_sec, &total_sec);

        char relTime[16], trackDur[16];
        snprintf(relTime, sizeof(relTime), "%02d:%02d:%02d",
                 curr_sec/3600, (curr_sec%3600)/60, curr_sec%60);
        snprintf(trackDur, sizeof(trackDur), "%02d:%02d:%02d",
                 total_sec/3600, (total_sec%3600)/60, total_sec%60);

        // 转义当前URI
        char *escaped_uri = escape_xml(g_renderer_ctx.current_uri);
        if (!escaped_uri) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 715, "Failed to escape URI");
        }

        // 使用更大的缓冲区
        char content[8192];
        int written = snprintf(content, sizeof(content),
            "<Track>0</Track>"
            "<TrackDuration>%s</TrackDuration>"
            "<TrackMetaData></TrackMetaData>"
            "<TrackURI>%s</TrackURI>"
            "<RelTime>%s</RelTime>"
            "<AbsTime>%s</AbsTime>"
            "<RelCount>2147483647</RelCount>"
            "<AbsCount>2147483647</AbsCount>",
            trackDur, escaped_uri,
            relTime, relTime);

        free(escaped_uri);

        if (written >= sizeof(content)) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 716, "Response too large");
        }

        IXML_Document *resp = create_response_document(request->ActionName, service_type, content);
        if (resp) {
            request->ActionResult = resp;
        } else {
            LOG_ERROR( "Failed to create response document for GetPositionInfo");
            LOG_ERROR( "Content was: %s", content);
        }
    }
    else if (strcmp(request->ActionName, "GetTransportInfo") == 0) {
        // 构造响应XML
        const char *transport_state;
        if (g_renderer_ctx.playing) {
            transport_state = "PLAYING";
        } else if (g_renderer_ctx.paused) {
            transport_state = "PAUSED_PLAYBACK";
        } else {
            transport_state = "STOPPED";
        }

        char content[512];
        snprintf(content, sizeof(content),
            "<CurrentTransportState>%s</CurrentTransportState>"
            "<CurrentTransportStatus>OK</CurrentTransportStatus>"
            "<CurrentSpeed>1</CurrentSpeed>",
            transport_state);

        IXML_Document *resp = create_response_document(request->ActionName, service_type, content);
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "GetMediaInfo") == 0) {
        // 获取当前播放位置和总时长
        int curr_sec = 0, total_sec = 0;
        player_get_position(&curr_sec, &total_sec);

        char relTime[16], trackDur[16];
        snprintf(relTime, sizeof(relTime), "%02d:%02d:%02d",
                 curr_sec/3600, (curr_sec%3600)/60, curr_sec%60);
        snprintf(trackDur, sizeof(trackDur), "%02d:%02d:%02d",
                 total_sec/3600, (total_sec%3600)/60, total_sec%60);

        // 构造响应XML
        char content[8192];
        snprintf(content, sizeof(content),
            "<NrTracks>1</NrTracks>"
            "<MediaDuration>%s</MediaDuration>"
            "<CurrentURI>%s</CurrentURI>"
            "<CurrentURIMetaData></CurrentURIMetaData>"
            "<NextURI></NextURI>"
            "<NextURIMetaData></NextURIMetaData>"
            "<PlayMedium>NETWORK</PlayMedium>"
            "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
            "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
            trackDur, g_renderer_ctx.current_uri);

        IXML_Document *resp = create_response_document(request->ActionName, service_type, content);
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "GetVolume") == 0) {
        // 获取声道参数，默认为Master
        const char* channel = get_action_argument(request, "Channel");
        if (!channel) {
            channel = "Master";
        }

        // 获取当前音量值
        int volume = 0;
        if (strcmp(channel, "Master") == 0) {
            volume = player_get_volume(); // 假设有这样一个函数
        } else {
            // 可以处理其他声道如 LF, RF 等
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 710, "Unsupported channel");
        }

        // 构造响应XML
        char content[128];
        snprintf(content, sizeof(content),
                 "<CurrentVolume>%d</CurrentVolume>", volume);

        IXML_Document *resp = create_response_document(request->ActionName, service_type, content);
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "SetVolume") == 0) {
        // 获取声道参数
        const char* channel = get_action_argument(request, "Channel");
        if (!channel) {
            channel = "Master";
        }

        // 获取音量值
        const char* desired_volume = get_action_argument(request, "DesiredVolume");
        if (!desired_volume) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 711, "Missing volume value");
        }

        int volume = atoi(desired_volume);
        if (volume < 0 || volume > 100) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 712, "Volume out of range");
        }

        // 设置音量
        int ret = 0;
        if (strcmp(channel, "Master") == 0) {
            ret = player_set_volume(volume);
        } else {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 713, "Unsupported channel");
        }

        if (ret != 0) {
            pthread_mutex_unlock(&renderer_mutex);
            return set_error_response(request, 714, "Set volume failed");
        }

        IXML_Document *resp = create_response_document(request->ActionName, service_type, "");
        if (resp) {
            request->ActionResult = resp;
        }
    }
    else if (strcmp(request->ActionName, "GetMute") == 0) {

    }
    else if (strcmp(request->ActionName, "SetMute") == 0) {

    }else {
        LOG_ERROR( "Unhandled action: %s", request->ActionName);
        pthread_mutex_unlock(&renderer_mutex);
        return set_error_response(request, 709, "Unsupported action");
    }

    pthread_mutex_unlock(&renderer_mutex);
    return UPNP_E_SUCCESS;
}

static int device_event_handler(Upnp_EventType event_type, void* event, void* cookie) {
    switch (event_type) {
        case UPNP_EVENT_SUBSCRIPTION_REQUEST:
            LOG_INFO("[EVENT] Subscription request");
            break;
        case UPNP_CONTROL_ACTION_REQUEST:
            return action_handler(event_type, event, cookie);
        case UPNP_EVENT_RECEIVED:
            LOG_INFO("[EVENT] Event received");
            break;
        default:
            LOG_INFO("Unhandled event: %d", event_type);
            break;
    }
    return UPNP_E_SUCCESS;
}

char* generate_device_description(const char* udn) {
    char hostname[256];
    gethostname(hostname, sizeof(hostname) - 1);
    hostname[sizeof(hostname)-1] = '\0';

    // 使用更安全的动态分配大小
    const char* templ_fmt =
        "<?xml version=\"1.0\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "  <specVersion>"
        "    <major>1</major>"
        "    <minor>0</minor>"
        "  </specVersion>"
        "  <device>"
        "    <deviceType>%s</deviceType>"
        "    <friendlyName>DLNA Renderer (%s)</friendlyName>"
        "    <manufacturer>Open Source Project</manufacturer>"
        "    <manufacturerURL>https://github.com</manufacturerURL>"
        "    <modelDescription>UPnP Media Renderer</modelDescription>"
        "    <modelName>MediaRenderer</modelName>"
        "    <modelNumber>1.0</modelNumber>"
        "    <serialNumber>12345678</serialNumber>"
        "    <UDN>%s</UDN>"
        "    <iconList>"
        "      <icon>"
        "        <mimetype>image/png</mimetype>"
        "        <width>64</width>"
        "        <height>64</height>"
        "        <depth>24</depth>"
        "        <url>/virtual/grender-64x64.png</url>"
        "      </icon>"
        "      <icon>"
        "        <mimetype>image/png</mimetype>"
        "        <width>128</width>"
        "        <height>128</height>"
        "        <depth>24</depth>"
        "        <url>/virtual/grender-128x128.png</url>"
        "      </icon>"
        "    </iconList>"
        "    <serviceList>"
        "      <service>"
        "        <serviceType>%s</serviceType>"
        "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "        <SCPDURL>/virtual/AVTransport.xml</SCPDURL>"
        "        <controlURL>/virtual/control/AVTransport</controlURL>"
        "        <eventSubURL>/virtual/event/AVTransport</eventSubURL>"
        "      </service>"
        "      <service>"
        "        <serviceType>%s</serviceType>"
        "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "        <SCPDURL>/virtual/RenderingControl.xml</SCPDURL>"
        "        <controlURL>/virtual/control/RenderingControl</controlURL>"
        "        <eventSubURL>/virtual/event/RenderingControl</eventSubURL>"
        "      </service>"
        "    </serviceList>"
        "  </device>"
        "</root>";

    // 计算所需空间
    size_t needed = snprintf(NULL, 0, templ_fmt,
                           UPNP_DEVICE_TYPE, hostname, udn,
                           AVTRANSPORT_SERVICE, RENDERING_SERVICE) + 1;

    char* desc = malloc(needed);
    if (!desc) return NULL;

    snprintf(desc, needed, templ_fmt,
             UPNP_DEVICE_TYPE, hostname, udn,
             AVTRANSPORT_SERVICE, RENDERING_SERVICE);

    return desc;
}

int main(int argc, char *argv[]) {
    int rc;
    const char *interface = NULL;
    unsigned short port = 49494;
    UpnpDevice_Handle device_handle = 0;

    LOG_INFO("===== Starting DLNA Media Renderer =====");

    // 初始化播放器模块
    if (player_init() != 0) {
        LOG_ERROR("Failed to initialize player");
        return EXIT_FAILURE;
    }

    signal(SIGINT, handle_sigint);

    // 生成唯一设备ID
    char uuid_str[37];
    generate_uuid(uuid_str);
    char udn[64];
    snprintf(udn, sizeof(udn), "uuid:%s", uuid_str);
    LOG_DEBUG("Device UDN: %s", udn);

    VirtualFileEntry vfiles[] = {
        {"./icons/grender-64x64.png", "/virtual/grender-64x64.png", "image/png"},
        {"./icons/grender-128x128.png", "/virtual/grender-128x128.png", "image/png"},
        {"./service/AVTransport.xml", "/virtual/AVTransport.xml", "text/xml"},
        {"./service/RenderingControl.xml", "/virtual/RenderingControl.xml", "text/xml"},
    };

    // 加载虚拟文件
    if (load_virtual_files(vfiles, sizeof(vfiles)/sizeof(vfiles[0])) != 0) {
        LOG_ERROR("Failed to load virtual files");
        goto cleanup;
    }

    // 初始化UPnP
    rc = UpnpInit2(interface, port);
    if (rc != UPNP_E_SUCCESS) {
        LOG_ERROR( "UpnpInit2 failed: %s", UpnpGetErrorMessage(rc));
        goto cleanup;
    }

    LOG_INFO("UPnP running at %s:%d",
           UpnpGetServerIpAddress(), UpnpGetServerPort());

    // 注册web服务回调函数
    if (webserver_register_callbacks() != 0) {
        goto cleanup;
    }

    // 生成设备描述
    char* desc_xml = generate_device_description(udn);
    if (!desc_xml) {
        LOG_ERROR( "Failed to generate device description");
        goto cleanup;
    }

    // 注册根设备
    rc = UpnpRegisterRootDevice2(
        UPNPREG_BUF_DESC,
        desc_xml, strlen(desc_xml),
        TRUE,
        device_event_handler,
        NULL,
        &device_handle
    );
    free(desc_xml);

    if (rc != UPNP_E_SUCCESS) {
        LOG_ERROR( "Device registration failed: %s",
                UpnpGetErrorMessage(rc));
        goto cleanup;
    }

    rc = UpnpSendAdvertisement(device_handle, 1800);
    if (rc != UPNP_E_SUCCESS) {
        LOG_ERROR( "Advertisement failed: %s",
                UpnpGetErrorMessage(rc));
        goto cleanup;
    }

    LOG_INFO("DLNA Renderer is running. Press Ctrl+C to exit...");

    while (running) {
        sleep(10);
        // 定期重新发送广告
        UpnpSendAdvertisement(device_handle, 1800);
    }

cleanup:
    LOG_INFO("===== Cleaning up resources =====");

    // 释放资源
    player_deinit();
    free_virtual_files();

    if (device_handle) {
        UpnpUnRegisterRootDevice(device_handle);
    }

    UpnpFinish();
    pthread_mutex_destroy(&renderer_mutex);

    LOG_INFO("DLNA Renderer exited cleanly");
    return EXIT_SUCCESS;
}
