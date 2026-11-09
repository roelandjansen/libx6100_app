// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android app core: render thread with LVGL, input, audio and session
//

#include "platform/android/jni/app.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <android/log.h>
#include <android/looper.h>

#include "lvgl.h"

#include "platform/android/jni/audio_aaudio.h"
#include "ui/ui.h"

#define C_TAG		"libx6100_app"
#define C_FRAME_MS	33
#define C_HEARTBEAT_MS	1000
#define C_LOOPER_INPUT	1

//
// Fonts tried in order (system fonts of current Android versions)
//
static const char *const C_FONTS[] = {
	"/system/fonts/Roboto-Regular.ttf",
	"/system/fonts/RobotoStatic-Regular.ttf",
	"/system/fonts/DroidSans.ttf",
	NULL
};

static struct {

	//
	// Set before start
	//
	struct profile prof;
	bool mic_allowed;

	//
	// Main thread -> render thread handover (lock)
	//
	pthread_mutex_t lock;
	pthread_cond_t ack;
	ANativeWindow *new_win;
	bool win_changed;
	AInputQueue *new_queue;
	bool queue_changed;
	int32_t inset_top;
	int32_t inset_bottom;
	bool insets_changed;
	bool pause;
	bool quit;
	bool exited;

	pthread_t thread;
	bool running;
	_Atomic(ALooper *) looper;

	//
	// Render thread only
	//
	ANativeWindow *win;
	AInputQueue *queue;
	struct session *s;
	struct ui_ops ops;
	struct ui_font_data font;
	lv_display_t *disp;
	lv_indev_t *indev;
	uint16_t *shadow;
	void *draw_buf;
	int32_t w;
	int32_t h;
	int32_t ui_top;
	int32_t ui_bottom;
	bool ui_up;
	int32_t touch_x;
	int32_t touch_y;
	bool touch_down;
} G = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.ack = PTHREAD_COND_INITIALIZER,
};

static uint32_t tick_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void wake(void)
{
	ALooper *l = atomic_load(&G.looper);

	if (l != NULL)
		ALooper_wake(l);
}

void app_set_profile(const struct profile *p, bool mic_allowed)
{
	G.prof = *p;
	G.mic_allowed = mic_allowed;
}

void app_set_insets(int32_t top, int32_t bottom)
{
	pthread_mutex_lock(&G.lock);
	if (top != G.inset_top || bottom != G.inset_bottom) {
		G.inset_top = top;
		G.inset_bottom = bottom;
		G.insets_changed = true;
	}
	pthread_mutex_unlock(&G.lock);
	wake();
}

//
// Display: LVGL renders into partial buffers, the flush callback copies
// into a full-frame shadow, the last flush of a frame posts it
//

static void present(void)
{
	ANativeWindow_Buffer buf;
	int32_t y;

	if (G.win == NULL || G.shadow == NULL)
		return;
	if (ANativeWindow_lock(G.win, &buf, NULL) != 0)
		return;
	if (buf.width == G.w && buf.height == G.h && buf.format == WINDOW_FORMAT_RGB_565) {
		for (y = 0; y < G.h; y++)
			memcpy((uint16_t *)buf.bits + (size_t)y * (size_t)buf.stride,
					G.shadow + (size_t)y * (size_t)G.w, (size_t)G.w * 2u);
	}
	ANativeWindow_unlockAndPost(G.win);
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	const int32_t w = lv_area_get_width(a);
	int32_t y;

	for (y = a->y1; y <= a->y2; y++)
		memcpy(G.shadow + (size_t)y * (size_t)G.w + (size_t)a->x1,
				px + (size_t)(y - a->y1) * (size_t)w * 2u, (size_t)w * 2u);
	if (lv_display_flush_is_last(d))
		present();
	lv_display_flush_ready(d);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	data->point.x = G.touch_x;
	data->point.y = G.touch_y;
	data->state = G.touch_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void *read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	void *buf = NULL;
	long n;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
		buf = malloc((size_t)n);
		if (buf != NULL && fread(buf, 1, (size_t)n, f) != (size_t)n) {
			free(buf);
			buf = NULL;
		}
		*len = (size_t)n;
	}
	fclose(f);
	return buf;
}

static void ui_down(void)
{
	if (G.ui_up) {
		ui_release_ptt();
		ui_destroy();
		G.ui_up = false;
	}
	if (G.indev != NULL)
		lv_indev_delete(G.indev);
	G.indev = NULL;
	if (G.disp != NULL)
		lv_display_delete(G.disp);
	G.disp = NULL;
	free(G.shadow);
	free(G.draw_buf);
	G.shadow = NULL;
	G.draw_buf = NULL;
}

//
// Build display and UI for the window size (first window, size or inset change)
//

static void ui_up(int32_t top, int32_t bottom)
{
	const int32_t w = ANativeWindow_getWidth(G.win);
	const int32_t h = ANativeWindow_getHeight(G.win);
	const size_t part = (size_t)w * (size_t)(h / 8) * 2u;

	if (G.s == NULL)
		return;
	if (G.ui_up && w == G.w && h == G.h && top == G.ui_top && bottom == G.ui_bottom) {
		lv_obj_invalidate(lv_screen_active());
		return;
	}
	ui_down();

	G.w = w;
	G.h = h;
	G.ui_top = top;
	G.ui_bottom = bottom;
	G.shadow = calloc((size_t)w * (size_t)h, 2u);
	G.draw_buf = malloc(part);
	if (G.shadow == NULL || G.draw_buf == NULL)
		return;

	G.disp = lv_display_create(w, h);
	lv_display_set_color_format(G.disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(G.disp, G.draw_buf, NULL, (uint32_t)part, LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(G.disp, flush_cb);
	G.indev = lv_indev_create();
	lv_indev_set_type(G.indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(G.indev, touch_cb);
	lv_indev_set_display(G.indev, G.disp);

	if (ui_create(G.disp, &G.ops, &G.font, top, bottom) == 0)
		G.ui_up = true;
	else
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "ui_create failed");
}

//
// Input: pointer 0 drives the LVGL pointer; keys go back to the system
// (back finishes the activity, volume keys set the media volume)
//

static void handle_input(void)
{
	AInputEvent *ev = NULL;

	while (G.queue != NULL && AInputQueue_getEvent(G.queue, &ev) >= 0) {
		int handled = 0;

		if (AInputQueue_preDispatchEvent(G.queue, ev) != 0)
			continue;

		if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION) {
			const int32_t act = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;

			G.touch_x = (int32_t)AMotionEvent_getX(ev, 0);
			G.touch_y = (int32_t)AMotionEvent_getY(ev, 0);
			if (act == AMOTION_EVENT_ACTION_DOWN || act == AMOTION_EVENT_ACTION_MOVE)
				G.touch_down = true;
			else if (act == AMOTION_EVENT_ACTION_UP || act == AMOTION_EVENT_ACTION_CANCEL)
				G.touch_down = false;
			handled = 1;
		}
		AInputQueue_finishEvent(G.queue, ev, handled);
	}
}

static int on_looper_input(int fd, int events, void *data)
{
	(void)fd;
	(void)events;
	(void)data;
	handle_input();
	return 1;
}

static void on_notify(void *ctx)
{
	(void)ctx;
	wake();
}

//
// Pick up main-thread handovers
//

static bool take_changes(void)
{
	bool quit;
	bool win_changed;
	bool queue_changed;
	bool insets_changed;
	bool pause;
	ANativeWindow *win;
	AInputQueue *q;
	int32_t top;
	int32_t bottom;

	pthread_mutex_lock(&G.lock);
	quit = G.quit;
	win_changed = G.win_changed;
	queue_changed = G.queue_changed;
	insets_changed = G.insets_changed;
	pause = G.pause;
	win = G.new_win;
	q = G.new_queue;
	top = G.inset_top;
	bottom = G.inset_bottom;
	G.pause = false;
	G.insets_changed = false;
	pthread_mutex_unlock(&G.lock);

	if (pause && G.ui_up)
		ui_release_ptt();

	if (queue_changed) {
		if (G.queue != NULL)
			AInputQueue_detachLooper(G.queue);
		G.queue = q;
		G.touch_down = false;
		if (G.queue != NULL)
			AInputQueue_attachLooper(G.queue, atomic_load(&G.looper), C_LOOPER_INPUT, on_looper_input, NULL);
	}

	if (win_changed) {
		if (G.win != NULL) {
			// The window goes away: no heartbeat without a screen, release PTT now
			if (G.ui_up)
				ui_release_ptt();
			ANativeWindow_release(G.win);
		}
		G.win = win;
		if (G.win != NULL) {
			ANativeWindow_acquire(G.win);
			ANativeWindow_setBuffersGeometry(G.win, 0, 0, WINDOW_FORMAT_RGB_565);
			ui_up(top, bottom);
		}
	} else if (insets_changed && G.win != NULL) {
		ui_up(top, bottom);
	}

	if (queue_changed || win_changed) {
		pthread_mutex_lock(&G.lock);
		G.queue_changed = false;
		G.win_changed = false;
		pthread_cond_broadcast(&G.ack);
		pthread_mutex_unlock(&G.lock);
	}

	return quit;
}

static void start_session(void)
{
	const struct session_hooks hooks = { on_notify, NULL, NULL, NULL, NULL, NULL };

	G.prof.dev_rate = 0;
	G.prof.cap_rate = 0;
	if (audio_aa_open_playback() == 0)
		G.prof.dev_rate = AUDIO_AA_RATE;
	if (G.mic_allowed && audio_aa_open_capture() == 0)
		G.prof.cap_rate = AUDIO_AA_RATE;
	G.prof.ptt_heartbeat_ms = C_HEARTBEAT_MS;

	if (session_connect(&G.s, &G.prof, &hooks) != 0) {
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "session_connect failed");
		audio_aa_close();
		G.s = NULL;
		return;
	}
	audio_aa_attach(G.s);
	ui_ops_from_session(&G.ops, G.s);
}

static void *render_thread(void *arg)
{
	ALooper *looper = ALooper_prepare(0);
	size_t i;

	(void)arg;
	atomic_store(&G.looper, looper);

	for (i = 0; C_FONTS[i] != NULL && G.font.ttf == NULL; i++)
		G.font.ttf = read_file(C_FONTS[i], &G.font.len);
	if (G.font.ttf == NULL)
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "no system font found");

	lv_init();
	lv_tick_set_cb(tick_ms);
	start_session();

	for (;;) {
		uint32_t next = C_FRAME_MS;

		if (take_changes())
			break;

		audio_aa_service();
		if (G.s != NULL && !audio_aa_capture_ok())
			tx_capture_lost(G.s);

		if (G.s != NULL && G.ui_up && G.win != NULL) {
			ui_poll();
			next = lv_timer_handler();
			if (next > C_FRAME_MS)
				next = C_FRAME_MS;
		} else if (G.win == NULL) {
			// No screen: nothing to draw, look again for handovers now and then
			next = 250;
		}

		ALooper_pollOnce((int)next, NULL, NULL, NULL);
	}

	// Shutdown: PTT off, audio callbacks stopped, then the session
	if (G.ui_up)
		ui_release_ptt();
	audio_aa_close();
	if (G.s != NULL)
		session_disconnect(G.s);
	G.s = NULL;
	ui_down();
	if (G.queue != NULL)
		AInputQueue_detachLooper(G.queue);
	G.queue = NULL;
	if (G.win != NULL)
		ANativeWindow_release(G.win);
	G.win = NULL;
	lv_deinit();
	free((void *)(uintptr_t)G.font.ttf);
	G.font.ttf = NULL;
	atomic_store(&G.looper, NULL);

	pthread_mutex_lock(&G.lock);
	G.exited = true;
	pthread_cond_broadcast(&G.ack);
	pthread_mutex_unlock(&G.lock);
	return NULL;
}

int app_start(void)
{
	if (G.running)
		return 0;

	pthread_mutex_lock(&G.lock);
	G.quit = false;
	G.exited = false;
	G.new_win = NULL;
	G.new_queue = NULL;
	G.win_changed = false;
	G.queue_changed = false;
	pthread_mutex_unlock(&G.lock);

	if (pthread_create(&G.thread, NULL, render_thread, NULL) != 0)
		return -1;
	G.running = true;
	return 0;
}

//
// Hand an object over and wait until the render thread took it (and let go
// of the previous one)
//

static void handover_wait(bool *flag)
{
	struct timespec ts;

	while (*flag && G.running && !G.exited) {
		wake();
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 100000000L;
		if (ts.tv_nsec >= 1000000000L) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000L;
		}
		pthread_cond_timedwait(&G.ack, &G.lock, &ts);
	}
}

void app_set_window(ANativeWindow *win)
{
	pthread_mutex_lock(&G.lock);
	G.new_win = win;
	G.win_changed = true;
	handover_wait(&G.win_changed);
	pthread_mutex_unlock(&G.lock);
}

void app_set_input(AInputQueue *q)
{
	pthread_mutex_lock(&G.lock);
	G.new_queue = q;
	G.queue_changed = true;
	handover_wait(&G.queue_changed);
	pthread_mutex_unlock(&G.lock);
}

void app_pause(void)
{
	pthread_mutex_lock(&G.lock);
	G.pause = true;
	pthread_mutex_unlock(&G.lock);
	wake();
}

void app_stop(void)
{
	if (!G.running)
		return;

	pthread_mutex_lock(&G.lock);
	G.quit = true;
	pthread_mutex_unlock(&G.lock);
	wake();
	pthread_join(G.thread, NULL);
	G.running = false;
}
