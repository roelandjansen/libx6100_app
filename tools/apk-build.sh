#!/usr/bin/env bash
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the APK without Gradle: aapt2, javac, d8, zipalign, apksigner.
#
#	tools/apk-build.sh <build-dir>
#
# Environment (exported by the Makefile from config.mk):
#	ANDROID_SDK, ANDROID_PLATFORM, ANDROID_BUILD_TOOLS, RADIO_KEYSTORE
#	RADIO_KEYSTORE_PASS (optional, default "android")
#
# A missing keystore is created with keytool (local only, never committed).
#

set -euo pipefail

readonly C_PLATFORM="${ANDROID_PLATFORM:?ANDROID_PLATFORM not set}"
readonly C_SDK="${ANDROID_SDK:?ANDROID_SDK not set}"
readonly C_BT="${C_SDK}/build-tools/${ANDROID_BUILD_TOOLS:?ANDROID_BUILD_TOOLS not set}"
readonly C_JAR="${C_SDK}/platforms/android-${C_PLATFORM}/android.jar"
readonly C_KEY="${RADIO_KEYSTORE:?RADIO_KEYSTORE not set}"
readonly C_KEY_PASS="${RADIO_KEYSTORE_PASS:-android}"
readonly C_SRC="platform/android"
readonly C_MIN_API="29"

G_OUT=""
G_TMP=""

#
# Remove the temporary work directory
#

cleanup() {
	if [[ -n "${G_TMP}" && -d "${G_TMP}" ]]; then
		rm -rf -- "${G_TMP}"
	fi
}

#
# Fail with a message on stderr
#

die() {
	local L_MSG="${1}"

	printf 'error: %s\n' "${L_MSG}" >&2
	exit 1
}

#
# Create the signing keystore when it does not exist yet
#

ensure_keystore() {
	if [[ -f "${C_KEY}" ]]; then
		return 0
	fi
	printf 'creating keystore %s\n' "${C_KEY}"
	mkdir -p -- "$(dirname -- "${C_KEY}")"
	keytool -genkeypair -keystore "${C_KEY}" -storepass "${C_KEY_PASS}" -keypass "${C_KEY_PASS}" \
		-alias radio -keyalg RSA -keysize 3072 -validity 10000 -dname "CN=libx6100_app"
}

#
# Rig files into assets/rigs
#

stage_assets() {
	local L_DIR="${G_TMP}/assets/rigs"

	mkdir -p -- "${L_DIR}"
	cp -- rigs/*.rig "${L_DIR}/"
}

#
# Compile and link resources, manifest and assets into the unsigned base apk
#

build_resources() {
	local L_RES_ZIP="${G_TMP}/res.zip"

	"${C_BT}/aapt2" compile --dir "${C_SRC}/res" -o "${L_RES_ZIP}"
	"${C_BT}/aapt2" link -I "${C_JAR}" \
		--manifest "${C_SRC}/AndroidManifest.xml" \
		-A "${G_TMP}/assets" \
		-o "${G_TMP}/base.apk" "${L_RES_ZIP}"
}

#
# Compile java sources and convert to dex
#

build_dex() {
	local L_CLS="${G_TMP}/classes"
	local L_LIST="${G_TMP}/classes.list"

	mkdir -p -- "${L_CLS}"
	find "${C_SRC}/java" -name '*.java' -print0 \
		| xargs -0 javac --release 11 -classpath "${C_JAR}" -d "${L_CLS}"
	find "${L_CLS}" -name '*.class' > "${L_LIST}"
	"${C_BT}/d8" --release --min-api "${C_MIN_API}" --lib "${C_JAR}" --output "${G_TMP}" "@${L_LIST}"
}

#
# Add dex and native library (stored), align, sign, verify
#

package() {
	local L_APK="${G_OUT}/radio.apk"

	(cd -- "${G_TMP}" && zip -q base.apk classes.dex)
	(cd -- "${G_OUT}" && zip -q -0 "${G_TMP}/base.apk" lib/arm64-v8a/libradio.so)
	"${C_BT}/zipalign" -P 16 -f 4 "${G_TMP}/base.apk" "${G_TMP}/aligned.apk"
	"${C_BT}/apksigner" sign --ks "${C_KEY}" --ks-pass "pass:${C_KEY_PASS}" \
		--out "${L_APK}" "${G_TMP}/aligned.apk"
	"${C_BT}/apksigner" verify "${L_APK}"
	printf 'built %s\n' "${L_APK}"
}

main() {
	if [[ $# -ne 1 ]]; then
		die "usage: tools/apk-build.sh <build-dir>"
	fi
	G_OUT="${1}"

	[[ -f "${C_JAR}" ]] || die "android.jar not found: ${C_JAR}"
	[[ -x "${C_BT}/aapt2" ]] || die "build-tools not found: ${C_BT}"
	[[ -f "${G_OUT}/lib/arm64-v8a/libradio.so" ]] || die "libradio.so not built in ${G_OUT}"

	G_TMP="$(mktemp -d)"
	trap cleanup EXIT

	ensure_keystore
	stage_assets
	build_resources
	build_dex
	package
}

main "$@"
