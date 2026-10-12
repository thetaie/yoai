package com.yoai.bodyclock.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.staticCompositionLocalOf

/**
 * The app's theme: one of the six palettes, resolved from a family and the
 * system's light/dark.
 *
 * **Not Material You.** `dynamicColorScheme` reads the wallpaper, which is a
 * different design than this one and cannot be one of the six. `YoaiBenchTheme`
 * in Theme.kt still follows the device, and stays that way: it dresses the
 * measurement bench, which is a set of readings rather than a designed surface.
 *
 * The palette reaches the screens through [LocalPalette] rather than through
 * MaterialTheme's color scheme, because the palette has 19 tokens and Material's
 * scheme has its own vocabulary that does not line up -- `bg_bubble` and
 * `accent_on_inverse` have no Material name, and `surface_inverse` means
 * something narrower than `inverseSurface` does. The scheme below is filled in
 * anyway so that a stray Material component (a ripple, a text field's cursor)
 * lands somewhere in the palette instead of on Material's purple default; it is
 * a backstop, not the thing to read a color from.
 */
val LocalPalette = staticCompositionLocalOf<Palette> {
    error("No Palette in scope -- wrap the screen in YoaiTheme")
}

@Composable
fun YoaiTheme(
    family: String = DEFAULT_FAMILY,
    dark: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    val palette = resolvePalette(family, dark)

    // `onBrand` is the text color for a strongly colored ground, and
    // `surfaceInverse` is exactly such a ground: white on the dark card in a
    // light palette, near-black on the light card in a dark one. Using
    // `textPrimary` there would be invisible in dark mode, where the text and
    // the inverse surface are the same near-white.
    val scheme = if (dark) {
        darkColorScheme(
            primary = palette.brand,
            onPrimary = palette.onBrand,
            secondary = palette.accentOnInverse,
            background = palette.bgPage,
            onBackground = palette.textPrimary,
            surface = palette.bgSurface,
            onSurface = palette.textPrimary,
            surfaceVariant = palette.bgBubble,
            onSurfaceVariant = palette.textSecondary,
            outline = palette.border,
            inverseSurface = palette.surfaceInverse,
            inverseOnSurface = palette.onBrand,
            error = palette.danger,
            onError = palette.onBrand,
        )
    } else {
        lightColorScheme(
            primary = palette.brand,
            onPrimary = palette.onBrand,
            secondary = palette.accentOnInverse,
            background = palette.bgPage,
            onBackground = palette.textPrimary,
            surface = palette.bgSurface,
            onSurface = palette.textPrimary,
            surfaceVariant = palette.bgBubble,
            onSurfaceVariant = palette.textSecondary,
            outline = palette.border,
            inverseSurface = palette.surfaceInverse,
            inverseOnSurface = palette.onBrand,
            error = palette.danger,
            onError = palette.onBrand,
        )
    }

    CompositionLocalProvider(LocalPalette provides palette) {
        MaterialTheme(colorScheme = scheme, content = content)
    }
}
