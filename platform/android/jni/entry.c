// SPDX-License-Identifier: GPL-3.0-or-later
//
// Android native library entry, build check only
//

#include <jni.h>

#include <android/log.h>

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
	(void)vm;
	(void)reserved;

	__android_log_print(ANDROID_LOG_INFO, "libx6100_app", "native library loaded");

	return JNI_VERSION_1_6;
}
