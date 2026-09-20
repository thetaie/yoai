// No `kotlin.android` here: AGP 9 has built-in Kotlin support and registers the
// `kotlin` extension itself, so applying the standalone plugin as well fails
// with "an extension already registered with that name".
plugins {
    alias(libs.plugins.android.application)
    alias(libs.plugins.kotlin.compose)
}

// Read once, used by both the ABI filter and the CMake arguments below.
val bodyclockAbi: String = providers.gradleProperty("bodyclock.abi").get()

android {
    namespace = "ai.bodyclock.app"
    compileSdk = 36
    ndkVersion = providers.gradleProperty("bodyclock.ndk").get()

    defaultConfig {
        applicationId = "ai.bodyclock.app"
        // 26 is what the core needs, not what the lane targets. Nothing here
        // uses an API past it except through the NDK, and the device this
        // exists to measure is running something far newer.
        minSdk = 26
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += bodyclockAbi
        }

        externalNativeBuild {
            cmake {
                // The native module's own CMakeLists adds the repo root as a
                // subdirectory; everything below is for that root, not for the
                // shim. See src/main/cpp/CMakeLists.txt.
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DBODYCLOCK_ANDROID_ABI=$bodyclockAbi",
                )
                cppFlags += "-std=c++11"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            // The SDK ships 3.22.1 and the core asks for 3.19. Pinned rather
            // than left to AGP's default so a newer CMake appearing in the SDK
            // cannot change how this configures without the change being made
            // here first -- the HarmonyOS lane lost an afternoon to exactly
            // that, on a CMake version the SDK had bumped underneath it.
            version = "3.22.1"
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
            // Native debug symbols are the point of a measurement build; the
            // default strips them out of the APK.
            ndk { debugSymbolLevel = "FULL" }
        }
        release {
            isMinifyEnabled = true
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
            ndk { debugSymbolLevel = "SYMBOL_TABLE" }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }

    packaging {
        // Two lanes may each contribute a libc++_shared.so; they are the same
        // file from the same NDK.
        jniLibs.pickFirsts += "**/libc++_shared.so"
    }

    sourceSets.named("main") {
        kotlin.srcDir("src/main/kotlin")
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)

    implementation(libs.compose.ui)
    implementation(libs.compose.ui.graphics)
    implementation(libs.compose.foundation)
    implementation(libs.compose.material3)

    debugImplementation(libs.compose.ui.tooling)
    implementation(libs.compose.ui.tooling.preview)
}
