package dev.arlinux.pocketstudio

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(28, 28, 28, 28)
        }
        layout.addView(TextView(this).apply {
            text = "Kotlin, built on this phone"
            textSize = 24f
        })
        layout.addView(Button(this).apply {
            text = "Edit note"
            setOnClickListener { startActivity(Intent(this@MainActivity, NoteActivity::class.java)) }
        })
        layout.addView(Button(this).apply {
            text = "Open Compose"
            setOnClickListener { startActivity(Intent(this@MainActivity, ComposeActivity::class.java)) }
        })
        layout.addView(TextView(this).apply {
            text = NativeBridge.summary()
            textSize = 20f
        })
        layout.addView(Button(this).apply {
            text = "About this build"
            setOnClickListener {
                AlertDialog.Builder(this@MainActivity)
                    .setTitle("Phone-built APK")
                    .setMessage("Standard Gradle + Kotlin. No system installation.")
                    .setPositiveButton("Done", null).show()
            }
        })
        setContentView(layout)
    }
}
