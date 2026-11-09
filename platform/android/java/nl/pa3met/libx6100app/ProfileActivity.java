// SPDX-License-Identifier: GPL-3.0-or-later
//
// Connection profile editor and launcher. Built in code (no layout
// resources); values kept in SharedPreferences.
//

package nl.pa3met.libx6100app;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.text.InputType;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;

import java.util.ArrayList;

public class ProfileActivity extends Activity {

	//
	// Codec menu as wfview: label, RS-BA1 codec byte
	//
	private static final String[] RX_CODEC_NAMES = {
		"Opus 2ch", "Opus 1ch", "PCM16 2ch", "PCM16 1ch", "ADPCM 1ch", "uLaw 2ch", "uLaw 1ch", "PCM8 2ch", "PCM8 1ch"
	};
	private static final int[] RX_CODECS = { 0x41, 0x40, 0x10, 0x04, 0x80, 0x20, 0x01, 0x08, 0x02 };
	private static final String[] TX_CODEC_NAMES = { "Opus", "PCM16", "ADPCM", "uLaw", "PCM8" };
	private static final int[] TX_CODECS = { 0x40, 0x04, 0x80, 0x01, 0x02 };
	private static final String[] RATE_NAMES = { "48000", "24000", "16000", "8000" };
	private static final int[] RATES = { 48000, 24000, 16000, 8000 };
	private static final int REQ_PERMS = 1;

	private SharedPreferences prefs;
	private EditText host;
	private EditText port;
	private EditText user;
	private EditText pass;
	private EditText name;
	private EditText latency;
	private EditText pttTimeout;
	private Spinner rxCodec;
	private Spinner rxRate;
	private Spinner txCodec;
	private Spinner txRate;

	private LinearLayout form;

	private EditText field(String label, String value, int type) {
		TextView t = new TextView(this);
		EditText e = new EditText(this);

		t.setText(label);
		e.setText(value);
		e.setInputType(type);
		e.setSingleLine(true);
		form.addView(t);
		form.addView(e);
		return e;
	}

	private Spinner choice(String label, String[] names, int sel) {
		TextView t = new TextView(this);
		Spinner s = new Spinner(this);
		ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, names);

		t.setText(label);
		a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
		s.setAdapter(a);
		s.setSelection(sel);
		form.addView(t);
		form.addView(s);
		return s;
	}

	private static int indexOf(int[] list, int value, int dflt) {
		for (int i = 0; i < list.length; i++)
			if (list[i] == value)
				return i;
		return dflt;
	}

	@Override
	protected void onCreate(Bundle saved) {
		super.onCreate(saved);
		prefs = getSharedPreferences("profile", MODE_PRIVATE);

		ScrollView sv = new ScrollView(this);
		form = new LinearLayout(this);
		form.setOrientation(LinearLayout.VERTICAL);
		int pad = (int)(16 * getResources().getDisplayMetrics().density);
		form.setPadding(pad, pad, pad, pad);
		sv.addView(form, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
				ViewGroup.LayoutParams.WRAP_CONTENT));
		sv.setFitsSystemWindows(true);

		final int text = InputType.TYPE_CLASS_TEXT;
		final int number = InputType.TYPE_CLASS_NUMBER;

		host = field("Server (host name or IPv4 address)", prefs.getString("host", ""), text | InputType.TYPE_TEXT_VARIATION_URI);
		port = field("Control port", Integer.toString(prefs.getInt("port", 50001)), number);
		user = field("Username", prefs.getString("user", ""), text);
		pass = field("Password", prefs.getString("pass", ""), text | InputType.TYPE_TEXT_VARIATION_PASSWORD);
		name = field("Client name (max 8)", prefs.getString("name", "android"), text);
		rxCodec = choice("RX codec", RX_CODEC_NAMES, indexOf(RX_CODECS, prefs.getInt("rx_codec", 0x04), 3));
		rxRate = choice("RX sample rate", RATE_NAMES, indexOf(RATES, prefs.getInt("rx_rate", 48000), 0));
		latency = field("Audio latency (ms)", Integer.toString(prefs.getInt("latency", 150)), number);
		txCodec = choice("TX codec", TX_CODEC_NAMES, indexOf(TX_CODECS, prefs.getInt("tx_codec", 0x04), 1));
		txRate = choice("TX sample rate", RATE_NAMES, indexOf(RATES, prefs.getInt("tx_rate", 48000), 0));
		pttTimeout = field("PTT timeout (s, 0 = off, 30-999)", Integer.toString(prefs.getInt("ptt_timeout", 300)), number);

		Button connect = new Button(this);
		connect.setText("Connect");
		connect.setOnClickListener(v -> connect());
		form.addView(connect);

		setContentView(sv);
	}

	private static int parse(EditText e, int dflt) {
		try {
			return Integer.parseInt(e.getText().toString().trim());
		} catch (NumberFormatException ex) {
			return dflt;
		}
	}

	private void save() {
		String n = name.getText().toString().trim();

		if (n.length() > 8)
			n = n.substring(0, 8);
		prefs.edit()
			.putString("host", host.getText().toString().trim())
			.putInt("port", parse(port, 50001))
			.putString("user", user.getText().toString())
			.putString("pass", pass.getText().toString())
			.putString("name", n)
			.putInt("rx_codec", RX_CODECS[rxCodec.getSelectedItemPosition()])
			.putInt("rx_rate", RATES[rxRate.getSelectedItemPosition()])
			.putInt("latency", parse(latency, 150))
			.putInt("tx_codec", TX_CODECS[txCodec.getSelectedItemPosition()])
			.putInt("tx_rate", RATES[txRate.getSelectedItemPosition()])
			.putInt("ptt_timeout", parse(pttTimeout, 300))
			.apply();
	}

	//
	// Ask for microphone (TX) and notifications (service) once; connect
	// either way, without the microphone TX is not available
	//
	private void connect() {
		ArrayList<String> want = new ArrayList<>();

		save();
		if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED)
			want.add(Manifest.permission.RECORD_AUDIO);
		if (Build.VERSION.SDK_INT >= 33
				&& checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
			want.add(Manifest.permission.POST_NOTIFICATIONS);

		if (!want.isEmpty() && !prefs.getBoolean("asked", false)) {
			prefs.edit().putBoolean("asked", true).apply();
			requestPermissions(want.toArray(new String[0]), REQ_PERMS);
			return;
		}
		launch();
	}

	@Override
	public void onRequestPermissionsResult(int code, String[] perms, int[] results) {
		if (code == REQ_PERMS)
			launch();
	}

	private void launch() {
		startActivity(new Intent(this, RadioActivity.class));
	}
}
