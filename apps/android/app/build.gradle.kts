plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "app.localarchiver"
    compileSdk = 35
    buildToolsVersion = "35.0.0"
    defaultConfig {
        applicationId = "app.localarchiver"
        minSdk = 26
        targetSdk = 35
        versionCode = 6001
        versionName = "0.6.0-android.1"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        ndk {
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }
    signingConfigs.getByName("debug") {
        storeFile = file("../.local/debug.keystore")
    }
    buildFeatures {
        buildConfig = false
    }
    // Package the ABI libraries built by scripts/build-android.sh.
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
    packaging {
        // 7-Zip is loaded by its on-disk path through the shared C API.
        jniLibs.useLegacyPackaging = true
    }
    testOptions {
        animationsDisabled = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.15.0")
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
    androidTestImplementation("androidx.test:rules:1.6.1")
    androidTestImplementation("androidx.test.espresso:espresso-core:3.6.1")
}
