#include <gio/gio.h>
#include <glib.h>
#include <libssc-sensor.h>
#include <libssc-sensor-accelerometer.h>
#include <libssc-sensor-gyroscope.h>
#include <libssc-sensor-magnetometer.h>
#include <ssc-sensor-accelerometer.pb-c.h>

#define SSC_MSG_REPORT_MEASUREMENT 1025

typedef GObject *(*SensorConstructor)(GCancellable *cancellable, GError **error);

typedef struct {
	GMainLoop *loop;
	GCancellable *cancellable;
	const char *data_type;
	guint timeout_id;
	gboolean success;
} GenericInspectContext;

typedef struct {
	GMainLoop *loop;
	GCancellable *cancellable;
	SSCSensor *sensor;
	GObject *client;
	const char *data_type;
	guint seconds;
	guint timeout_id;
	gulong report_id;
	guint reports;
	guint64 uid_low;
	guint64 uid_high;
	gboolean success;
} StreamContext;

static void
print_properties(const char *label, GObject *sensor)
{
	g_autofree gchar *name = NULL;
	g_autofree gchar *vendor = NULL;
	g_autofree gchar *data_type = NULL;
	guint64 uid_low = 0;
	guint64 uid_high = 0;

	/* libssc 0.4.4 misdeclares several non-identity property types. */
	g_object_get(sensor,
	             "name", &name,
	             "vendor", &vendor,
	             "data-type", &data_type,
	             "uid-low", &uid_low,
	             "uid-high", &uid_high,
	             NULL);
	g_print("[%s]\n"
	        "name             %s\n"
	        "vendor           %s\n"
	        "data-type        %s\n"
	        "uid-low          %" G_GUINT64_FORMAT "\n"
	        "uid-high         %" G_GUINT64_FORMAT "\n\n",
	        label, name, vendor, data_type, uid_low, uid_high);
}

static gboolean
inspect_sensor(const char *label, SensorConstructor constructor)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GObject) sensor = constructor(NULL, &error);

	if (!sensor) {
		g_printerr("[%s]\nerror: %s\n\n", label,
		           error ? error->message : "unknown error");
		return FALSE;
	}

	print_properties(label, sensor);
	return TRUE;
}

static gboolean
generic_inspect_timeout(gpointer user_data)
{
	GenericInspectContext *ctx = user_data;

	ctx->timeout_id = 0;
	g_cancellable_cancel(ctx->cancellable);
	return G_SOURCE_REMOVE;
}

static void
generic_sensor_ready(GObject *source_object, GAsyncResult *result,
		     gpointer user_data)
{
	GenericInspectContext *ctx = user_data;
	g_autoptr(GError) error = NULL;
	g_autoptr(SSCSensor) sensor = NULL;

	(void)source_object;
	sensor = ssc_sensor_new_finish(result, &error);
	if (!sensor) {
		g_printerr("[%s]\nerror: %s\n\n", ctx->data_type,
		           error ? error->message : "sensor unavailable");
	} else {
		print_properties(ctx->data_type, G_OBJECT(sensor));
		ctx->success = TRUE;
	}

	if (ctx->timeout_id)
		g_clear_handle_id(&ctx->timeout_id, g_source_remove);
	g_main_loop_quit(ctx->loop);
}

static gboolean
inspect_data_type(const char *data_type)
{
	GenericInspectContext ctx = {
		.loop = g_main_loop_new(NULL, FALSE),
		.cancellable = g_cancellable_new(),
		.data_type = data_type,
	};

	ctx.timeout_id = g_timeout_add_seconds(20, generic_inspect_timeout,
	                                      &ctx);
	ssc_sensor_new((gchar *)data_type, ctx.cancellable,
	               generic_sensor_ready, &ctx);
	g_main_loop_run(ctx.loop);

	g_object_unref(ctx.cancellable);
	g_main_loop_unref(ctx.loop);
	return ctx.success;
}

static void
stream_close_ready(GObject *source_object, GAsyncResult *result,
		   gpointer user_data)
{
	StreamContext *ctx = user_data;
	g_autoptr(GError) error = NULL;

	if (!ssc_sensor_close_finish(SSC_SENSOR(source_object), result, &error))
		g_printerr("[%s]\ndisable error: %s\n", ctx->data_type,
		           error ? error->message : "unknown error");
	g_main_loop_quit(ctx->loop);
}

static gboolean
stream_stop(gpointer user_data)
{
	StreamContext *ctx = user_data;

	ctx->timeout_id = 0;
	ssc_sensor_close(ctx->sensor, NULL, stream_close_ready, ctx);
	return G_SOURCE_REMOVE;
}

static void
stream_report_received(GObject *client, guint32 msg_id, guint64 uid_high,
		       guint64 uid_low, GArray *buf, gpointer user_data)
{
	StreamContext *ctx = user_data;
	SscAccelerometerResponse *event;

	(void)client;
	if (msg_id != SSC_MSG_REPORT_MEASUREMENT ||
	    uid_high != ctx->uid_high || uid_low != ctx->uid_low)
		return;

	/* sns_std_sensor_event uses the same float-array/status wire schema. */
	event = ssc_accelerometer_response__unpack(
		NULL, buf->len, (const guint8 *)buf->data);
	ctx->reports++;
	g_print("report %u: bytes=%u", ctx->reports, buf->len);
	if (!event) {
		g_print(" decode-error\n");
		return;
	}

	g_print(" values=[");
	for (gsize i = 0; i < event->n_acceleration; i++)
		g_print("%s%g", i ? ", " : "", event->acceleration[i]);
	g_print("] status=%d\n", event->accuracy);
	ssc_accelerometer_response__free_unpacked(event, NULL);
}

static gboolean
stream_operation_timeout(gpointer user_data)
{
	StreamContext *ctx = user_data;

	ctx->timeout_id = 0;
	g_cancellable_cancel(ctx->cancellable);
	return G_SOURCE_REMOVE;
}

static void
stream_open_ready(GObject *source_object, GAsyncResult *result,
		  gpointer user_data)
{
	StreamContext *ctx = user_data;
	g_autoptr(GError) error = NULL;

	if (!ssc_sensor_open_finish(SSC_SENSOR(source_object), result, &error)) {
		g_printerr("[%s]\nopen error: %s\n\n", ctx->data_type,
		           error ? error->message : "unknown error");
		g_main_loop_quit(ctx->loop);
		return;
	}

	if (ctx->timeout_id)
		g_clear_handle_id(&ctx->timeout_id, g_source_remove);
	ctx->success = TRUE;
	ctx->timeout_id = g_timeout_add_seconds(ctx->seconds, stream_stop, ctx);
	g_print("streaming %s for %u seconds\n", ctx->data_type, ctx->seconds);
}

static void
stream_sensor_ready(GObject *source_object, GAsyncResult *result,
		    gpointer user_data)
{
	StreamContext *ctx = user_data;
	g_autoptr(GError) error = NULL;

	(void)source_object;
	ctx->sensor = ssc_sensor_new_finish(result, &error);
	if (!ctx->sensor) {
		g_printerr("[%s]\nerror: %s\n\n", ctx->data_type,
		           error ? error->message : "sensor unavailable");
		g_main_loop_quit(ctx->loop);
		return;
	}

	if (ctx->timeout_id)
		g_clear_handle_id(&ctx->timeout_id, g_source_remove);
	print_properties(ctx->data_type, G_OBJECT(ctx->sensor));
	g_object_get(ctx->sensor,
	             "client", &ctx->client,
	             "uid-low", &ctx->uid_low,
	             "uid-high", &ctx->uid_high,
	             NULL);
	ctx->report_id = g_signal_connect(ctx->client, "report",
	                                  G_CALLBACK(stream_report_received), ctx);
	ctx->timeout_id = g_timeout_add_seconds(20, stream_operation_timeout, ctx);
	ssc_sensor_open(ctx->sensor, ctx->cancellable, stream_open_ready, ctx);
}

static gboolean
stream_data_type(const char *data_type, guint seconds)
{
	StreamContext ctx = {
		.loop = g_main_loop_new(NULL, FALSE),
		.cancellable = g_cancellable_new(),
		.data_type = data_type,
		.seconds = seconds,
	};

	ctx.timeout_id = g_timeout_add_seconds(20, stream_operation_timeout, &ctx);
	ssc_sensor_new((gchar *)data_type, ctx.cancellable,
	               stream_sensor_ready, &ctx);
	g_main_loop_run(ctx.loop);

	if (ctx.timeout_id)
		g_clear_handle_id(&ctx.timeout_id, g_source_remove);
	if (ctx.report_id)
		g_clear_signal_handler(&ctx.report_id, ctx.client);
	g_clear_object(&ctx.client);
	g_clear_object(&ctx.sensor);
	g_object_unref(ctx.cancellable);
	g_main_loop_unref(ctx.loop);

	if (ctx.success && !ctx.reports)
		g_printerr("[%s]\nno measurement reports received\n", data_type);
	return ctx.success && ctx.reports > 0;
}

static GObject *
new_accelerometer(GCancellable *cancellable, GError **error)
{
	return G_OBJECT(ssc_sensor_accelerometer_new_sync(cancellable, error));
}

static GObject *
new_gyroscope(GCancellable *cancellable, GError **error)
{
	return G_OBJECT(ssc_sensor_gyroscope_new_sync(cancellable, error));
}

static GObject *
new_magnetometer(GCancellable *cancellable, GError **error)
{
	return G_OBJECT(ssc_sensor_magnetometer_new_sync(cancellable, error));
}

int
main(int argc, char **argv)
{
	guint failures = 0;
	int i;

	if (argc >= 3 && g_str_equal(argv[1], "--stream")) {
		guint64 seconds = 5;

		if (argc > 4) {
			g_printerr("usage: %s [data-type ...]\n"
			           "       %s --stream data-type [seconds]\n",
			           argv[0], argv[0]);
			return 2;
		}
		if (argc == 4) {
			char *end = NULL;

			seconds = g_ascii_strtoull(argv[3], &end, 10);
			if (!end || *end || seconds < 1 || seconds > G_MAXUINT) {
				g_printerr("invalid stream duration: %s\n", argv[3]);
				return 2;
			}
		}
		return stream_data_type(argv[2], (guint)seconds) ? 0 : 1;
	}

	if (argc > 1) {
		for (i = 1; i < argc; i++)
			failures += !inspect_data_type(argv[i]);
		return failures ? 1 : 0;
	}

	failures += !inspect_sensor("accelerometer", new_accelerometer);
	failures += !inspect_sensor("gyroscope", new_gyroscope);
	failures += !inspect_sensor("magnetometer", new_magnetometer);

	return failures ? 1 : 0;
}
