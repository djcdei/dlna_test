#include <upnp/upnp.h>
#include <upnp/upnptools.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ctrlpt_callback(Upnp_EventType EventType, void *Event, void *Cookie) {
    if (EventType == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE ||
        EventType == UPNP_DISCOVERY_SEARCH_RESULT) {

        struct Upnp_Discovery *d_event = (struct Upnp_Discovery *)Event;

        if (d_event->ErrCode != UPNP_E_SUCCESS) {
            printf("Discovery error: %d\n", d_event->ErrCode);
            return 0;
        }

        printf("\n[Device Found]\n");
        printf("Device Type  : %s\n", d_event->DeviceType);
        printf("Device UDN   : %s\n", d_event->DeviceId);
        printf("Location URL : %s\n", d_event->Location);
        printf("Service Type : %s\n", d_event->ServiceType);
        printf("-----------------------------------\n");
    }

    return 0;
}

int main(int argc, char *argv[]) {
    int rc;
    UpnpClient_Handle ctrlpt_handle;

    // 初始化 libupnp
    rc = UpnpInit(NULL, 0);
    if (rc != UPNP_E_SUCCESS) {
        printf("UpnpInit failed: %s\n", UpnpGetErrorMessage(rc));
        return 1;
    }

    printf("UPnP initialized at %s:%d\n", 
           UpnpGetServerIpAddress(), 
           UpnpGetServerPort());

    // 注册控制点
    rc = UpnpRegisterClient(ctrlpt_callback, NULL, &ctrlpt_handle);
    if (rc != UPNP_E_SUCCESS) {
        printf("UpnpRegisterClient failed: %s\n", UpnpGetErrorMessage(rc));
        UpnpFinish();
        return 1;
    }

    // 主动搜索局域网内设备（搜索所有UPnP设备）
    rc = UpnpSearchAsync(ctrlpt_handle, 5, "ssdp:all", NULL);
    if (rc != UPNP_E_SUCCESS) {
        printf("UpnpSearchAsync failed: %s\n", UpnpGetErrorMessage(rc));
        UpnpUnRegisterClient(ctrlpt_handle);
        UpnpFinish();
        return 1;
    }

    printf("Searching for devices...\n");

    // 运行一段时间等待响应
    for (int i = 0; i < 6; ++i) {
        sleep(5);
    }

    // 清理资源
    UpnpUnRegisterClient(ctrlpt_handle);
    UpnpFinish();

    return 0;
}

