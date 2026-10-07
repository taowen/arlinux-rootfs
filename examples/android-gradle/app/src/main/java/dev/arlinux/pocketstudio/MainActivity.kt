package dev.arlinux.pocketstudio

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val wide = resources.displayMetrics.widthPixels > resources.displayMetrics.heightPixels
        val root = LinearLayout(this).apply {
            orientation = if (wide) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            setPadding(dp(32), dp(28), dp(32), dp(28))
            background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(Ui.BACKGROUND_TOP, Ui.BACKGROUND_BOTTOM))
        }
        val text = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL
        }
        text.addView(label("POCKET STUDIO", 16f, Ui.ACCENT, bold = true).apply { letterSpacing = 0.2f })
        text.addView(label("Kotlin, built on this phone", 46f, Color.WHITE, bold = true).apply { setPadding(0, dp(6), 0, dp(6)) })
        text.addView(label("Edited in Geany. Compiled by Gradle on ARM64. Running without installation.", 20f, Ui.MUTED))
        text.addView(LinearLayout(this).apply {
            setPadding(0, dp(18), 0, dp(18))
            for ((name, color) in listOf("Kotlin" to Ui.KOTLIN, "Compose" to Ui.ACCENT, "C++ NDK" to Ui.NATIVE))
                addView(chip(name, color))
        })
        text.addView(label(NativeBridge.summary(), 24f, Ui.NATIVE, bold = true).apply {
            typeface = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)
            setPadding(0, 0, 0, dp(18))
        })
        text.addView(action("Edit note", Ui.KOTLIN) { startActivity(Intent(this, NoteActivity::class.java)) })
        text.addView(action("Open Compose", Ui.ACCENT) { startActivity(Intent(this, ComposeActivity::class.java)) })
        text.addView(action("About this build", Ui.CARD) {
            AlertDialog.Builder(this)
                .setTitle("Phone-built APK")
                .setMessage("Standard Gradle + Kotlin. No system installation.")
                .setPositiveButton("Done", null).show()
        })
        root.addView(text, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1f).takeIf { wide }
            ?: LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        root.addView(fractalCard(), LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1.1f).apply { leftMargin = dp(28) }.takeIf { wide }
            ?: LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f).apply { topMargin = dp(20) })
        setContentView(root)
    }

    /** An image computed by C++: edit kHue in native.cpp and rebuild to recolor it. */
    private fun fractalCard(): View {
        val width = 600
        val height = 600
        val pixels = IntArray(width * height)
        NativeBridge.render(pixels, width, height)
        val card = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14), dp(14), dp(14), dp(14))
            background = rounded(Ui.CARD, 22)
        }
        card.addView(ImageView(this).apply {
            setImageBitmap(Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888))
            scaleType = ImageView.ScaleType.CENTER_CROP
            clipToOutline = true
            background = rounded(Color.BLACK, 16)
            contentDescription = "Fractal rendered by C++"
        }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        card.addView(label("Rendered by C++ through JNI · native.cpp", 16f, Ui.MUTED).apply { setPadding(dp(4), dp(12), 0, 0) })
        return card
    }

    private fun label(value: String, size: Float, color: Int, bold: Boolean = false) = TextView(this).apply {
        text = value
        textSize = size
        setTextColor(color)
        if (bold) typeface = Typeface.DEFAULT_BOLD
    }

    private fun chip(value: String, color: Int) = label(value, 16f, color, bold = true).apply {
        setPadding(dp(14), dp(6), dp(14), dp(6))
        background = rounded(Color.argb(40, Color.red(color), Color.green(color), Color.blue(color)), 30)
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply { rightMargin = dp(10) }
    }

    private fun action(value: String, color: Int, onClick: () -> Unit) = Button(this).apply {
        text = value
        isAllCaps = false
        textSize = 21f
        setTextColor(Color.WHITE)
        typeface = Typeface.DEFAULT_BOLD
        stateListAnimator = null
        background = rounded(color, 28)
        setOnClickListener { onClick() }
        layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(62)).apply { bottomMargin = dp(12) }
    }

    private fun rounded(color: Int, radius: Int) = GradientDrawable().apply {
        setColor(color)
        cornerRadius = dp(radius).toFloat()
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
}

/** Shared palette, so every screen looks like one app. */
object Ui {
    val BACKGROUND_TOP = Color.parseColor("#141B2D")
    val BACKGROUND_BOTTOM = Color.parseColor("#0B0F19")
    val CARD = Color.parseColor("#232B40")
    val MUTED = Color.parseColor("#9AA6BF")
    val ACCENT = Color.parseColor("#3DDC97")
    val KOTLIN = Color.parseColor("#7F52FF")
    val NATIVE = Color.parseColor("#FFB547")
}
