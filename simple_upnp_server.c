#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <upnp/upnp.h>

UpnpDevice_Handle device_handle = -1;

// 设备描述文件
const char *device_description =
"<?xml version=\"1.0\"?>\n"
"<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n"
"  <specVersion>\n"
"    <major>1</major>\n"
"    <minor>0</minor>\n"
"  </specVersion>\n"
"  <device>\n"
"    <deviceType>urn:schemas-upnp-org:device:MediaServer:1</deviceType>\n"
"    <friendlyName>Simple DLNA Server</friendlyName>\n"
"    <manufacturer>DeiDei Inc.</manufacturer>\n"
"    <modelName>SimpleDLNA</modelName>\n"
"    <UDN>uuid:12345678-90ab-cdef-1234-567890abcdef</UDN>\n"
"  </device>\n"
"</root>\n";

// 控制信号处理
void handle_sigint(int sig)
{
    printf("Shutting down UPnP device...\n");
    if (device_handle != -1)
        UpnpUnRegisterRootDevice(device_handle);
    UpnpFinish();
    exit(0);
}

// 回调函数
int callback(Upnp_EventType EventType, void *Event, void *Cookie)
{
    (void)Event;
    (void)Cookie;

    switch (EventType) {
        case UPNP_EVENT_SUBSCRIPTION_REQUEST:
            printf("Subscription request received\n");
            break;
        case UPNP_CONTROL_ACTION_REQUEST:
            printf("Action request received\n");
            break;
        default:
            printf("Other event type: %d\n", EventType);
            break;
    }
    return UPNP_E_SUCCESS;
}

int main(int argc, char *argv[])
{
    int ret;

    signal(SIGINT, handle_sigint);

    // 初始化 libupnp
    ret = UpnpInit2(NULL, 0);
    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpInit failed: %s\n", UpnpGetErrorMessage(ret));
        return 1;
    }

    // 获取本地地址
    const char *ip_address = UpnpGetServerIpAddress();
    unsigned short port = UpnpGetServerPort();

    printf("UPnP server initialized at %s:%d\n", ip_address, port);

    // 注册 root 设备
    ret = UpnpRegisterRootDevice2(
        UPNPREG_BUF_DESC,
        device_description,
        strlen(device_description),
        1,
        callback,
        NULL,
        &device_handle);

    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpRegisterRootDevice2 failed: %s\n", UpnpGetErrorMessage(ret));
        UpnpFinish();
        return 1;
    }

    // 启动设备广播
    ret = UpnpSendAdvertisement(device_handle, 1800);
    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpSendAdvertisement failed: %s\n", UpnpGetErrorMessage(ret));
        UpnpUnRegisterRootDevice(device_handle);
        UpnpFinish();
        return 1;
    }

    printf("DLNA/UPnP Device is now running...\n");
    printf("Press Ctrl+C to exit.\n");

    // 保持运行状态
    while (1) {
        sleep(1);
    }

    return 0;
}

