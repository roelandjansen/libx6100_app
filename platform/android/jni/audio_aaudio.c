// SPDX-License-Identifier: GPL-3.0-or-later
//
// AAudio playback (RX, float stereo) and capture (TX, float mono).
//
// The data callbacks only copy to / from the session rings. An error
// callback (device disconnected) only sets a flag; the render thread
// closes and reopens the stream in audio_aa_service(), as AAudio does not
// allow that from the callback.
//

#include "platform/android/jni/audio_aaudio.h"

#include <stdatomic.h>
#include <stddef.h>

#include <aaudio/AAudio.h>
#include <android/log.h>

#include "core/session.h"

#define C_TAG		"libx6100_app"

static AAudioStream *G_OUT;
static AAudioStream *G_IN;
static bool G_WANT_IN;
static atomic_bool G_OUT_ERR;
static atomic_bool G_IN_ERR;
static atomic_bool G_IN_LOST;
static _Atomic(struct session *) G_SESSION;

static aaudio_data_callback_result_t out_cb(AAudioStream *st, void *ud, void *data, int32_t frames)
{
	struct session *s = atomic_load_explicit(&G_SESSION, memory_order_acquire);

	(void)st;
	(void)ud;
	session_audio_read(s, data, (size_t)frames);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static aaudio_data_callback_result_t in_cb(AAudioStream *st, void *ud, void *data, int32_t frames)
{
	struct session *s = atomic_load_explicit(&G_SESSION, memory_order_acquire);

	(void)st;
	(void)ud;
	tx_capture_write(s, data, (size_t)frames, 1);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void err_cb(AAudioStream *st, void *ud, aaudio_result_t err)
{
	(void)st;
	(void)err;
	atomic_store((atomic_bool *)ud, true);
}

static AAudioStream *open_stream(bool input)
{
	AAudioStreamBuilder *b = NULL;
	AAudioStream *st = NULL;
	aaudio_result_t rc;

	if (AAudio_createStreamBuilder(&b) != AAUDIO_OK)
		return NULL;

	AAudioStreamBuilder_setDirection(b, input ? AAUDIO_DIRECTION_INPUT : AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
	AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setSampleRate(b, AUDIO_AA_RATE);
	AAudioStreamBuilder_setChannelCount(b, input ? 1 : 2);
	if (input) {
		// Voice recognition: no echo cancel / noise suppression on the mic path
		AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
		AAudioStreamBuilder_setDataCallback(b, in_cb, NULL);
		AAudioStreamBuilder_setErrorCallback(b, err_cb, &G_IN_ERR);
	} else {
		AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
		AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_SPEECH);
		AAudioStreamBuilder_setDataCallback(b, out_cb, NULL);
		AAudioStreamBuilder_setErrorCallback(b, err_cb, &G_OUT_ERR);
	}

	rc = AAudioStreamBuilder_openStream(b, &st);
	AAudioStreamBuilder_delete(b);
	if (rc != AAUDIO_OK) {
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "AAudio open %s: %s", input ? "input" : "output",
				AAudio_convertResultToText(rc));
		return NULL;
	}

	// Rate is fixed at 48 kHz: the session pipelines are set up for it
	if (AAudioStream_getSampleRate(st) != AUDIO_AA_RATE) {
		__android_log_print(ANDROID_LOG_ERROR, C_TAG, "AAudio %s rate %d, need %d", input ? "input" : "output",
				AAudioStream_getSampleRate(st), AUDIO_AA_RATE);
		AAudioStream_close(st);
		return NULL;
	}

	// Two bursts of buffering, as low as the device allows
	if (!input)
		AAudioStream_setBufferSizeInFrames(st, AAudioStream_getFramesPerBurst(st) * 2);
	return st;
}

int audio_aa_open_playback(void)
{
	atomic_store(&G_SESSION, NULL);
	atomic_store(&G_OUT_ERR, false);
	G_OUT = open_stream(false);
	return G_OUT != NULL ? 0 : -1;
}

int audio_aa_open_capture(void)
{
	atomic_store(&G_IN_ERR, false);
	atomic_store(&G_IN_LOST, false);
	G_IN = open_stream(true);
	G_WANT_IN = G_IN != NULL;
	return G_IN != NULL ? 0 : -1;
}

void audio_aa_attach(struct session *s)
{
	atomic_store_explicit(&G_SESSION, s, memory_order_release);
	if (G_OUT != NULL)
		AAudioStream_requestStart(G_OUT);
	if (G_IN != NULL)
		AAudioStream_requestStart(G_IN);
}

static void close_stream(AAudioStream **st)
{
	if (*st != NULL) {
		AAudioStream_requestStop(*st);
		AAudioStream_close(*st);
		*st = NULL;
	}
}

void audio_aa_close(void)
{
	close_stream(&G_OUT);
	close_stream(&G_IN);
	G_WANT_IN = false;
	atomic_store(&G_SESSION, NULL);
}

void audio_aa_service(void)
{
	if (atomic_exchange(&G_OUT_ERR, false)) {
		close_stream(&G_OUT);
		G_OUT = open_stream(false);
		if (G_OUT != NULL && atomic_load(&G_SESSION) != NULL)
			AAudioStream_requestStart(G_OUT);
	}

	if (atomic_exchange(&G_IN_ERR, false)) {
		close_stream(&G_IN);
		// The capture went away: PTT is released by the caller, then reopen
		atomic_store(&G_IN_LOST, true);
		if (G_WANT_IN) {
			G_IN = open_stream(true);
			if (G_IN != NULL && atomic_load(&G_SESSION) != NULL)
				AAudioStream_requestStart(G_IN);
		}
	}
}

//
// False once after the capture stream failed (reported once per failure)
//

bool audio_aa_capture_ok(void)
{
	return !atomic_exchange(&G_IN_LOST, false);
}
