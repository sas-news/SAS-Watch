package dev.sasnews.amoledwatch

import dev.sasnews.amoledwatch.ui.screens.filterInstalledApps
import org.junit.Assert.assertEquals
import org.junit.Test

class FilterInstalledAppsTest {

    private val apps = listOf(
        InstalledApp("com.example.alpha", "Alpha"),
        InstalledApp("com.example.beta", "Beta App"),
        InstalledApp("com.spotify.music", "Spotify"),
        InstalledApp("com.example.mail", "メール"),
    )

    @Test
    fun `enabled apps sort first keeping name order`() {
        val result = filterInstalledApps(
            apps,
            sortEnabled = setOf("com.spotify.music", "com.example.mail"),
            query = "",
            onlyEnabled = false,
        )
        assertEquals(
            listOf(
                "com.spotify.music",
                "com.example.mail",
                "com.example.alpha",
                "com.example.beta",
            ),
            result.map { it.pkg },
        )
    }

    @Test
    fun `sort order does not change when enabled changes after open`() {
        // 画面を開いた時点のスナップショット (sortEnabled) で並ぶので、
        // 現在値 (enabledNow) の変化では行が飛ばない。
        val baseline = setOf("com.spotify.music")
        val now = setOf("com.example.alpha", "com.example.mail")
        val result = filterInstalledApps(
            apps, sortEnabled = baseline, query = "", onlyEnabled = false,
            enabledNow = now,
        )
        assertEquals("com.spotify.music", result.first().pkg)
        assertEquals(
            listOf(
                "com.example.alpha",
                "com.example.beta",
                "com.example.mail",
            ),
            result.drop(1).map { it.pkg },
        )
    }

    @Test
    fun `query matches label and package case insensitively`() {
        assertEquals(
            listOf("com.spotify.music"),
            filterInstalledApps(apps, emptySet(), "SPOT", false).map { it.pkg },
        )
        assertEquals(
            listOf("com.example.mail"),
            filterInstalledApps(apps, emptySet(), "メール", false).map { it.pkg },
        )
    }

    @Test
    fun `onlyEnabled filters by current enabled set`() {
        val result = filterInstalledApps(
            apps,
            sortEnabled = emptySet(),
            query = "",
            onlyEnabled = true,
            enabledNow = setOf("com.example.beta", "com.example.mail"),
        )
        assertEquals(
            listOf("com.example.beta", "com.example.mail"),
            result.map { it.pkg },
        )
    }

    @Test
    fun `query and filter combined`() {
        val result = filterInstalledApps(
            apps,
            sortEnabled = setOf("com.example.mail"),
            query = "example",
            onlyEnabled = false,
        )
        assertEquals(
            listOf("com.example.mail", "com.example.alpha", "com.example.beta"),
            result.map { it.pkg },
        )
    }
}
