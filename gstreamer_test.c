#include <gst/gst.h>
#include <stdio.h>

static void on_pad_added(GstElement *src, GstPad *pad, gpointer data) {
    GstElement *convert = (GstElement *)data;
    GstPad *sink_pad = gst_element_get_static_pad(convert, "sink");

    printf("[pad-added] Received new pad '%s' from '%s'\n",
           GST_PAD_NAME(pad), GST_ELEMENT_NAME(src));

    if (gst_pad_is_linked(sink_pad)) {
        printf("[pad-added] Sink pad already linked. Ignoring.\n");
        gst_object_unref(sink_pad);
        return;
    }

    if (gst_pad_link(pad, sink_pad) == GST_PAD_LINK_OK) {
        printf("[pad-added] Successfully linked decoder to convert\n");
    } else {
        printf("[pad-added] Failed to link decoder to convert\n");
    }

    gst_object_unref(sink_pad);
}

int main(int argc, char *argv[]) {
    GstElement *pipeline, *source, *convert, *resample, *sink;
    GstBus *bus;
    GstMessage *msg;
    GstStateChangeReturn ret;

    if (argc != 2) {
        printf("Usage: %s <http://url/audio.mp3>\n", argv[0]);
        return -1;
    }

    printf("[init] Initializing GStreamer...\n");
    gst_init(&argc, &argv);

    printf("[init] Creating GStreamer elements...\n");
    pipeline = gst_pipeline_new("audio-player");
    source = gst_element_factory_make("uridecodebin", "source");
    convert = gst_element_factory_make("audioconvert", "convert");
    resample = gst_element_factory_make("audioresample", "resample");
    sink = gst_element_factory_make("autoaudiosink", "sink");

    if (!pipeline || !source || !convert || !resample || !sink) {
        printf("[error] Failed to create one or more GStreamer elements\n");
        return -1;
    }

    printf("[config] Setting URI: %s\n", argv[1]);
    g_object_set(source, "uri", argv[1], NULL);

    printf("[pipeline] Adding elements to pipeline...\n");
    gst_bin_add_many(GST_BIN(pipeline), source, convert, resample, sink, NULL);

    printf("[pipeline] Linking convert → resample → sink...\n");
    if (!gst_element_link_many(convert, resample, sink, NULL)) {
        printf("[error] Failed to link convert → resample → sink\n");
        gst_object_unref(pipeline);
        return -1;
    }

    printf("[signal] Connecting pad-added signal handler...\n");
    g_signal_connect(source, "pad-added", G_CALLBACK(on_pad_added), convert);

    printf("[state] Setting pipeline state to PLAYING...\n");
    ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        printf("[error] Unable to set the pipeline to the playing state\n");
        gst_object_unref(pipeline);
        return -1;
    }

    printf("[main] Waiting for end of stream or error...\n");
    bus = gst_element_get_bus(pipeline);
    msg = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
                                     GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

    if (msg != NULL) {
        GError *err;
        gchar *debug;

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR:
                gst_message_parse_error(msg, &err, &debug);
                printf("[error] %s\n", err->message);
                g_error_free(err);
                g_free(debug);
                break;
            case GST_MESSAGE_EOS:
                printf("[eos] End of stream reached\n");
                break;
            default:
                printf("[info] Unexpected message received\n");
                break;
        }
        gst_message_unref(msg);
    }

    printf("[cleanup] Cleaning up...\n");
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return 0;
}

