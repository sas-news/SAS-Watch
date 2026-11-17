import java.io.File

plugins {
    id("com.android.application")
    kotlin("android")
    kotlin("plugin.compose")
    id("io.github.takahirom.roborazzi")
}

android {
    namespace = "dev.sasnews.amoledwatch"
    compileSdk = 35

    defaultConfig {
        applicationId = "dev.sasnews.amoledwatch"
        minSdk = 31
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // 実機は OPPO A79 5G (arm64-v8a)。Vosk の native ライブラリで APK が
        // 大きくなるため 1 ABI に絞る。エミュレータで試すときは "x86_64" を足す。
        ndk {
            abiFilters += listOf("arm64-v8a")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro",
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    lint {
        abortOnError = true
    }
    testOptions {
        unitTests {
            isIncludeAndroidResources = true
            all { test ->
                test.systemProperty("robolectric.graphicsMode", "NATIVE")
                // repo1.maven.org が 429 を返す環境向けに Robolectric の依存取得先をミラーにする
                test.systemProperty("robolectric.dependency.repo.url", "https://cache-redirector.jetbrains.com/maven-central")
                // HOME が読み取り専用の環境 (CI サンドボックス等) では Robolectric が
                // ~/.m2 や ~/.robolectric-download-lock を作れずに落ちる。その場合だけ
                // キャッシュ先を build/ 配下へ逃がす (通常の開発機では従来どおり)。
                if (!File(System.getProperty("user.home")).canWrite()) {
                    val robolectricHome = layout.buildDirectory.dir("robolectric-home").get().asFile
                    robolectricHome.mkdirs()
                    test.systemProperty("user.home", robolectricHome.absolutePath)
                }
            }
        }
    }
}

dependencies {
    implementation(project(":protocol"))
    implementation(project(":agent"))
    // 歩く大会: GPX の読み込みと進捗計算 (Android 非依存の純 Kotlin)
    implementation(project(":walk"))

    // AI設定: API キーを EncryptedSharedPreferences に入れるために使う
    implementation("androidx.security:security-crypto:1.1.0-alpha07")

    // 文字起こし: スマホ内のオフライン認識 (Vosk 日本語小型モデル, Apache-2.0)
    implementation("com.alphacephei:vosk-android:0.3.75")

    implementation("androidx.core:core-ktx:1.15.0")
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.8.7")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.8.7")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.9.0")

    implementation(platform("androidx.compose:compose-bom:2024.12.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("junit:junit:4.13.2")
    testImplementation("org.robolectric:robolectric:4.14.1")
    testImplementation("io.github.takahirom.roborazzi:roborazzi-compose:1.49.0")
    testImplementation("io.github.takahirom.roborazzi:roborazzi-core:1.49.0")
    testImplementation("androidx.compose.ui:ui-test-junit4")
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
