// SPDX-License-Identifier: GPL-3.0-or-later
//
// Connection profile editor and launcher. Built in code (no layout
// resources). The values in use are kept in SharedPreferences "profile"
// (RadioActivity reads them there); up to 16 named connections are kept in
// "connections" to pick from (2026-11-15).
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
import android.graphics.Color;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;

public class ProfileActivity extends Activity {

	//
	// Spinner lists with explicit colours: the current choice on an accent
	// background, the rest white on dark (the theme greys out the checked
	// row, which made it unreadable)
	//
	private final class ChoiceAdapter extends ArrayAdapter<String> {
		private final Spinner owner;

		ChoiceAdapter(Spinner owner, String[] names) {
			super(ProfileActivity.this, android.R.layout.simple_spinner_item, names);
			this.owner = owner;
			setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
		}

		@Override
		public View getView(int pos, View convert, ViewGroup parent) {
			TextView t = (TextView)super.getView(pos, convert, parent);

			t.setTextColor(Color.WHITE);
			return t;
		}

		@Override
		public View getDropDownView(int pos, View convert, ViewGroup parent) {
			TextView t = (TextView)super.getDropDownView(pos, convert, parent);
			boolean cur = pos == owner.getSelectedItemPosition();

			t.setEnabled(true);
			t.setTextColor(Color.WHITE);
			t.setBackgroundColor(cur ? 0xff2e6d8e : 0xff1d2733);
			if (t instanceof android.widget.CheckedTextView)
				((android.widget.CheckedTextView)t).setCheckMarkDrawable(null);
			return t;
		}
	}

	//
	// One connection: every field of the form, and its name in the list
	//
	private static final class Conn {
		String label = "";
		String host = "";
		int port = 50001;
		String user = "";
		String pass = "";
		String name = "android";
		int rxCodec = 0x04;
		int rxRate = 48000;
		int latency = 150;
		int txCodec = 0x04;
		int txRate = 48000;
		int pttTimeout = 300;
		boolean micBt;			/* the mic: false the phone, true a Bluetooth headset */
		int micGain = 100;		/* %, 100-400 */
		boolean lanMod = true;		/* the rig's DATA OFF MOD to LAN while connected, back after */
	}

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
	private static final int MAX_CONNS = 16;
	private static final String NEW_ENTRY = "(new connection)";
	private static final String[] MIC_NAMES = { "Phone", "Bluetooth headset" };
	private static final String[] LANMOD_NAMES = { "LAN while connected", "Leave as it is" };

	private SharedPreferences prefs;
	private SharedPreferences store;
	private final ArrayList<Conn> conns = new ArrayList<>();
	private Spinner list;
	private int shown;			/* list position the form was filled from */
	private EditText label;
	private EditText host;
	private EditText port;
	private EditText user;
	private EditText pass;
	private EditText name;
	private EditText latency;
	private EditText pttTimeout;
	private Spinner micSrc;
	private EditText micGain;
	private Spinner lanMod;
	private Spinner rxCodec;
	private Spinner rxRate;
	private Spinner txCodec;
	private Spinner txRate;

	private LinearLayout form;

	private TextView caption(String text) {
		TextView t = new TextView(this);

		t.setText(text);
		form.addView(t);
		return t;
	}

	private EditText field(String label, String value, int type) {
		EditText e = new EditText(this);

		caption(label);
		e.setText(value);
		// Single line first: setSingleLine() after setInputType() undoes the password
		// masking (the password was shown in clear, 2026-11-15)
		e.setSingleLine(true);
		e.setInputType(type);
		form.addView(e);
		return e;
	}

	private Spinner choice(String label, String[] names, int sel) {
		Spinner s = new Spinner(this);
		ArrayAdapter<String> a = new ChoiceAdapter(s, names);

		caption(label);
		s.setAdapter(a);
		s.setSelection(sel);
		form.addView(s);
		return s;
	}

	//
	// The password with an eye to its right (2026-11-15): dim while hidden, bright while
	// shown; a tap switches. Always hidden when the page opens
	//
	private EditText passwordField(String label) {
		final int hidden = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD;
		final int shown = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
		LinearLayout row = new LinearLayout(this);
		EditText e = new EditText(this);
		Button eye = new Button(this);

		caption(label);
		row.setOrientation(LinearLayout.HORIZONTAL);
		e.setSingleLine(true);	/* before the input type: see field() */
		e.setInputType(hidden);
		row.addView(e, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
		eye.setText("👁");	/* the eye */
		eye.setAlpha(0.4f);
		eye.setOnClickListener(v -> {
			boolean show = e.getInputType() == hidden;

			e.setInputType(show ? shown : hidden);
			e.setSelection(e.getText().length());
			eye.setAlpha(show ? 1.0f : 0.4f);
		});
		row.addView(eye, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
				ViewGroup.LayoutParams.WRAP_CONTENT, 0.0f));
		form.addView(row);
		return e;
	}

	private Button button(LinearLayout row, String text, View.OnClickListener l) {
		Button b = new Button(this);

		b.setText(text);
		b.setOnClickListener(l);
		row.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.0f));
		return b;
	}

	private static int indexOf(int[] list, int value, int dflt) {
		for (int i = 0; i < list.length; i++)
			if (list[i] == value)
				return i;
		return dflt;
	}

	private static int parse(EditText e, int dflt) {
		try {
			return Integer.parseInt(e.getText().toString().trim());
		} catch (NumberFormatException ex) {
			return dflt;
		}
	}

	//
	// The values in use ("profile") as a connection
	//
	private Conn current() {
		Conn c = new Conn();

		c.label = prefs.getString("label", "");
		c.host = prefs.getString("host", "");
		c.port = prefs.getInt("port", 50001);
		c.user = prefs.getString("user", "");
		c.pass = prefs.getString("pass", "");
		c.name = prefs.getString("name", "android");
		c.rxCodec = prefs.getInt("rx_codec", 0x04);
		c.rxRate = prefs.getInt("rx_rate", 48000);
		c.latency = prefs.getInt("latency", 150);
		c.txCodec = prefs.getInt("tx_codec", 0x04);
		c.txRate = prefs.getInt("tx_rate", 48000);
		c.pttTimeout = prefs.getInt("ptt_timeout", 300);
		c.micBt = prefs.getBoolean("mic_bt", false);
		c.micGain = prefs.getInt("mic_gain", 100);
		c.lanMod = prefs.getBoolean("lan_mod", true);
		return c;
	}

	//
	// The saved list ("connections"): count, then <key>_<i> per connection
	//
	private void loadList() {
		int n = Math.min(store.getInt("count", 0), MAX_CONNS);

		conns.clear();
		for (int i = 0; i < n; i++) {
			Conn c = new Conn();

			c.label = store.getString("label_" + i, "");
			c.host = store.getString("host_" + i, "");
			c.port = store.getInt("port_" + i, 50001);
			c.user = store.getString("user_" + i, "");
			c.pass = store.getString("pass_" + i, "");
			c.name = store.getString("name_" + i, "android");
			c.rxCodec = store.getInt("rx_codec_" + i, 0x04);
			c.rxRate = store.getInt("rx_rate_" + i, 48000);
			c.latency = store.getInt("latency_" + i, 150);
			c.txCodec = store.getInt("tx_codec_" + i, 0x04);
			c.txRate = store.getInt("tx_rate_" + i, 48000);
			c.pttTimeout = store.getInt("ptt_timeout_" + i, 300);
			c.micBt = store.getBoolean("mic_bt_" + i, false);
			c.micGain = store.getInt("mic_gain_" + i, 100);
			c.lanMod = store.getBoolean("lan_mod_" + i, true);
			conns.add(c);
		}
	}

	private void storeList() {
		SharedPreferences.Editor ed = store.edit().clear().putInt("count", conns.size());

		for (int i = 0; i < conns.size(); i++) {
			Conn c = conns.get(i);

			ed.putString("label_" + i, c.label)
				.putString("host_" + i, c.host)
				.putInt("port_" + i, c.port)
				.putString("user_" + i, c.user)
				.putString("pass_" + i, c.pass)
				.putString("name_" + i, c.name)
				.putInt("rx_codec_" + i, c.rxCodec)
				.putInt("rx_rate_" + i, c.rxRate)
				.putInt("latency_" + i, c.latency)
				.putInt("tx_codec_" + i, c.txCodec)
				.putInt("tx_rate_" + i, c.txRate)
				.putInt("ptt_timeout_" + i, c.pttTimeout)
				.putBoolean("mic_bt_" + i, c.micBt)
				.putInt("mic_gain_" + i, c.micGain)
				.putBoolean("lan_mod_" + i, c.lanMod);
		}
		ed.apply();
	}

	private int find(String lbl) {
		for (int i = 0; i < conns.size(); i++)
			if (conns.get(i).label.equals(lbl))
				return i;
		return -1;
	}

	//
	// Form <-> connection
	//
	private void fill(Conn c) {
		label.setText(c.label);
		host.setText(c.host);
		port.setText(Integer.toString(c.port));
		user.setText(c.user);
		pass.setText(c.pass);
		name.setText(c.name);
		rxCodec.setSelection(indexOf(RX_CODECS, c.rxCodec, 3));
		rxRate.setSelection(indexOf(RATES, c.rxRate, 0));
		latency.setText(Integer.toString(c.latency));
		txCodec.setSelection(indexOf(TX_CODECS, c.txCodec, 1));
		txRate.setSelection(indexOf(RATES, c.txRate, 0));
		pttTimeout.setText(Integer.toString(c.pttTimeout));
		micSrc.setSelection(c.micBt ? 1 : 0);
		micGain.setText(Integer.toString(c.micGain));
		lanMod.setSelection(c.lanMod ? 0 : 1);
	}

	private Conn read() {
		Conn c = new Conn();
		String n = name.getText().toString().trim();

		c.label = label.getText().toString().trim();
		c.host = host.getText().toString().trim();
		c.port = parse(port, 50001);
		c.user = user.getText().toString();
		c.pass = pass.getText().toString();
		c.name = n.length() > 8 ? n.substring(0, 8) : n;
		c.rxCodec = RX_CODECS[rxCodec.getSelectedItemPosition()];
		c.rxRate = RATES[rxRate.getSelectedItemPosition()];
		c.latency = parse(latency, 150);
		c.txCodec = TX_CODECS[txCodec.getSelectedItemPosition()];
		c.txRate = RATES[txRate.getSelectedItemPosition()];
		c.pttTimeout = parse(pttTimeout, 300);
		c.micBt = micSrc.getSelectedItemPosition() == 1;
		c.micGain = Math.max(100, Math.min(400, parse(micGain, 100)));
		c.lanMod = lanMod.getSelectedItemPosition() == 0;
		return c;
	}

	//
	// The list on screen: the saved connections, then "(new connection)";
	// sel: the position to show (no form fill)
	//
	private void showList(int sel) {
		String[] names = new String[conns.size() + 1];

		for (int i = 0; i < conns.size(); i++)
			names[i] = conns.get(i).label;
		names[conns.size()] = NEW_ENTRY;
		list.setAdapter(new ChoiceAdapter(list, names));
		shown = sel;
		list.setSelection(sel, false);
	}

	@Override
	protected void onCreate(Bundle saved) {
		super.onCreate(saved);
		prefs = getSharedPreferences("profile", MODE_PRIVATE);
		store = getSharedPreferences("connections", MODE_PRIVATE);
		loadList();

		// First start with a list: the single profile used so far becomes its first
		// entry, named after its host
		Conn cur = current();

		if (conns.isEmpty() && !store.getBoolean("seeded", false)) {
			if (!cur.host.isEmpty()) {
				if (cur.label.isEmpty())
					cur.label = cur.host;
				conns.add(cur);
			}
			storeList();
			store.edit().putBoolean("seeded", true).apply();
		}

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

		caption("Saved connections (up to " + MAX_CONNS + ")");
		list = new Spinner(this);
		form.addView(list);
		label = field("Name of this connection", "", text);

		LinearLayout row = new LinearLayout(this);
		row.setOrientation(LinearLayout.HORIZONTAL);
		button(row, "Connect", v -> connect());
		button(row, "Save", v -> saveEntry());
		button(row, "Delete", v -> deleteEntry());
		form.addView(row);

		host = field("Server (host name or IPv4 address)", "", text | InputType.TYPE_TEXT_VARIATION_URI);
		port = field("Control port", "", number);
		user = field("Username", "", text);
		pass = passwordField("Password");
		name = field("Client name (max 8)", "", text);
		rxCodec = choice("RX codec", RX_CODEC_NAMES, 3);
		rxRate = choice("RX sample rate", RATE_NAMES, 0);
		latency = field("Audio latency (ms)", "", number);
		txCodec = choice("TX codec", TX_CODEC_NAMES, 1);
		txRate = choice("TX sample rate", RATE_NAMES, 0);
		pttTimeout = field("PTT timeout (s, 0 = off, 30-999)", "", number);
		micSrc = choice("Microphone", MIC_NAMES, 0);
		micGain = field("Mic gain (%, 100-400)", "", number);
		lanMod = choice("Rig mic input (DATA OFF MOD)", LANMOD_NAMES, 0);

		// The form shows the values in use; the list stands on their entry, if saved
		cur = current();
		// The form from the saved entry of the connection in use, if there is one: Save
		// wrote only the list, so after a Save and a restart the form showed the values
		// of the last Connect under the entry's name (Bluetooth back to Phone, 2026-11-15)
		int at = find(cur.label);
		fill(at >= 0 ? conns.get(at) : cur);
		showList(at >= 0 ? at : conns.size());

		list.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
			@Override
			public void onItemSelected(AdapterView<?> parent, View v, int pos, long id) {
				if (pos == shown)
					return;
				shown = pos;
				if (pos < conns.size()) {
					fill(conns.get(pos));
				} else {
					// New: the form stays as a start, without a name
					label.setText("");
				}
			}

			@Override
			public void onNothingSelected(AdapterView<?> parent) {
			}
		});

		setContentView(sv);
	}

	//
	// Save: a name in the list is replaced, a new one added (up to MAX_CONNS)
	//
	private void saveEntry() {
		Conn c = read();
		int at;

		if (c.label.isEmpty())
			c.label = c.host;
		if (c.label.isEmpty()) {
			Toast.makeText(this, "Give the connection a name or a server", Toast.LENGTH_SHORT).show();
			return;
		}

		at = find(c.label);
		if (at >= 0) {
			conns.set(at, c);
		} else if (conns.size() >= MAX_CONNS) {
			Toast.makeText(this, "List full (" + MAX_CONNS + "): delete one first", Toast.LENGTH_SHORT).show();
			return;
		} else {
			conns.add(c);
			at = conns.size() - 1;
		}

		storeList();
		label.setText(c.label);
		save();		/* the values in use too: the next start and Connect take them */
		showList(at);
		Toast.makeText(this, "Saved: " + c.label, Toast.LENGTH_SHORT).show();
	}

	private void deleteEntry() {
		int at = list.getSelectedItemPosition();

		if (at < 0 || at >= conns.size()) {
			Toast.makeText(this, "Pick a saved connection to delete", Toast.LENGTH_SHORT).show();
			return;
		}

		String gone = conns.remove(at).label;

		storeList();
		showList(conns.size());		/* the form stays: Save brings it back */
		label.setText("");
		Toast.makeText(this, "Deleted: " + gone, Toast.LENGTH_SHORT).show();
	}

	//
	// The form into the values in use (RadioActivity reads them)
	//
	private void save() {
		Conn c = read();

		prefs.edit()
			.putString("label", c.label)
			.putString("host", c.host)
			.putInt("port", c.port)
			.putString("user", c.user)
			.putString("pass", c.pass)
			.putString("name", c.name)
			.putInt("rx_codec", c.rxCodec)
			.putInt("rx_rate", c.rxRate)
			.putInt("latency", c.latency)
			.putInt("tx_codec", c.txCodec)
			.putInt("tx_rate", c.txRate)
			.putInt("ptt_timeout", c.pttTimeout)
			.putBoolean("mic_bt", c.micBt)
			.putInt("mic_gain", c.micGain)
			.putBoolean("lan_mod", c.lanMod)
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
