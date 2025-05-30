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

#define VIRTUAL_DIR "/virtual"
#define UPNP_DEVICE_TYPE "urn:schemas-upnp-org:device:MediaRenderer:1"
#define AVTRANSPORT_SERVICE "urn:schemas-upnp-org:service:AVTransport:1"
#define RENDERING_SERVICE "urn:schemas-upnp-org:service:RenderingControl:1"

// 虚拟文件系统结构
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
    const char *virtual_fname;
    const char *content_type;
    const char *data;
    size_t len;
    struct virtual_file *next;
}*virtual_files = NULL;

// 生成UUID
void generate_uuid(char *uuid_str) {
    uuid_t uuid;
    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);
}

// 读取文件到内存
static char *read_file_to_memory(const char *filepath, size_t *length) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    char *buf = malloc(st.st_size);
    if (!buf) {
        close(fd);
        return NULL;
    }

    ssize_t bytes_read = read(fd, buf, st.st_size);
    close(fd);

    if (bytes_read != st.st_size) {
        free(buf);
        return NULL;
    }

    *length = st.st_size;
    return buf;
}

// 创建虚拟文件
int create_virtual_file(const char *real_path, const char *virtual_path, const char *content_type) {
    size_t size;
    char *data = read_file_to_memory(real_path, &size);
    if (!data) return -1;

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

    return 0;
}

// 批量加载虚拟文件
int load_virtual_files(const VirtualFileEntry *entries, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (create_virtual_file(entries[i].real_path,
                              entries[i].virtual_path,
                              entries[i].content_type) != 0) {
            fprintf(stderr, "Failed to load: %s\n", entries[i].real_path);
            return -1;
        }
    }
    return 0;
}

// 通过文件名获取虚拟文件
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

// 虚拟目录回调函数实现
int my_get_info(const char *filename, UpnpFileInfo *info) {
    struct virtual_file *virtfile = get_file_by_name(filename);

    if (virtfile) {
        UpnpFileInfo_set_FileLength(info, virtfile->len);
        UpnpFileInfo_set_LastModified(info, time(NULL));
        UpnpFileInfo_set_IsDirectory(info, 0);
        UpnpFileInfo_set_IsReadable(info, 1);
        UpnpFileInfo_set_ContentType(info, ixmlCloneDOMString(virtfile->content_type));
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
    return -1; // 只读不支持写入
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

// 虚拟目录回调结构体
struct UpnpVirtualDirCallbacks virtual_dir_callbacks = {
    .get_info = my_get_info,
    .open = my_open,
    .read = my_read,
    .close = my_close,
    .write = my_write,
    .seek = my_seek,
};

// 注册虚拟目录回调
int webserver_register_callbacks(void) {
    int rc = UpnpSetVirtualDirCallbacks(&virtual_dir_callbacks);
    if (rc != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpSetVirtualDirCallbacks failed: %s (%d)\n",
                UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    rc = UpnpAddVirtualDir(VIRTUAL_DIR);
    if (rc != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpAddVirtualDir failed: %s (%d)\n",
                UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    return 0;
}

// 动作处理函数
int action_handler(Upnp_EventType event_type, void* event, void* cookie) {
    switch (event_type) {
        case UPNP_CONTROL_ACTION_REQUEST: {
            struct Upnp_Action_Request* request = (struct Upnp_Action_Request*)event;
            printf("Action request: %s for service: %s\n",
                   request->ActionName, request->ServiceID);

            // 创建响应文档
            IXML_Document* response = NULL;
            const char* service_type = NULL;

            if (strstr(request->ServiceID, "AVTransport")) {
                service_type = AVTRANSPORT_SERVICE;
            } else if (strstr(request->ServiceID, "RenderingControl")) {
                service_type = RENDERING_SERVICE;
            }

            // 创建基本响应
            char resp_buf[512];
            snprintf(resp_buf, sizeof(resp_buf),
                    "<u:%sResponse xmlns:u=\"%s\"></u:%sResponse>",
                    request->ActionName, service_type, request->ActionName);

            response = ixmlParseBuffer(resp_buf);

            // 根据动作类型添加参数
            if (strcmp(request->ActionName, "SetAVTransportURI") == 0) {
                UpnpAddToActionResponse(&response, "SetAVTransportURIResponse",
                                      service_type, "CurrentURIMetaData", "");
            } else if (strcmp(request->ActionName, "Play") == 0) {
                UpnpAddToActionResponse(&response, "PlayResponse",
                                      service_type, "Speed", "1");
            }

            request->ActionResult = response;
            return UPNP_E_SUCCESS;
        }
        default:
            return UPNP_E_SUCCESS;
    }
}

static int device_event_handler(Upnp_EventType event_type, void* event, void* cookie) {
    switch (event_type) {

        case UPNP_EVENT_SUBSCRIPTION_REQUEST: {
            printf("[EVENT] Subscription request\n");
            struct Upnp_Subscription_Request* sub_req =
                (struct Upnp_Subscription_Request*)event;
            printf("Service ID: %s\n", sub_req->ServiceId);
            // 处理订阅请求，例如调用 UpnpAcceptSubscription()
            break;
        }

        case UPNP_CONTROL_ACTION_REQUEST:
            // 处理控制命令
            return action_handler(event_type, event, cookie);

        case UPNP_EVENT_RECEIVED: {
            printf("[EVENT] Event received\n");
            // 处理事件通知
            break;
        }

        default:
            printf("Unhandled event: %d\n", event_type);
            break;
    }
    return UPNP_E_SUCCESS;
}


// 生成设备描述XML
char* generate_device_description(const char* udn) {
    char hostname[256];
    gethostname(hostname, sizeof(hostname));

    const char* templ =
        "<?xml version=\"1.0\"?>"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">"
        "  <specVersion>"
        "    <major>1</major>"
        "    <minor>0</minor>"
        "  </specVersion>"
        "  <device>"
        "    <deviceType>" UPNP_DEVICE_TYPE "</deviceType>"
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
        "        <serviceType>" AVTRANSPORT_SERVICE "</serviceType>"
        "        <serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
        "        <SCPDURL>/virtual/AVTransport.xml</SCPDURL>"
        "        <controlURL>/virtual/control/AVTransport</controlURL>"
        "        <eventSubURL>/virtual/event/AVTransport</eventSubURL>"
        "      </service>"
        "      <service>"
        "        <serviceType>" RENDERING_SERVICE "</serviceType>"
        "        <serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
        "        <SCPDURL>/virtual/RenderingControl.xml</SCPDURL>"
        "        <controlURL>/virtual/control/RenderingControl</controlURL>"
        "        <eventSubURL>/virtual/event/RenderingControl</eventSubURL>"
        "      </service>"
        "    </serviceList>"
        "  </device>"
        "</root>";

    char* desc = malloc(4096);
    snprintf(desc, 4096, templ, hostname, udn);
    return desc;
}

int main(int argc, char *argv[]) {
    int rc;
    const char *interface = NULL; // 或用你的实际网卡名
    unsigned short port = 49494; // 0 自动分配端口

    // 生成唯一设备ID
    char uuid_str[37];
    generate_uuid(uuid_str);
    char udn[64];
    snprintf(udn, sizeof(udn), "uuid:%s", uuid_str);
    printf("Device UDN: %s\n", udn);

    // 加载虚拟文件（必须包含服务描述文件）
    VirtualFileEntry vfiles[] = {
        {"./IUpnpInfoFile.txt", "/virtual/IUpnpInfoFile.txt", "text/plain"},
        {"./icons/grender-64x64.png", "/virtual/grender-64x64.png", "image/png"},
        {"./icons/grender-128x128.png", "/virtual/grender-128x128.png", "image/png"},
        {"./service/AVTransport.xml", "/virtual/AVTransport.xml", "text/xml"},
        {"./service/RenderingControl.xml", "/virtual/RenderingControl.xml", "text/xml"},
    };

    if (load_virtual_files(vfiles, sizeof(vfiles)/sizeof(vfiles[0])) != 0) {
        fprintf(stderr, "Failed to load virtual files\n");
        return 1;
    }

    // 初始化UPnP
    rc = UpnpInit2(NULL, port);
    if (rc != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpInit2 failed: %s\n", UpnpGetErrorMessage(rc));
        return 1;
    }

    printf("UPnP running at %s:%d\n",
           UpnpGetServerIpAddress(), UpnpGetServerPort());

    if (webserver_register_callbacks() != 0) {
        return 1;
    }

    // 生成设备描述
    char* desc_xml = generate_device_description(udn);
    size_t desc_len = strlen(desc_xml);

    // 启用HTTP服务器
    if (UpnpEnableWebserver(TRUE) != UPNP_E_SUCCESS) {
        fprintf(stderr, "Failed to enable webserver\n");
        free(desc_xml);
        return 1;
    }

    // 注册根设备
    UpnpDevice_Handle device_handle;
    rc = UpnpRegisterRootDevice2(
        UPNPREG_BUF_DESC,
        desc_xml, desc_len,
        TRUE,  // 启用阻塞调用
        device_event_handler,
        NULL,  // Cookie
        &device_handle
    );
    free(desc_xml);

    if (rc != UPNP_E_SUCCESS) {
        fprintf(stderr, "Device registration failed: %s\n",
                UpnpGetErrorMessage(rc));
        return 1;
    }

    // 发送设备广播
    rc = UpnpSendAdvertisement(device_handle, 1800);
    if (rc != UPNP_E_SUCCESS) {
        fprintf(stderr, "Advertisement failed: %s\n",
                UpnpGetErrorMessage(rc));
        return 1;
    }

    printf("DLNA Renderer is running. Press Ctrl+C to exit...\n");

    // 主循环
    while (1) {
        sleep(10);
        // 定期重新发送广播
        UpnpSendAdvertisement(device_handle, 1800);
    }

    UpnpFinish();
    return 0;
}
