package dev.arlinux.workbench;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;

/** A real Android app, built and tested on the phone without system installation. */
public class MainActivity extends Activity {
    private int count;
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        count = getPreferences(MODE_PRIVATE).getInt("count", 0);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(28, 28, 28, 28);
        TextView title = new TextView(this);
        title.setText("Built on this phone — v1");
        title.setTextSize(24);
        layout.addView(title);
        EditText note = new EditText(this);
        note.setHint("Type a note with the hosted keyboard");
        layout.addView(note);
        Button button = new Button(this);
        button.setText("Clicks: " + count);
        button.setOnClickListener(view -> {
            button.setText("Clicks: " + ++count);
            getPreferences(MODE_PRIVATE).edit().putInt("count", count).apply();
        });
        layout.addView(button);
        setContentView(layout);
    }
}
