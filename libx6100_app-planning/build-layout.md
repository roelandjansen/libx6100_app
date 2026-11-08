# Build layout

Status: draft for review
Host: openSUSE Tumbleweed, no root needed for any build step.
Targets: `native` (SDL, Tumbleweed), `x6100` (ARMv7 hard-float, glibc), `android-arm64` (NDK, bionic).

---

## 1. Source tree

```
.
├── Makefile                 top level, selects target
├── mk/
│   ├── common.mk            flags, warnings, source lists
│   ├── target-native.mk
│   ├── target-x6100.mk
│   └── target-android-arm64.mk
├── core/                    rsba1 session, rig file parser, civ, audio codec, resampler, jitter buffer
├── model/                   radio state, notifications
├── ui/                      lvgl widgets, operating screen
├── platform/
│   ├── native/              sdl main, sdl audio
│   ├── x6100/               x6100 main, base control glue
│   └── android/
│       ├── jni/             native entry, ANativeWindow, AAudio, input
│       ├── java/            foreground service, permissions, profile editor
│       ├── res/
│       └── AndroidManifest.xml
├── rigs/                    .rig files: wfview v3-initial unchanged, plus X6100.rig from libx6100 contrib/wfview
├── third_party/
│   ├── lvgl/                vendored source, MIT notice retained
│   ├── opus/                vendored source, BSD notice retained
│   └── adpcm/               adpcm-xq (from wfview src/audio/adpcm), BSD notice retained
├── tests/                   host-run unit tests (rig file parser, civ decode, resampler, codecs)
├── tools/
│   ├── apk-build.sh
│   └── toolchain-check.sh
└── build/<target>/          all output, never committed
```

Rules:
- `core/`, `model/` and `ui/` contain no platform `#ifdef`s. Platform differences live only in `platform/`.
- `third_party/` is built from source per target by the same Makefile. No prebuilt libraries in the tree.
- All existing copyright notices stay in place, always.

---

## 2. Toolchains

| Target | CC | Sysroot / libs | Notes |
|---|---|---|---|
| native | system gcc or clang | system (SDL2 devel package) | daily dev loop |
| x6100 | `zig cc -target arm-linux-gnueabihf.<glibc>` | X6100 rootfs libs as sysroot | glibc version pinned to stock v1.2.0 |
| android-arm64 | NDK `aarch64-linux-android<api>-clang` | NDK sysroot | output is a shared library for the APK |

Locations, set once per user, no root:

```
~/opt/android-ndk-<ver>/
~/opt/android-sdk/          cmdline-tools, build-tools, platforms;android-<api>
~/opt/x6100-sysroot/        copied from the radio rootfs
```

Paths come from environment or a `config.mk` (not committed): `NDK`, `ANDROID_SDK`, `ANDROID_API`, `X6100_SYSROOT`, `X6100_GLIBC`.

`tools/toolchain-check.sh` verifies all of them and prints what is missing.

---

## 3. Makefile structure

Invocation:

```
make TARGET=native
make TARGET=x6100
make TARGET=android-arm64
make TARGET=native test
make TARGET=android-arm64 apk
make clean TARGET=...
```

Default `TARGET=native`.

`common.mk`:
- `CFLAGS` common: `-std=c11 -Wall -Wextra -Werror -Wshadow -Wconversion`; optimisation and debug flags from `DEBUG` (section 6)
- source lists per directory
- object dir `build/$(TARGET)/obj/` (or `build/$(TARGET)-debug/obj/`), dependency files via `-MMD -MP`

`target-*.mk` set only: `CC`, `AR`, target `CFLAGS` / `LDFLAGS`, platform sources, output name.

| Target | Output |
|---|---|
| native | `build/native/radio` (SDL executable) |
| x6100 | `build/x6100/radio` (executable, copy to the radio) |
| android-arm64 | `build/android-arm64/lib/arm64-v8a/libradio.so` |

Android link flags: `-shared`, `-Wl,-z,max-page-size=16384` (16 KB page alignment, required for current Android), link `-laaudio -landroid -llog`.

x6100: check after link that no glibc symbol above the pinned version is referenced (`objdump -T` scan in a make step); fail the build otherwise.

Tests: built for `native` only, run on the host. Core code is target-independent, so host tests cover the logic for all targets.

---

## 4. APK build without Gradle

`tools/apk-build.sh`, called by `make apk`. Steps:

1. `aapt2 compile` the resources
2. `aapt2 link` with `android.jar`, manifest, and the compiled resources into an unsigned base APK
3. `javac` the few Java classes against `android.jar`
4. `d8` to `classes.dex`
5. add `classes.dex` and `lib/arm64-v8a/libradio.so` to the APK, the `.so` stored uncompressed
6. `zipalign -P 16 -f 4` (16 KB alignment for the uncompressed `.so`)
7. `apksigner sign` with a local keystore (not committed)
8. `apksigner verify`

Order matters: zipalign before apksigner; signing invalidates on any later change.

Manifest: `android:extractNativeLibs="false"`, so the `.so` is loaded straight from the APK (hence uncompressed and aligned).

Install: `adb install -r build/android-arm64/radio.apk`.

Skeleton:

```bash
#!/usr/bin/env bash
set -euo pipefail

readonly C_API="${ANDROID_API:?ANDROID_API not set}"
readonly C_SDK="${ANDROID_SDK:?ANDROID_SDK not set}"
readonly C_BT="${C_SDK}/build-tools/${ANDROID_BUILD_TOOLS:?ANDROID_BUILD_TOOLS not set}"
readonly C_JAR="${C_SDK}/platforms/android-${C_API}/android.jar"
readonly C_SRC="platform/android"
readonly C_OUT="build/android-arm64"

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
# Compile and link resources into the unsigned base apk
#

build_resources() {
	local L_RES_ZIP="${G_TMP}/res.zip"

	"${C_BT}/aapt2" compile --dir "${C_SRC}/res" -o "${L_RES_ZIP}"
	"${C_BT}/aapt2" link -I "${C_JAR}" \
		--manifest "${C_SRC}/AndroidManifest.xml" \
		-o "${G_TMP}/base.apk" "${L_RES_ZIP}"
}

#
# Compile java sources and convert to dex
#

build_dex() {
	local L_CLS="${G_TMP}/classes"

	mkdir -p -- "${L_CLS}"
	find "${C_SRC}/java" -name '*.java' -print0 \
		| xargs -0 javac --release 11 -classpath "${C_JAR}" -d "${L_CLS}"
	find "${L_CLS}" -name '*.class' -print0 \
		| xargs -0 "${C_BT}/d8" --lib "${C_JAR}" --output "${G_TMP}"
}

#
# Add dex and native library, align, sign, verify
#

package() {
	local L_APK="${C_OUT}/radio.apk"
	local L_KEY="${RADIO_KEYSTORE:?RADIO_KEYSTORE not set}"

	(cd -- "${G_TMP}" && zip -q base.apk classes.dex)
	(cd -- "${C_OUT}" && zip -q -0 "${G_TMP}/base.apk" lib/arm64-v8a/libradio.so)
	"${C_BT}/zipalign" -P 16 -f 4 "${G_TMP}/base.apk" "${G_TMP}/aligned.apk"
	"${C_BT}/apksigner" sign --ks "${L_KEY}" --out "${L_APK}" "${G_TMP}/aligned.apk"
	"${C_BT}/apksigner" verify "${L_APK}"
	printf 'built %s\n' "${L_APK}"
}

main() {
	[[ -f "${C_JAR}" ]] || die "android.jar not found: ${C_JAR}"
	[[ -f "${C_OUT}/lib/arm64-v8a/libradio.so" ]] || die "libradio.so not built"

	G_TMP="$(mktemp -d)"
	trap cleanup EXIT

	build_resources
	build_dex
	package
}

main "$@"
```

Skeleton only: flags and paths to be checked against the installed build-tools version.

---

## 5. X6100 deploy

- `make TARGET=x6100` and copy `build/x6100/radio` to the radio (scp or SD card).
- No installer in v1. Deploy method is outside this spec.

---

## 6. Decisions

1. Build system: plain make with `mk/` fragments. No CMake.
2. Minimum Android API level: 29 (`ANDROID_API=29`, NDK compiler `aarch64-linux-android29-clang`, `minSdkVersion 29`).
3. Java language level: `javac --release 11`. The shell classes need nothing newer, and d8 handles Java 11 bytecode without desugaring issues.
4. Debug builds: one switch, `DEBUG=1`.
   - `DEBUG=0` (default): `-O2 -g`, `NDEBUG` defined, APK signed with the release keystore.
   - `DEBUG=1`: `-O0 -g3`, `-fsanitize=address,undefined` on `native` only, debug overlay enabled, APK `android:debuggable="true"`, signed with a debug keystore.
   - Output in `build/<target>-debug/` so debug and release objects never mix.
