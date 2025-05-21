#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <upnp/upnp.h>

UpnpClient_Handle client_handle = -1;

void handle_sigint(int sig)
{
    printf("Shutting down UPnP client...\n");
    if (client_handle != -1)
        UpnpUnRegisterClient(client_handle);
    UpnpFinish();
    exit(0);
}

// 回调函数：接收设备发现事件
int client_callback(Upnp_EventType EventType, void *Event, void *Cookie)
{
    (void)Cookie;

    if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE ||
        EventType == UPNP_DISCOVERY_SEARCH_RESULT)
    {
        struct Upnp_Discovery *d_event = (struct Upnp_Discovery *)Event;

        if (d_event->ErrCode != UPNP_E_SUCCESS) {
            printf("Discovery error: %d\n", d_event->ErrCode);
            return 0;
        }

        printf("\nDevice Found:\n");
        printf("  Device Type:     %s\n", d_event->DeviceType);
        printf("  USN:             %s\n", d_event->DeviceId);
        printf("  Location:        %s\n", d_event->Location);
        printf("  Server:          %s\n", d_event->Os);
        printf("  Ext:             %s\n", d_event->Ext == NULL ? "(none)" : d_event->Ext);

    } else if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_BYEBYE) {
        struct Upnp_Discovery *d_event = (struct Upnp_Discovery *)Event;
        printf("\nDevice ByeBye:\n");
        printf("  USN:             %s\n", d_event->DeviceId);
    }

    return 0;
}

int main()
{
    int ret;

    signal(SIGINT, handle_sigint);

    // 初始化 UPnP 客户端环境
    ret = UpnpInit2(NULL, 0);
    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpInit failed: %s\n", UpnpGetErrorMessage(ret));
        return 1;
    }

    printf("UPnP client initialized on IP: %s\n", UpnpGetServerIpAddress());

    // 注册客户端
    ret = UpnpRegisterClient(client_callback, NULL, &client_handle);
    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpRegisterClient failed: %s\n", UpnpGetErrorMessage(ret));
        UpnpFinish();
        return 1;
    }

    // 发送 M-SEARCH 查找设备（MediaServer）
    const char *target = "urn:schemas-upnp-org:device:MediaServer:1";
    ret = UpnpSearchAsync(client_handle, 5, target, NULL);
    if (ret != UPNP_E_SUCCESS) {
        fprintf(stderr, "UpnpSearchAsync failed: %s\n", UpnpGetErrorMessage(ret));
        UpnpUnRegisterClient(client_handle);
        UpnpFinish();
        return 1;
    }

    printf("Searching for UPnP MediaServer devices...\n");
    printf("Press Ctrl+C to stop.\n");

    // 保持运行以接收事件
    while (1) {
        sleep(1);
    }

    return 0;
}

