plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.localarchiver"
    compileSdk = 34
    defaultConfig {
        applicationId = "app.localarchiver"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.6.0"
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }
    buildFeatures {
        buildConfig = false
    }
    // Package the arm64 libraries already built by scripts/build-android.sh.
    // This debug APK does not compile 7-Zip or the core again.
    sourceSets {
        getByName("main") {
            // Only an ABI directory belongs here. The CMake output also drops
            // loose .so files next to it, and AGP rejects those.
            jniLibs.srcDir("${rootDir}/../../build-android/jni-package")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
}
