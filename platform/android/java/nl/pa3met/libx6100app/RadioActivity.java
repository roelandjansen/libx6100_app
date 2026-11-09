// SPDX-License-Identifier: GPL-3.0-or-later
//
// Operating screen: a NativeActivity running libradio.so. Before the
// native side starts it gets the profile, the rig files are copied out of
// the APK, and the foreground service is started so audio keeps running
// with the screen off. Leaving (back) disconnects.
//

package nl.pa3met.libx6100app;

import android.Manifest;
import android.app.NativeActivity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowManager;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

public class RadioActivity extends NativeActivity {

	private static final String TAG = "libx6100_app";

	static {
		System.loadLibrary("radio");
	}

	private static native void nativeSetProfile(String host, int port, String user, String pass, String name,
			int rxCodec, int rxRate, int latency, int txCodec, int txRate, String rigDir, boolean mic,
			int pttTimeout);

	private static native void nativeSetInsets(int top, int bottom);

	//
	// assets/rigs -> files/rigs, every start (small, keeps them in step with the APK)
	//
	private String copyRigs() {
		File dir = new File(getFilesDir(), "rigs");

		if (!dir.isDirectory() && !dir.mkdirs())
			Log.e(TAG, "cannot create " + dir);
		try {
			String[] names = getAssets().list("rigs");

			if (names != null) {
				byte[] buf = new byte[16384];

				for (String n : names) {
					try (InputStream in = getAssets().open("rigs/" + n);
							OutputStream out = new FileOutputStream(new File(dir, n))) {
						int len;

						while ((len = in.read(buf)) > 0)
							out.write(buf, 0, len);
					}
				}
			}
		} catch (IOException e) {
			Log.e(TAG, "copying rig files", e);
		}
		return dir.getAbsolutePath();
	}

	@Override
	protected void onCreate(Bundle saved) {
		SharedPreferences p = getSharedPreferences("profile", MODE_PRIVATE);
		boolean mic = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED;

		nativeSetProfile(p.getString("host", ""), p.getInt("port", 50001), p.getString("user", ""),
				p.getString("pass", ""), p.getString("name", "android"), p.getInt("rx_codec", 0x04),
				p.getInt("rx_rate", 48000), p.getInt("latency", 150), p.getInt("tx_codec", 0x04),
				p.getInt("tx_rate", 48000), copyRigs(), mic, p.getInt("ptt_timeout", 300));

		Intent svc = new Intent(this, RadioService.class);
		svc.putExtra(RadioService.EXTRA_MIC, mic);
		startForegroundService(svc);

		super.onCreate(saved);
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

		// System bars: pass their heights so the screen lays out between them
		View decor = getWindow().getDecorView();
		decor.setOnApplyWindowInsetsListener((v, ins) -> {
			if (Build.VERSION.SDK_INT >= 30) {
				Insets b = ins.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());

				nativeSetInsets(b.top, b.bottom);
			} else {
				nativeSetInsets(ins.getSystemWindowInsetTop(), ins.getSystemWindowInsetBottom());
			}
			return v.onApplyWindowInsets(ins);
		});
		decor.requestApplyInsets();
	}

	@Override
	protected void onDestroy() {
		super.onDestroy();
		stopService(new Intent(this, RadioService.class));
	}
}
