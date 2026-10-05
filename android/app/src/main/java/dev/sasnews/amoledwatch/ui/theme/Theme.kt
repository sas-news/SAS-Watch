package dev.sasnews.amoledwatch.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// AMOLED 時計に合わせて黒基調
private val Colors = darkColorScheme(
    primary = Color(0xFF90CAF9),
    onPrimary = Color(0xFF0B3050),
    secondary = Color(0xFFCE93D8),
    surface = Color(0xFF101418),
    background = Color(0xFF000000),
    surfaceVariant = Color(0xFF1B2228),
    onSurface = Color(0xFFE6EDF2),
    onSurfaceVariant = Color(0xFFAEB9C2),
)

@Composable
fun AmoledWatchTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, content = content)
}
