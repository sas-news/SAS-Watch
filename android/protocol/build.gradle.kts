plugins {
    kotlin("jvm")
}

java {
    toolchain {
        languageVersion.set(JavaLanguageVersion.of(17))
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    // テストベクタ (docs/protocol-vectors/*.json) の読み込み用。
    // JsonObject が挿入順を保持するので CBOR map キー順の検査に使う。
    testImplementation("com.google.code.gson:gson:2.11.0")
}
