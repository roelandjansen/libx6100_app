// SPDX-License-Identifier: GPL-3.0-or-later
//
// Foreground service while connected: keeps the process (network thread,
// AAudio streams) running with the screen off, plus a low-latency Wi-Fi
// lock. Types mediaPlayback, and microphone when the permission is there.
//

package nl.pa3met.libx6100app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.net.wifi.WifiManager;
import android.os.IBinder;

public class RadioService extends Service {

	static final String EXTRA_MIC = "mic";
	private static final String CHANNEL = "radio";
	private static final int ID = 1;

	private WifiManager.WifiLock wifiLock;

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		boolean mic = intent != null && intent.getBooleanExtra(EXTRA_MIC, false);
		NotificationManager nm = getSystemService(NotificationManager.class);

		nm.createNotificationChannel(new NotificationChannel(CHANNEL, "Connection",
				NotificationManager.IMPORTANCE_LOW));

		PendingIntent open = PendingIntent.getActivity(this, 0,
				new Intent(this, RadioActivity.class).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
				PendingIntent.FLAG_IMMUTABLE);
		Notification n = new Notification.Builder(this, CHANNEL)
			.setSmallIcon(android.R.drawable.stat_sys_headset)
			.setContentTitle("Connected to radio")
			.setContentIntent(open)
			.setOngoing(true)
			.build();

		int type = ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK;
		if (mic)
			type |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE;
		startForeground(ID, n, type);

		if (wifiLock == null) {
			WifiManager wm = getApplicationContext().getSystemService(WifiManager.class);

			wifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "libx6100_app");
			wifiLock.setReferenceCounted(false);
			wifiLock.acquire();
		}
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy() {
		if (wifiLock != null)
			wifiLock.release();
		wifiLock = null;
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}
}
