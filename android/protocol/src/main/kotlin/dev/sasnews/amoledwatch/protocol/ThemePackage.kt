package dev.sasnews.amoledwatch.protocol

import java.io.ByteArrayInputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipInputStream

/**
 * docs/theme-format.md のテーマパッケージ（stored zip + manifest.cbor）検査。
 * BULK `kind:"theme"` で送る前の事前検査と、FakeWatch の commit 検査で使う。
 */
object ThemePackage {

    const val API_VERSION = 1
    const val MAX_PACKAGE_SIZE = 3 * 1024 * 1024 // 3 MiB（theme-format.md 予算表）
    const val MAX_ENTRIES = 16

    private val ENTRY_NAME = Regex("[a-z0-9._-]+")
    private val THEME_ID = Regex("[a-z0-9-]{1,31}")

    /** 検査に通ったパッケージの情報。 */
    data class ThemePkgInfo(
        val id: String,
        val name: String,
        val api: Int = API_VERSION,
    )

    /** パッケージが規格外のときに投げる。 */
    class Invalid(message: String) : Exception(message)

    /**
     * bytes を theme-format.md の規則で検査する。
     * - stored zip（圧縮エントリ不可）
     * - エントリ名は `[a-z0-9._-]`、ディレクトリ・`..` 不可、16 個まで
     * - `manifest.cbor` 必須。CBOR map に id（theme id 形式）と api==1 が必要
     * 全て通れば ThemePkgInfo、駄目なら Invalid を投げる。
     */
    fun inspect(bytes: ByteArray): ThemePkgInfo {
        if (bytes.isEmpty()) throw Invalid("空のパッケージです")
        if (bytes.size > MAX_PACKAGE_SIZE) {
            throw Invalid("パッケージが上限 ${MAX_PACKAGE_SIZE / 1024 / 1024} MiB を超えています")
        }
        var manifest: ByteArray? = null
        var count = 0
        try {
            ZipInputStream(ByteArrayInputStream(bytes)).use { z ->
                while (true) {
                    val e = z.nextEntry ?: break
                    count++
                    if (count > MAX_ENTRIES) throw Invalid("エントリが多すぎます（$MAX_ENTRIES まで）")
                    val name = e.name
                    if (e.isDirectory || name.contains("..") || !ENTRY_NAME.matches(name)) {
                        throw Invalid("エントリ名が不正です: $name")
                    }
                    if (e.method != ZipEntry.STORED) {
                        throw Invalid("圧縮エントリは受け付けません（STORED のみ）: $name")
                    }
                    val data = z.readBytes()
                    if (name == "manifest.cbor") manifest = data
                    z.closeEntry()
                }
            }
        } catch (e: ZipException) {
            throw Invalid("zip として読めません: ${e.message}")
        }
        if (count == 0) throw Invalid("空の zip です")

        val m = try {
            CborCodec.decode(manifest ?: throw Invalid("manifest.cbor がありません"))
        } catch (e: CborCodec.DecodeException) {
            throw Invalid("manifest.cbor が CBOR として読めません")
        } as? Cbor.Cmap ?: throw Invalid("manifest.cbor が map ではありません")

        val id = (m.value["id"] as? Cbor.Ctext)?.value
            ?: throw Invalid("manifest に id がありません")
        if (!THEME_ID.matches(id)) throw Invalid("theme id の形式が不正です: $id")
        val api = m.int("api", -1)
        if (api != API_VERSION.toLong()) throw Invalid("未対応の api バージョンです: $api")
        val name = m.text("name", id)
        return ThemePkgInfo(id = id, name = name)
    }
}
