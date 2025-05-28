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

typedef struct {
    const char *real_path;     // 实际路径，如 "/opt/data/image.png"
    const char *virtual_path;  // 虚拟路径，如 "/virtual/image.png"
    const char *content_type;  // MIME 类型，如 "image/png"
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
    vf->content_type = content_type;
    vf->data = data;
    vf->len = size;
    vf->next = virtual_files;
    virtual_files = vf;

    return 0;
}

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

int my_get_info(const char *filename, UpnpFileInfo *info)
{
    struct virtual_file *virtfile = get_file_by_name(filename);

    if (virtfile) {
        UpnpFileInfo_set_FileLength(info, virtfile->len);
        UpnpFileInfo_set_LastModified(info, time(NULL));
        UpnpFileInfo_set_IsDirectory(info, 0);
        UpnpFileInfo_set_IsReadable(info, 1);
        UpnpFileInfo_set_ContentType(info, (char*)ixmlCloneDOMString(virtfile->content_type));
        return 0;
    }
    return -1;
}

UpnpWebFileHandle my_open(const char *filename, enum UpnpOpenFileMode Mode)
{
    if (Mode != UPNP_READ) {
        printf("%s: ignoring request to open file for writing.",filename);
        return NULL;
    }
    struct virtual_file *virtfile = virtual_files;
    while (virtfile) {
        if (strcmp(filename, virtfile->virtual_fname) == 0) {
	    printf("szbaijie [%s] open %s\n",__func__,virtfile->virtual_fname);
	    WebServerFile *file = (WebServerFile*)malloc(sizeof(WebServerFile));
            file->pos = 0;
            file->len = virtfile->len;
            file->data = virtfile->data;
            return (UpnpWebFileHandle)file;//交由read,close,seek函数管理
        }
        virtfile = virtfile->next;
    }

    return NULL;
}

static inline int minimum(int a, int b)
{
    return (a<b)?a:b;
}

int my_read(UpnpWebFileHandle fileHnd, char *buf, size_t buflen) {
    printf("szbaijie [%s] start, buflen=%lu\n",__func__, buflen);
    WebServerFile *file = (WebServerFile *) fileHnd;
    ssize_t len = -1;

    len = minimum(buflen, file->len - file->pos);
    memcpy(buf, file->data + file->pos, len);

    if (len < 0) {
        printf("szbaijie webserver In %s: %s", __FUNCTION__, strerror(errno));

    } else {
        file->pos += len;
    }

    return len;
}

int my_close(UpnpWebFileHandle fileHnd) {
    printf("[%s] start\n", __func__);
    WebServerFile *file = (WebServerFile *)fileHnd;
    if (file) {
        free((void *)file);  // 只释放包装结构体，不释放 data 指针（data 由虚拟文件链表管理）
    }
    return 0;
}
int my_write(UpnpWebFileHandle fileHnd, char *buf, size_t buflen) {
    printf("szbaijie [%s] write attempt denied, buflen=%zu\n", __func__, buflen);
    return -1;  // 只读虚拟文件系统，不支持写入
}
int my_seek(UpnpWebFileHandle fileHnd, off_t offset, int origin) {
    printf("[%s] offset=%ld, origin=%d\n", __func__, offset, origin);
    WebServerFile *file = (WebServerFile *)fileHnd;
    if (!file) return -1;

    off_t new_pos = 0;

    switch (origin) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = file->pos + offset;
            break;
        case SEEK_END:
            new_pos = (off_t)file->len + offset;
            break;
        default:
            return -1;
    }

    // 边界检查
    if (new_pos < 0 || (size_t)new_pos > file->len) {
        return -1;
    }

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
        printf("UpnpSetVirtualDirCallbacks() failed: %s (%d)",
                  UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    rc = UpnpAddVirtualDir("/virtual");
    if (rc != UPNP_E_SUCCESS) {
        printf( "UpnpAddVirtualDir() failed: %s (%d)",
                  UpnpGetErrorMessage(rc), rc);
        return -1;
    }

    return 0;
}

static int device_event_handler(Upnp_EventType EventType, void *Event, void *Cookie) {
    // 简化处理：直接忽略事件
    return 0;
}

static const char *GetVersionInfo(char *buffer, size_t len) {
    snprintf(buffer, len, "(libupnp-%s)",UPNP_VERSION_STRING);
    return buffer;
}
int main(int argc, char *argv[]) {
    int rc;
    char version[1024];
    const char *interface ="wlan0"; // 或用你的实际网卡名
    unsigned short port = 49494; // 0 表示随机分配端口

    UpnpDevice_Handle device_handle;
    GetVersionInfo(version,sizeof(version));
    printf("szbaijie version: %s, %d\n",version,UPNP_VERSION);

    VirtualFileEntry vfiles[] = {
	{"/root/dlna_test/IUpnpInfoFile.txt","/virtual/IUpnpInfoFile.txt","txt/plain"},
        { "/usr/local/share/gmediarender/grender-64x64.png","/virtual/grender-64x64.png","image/png" },
        { "/usr/local/share/gmediarender/grender-128x128.png","/virtual/grender-128x128.png","image/png"},
    };
    if (load_virtual_files(vfiles, sizeof(vfiles) / sizeof(vfiles[0])) != 0) {
        fprintf(stderr, "Some virtual files failed to load.\n");
    }

    rc = UpnpInit2(interface, port);
    if (rc != UPNP_E_SUCCESS){
    	printf("UpnpInit2 failed: %s\n", UpnpGetErrorMessage(rc));
    	return 1;
    }

    printf("UPnP started at %s:%d\n", UpnpGetServerIpAddress(), UpnpGetServerPort());
    if(webserver_register_callbacks() == -1){
    	return -1;	
    }

    // 简单设备描述 XML（必须合法）
    const char *desc =
        "<?xml version=\"1.0\"?>\n"
        "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
        "  <specVersion><major>1</major><minor>0</minor></specVersion>\n"
        "  <device>\n"
        "    <deviceType>urn:schemas-upnp-org:device:Basic:1</deviceType>\n"
        "    <friendlyName>Demo Device</friendlyName>\n"
        "    <manufacturer>deidei inc.</manufacturer>\n"
        "    <modelName>MiniUPNPDevice</modelName>\n"
        "    <UDN>uuid:12345678-1234-1234-1234-123456789abc</UDN>\n"
        "  </device>\n"
        "</root>\n";

    rc = UpnpEnableWebserver(TRUE);
    if (rc != UPNP_E_SUCCESS) {
        printf("Enable webserver failed: %s\n", UpnpGetErrorMessage(rc));
        return 1;
    }

    rc = UpnpRegisterRootDevice2(
        UPNPREG_BUF_DESC,
        desc, strlen(desc), 1,
        &device_event_handler, NULL,
        &device_handle
    );

    if (rc != UPNP_E_SUCCESS) {
        printf("Register device failed: %s\n", UpnpGetErrorMessage(rc));
        return 1;
    }

    rc = UpnpSendAdvertisement(device_handle, 1800);
    if (rc != UPNP_E_SUCCESS) {
        printf("Send advertisement failed: %s\n", UpnpGetErrorMessage(rc));
        return 1;
    }

    printf("Device announced. Waiting...\n");

    while (1) {
        sleep(60);
    }

    UpnpFinish();
    return 0;
}

