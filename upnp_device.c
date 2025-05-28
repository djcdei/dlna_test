#include <upnp/upnp.h>
#include <upnp/upnptools.h>
#include <upnp/upnpdebug.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static int device_event_handler(Upnp_EventType EventType, void *Event, void *Cookie) {
    // 简化处理：直接忽略事件
    return 0;
}

int main(int argc, char *argv[]) {
    int rc;
    const char *interface ="eth0"; // 或用你的实际网卡名
    unsigned short port = 49494; // 0 表示随机分配端口

    UpnpDevice_Handle device_handle;

    rc = UpnpInit2(interface, port);
    //int retries_left = 30;
    //static const int kRetryTimeMs = 1000;
    //while (rc != UPNP_E_SUCCESS && retries_left--){
    //    usleep(kRetryTimeMs * 1000);
    //	printf("UpnpInit2 failed: %s\n", UpnpGetErrorMessage(rc));
    //	rc = UpnpInit2(interface, port);
    //}
    if (rc != UPNP_E_SUCCESS){
    	printf("UpnpInit2 failed: %s\n", UpnpGetErrorMessage(rc));
    	return 1;
    }

    printf("UPnP started at %s:%d\n", UpnpGetServerIpAddress(), UpnpGetServerPort());

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
        device_event_handler, NULL,
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

