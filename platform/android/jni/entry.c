// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android native library entry: JNI calls from RadioActivity (profile,
// window insets) and the NativeActivity callbacks, passed to app.c.
//

#include <jni.h>
#include <string.h>

#include <android/log.h>
#include <android/native_activity.h>

#include "platform/android/jni/app.h"

#define C_TAG	"libx6100_app"

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)vm;
	(void)reserved;
	return JNI_VERSION_1_6;
}

static void copy_jstring(JNIEnv *env, jstring js, char *out, size_t size)
{
	const char *c;

	out[0] = '\0';
	if (js == NULL)
		return;
	c = (*env)->GetStringUTFChars(env, js, NULL);
	if (c == NULL)
		return;
	strncpy(out, c, size - 1);
	out[size - 1] = '\0';
	(*env)->ReleaseStringUTFChars(env, js, c);
}

//
// RadioActivity.nativeSetProfile(), called before super.onCreate()
//

JNIEXPORT void JNICALL Java_nl_pa3met_libx6100app_RadioActivity_nativeSetProfile(JNIEnv *env, jclass cls,
		jstring host, jint port, jstring user, jstring pass, jstring name,
		jint rx_codec, jint rx_rate, jint latency, jint tx_codec, jint tx_rate,
		jstring rig_dir, jboolean mic, jint ptt_timeout_s)
{
	struct profile p;

	(void)cls;
	memset(&p, 0, sizeof(p));
	copy_jstring(env, host, p.host, sizeof(p.host));
	copy_jstring(env, user, p.username, sizeof(p.username));
	copy_jstring(env, pass, p.password, sizeof(p.password));
	copy_jstring(env, name, p.client_name, sizeof(p.client_name));
	copy_jstring(env, rig_dir, p.rig_dir, sizeof(p.rig_dir));
	p.port_control = (uint16_t)port;
	p.rx_codec = (uint8_t)rx_codec;
	p.rx_sample = (uint32_t)rx_rate;
	p.rx_latency = (uint16_t)latency;
	p.tx_codec = (uint8_t)tx_codec;
	p.tx_sample = (uint32_t)tx_rate;
	p.tx_latency = (uint16_t)latency;
	p.enable_ptt = true;
	p.ptt_timeout_enabled = ptt_timeout_s > 0;
	p.ptt_timeout_s = ptt_timeout_s > 0 ? ptt_timeout_s : 300;
	p.mic_gain = 100;
	app_set_profile(&p, mic != JNI_FALSE);
}

//
// RadioActivity.nativeSetInsets(): system bar heights in pixels
//

JNIEXPORT void JNICALL Java_nl_pa3met_libx6100app_RadioActivity_nativeSetInsets(JNIEnv *env, jclass cls,
		jint top, jint bottom)
{
	(void)env;
	(void)cls;
	app_set_insets(top, bottom);
}

//
// NativeActivity callbacks (main thread)
//

static void on_window_created(ANativeActivity *a, ANativeWindow *win)
{
	(void)a;
	app_set_window(win);
}

static void on_window_destroyed(ANativeActivity *a, ANativeWindow *win)
{
	(void)a;
	(void)win;
	app_set_window(NULL);
}

static void on_input_created(ANativeActivity *a, AInputQueue *q)
{
	(void)a;
	app_set_input(q);
}

static void on_input_destroyed(ANativeActivity *a, AInputQueue *q)
{
	(void)a;
	(void)q;
	app_set_input(NULL);
}

static void on_pause(ANativeActivity *a)
{
	(void)a;
	app_pause();
}

static void on_destroy(ANativeActivity *a)
{
	(void)a;
	app_stop();
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *a, void *saved, size_t saved_size)
{
	(void)saved;
	(void)saved_size;

	a->callbacks->onNativeWindowCreated = on_window_created;
	a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
	a->callbacks->onInputQueueCreated = on_input_created;
	a->callbacks->onInputQueueDestroyed = on_input_destroyed;
	a->callbacks->onPause = on_pause;
	a->callbacks->onDestroy = on_destroy;

	if (app_start() != 0)
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "render thread did not start");
}
