#include <bluetooth/bluetooth.h>

#include <glib.h>

#include "myo-bluez.h"

#define ASSERT(GERR, MSG) \
	if(GERR != NULL) { \
		debug(MSG "; %s", GERR->message); \
		g_clear_error(&GERR); \
	}

#define DEVICE_IFACE "org.bluez.Device1"
#define GATT_SERVICE_IFACE "org.bluez.GattService1"
#define GATT_CHARACTERISTIC_IFACE "org.bluez.GattCharacteristic1"

static const char *BATT_UUID = "0000180f-0000-1000-8000-00805f9b34fb";
static const char *BATT_CHAR_UUIDS[] = {"00002a19-0000-1000-8000-00805f9b34fb"};

static const char *MYO_UUID = "d5060001-a904-deb9-4748-2c7f4a124842";
static const char *MYO_CHAR_UUIDS[] = {
		"d5060101-a904-deb9-4748-2c7f4a124842",
		"d5060201-a904-deb9-4748-2c7f4a124842",
		"d5060401-a904-deb9-4748-2c7f4a124842"
};

static const char *IMU_UUID = "d5060002-a904-deb9-4748-2c7f4a124842";
static const char *IMU_CHAR_UUIDS[] = {
		"d5060402-a904-deb9-4748-2c7f4a124842",
		"d5060502-a904-deb9-4748-2c7f4a124842"
};

static const char *ARM_UUID = "d5060003-a904-deb9-4748-2c7f4a124842";
static const char *ARM_CHAR_UUIDS[] = {"d5060103-a904-deb9-4748-2c7f4a124842"};

static const char *EMG_UUID = "d5060004-a904-deb9-4748-2c7f4a124842";
static const char *EMG_CHAR_UUIDS[] = {"d5060104-a904-deb9-4748-2c7f4a124842"};

static GError *error;

static gulong cb_id;

static GDBusObjectManagerClient *bluez_manager;

typedef struct {
	const char *UUID;
	GDBusProxy *proxy;
	const char **char_UUIDs;
	GDBusProxy **char_proxies;
	int num_chars;
} GattService;

#define NUM_SERVICES 5

typedef enum {
	UNKNOWN,
	DISCOVERED,
	INITIALIZED
} MyoStatus;

typedef struct {
	GDBusProxy *proxy;

	GattService services[NUM_SERVICES];

	myohw_fw_version_t version;
	myohw_fw_info_t info;

	gulong imu_sig_id;
	gulong arm_sig_id;
	gulong emg_sig_id;

	imu_cb_t on_imu;
	arm_cb_t on_arm;
	emg_cb_t on_emg;

	GSource *source;

	MyoStatus myo_status;
	ConnectionStatus conn_status;
} Myo;

//TODO: add unknown services
#define battery_service services[0]
#define myo_control_service services[1]
#define imu_service services[2]
#define arm_service services[3]
#define emg_service services[4]

#define firmware_info myo_control_service.char_proxies[0]
#define version_data myo_control_service.char_proxies[1]
#define cmd_input myo_control_service.char_proxies[2]
#define imu_data imu_service.char_proxies[0]
#define imu_events imu_service.char_proxies[1]
#define arm_data arm_service.char_proxies[0]
#define emg_data emg_service.char_proxies[0]

#define MAX_MYOS 4
static Myo myos[MAX_MYOS];
static int num_myos;

typedef struct {
	GSource parent;
	Myo *myo;
} MyoInitSource;

static gboolean myo_init_prepare(GSource *source, gint *timeout_);
static gboolean myo_init_dispatch(GSource *source, GSourceFunc callback, gpointer user_data);
static int (*myo_initialize)(myobluez_myo_t myo);

GSourceFuncs myo_init_funcs = {
	myo_init_prepare,
	NULL,
	myo_init_dispatch,
	NULL
};

static void set_myo(const gchar *path);
static GSource* myo_init_source_new(Myo *myo, GCancellable *cancellable);

static void init_GattService(GattService *service, const char *UUID, const char **char_UUIDs, int num_chars) {
	service->UUID = UUID;
	service->char_UUIDs = char_UUIDs;
	service->num_chars = num_chars;
	service->char_proxies = calloc(num_chars, sizeof(GDBusProxy*));
}

static gint is_device(gconstpointer a, gconstpointer b) {
	GDBusInterface *interface;

	interface = g_dbus_object_get_interface((GDBusObject*) a, DEVICE_IFACE);
	if(NULL == interface) {
		return 1;
	} else {
		g_object_unref(interface);
		return 0;
	}
}

static gint is_service(gconstpointer a, gconstpointer b) {
	GDBusInterface *interface;

	interface = g_dbus_object_get_interface((GDBusObject*) a,
			GATT_SERVICE_IFACE);
	if(NULL == interface) {
		return 1;
	} else {
		g_object_unref(interface);
		return 0;
	}
}

static gint is_characteristic(gconstpointer a, gconstpointer b) {
	GDBusInterface *interface;

	interface = g_dbus_object_get_interface((GDBusObject*) a,
			GATT_CHARACTERISTIC_IFACE);
	if(NULL == interface) {
		return 1;
	} else {
		g_object_unref(interface);
		return 0;
	}
}

static Myo* get_myo_from_proxy(GDBusProxy *proxy) {
	int i;

	for(i = 0; i < num_myos; i++) {
		if(myos[i].proxy == proxy) {
			debug("Myo Index: %d", i);
			return &myos[i];
		}
	}

	return NULL;
}

static void set_characteristic(GattService *serv, const gchar *char_path) {
	int i;
	GDBusProxy *proxy;
	GVariant *UUID;
	const char *UUID_str;

	debug("Setting Characteristic at %s", char_path);

	proxy = g_dbus_proxy_new_for_bus_sync(
			G_BUS_TYPE_SYSTEM,
			G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez", char_path,
			GATT_CHARACTERISTIC_IFACE, NULL, &error);
	ASSERT(error, "Get characteristic proxy failed");
	if(proxy == NULL) {
		fprintf(stderr, "Get characteristic proxy failed\n");
	}

	UUID = g_dbus_proxy_get_cached_property(proxy, "UUID");
	if(UUID == NULL) {
		//This shouldn't happen
		debug("Characteristic UUID not set");
		return;
	}
	UUID_str = g_variant_get_string(UUID, NULL);

	for(i = 0; i < serv->num_chars; i++) {
		if(strcmp(serv->char_UUIDs[i], UUID_str) == 0) {
			debug("Characteristic set");
			serv->char_proxies[i] = proxy;

			g_variant_unref(UUID);
			return;
		}
	}

	debug("Characteristic was not set!");
	g_variant_unref(UUID);
	g_object_unref(proxy);
	return;
}

static void set_service(Myo *myo, const gchar *serv_path) {
	int i;
	GDBusProxy *proxy, *chara;
	GVariant *UUID, *serv;
	const char *UUID_str;

	GList *objects, *object;
	const gchar *char_path, *char_serv_path;

	debug("Setting Service at %s", serv_path);

	proxy = g_dbus_proxy_new_for_bus_sync(
			G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez",
			serv_path, GATT_SERVICE_IFACE, NULL, &error);
	ASSERT(error, "Get service proxy failed");
	if(proxy == NULL) {
		return;
	}

	UUID = g_dbus_proxy_get_cached_property(proxy, "UUID");
	if(UUID == NULL) {
		//This shouldn't happen
		debug("Service UUID not set");
		return;
	}
	UUID_str = g_variant_get_string(UUID, NULL);

	for(i = 0; i < NUM_SERVICES; i++) {
		if(strcmp(UUID_str, myo->services[i].UUID) == 0) {
			debug("Service set");
			myo->services[i].proxy = proxy;

			g_variant_unref(UUID);

			//search for chars
			objects = g_dbus_object_manager_get_objects(
					(GDBusObjectManager*) bluez_manager);
			if(NULL == objects) {
				debug("Manager did not give us objects!");
				return;
			}

			debug("Searching objects for characteristics, expecting %d", myo->services[i].num_chars);
			do {
				object = NULL;
				object = g_list_find_custom(objects, NULL, is_characteristic);
				if(object != NULL) {
					char_path = g_dbus_object_get_object_path((GDBusObject*) object->data);
					chara = g_dbus_proxy_new_for_bus_sync(
							G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez",
							char_path, GATT_CHARACTERISTIC_IFACE, NULL, &error);
					ASSERT(error, "Get characteristic proxy failed");
					if(chara == NULL) {
						objects = g_list_delete_link(objects, object);
						continue;
					}

					serv = g_dbus_proxy_get_cached_property(chara, "Service");
					if(serv != NULL) {
						char_serv_path = g_variant_get_string(serv, NULL);
					} else {
						debug("Characteristic Service not set");
						g_variant_unref(serv);
						g_list_free_full(objects, g_object_unref);
						return;
					}

					if(strcmp(serv_path, char_serv_path) == 0) {
						set_characteristic(&myo->services[i], char_path);
					}
					g_variant_unref(serv);
					objects = g_list_delete_link(objects, object);
				}
			} while(object != NULL);

			g_list_free_full(objects, g_object_unref);

			return;
		}
	}

	debug("Service was not set!");
	g_variant_unref(UUID);
	g_object_unref(proxy);
	return;
}

static void set_services(Myo *myo) {
	GList *objects, *object;
	GVariant *device;
	GDBusProxy *serv;
	const gchar *myo_path, *serv_path, *serv_dev_path;

	myo_path = g_dbus_proxy_get_object_path(myo->proxy);

	//search for services that are already registered
	objects = g_dbus_object_manager_get_objects(
			(GDBusObjectManager*) bluez_manager);
	if(NULL == objects) {
		//failed
		fprintf(stderr, "Manager did not give us objects!\n");
		return;
	}

	debug("Searching objects for services");
	do {
		object = NULL;
		object = g_list_find_custom(objects, NULL, is_service);
		if(object != NULL) {
			serv_path = g_dbus_object_get_object_path((GDBusObject*) object->data);
			serv = g_dbus_proxy_new_for_bus_sync(
					G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.bluez",
					serv_path, GATT_SERVICE_IFACE, NULL, &error);
			ASSERT(error, "Get service proxy failed");
			if(serv == NULL) {
				objects = g_list_delete_link(objects, object);
				continue;
			}

			device = g_dbus_proxy_get_cached_property(serv, "Device");
			if(device != NULL) {
				serv_dev_path = g_variant_get_string(device, NULL);
			} else {
				debug("Service Device not set");
				g_variant_unref(device);
				g_list_free_full(objects, g_object_unref);
				return;
			}

			if(strcmp(myo_path, serv_dev_path) == 0) {
				set_service(myo, serv_path);
			}
			g_variant_unref(device);
			objects = g_list_delete_link(objects, object);
		}
	} while(object != NULL);

	g_list_free_full(objects, g_object_unref);

	myo->myo_status = DISCOVERED;
}

static void device_connect_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
	Myo *myo;
	GError *err = NULL;
	GVariant *reply;

	myo = get_myo_from_proxy((GDBusProxy*) source);

	if(myo == NULL || !G_IS_DBUS_PROXY(myo->proxy)) {
		//couldn't find myo in registered myos
		//this REALLY shouldn't ever happen
		return;
	}

	reply = g_dbus_proxy_call_finish(myo->proxy, res, &err);
	if(err != NULL) {
		debug("Connection failed ; %s", err->message);
		if(strstr(err->message, "Timeout") != NULL) {
			reply = g_dbus_proxy_call_sync(
					myo->proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE,
					-1, NULL, &error);
			ASSERT(error, "Disconnect failed");
			g_variant_unref(reply);
			printf("Retrying...\n");
			g_dbus_proxy_call(
					myo->proxy, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE,
					-1, NULL, (GAsyncReadyCallback) device_connect_cb, NULL);

			myo->conn_status = CONNECTING;
		}
		g_clear_error(&err);
	} else {
		g_variant_unref(reply);

		myo->conn_status = CONNECTED;
		printf("Connected!\n");
	}
}

static void myo_signal_cb(GDBusProxy *proxy, GVariant *changed, GStrv invalid, gpointer user_data) {
	GVariantIter *iter;
	const gchar *key;
	GVariant *value;

	Myo *myo = NULL;

	if(g_variant_n_children(changed) > 0) {
		g_variant_get(changed, "a{sv}", &iter);
		while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
			if(strcmp(key, "Connected") == 0) {
				//check for disconnect
				if(!g_variant_get_boolean(value)) {
					myo = get_myo_from_proxy(proxy);

					if(myo == NULL || !G_IS_DBUS_PROXY(myo->proxy)) {
						//couldn't find myo in registered myos
						//this REALLY shouldn't ever happen
						return;
					}

					//disconnected
					printf("Myo disconnected\n");

					printf("Reconnecting...\n");
					g_dbus_proxy_call(
							proxy, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE,
							-1, NULL, (GAsyncReadyCallback) device_connect_cb,
							NULL);

					myo->conn_status = CONNECTING;
				}
			}  else if(strcmp(key, "ServicesResolved") == 0) {
				if(g_variant_get_boolean(value)) {
					myo = get_myo_from_proxy(proxy);
					if(myo == NULL) {
						//couldn't find myo in registered myos
						//this REALLY shouldn't ever happen
						return;
					}
					if(myo->myo_status == UNKNOWN) {
						debug("ServicesResolved");
						set_services(myo);
					}
				}
			}
		}
		g_variant_iter_free(iter);
	}
}

static void device_UUID_cb(GDBusProxy *proxy, GVariant *changed, GStrv invalid, gpointer user_data) {
	GVariantIter *iter;
	const gchar *key;
	GVariant *value;

	//check if UUIDs was set
	if(g_variant_n_children(changed) > 0) {
		g_variant_get(changed, "a{sv}", &iter);
		while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
			if(strcmp(key, "UUIDs") == 0) {
				//if UUIDs set, kill notifier and call set_myo
				//TODO: might cause problems if not all UUIDs are set at once
				g_signal_handler_disconnect(proxy, *((gulong*) user_data));
				free(user_data);
				set_myo(g_dbus_proxy_get_object_path(proxy));
				break;
			}
		}
		g_variant_iter_free(iter);
	}
}

static void set_myo(const gchar *path) {
	gulong *sig_id;

	GDBusProxy *proxy;
	GVariant *UUIDs, *serv_res;
	GVariantIter *iter;
	gchar *uuid;

	Myo *myo;

	GSource *source;

	if(num_myos == MAX_MYOS) {
		fprintf(stderr, "Maximum myos already registered\n");
		return;
	}

	proxy = (GDBusProxy*) g_dbus_object_manager_get_interface(
			(GDBusObjectManager*) bluez_manager, path, DEVICE_IFACE);
	if(!G_IS_DBUS_PROXY(proxy)) {
		debug("Get device proxy failed");
		return;
	}
	//get UUIDs
	//if cannot get UUIDs set notify
	UUIDs = g_dbus_proxy_get_cached_property(proxy, "UUIDs");
	if(UUIDs == NULL || (g_variant_n_children(UUIDs) == 0)) {
		debug("Device UUIDs not set");
		//set notifier
		if(UUIDs != NULL) {
			g_variant_unref(UUIDs);
		}
		sig_id = (gulong*) malloc(sizeof(gulong));
		*sig_id = g_signal_connect(proxy, "g-properties-changed",
									G_CALLBACK(device_UUID_cb), sig_id);
		g_object_unref(proxy);
		return;
	}

	myo = NULL;
	g_variant_get(UUIDs, "as", &iter);
	while(g_variant_iter_loop(iter, "&s", &uuid)) {
		if(strcmp(uuid, MYO_UUID) == 0) {
			debug("Myo Index:%d", num_myos);
			myo = &myos[num_myos++];
			myo->proxy = proxy;
			break;
		}
	}
	g_variant_iter_free(iter);
	g_variant_unref(UUIDs);

	if(myo == NULL) {
		g_object_unref(proxy);
		return;
	}

	printf("Myo found!\n");

	g_signal_connect(myo->proxy, "g-properties-changed", G_CALLBACK(myo_signal_cb), NULL);

	printf("Connecting...\n");
	g_dbus_proxy_call(
			myo->proxy, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE,
			-1, NULL, (GAsyncReadyCallback) device_connect_cb,
			NULL);

	myo->conn_status = CONNECTING;
	myo->myo_status = UNKNOWN;
	myo->version.hardware_rev = 0xFFFF;
	myo->info.reserved[0] = 0xFF;
	myo->on_imu = NULL;
	myo->on_arm = NULL;
	myo->on_emg = NULL;

	//check ServicesResolved
	serv_res = g_dbus_proxy_get_cached_property(myo->proxy, "ServicesResolved");
	if(serv_res != NULL && g_variant_get_boolean(serv_res) && myo->services[0].proxy == NULL) {
		g_variant_unref(serv_res);
		debug("ServicesResolved");
		set_services(myo);
	}

	//TODO: use a cancellable
	source = myo_init_source_new(myo, NULL);
	if(myo->source == NULL && source != NULL) {
		debug("Attaching source");
		myo->source = source;
		if(g_main_context_get_thread_default() == NULL) {
			debug("Using default context");
		}
		g_source_attach(source, g_main_context_get_thread_default());
	}
}

static void object_added_cb(GDBusObjectManager *manager, GDBusObject *object, gpointer user_data) {
	if(is_device(object, NULL) == 0) {
		debug("object_added_devce");
		set_myo(g_dbus_object_get_object_path(object));
	} 
}

static void myo_imu_cb(GDBusProxy *proxy, GVariant *changed, GStrv invalid, gpointer user_data) {
	GVariantIter *iter;
	const gchar *key, *vals;
	GVariant *value;
	gsize elements;

	Myo* myo;
	myohw_imu_data_t data;

	if(g_variant_n_children(changed) > 0) {
		g_variant_get(changed, "a{sv}", &iter);
		while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
			if(strcmp(key, "Value") == 0) {
				vals = g_variant_get_fixed_array(value, &elements, sizeof(gchar));
				memcpy(&data, vals, sizeof(myohw_imu_data_t));

				myo = (Myo*) user_data;
				if(myo != NULL && myo->on_imu != NULL) {
					myo->on_imu(data);
				}
			}
		}
		g_variant_iter_free (iter);
	}
}

static void myo_arm_cb(GDBusProxy *proxy, GVariant *changed, GStrv invalid, gpointer user_data) {
	GVariantIter *iter;
	myohw_classifier_event_t event;
	const gchar *key, *vals;
	GVariant *value;
	gsize elements;
	Myo* myo;

	if(g_variant_n_children(changed) > 0) {
		g_variant_get(changed, "a{sv}", &iter);
		while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
			if(strcmp(key, "Value") == 0) {
				vals = g_variant_get_fixed_array(value, &elements, sizeof(gchar));
				memcpy(&event, vals, sizeof(event));

				myo = (Myo*) user_data;
				if(myo != NULL && myo->on_arm != NULL) {
					myo->on_arm(event);
				}
			}
		}
		g_variant_iter_free (iter);
	}
}

static void myo_emg_cb(GDBusProxy *proxy, GVariant *changed, GStrv invalid, gpointer user_data) {
	GVariantIter *iter;
	const gchar *key, *vals;
	GVariant *value;

	Myo* myo;
	short emg[8];
	unsigned char moving;

	if(g_variant_n_children(changed) > 0) {
		g_variant_get(changed, "a{sv}", &iter);
		while(g_variant_iter_loop(iter, "{&sv}", &key, &value)) {
			if(strcmp(key, "Value") == 0) {
				vals = g_variant_get_fixed_array(value, NULL, sizeof(gchar));
				//not entirely sure what the last byte is, but it's a bitmask
				//that seems to indicate which sensors think they're being moved
				//around or something
				memcpy(emg, vals, 16);
				moving = vals[16];

				myo = (Myo*) user_data;
				if(myo != NULL && myo->on_emg != NULL) {
					myo->on_emg(emg, moving);
				}
			}
		}
		g_variant_iter_free (iter);
	}
}

void myo_imu_cb_register(myobluez_myo_t bmyo, imu_cb_t callback) {
	Myo* myo = (Myo*) bmyo;
	myo->on_imu = callback;
}

void myo_arm_cb_register(myobluez_myo_t bmyo, arm_cb_t callback) {
	Myo* myo = (Myo*) bmyo;
	myo->on_arm = callback;
}

void myo_emg_cb_register(myobluez_myo_t bmyo, emg_cb_t callback) {
	Myo* myo = (Myo*) bmyo;
	myo->on_emg = callback;
}

static GVariant* myo_read_value(GDBusProxy *proxy) {
	GVariantBuilder build_opt;
	GVariant *var;

	if(proxy == NULL) {
		debug("Proxy was NULL\n");
		return NULL;
	}

	g_variant_builder_init(&build_opt, G_VARIANT_TYPE("a{sv}"));

	var = g_dbus_proxy_call_sync(proxy, "ReadValue",
			g_variant_new("(a{sv})", &build_opt),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	ASSERT(error, "Failed to get value");
	return var;
}

char* pose2str(myohw_pose_t pose) {
	switch(pose) {
		case myohw_pose_rest:
			return "Rest";
		case myohw_pose_fist:
			return "Fist";
		case myohw_pose_wave_in:
			return "Wave in";
		case myohw_pose_wave_out:
			return "Wave out";
		case myohw_pose_fingers_spread:
			return "Spread";
		case myohw_pose_double_tap:
			return "Double Tap";
		default:
			return "Unknown";
	}
}

int myo_get_name(myobluez_myo_t bmyo, char *str) {
	GVariant *name;
	gsize length;
	Myo *myo = (Myo*) bmyo;

	if(myo->proxy == NULL) {
		debug("Myo proxy not set\n");
		return -1;
	}

	name = g_dbus_proxy_get_cached_property(myo->proxy, "Alias");
	if(name == NULL) {
		return -1;
	}
	strncpy(str, g_variant_get_string(name, &length), 25);
	str[24] = '\0';
	g_variant_unref(name);
	return (int) length;
}

int myo_get_version(myobluez_myo_t bmyo, myohw_fw_version_t *ver) {
	GVariant *ver_var;
	GVariantIter *iter;
	unsigned char *ver_char;

	Myo *myo = (Myo*) bmyo;

	if(myo->version_data == NULL) {
		debug("Version data proxy not set\n");
		return MYOBLUEZ_ERROR;
	}

	if(myo->version.hardware_rev == 0xFFFF) {
		ver_var = myo_read_value(myo->version_data);
		if(ver_var != NULL) {
			g_variant_get(ver_var, "(ay)", &iter);

			ver_char = (unsigned char*) &myo->version;

			//TODO:check for overruns
			while(g_variant_iter_loop(iter, "y", ver_char))  ver_char++;
			g_variant_iter_free(iter);
		} else {
			debug("Failled to read version");
			return MYOBLUEZ_ERROR;
		}
		g_variant_unref(ver_var);
	}

	memcpy(ver, &myo->version, sizeof(myohw_fw_version_t));
	return MYOBLUEZ_OK;
}

void myo_EMG_notify_enable(myobluez_myo_t bmyo, bool enable) {
	Myo *myo = (Myo*) bmyo;

	if(myo->emg_data == NULL) {
		return;
	}
	//call start notify or stop notify
	if(enable) {
		g_dbus_proxy_call_sync(myo->emg_data, "StartNotify", NULL,
								G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "EMG notify enable failed");
		if(myo->emg_sig_id == 0) {
			myo->emg_sig_id = g_signal_connect(myo->emg_data,
					"g-properties-changed", G_CALLBACK(myo_emg_cb), myo);
		}
	} else {
		g_dbus_proxy_call_sync(myo->emg_data, "StopNotify", NULL,
				G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "EMG notify disable failed");
		if(myo->emg_sig_id != 0) {
			g_signal_handler_disconnect(myo->emg_data, myo->emg_sig_id);
		}
	}
}

void myo_IMU_notify_enable(myobluez_myo_t bmyo, bool enable) {
	Myo *myo = (Myo*) bmyo;

	if(myo->imu_data == NULL) {
		return;
	}
	//call start notify or stop notify
	if(enable) {
		g_dbus_proxy_call_sync(myo->imu_data, "StartNotify", NULL,
								G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "IMU notify enable failed");
		if(myo->imu_sig_id == 0) {
			myo->imu_sig_id = g_signal_connect(myo->imu_data, "g-properties-changed",
											G_CALLBACK(myo_imu_cb), myo);
		}
	} else {
		g_dbus_proxy_call_sync(myo->imu_data, "StopNotify", NULL,
								G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "IMU notify disable failed");
		if(myo->imu_sig_id != 0) {
			g_signal_handler_disconnect(myo->imu_data, myo->imu_sig_id);
		}
	}
}

void myo_arm_indicate_enable(myobluez_myo_t bmyo, bool enable) {
	Myo *myo = (Myo*) bmyo;

	if(myo->arm_data == NULL) {
		return;
	}
	//call start notify or stop notify
	if(enable) {
		g_dbus_proxy_call_sync(myo->arm_data, "StartNotify", NULL,
								G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "Arm notify enable failed");
		if(myo->arm_sig_id == 0) {
			myo->arm_sig_id = g_signal_connect(myo->arm_data, "g-properties-changed",
											G_CALLBACK(myo_arm_cb), myo);
		}
	} else {
		g_dbus_proxy_call_sync(myo->arm_data, "StopNotify", NULL,
				G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
		ASSERT(error, "Arm notify disable failed");
		if(myo->arm_sig_id != 0) {
			g_signal_handler_disconnect(myo->arm_data, myo->arm_sig_id);
		}
	}
}

void myo_update_enable(
		myobluez_myo_t bmyo,
		myohw_emg_mode_t emg,
		myohw_imu_mode_t imu,
		myohw_classifier_mode_t arm)
{
	GVariantBuilder build_opt;
	GVariant *reply;
	Myo *myo = (Myo*) bmyo;
	myohw_command_set_mode_t cmd;

	cmd.header.command = myohw_command_set_mode;
	cmd.header.payload_size = 3;
	cmd.emg_mode = emg;
	cmd.imu_mode = imu;
	cmd.classifier_mode = arm;

	g_variant_builder_init(&build_opt, G_VARIANT_TYPE("a{sv}"));

	reply = g_dbus_proxy_call_sync(myo->cmd_input, "WriteValue",
			g_variant_new("(@aya{sv})",
				g_variant_new_fixed_array(
					G_VARIANT_TYPE_BYTE, (uint8_t*) &cmd, 5, sizeof(uint8_t)),
				&build_opt),
			G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
	ASSERT(error, "Update enable failed");
	if(reply != NULL) {
		g_variant_unref(reply);
	}
}

int myo_get_info(myobluez_myo_t bmyo, myohw_fw_info_t *info) {
	GVariant *info_var;
	GVariantIter *iter;
	unsigned char *info_char = (unsigned char*) info;

	Myo *myo = (Myo*) bmyo;

	if(myo->firmware_info == NULL) {
		debug("Firmware info proxy not set\n");
		return MYOBLUEZ_ERROR;
	}

	if(myo->info.reserved[0] == 0xFF) {
		info_var = myo_read_value(myo->firmware_info);
		if(info_var != NULL) {
			info_char = (unsigned char*) &myo->info;

			g_variant_get(info_var, "(ay)", &iter);

			//TODO: check for overruns
			while(g_variant_iter_loop(iter, "y", info_char))  info_char++;
			g_variant_iter_free(iter);
		} else {
			debug("Failled to read firmware info");
			return MYOBLUEZ_ERROR;
		}
		g_variant_unref(info_var);
	}

	memcpy(info, &myo->info, sizeof(myohw_fw_info_t));
	return MYOBLUEZ_OK;
}

static gboolean myo_init_prepare(GSource *source, gint *timeout_) {
	int i, j;
	MyoInitSource *myo_source = (MyoInitSource*) source;

	*timeout_ = -1;

	if(myo_source->myo->myo_status == INITIALIZED) return false;

	for(i = 0; i < NUM_SERVICES; i++) {
		if(G_IS_DBUS_PROXY(myo_source->myo->services[i].proxy)) {
			for(j = 0; j < myo_source->myo->services[i].num_chars; j++) {
				if(!G_IS_DBUS_PROXY(myo_source->myo->services[i].char_proxies[j])) {
					debug("Service %d char %d proxy not set", i, j);
					return false;
				}
			}
		} else {
			debug("Service %d proxy not set", i);
			return false;
		}
	}

	myo_source->myo->myo_status = DISCOVERED;
	return true;
}

static gboolean myo_init_dispatch(GSource *source, GSourceFunc callback, gpointer user_data) {
	MyoInitSource *myo_source = (MyoInitSource*) source;

	if(myo_initialize((myobluez_myo_t) myo_source->myo) == MYOBLUEZ_OK) {
		myo_source->myo->myo_status = INITIALIZED;
	}

	return G_SOURCE_CONTINUE;
}

static GSource* myo_init_source_new(Myo *myo, GCancellable *cancellable) {
	GSource *source, *cancellable_source;
	MyoInitSource *myo_source;

	if(myo == NULL) {
		debug("Myo invalid");
		return NULL;
	}
	if(cancellable != NULL && !G_IS_CANCELLABLE(cancellable)) {
		debug("Cancellable invalid");
		return NULL;
	}

	source = g_source_new(&myo_init_funcs, sizeof(MyoInitSource));
	myo_source = (MyoInitSource*) source;

	g_source_set_name(source, "Myo Initialization Source");

	myo_source->myo = myo;

	if(cancellable != NULL) {
		cancellable_source = g_cancellable_source_new(cancellable);
		g_source_set_dummy_callback(cancellable_source);
		g_source_add_child_source(source, cancellable_source);
		g_source_unref(cancellable_source);
	}

	return source;
}

static int scan_myos() {
	GList *objects;
	GList *object;

	objects = g_dbus_object_manager_get_objects(
			(GDBusObjectManager*) bluez_manager);
	if(NULL == objects) {
		//failed
		fprintf(stderr, "Manager did not give us objects!\n");
		return 1;
	}

	debug("Searching objects for myo");
	do {
		object = NULL;
		object = g_list_find_custom(objects, NULL, is_device);
		if(object != NULL) {
			set_myo(g_dbus_object_get_object_path((GDBusObject*) object->data));
			objects = g_list_delete_link(objects, object);
		}
	} while(object != NULL && num_myos != MAX_MYOS);
	debug("Finished searching objects for myo");

	g_list_free_full(objects, g_object_unref);

	return 0;
}

void myobluez_deinit() {
	int i, j, k;
	GVariant *reply;

	//unref stuff
	for(i = 0; i < MAX_MYOS; i++) {
		if(myos[i].myo_status == INITIALIZED) {
			myo_IMU_notify_enable((myobluez_myo_t) &myos[i], false);
			myo_arm_indicate_enable((myobluez_myo_t) &myos[i], false);
			myo_update_enable((myobluez_myo_t) &myos[i],
				myohw_emg_mode_none,
				myohw_imu_mode_none,
				myohw_classifier_mode_disabled);
		}

		for(k = 0; k < NUM_SERVICES; k++) {
			for(j = 0; j < myos[i].services[k].num_chars; j++) {
				if(G_IS_DBUS_PROXY(myos[i].services[k].char_proxies[j])) {
					g_object_unref(myos[i].services[k].char_proxies[j]);
					myos[i].services[k].char_proxies[j] = NULL;
				}
			}
			if(G_IS_DBUS_PROXY(myos[i].services[k].proxy)) {
				debug("Freeing service proxy");
				g_object_unref(myos[i].services[k].proxy);
				myos[i].services[k].proxy = NULL;
			}
		}

		if(G_IS_DBUS_PROXY(myos[i].proxy)) {
			if(myos[i].conn_status == CONNECTED || myos[i].conn_status == CONNECTING) {
				//disconnect
				debug("Disconnecting from myo");
				reply = g_dbus_proxy_call_sync(
						myos[i].proxy, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
						&error);
				ASSERT(error, "Disconnect failed");
				g_variant_unref(reply);
			}
			//TODO: maybe remove device to work around bluez bug
			debug("Freeing myo proxy");
			g_object_unref(myos[i].proxy);
			myos[i].proxy = NULL;
		}

		if(myos[i].source != NULL && !g_source_is_destroyed(myos[i].source)) {
			g_source_destroy(myos[i].source);
		}
	}

	if(cb_id != 0 && G_IS_OBJECT(bluez_manager)) {
		debug("Disconnecting signal handler");
		g_signal_handler_disconnect(bluez_manager, cb_id);
		cb_id = 0;
	}
	
	if(G_IS_OBJECT(bluez_manager)) {
		debug("Freeing bluez manager");
		g_object_unref(bluez_manager);
		bluez_manager = NULL;
	}
}

int myobluez_init(int (*myo_init)(myobluez_myo_t)) {
	int i;

	for(i = 0; i < MAX_MYOS; i++) {
		init_GattService(&myos[i].battery_service, BATT_UUID, BATT_CHAR_UUIDS, 1);
		init_GattService(&myos[i].myo_control_service, MYO_UUID, MYO_CHAR_UUIDS, 3);
		init_GattService(&myos[i].imu_service, IMU_UUID, IMU_CHAR_UUIDS, 2);
		init_GattService(&myos[i].arm_service, ARM_UUID, ARM_CHAR_UUIDS, 1);
		init_GattService(&myos[i].emg_service, EMG_UUID, EMG_CHAR_UUIDS, 1);
	}
	num_myos = 0;

	bluez_manager =
			(GDBusObjectManagerClient*) g_dbus_object_manager_client_new_for_bus_sync(
					G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
					"org.bluez", "/", NULL, NULL, NULL, NULL, &error);
	ASSERT(error, "Get ObjectManager failed");
	if(bluez_manager == NULL) {
		fprintf(stderr, "Error: Is Bluez running?\n");
		return 1;
	}

	myo_initialize = myo_init;

	cb_id = g_signal_connect(bluez_manager, "object-added",
			G_CALLBACK(object_added_cb), NULL);

	scan_myos();

	return 0;
}
