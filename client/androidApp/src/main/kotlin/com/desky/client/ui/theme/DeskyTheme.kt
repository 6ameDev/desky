package com.desky.client.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Shapes
import androidx.compose.material3.Typography
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.ReadOnlyComposable
import androidx.compose.runtime.staticCompositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.desky.shared.theme.DeskyTokens

@Immutable
data class DeskyExtraColors(
    val glow: Color,
    val island: Color,
    val metal: Color,
    val film: Color,
    val hit: Color,
)

private fun argb(value: Long): Color = Color(value.toInt())

private val LightExtras = DeskyExtraColors(
    glow = argb(DeskyTokens.Light.glow),
    island = argb(DeskyTokens.Light.island),
    metal = argb(DeskyTokens.Light.metal),
    film = argb(DeskyTokens.Light.film),
    hit = argb(DeskyTokens.Light.hit),
)

private val DarkExtras = DeskyExtraColors(
    glow = argb(DeskyTokens.Dark.glow),
    island = argb(DeskyTokens.Dark.island),
    metal = argb(DeskyTokens.Dark.metal),
    film = argb(DeskyTokens.Dark.film),
    hit = argb(DeskyTokens.Dark.hit),
)

val LocalDeskyExtras = staticCompositionLocalOf { LightExtras }

val MaterialTheme.deskyExtras: DeskyExtraColors
    @Composable
    @ReadOnlyComposable
    get() = LocalDeskyExtras.current

private val DeskyLightScheme = lightColorScheme(
    primary = argb(DeskyTokens.Light.primary),
    onPrimary = Color.White,
    secondary = argb(DeskyTokens.Light.green),
    tertiary = argb(DeskyTokens.Light.amber),
    background = argb(DeskyTokens.Light.canvas),
    onBackground = argb(DeskyTokens.Light.text),
    surface = argb(DeskyTokens.Light.canvas),
    onSurface = argb(DeskyTokens.Light.text),
    surfaceContainer = argb(DeskyTokens.Light.card),
    surfaceVariant = argb(DeskyTokens.Light.island),
    onSurfaceVariant = argb(DeskyTokens.Light.secondary),
    outline = argb(DeskyTokens.Light.metal),
    error = argb(DeskyTokens.Light.danger),
    onError = Color.White,
    scrim = argb(DeskyTokens.Light.film),
)

private val DeskyDarkScheme = darkColorScheme(
    primary = argb(DeskyTokens.Dark.primary),
    onPrimary = Color.White,
    secondary = argb(DeskyTokens.Dark.green),
    tertiary = argb(DeskyTokens.Dark.amber),
    background = argb(DeskyTokens.Dark.canvas),
    onBackground = argb(DeskyTokens.Dark.text),
    surface = argb(DeskyTokens.Dark.canvas),
    onSurface = argb(DeskyTokens.Dark.text),
    surfaceContainer = argb(DeskyTokens.Dark.card),
    surfaceVariant = argb(DeskyTokens.Dark.island),
    onSurfaceVariant = argb(DeskyTokens.Dark.secondary),
    outline = argb(DeskyTokens.Dark.metal),
    error = argb(DeskyTokens.Dark.danger),
    onError = argb(DeskyTokens.Dark.canvas),
    scrim = argb(DeskyTokens.Dark.film),
)

private val DeskyTypography = Typography(
    headlineSmall = TextStyle(
        fontSize = 28.sp,
        fontWeight = FontWeight.Bold,
    ),
    bodyLarge = TextStyle(
        fontSize = 17.sp,
        fontWeight = FontWeight.Normal,
    ),
    bodyMedium = TextStyle(
        fontSize = 16.sp,
        fontWeight = FontWeight.Normal,
    ),
    labelMedium = TextStyle(
        fontSize = 13.sp,
        fontWeight = FontWeight.Medium,
    ),
)

private val DeskyShapes = Shapes(
    small = RoundedCornerShape(16.dp),
    medium = RoundedCornerShape(16.dp),
    large = RoundedCornerShape(28.dp),
    extraLarge = RoundedCornerShape(28.dp),
)

@Composable
fun DeskyTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DeskyDarkScheme else DeskyLightScheme,
        typography = DeskyTypography,
        shapes = DeskyShapes,
        content = {
            CompositionLocalProvider(
                LocalDeskyExtras provides if (darkTheme) DarkExtras else LightExtras,
            ) {
                content()
            }
        },
    )
}
