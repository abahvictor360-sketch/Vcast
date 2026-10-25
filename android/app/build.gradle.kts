plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Release signing: a private keystore can be supplied through the environment (CI secrets).
// Without one, release builds are signed with the debug keystore committed next to this file,
// so every CI build is installable and can be installed over the previous one.
val releaseKeystore: String? = System.getenv("VCAST_KEYSTORE_FILE")?.takeIf { it.isNotBlank() }

android {
    namespace = "com.vcast.sender"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.vcast.sender"
        minSdk = 26
        targetSdk = 35
        versionCode = System.getenv("VCAST_VERSION_CODE")?.toIntOrNull() ?: 1
        versionName = "1.0"
    }

    signingConfigs {
        getByName("debug") {
            storeFile = file("debug.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
        create("release") {
            if (releaseKeystore != null) {
                storeFile = file(releaseKeystore)
                storePassword = System.getenv("VCAST_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("VCAST_KEY_ALIAS")
                keyPassword = System.getenv("VCAST_KEY_PASSWORD") ?: System.getenv("VCAST_KEYSTORE_PASSWORD")
            } else {
                storeFile = file("debug.keystore")
                storePassword = "android"
                keyAlias = "androiddebugkey"
                keyPassword = "android"
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("release")
        }
    }

    buildFeatures {
        buildConfig = true
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }

    lint {
        abortOnError = true
        // Release lint runs as part of assembleRelease; CI runs `lint` explicitly instead.
        checkReleaseBuilds = false
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
}
