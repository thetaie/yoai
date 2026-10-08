// Declared here, applied in :app. The root project builds nothing itself.
plugins {
    alias(libs.plugins.android.application) apply false
    alias(libs.plugins.kotlin.compose) apply false
}
