package dev.sasnews.amoledwatch.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.sp

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

// 既定より一段太く・大きく (読みやすさ重視)
private val AppTypography = Typography().run {
    copy(
        bodyLarge = bodyLarge.copy(
            fontSize = 17.sp,
            lineHeight = 24.sp,
            fontWeight = FontWeight.Medium,
        ),
        bodyMedium = bodyMedium.copy(
            fontSize = 16.sp,
            fontWeight = FontWeight.Medium,
        ),
        bodySmall = bodySmall.copy(fontSize = 14.sp),
        titleLarge = titleLarge.copy(fontWeight = FontWeight.SemiBold),
        titleMedium = titleMedium.copy(fontWeight = FontWeight.SemiBold),
        titleSmall = titleSmall.copy(fontWeight = FontWeight.Medium),
        labelLarge = labelLarge.copy(fontWeight = FontWeight.Medium),
        labelMedium = labelMedium.copy(fontWeight = FontWeight.Medium),
        labelSmall = labelSmall.copy(fontWeight = FontWeight.Medium),
    )
}

@Composable
fun AmoledWatchTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = Colors, typography = AppTypography, content = content)
}
