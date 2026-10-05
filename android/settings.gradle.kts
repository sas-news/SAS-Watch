pluginManagement {
    repositories {
        google()
        gradlePluginPortal()
        // Maven Central がレート制限で応答しない環境向けの JetBrains キャッシュミラー
        maven { url = uri("https://cache-redirector.jetbrains.com/maven-central") }
        mavenCentral()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        maven { url = uri("https://cache-redirector.jetbrains.com/maven-central") }
        mavenCentral()
    }
}
rootProject.name = "amoledwatch-companion"
include(":protocol", ":app")
