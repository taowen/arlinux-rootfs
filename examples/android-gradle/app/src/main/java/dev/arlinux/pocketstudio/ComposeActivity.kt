package dev.arlinux.pocketstudio

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.darkColorScheme
import androidx.compose.ui.graphics.Color
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp

/** Standard Compose semantics, Android input connection and Activity lifecycle. */
class ComposeActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val preferences = getSharedPreferences("notes", MODE_PRIVATE)
        setContent {
            MaterialTheme(
                colorScheme = darkColorScheme(
                    primary = Color(0xFF3DDC97), onPrimary = Color(0xFF07130D),
                    background = Color(0xFF0B0F19), surface = Color(0xFF111726), surfaceVariant = Color(0xFF232B40)
                )
            ) {
                var note by rememberSaveable {
                    mutableStateOf(preferences.getString("compose_note", "") ?: "")
                }
                var about by rememberSaveable { mutableStateOf(false) }
                Surface(modifier = Modifier.fillMaxSize()) {
                    Column(
                        modifier = Modifier.padding(24.dp),
                        verticalArrangement = Arrangement.spacedBy(16.dp)
                    ) {
                        Text("Compose workspace", style = MaterialTheme.typography.headlineMedium)
                        Text("Edit here. Build here. Test here.")
                        OutlinedTextField(
                            value = note,
                            onValueChange = { note = it },
                            label = { Text("Compose note") },
                            modifier = Modifier.fillMaxWidth(),
                            minLines = 3
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                            Button(onClick = {
                                preferences.edit().putString("compose_note", note).apply()
                                finish()
                            }) { Text("Save Compose note") }
                            TextButton(onClick = { about = true }) { Text("Compose details") }
                        }
                    }
                }
                if (about) {
                    AlertDialog(
                        onDismissRequest = { about = false },
                        title = { Text("Real Android Compose") },
                        text = { Text("This signed APK was compiled on the phone and runs without system installation.") },
                        confirmButton = {
                            TextButton(onClick = { about = false }) { Text("Close details") }
                        }
                    )
                }
            }
        }
    }
}
