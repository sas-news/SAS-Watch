package dev.sasnews.amoledwatch

import android.util.Log
import java.net.HttpURLConnection
import java.net.URL
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject

/**
 * GitHub Releases の最新リリースから firmware.bin の URL と sha256 を取る。
 * `.github/workflows/release.yml` がタグごとに firmware.bin / firmware.bin.sha256
 * を添付する前提。取得失敗・未添付は null。
 */
object FirmwareRepo {

    private const val TAG = "FirmwareRepo"
    private const val RELEASES_API =
        "https://api.github.com/repos/sas-news/SAS-Watch/releases/latest"

    data class ReleaseInfo(
        val tag: String,
        val binUrl: String,
        val sha256Hex: String,
    ) {
        /** sha256 hex を ByteArray(32) に。形が違えば null。 */
        fun sha256Bytes(): ByteArray? {
            val hex = sha256Hex.trim()
            if (hex.length != 64) return null
            return runCatching {
                ByteArray(32) { i -> hex.substring(i * 2, i * 2 + 2).toInt(16).toByte() }
            }.getOrNull()
        }
    }

    suspend fun latest(): ReleaseInfo? = withContext(Dispatchers.IO) {
        try {
            val rel = JSONObject(getText(RELEASES_API))
            val tag = rel.optString("tag_name")
            var binUrl: String? = null
            var shaUrl: String? = null
            val assets = rel.optJSONArray("assets") ?: return@withContext null
            for (i in 0 until assets.length()) {
                val a = assets.getJSONObject(i)
                when (a.optString("name")) {
                    "firmware.bin" -> binUrl = a.optString("browser_download_url")
                    "firmware.bin.sha256" -> shaUrl = a.optString("browser_download_url")
                }
            }
            if (binUrl.isNullOrEmpty() || shaUrl.isNullOrEmpty()) return@withContext null
            ReleaseInfo(tag, binUrl, getText(shaUrl).trim().take(64))
        } catch (e: Exception) {
            Log.w(TAG, "latest failed", e)
            null
        }
    }

    private fun getText(url: String): String {
        val c = URL(url).openConnection() as HttpURLConnection
        c.connectTimeout = 10_000
        c.readTimeout = 15_000
        // GitHub API は UA 必須。asset のダウンロードにもそのまま使う。
        c.setRequestProperty("User-Agent", "saswatch-android")
        c.setRequestProperty("Accept", "application/vnd.github+json")
        try {
            return c.inputStream.bufferedReader().use { it.readText() }
        } finally {
            c.disconnect()
        }
    }
}
