package dev.arlinux.pocketstudio

import android.app.Activity
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView

class NoteActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val preferences = getSharedPreferences("notes", MODE_PRIVATE)
        val density = resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()
        fun rounded(color: Int) = GradientDrawable().apply { setColor(color); cornerRadius = dp(18).toFloat() }
        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(32), dp(28), dp(32), dp(28))
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Ui.BACKGROUND_TOP, Ui.BACKGROUND_BOTTOM))
        }
        layout.addView(TextView(this).apply {
            text = "Note"
            textSize = 40f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            setPadding(0, 0, 0, dp(16))
        })
        val note = EditText(this).apply {
            hint = "Write a note"
            setText(preferences.getString("text", ""))
            textSize = 24f
            setTextColor(Color.WHITE)
            setHintTextColor(Ui.MUTED)
            minLines = 4
            setPadding(dp(18), dp(16), dp(18), dp(16))
            background = rounded(Ui.CARD)
        }
        layout.addView(note)
        layout.addView(Button(this).apply {
            text = "Save and return"
            isAllCaps = false
            textSize = 21f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            stateListAnimator = null
            background = rounded(Ui.KOTLIN)
            setOnClickListener {
                preferences.edit().putString("text", note.text.toString()).apply()
                finish()
            }
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(62)).apply { topMargin = dp(16) })
        setContentView(layout)
    }
}
