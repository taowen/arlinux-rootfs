package dev.arlinux.pocketstudio

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout

class NoteActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val preferences = getSharedPreferences("notes", MODE_PRIVATE)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(28, 28, 28, 28)
        }
        val note = EditText(this).apply {
            hint = "Write a note"
            setText(preferences.getString("text", ""))
        }
        layout.addView(note)
        layout.addView(Button(this).apply {
            text = "Save and return"
            setOnClickListener {
                preferences.edit().putString("text", note.text.toString()).apply()
                finish()
            }
        })
        setContentView(layout)
    }
}
