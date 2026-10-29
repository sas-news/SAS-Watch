package dev.sasnews.amoledwatch.walk

import org.w3c.dom.Document
import org.w3c.dom.Element
import org.w3c.dom.Node
import java.io.ByteArrayInputStream
import javax.xml.XMLConstants
import javax.xml.parsers.DocumentBuilder
import javax.xml.parsers.DocumentBuilderFactory

/** GPX の読み込み失敗（上限超過・XML 不正・座標不正など）。 */
class GpxException(message: String, cause: Throwable? = null) : Exception(message, cause)

/**
 * GPX パーサ (Android 非依存 / JVM で動く)。
 *
 * - `<trk><trkseg><trkpt lat lon>` を優先し、無ければ `<rte><rtept lat lon>` を使う
 * - `<wpt>` をチェックポイントにする（`<name>` が無い wpt は除外）
 * - ルート名は `<trk><name>` → `<rte><name>` → ファイル名の順
 * - 上限 2MB / 20,000 点（超えたら [GpxException]）
 * - XXE 対策: DOCTYPE 禁止・外部エンティティ無効 (AGENTS.md のセキュリティ方針に合わせる)
 */
object GpxParser {
    /** ルートファイルの最大サイズ [byte]。 */
    const val MAX_BYTES = 2 * 1024 * 1024

    /** 最大点数の上限（trk/rte の点 + wpt）。 */
    const val MAX_POINTS = 20_000

    private const val DEFAULT_NAME = "ルート"

    /**
     * GPX バイト列を [Route] にする。
     *
     * @param fallbackName ルート名が GPX に無いときに使う名前（通常はファイル名）
     */
    fun parse(bytes: ByteArray, fallbackName: String = DEFAULT_NAME): Route {
        if (bytes.isEmpty()) throw GpxException("GPX が空です")
        if (bytes.size > MAX_BYTES) {
            throw GpxException("GPX が大きすぎます（上限 2MB / 実際 ${bytes.size} B）")
        }
        val doc = try {
            newDocumentBuilder().parse(ByteArrayInputStream(bytes))
        } catch (e: Exception) {
            throw GpxException("GPX の XML を解析できません", e)
        }
        // 二重の防御: パーサが DOCTYPE を拒否できない実装でもここで弾く。
        if (doc.doctype != null) {
            throw GpxException("GPX に DOCTYPE は使えません")
        }
        return toRoute(doc, fallbackName)
    }

    private fun newDocumentBuilder(): DocumentBuilder {
        val factory = DocumentBuilderFactory.newInstance()
        factory.isNamespaceAware = false
        factory.isXIncludeAware = false
        factory.isExpandEntityReferences = false
        setFeature(factory, "http://apache.org/xml/features/disallow-doctype-decl", true)
        setFeature(factory, "http://xml.org/sax/features/external-general-entities", false)
        setFeature(factory, "http://xml.org/sax/features/external-parameter-entities", false)
        setFeature(factory, "http://apache.org/xml/features/nonvalidating/load-external-dtd", false)
        setFeature(factory, XMLConstants.FEATURE_SECURE_PROCESSING, true)
        return factory.newDocumentBuilder()
    }

    private fun setFeature(factory: DocumentBuilderFactory, name: String, value: Boolean) {
        try {
            factory.setFeature(name, value)
        } catch (e: Exception) {
            // 実装が知らない機能名は無視する (JDK/Android では全対応)。
        }
    }

    private fun toRoute(doc: Document, fallbackName: String): Route {
        val root = doc.documentElement ?: throw GpxException("GPX の要素がありません")
        if (localName(root) != "gpx") throw GpxException("GPX ファイルではありません")

        val elements = ArrayList<Element>()
        collectElements(root, elements)

        val trkpts = elements.filter { localName(it) == "trkpt" }
        val rtepts = elements.filter { localName(it) == "rtept" }
        val wpts = elements.filter { localName(it) == "wpt" }

        val raw = when {
            trkpts.isNotEmpty() -> trkpts
            rtepts.isNotEmpty() -> rtepts
            else -> throw GpxException("ルートの点がありません（trkpt / rtept）")
        }
        if (raw.size + wpts.size > MAX_POINTS) {
            throw GpxException(
                "点が多すぎます（上限 $MAX_POINTS 点 / 実際 ${raw.size + wpts.size} 点）",
            )
        }

        val points = raw.map { parsePoint(it) }
        if (points.size < 2) throw GpxException("ルートの点が2点未満です")

        val name = trackName(elements) ?: routeName(elements) ?: fallbackName.trim()
        val checkpoints = wpts.mapNotNull { wpt ->
            val n = firstChildText(wpt, "name") ?: return@mapNotNull null
            Checkpoint(n, parsePoint(wpt))
        }

        return try {
            Route(name.ifBlank { DEFAULT_NAME }, points, checkpoints)
        } catch (e: IllegalArgumentException) {
            throw GpxException(e.message ?: "ルートを作れません", e)
        }
    }

    /** `<trk><name>` の最初の非空文字列。 */
    private fun trackName(elements: List<Element>): String? =
        elements.asSequence()
            .filter { localName(it) == "trk" }
            .mapNotNull { firstChildText(it, "name") }
            .firstOrNull()

    /** `<rte><name>` の最初の非空文字列。 */
    private fun routeName(elements: List<Element>): String? =
        elements.asSequence()
            .filter { localName(it) == "rte" }
            .mapNotNull { firstChildText(it, "name") }
            .firstOrNull()

    private fun parsePoint(el: Element): GeoPoint {
        val lat = el.getAttribute("lat").trim().toDoubleOrNull()
            ?: throw GpxException("<${localName(el)}> に lat がありません")
        val lon = el.getAttribute("lon").trim().toDoubleOrNull()
            ?: throw GpxException("<${localName(el)}> に lon がありません")
        return try {
            GeoPoint(lat, lon)
        } catch (e: IllegalArgumentException) {
            throw GpxException(e.message ?: "座標が不正です", e)
        }
    }

    /** 名前空間プレフィックスを落とした要素名。 */
    private fun localName(el: Element): String = el.tagName.substringAfterLast(':')

    private fun collectElements(el: Element, out: MutableList<Element>) {
        out.add(el)
        val children = el.childNodes
        for (i in 0 until children.length) {
            val node = children.item(i)
            if (node.nodeType == Node.ELEMENT_NODE) collectElements(node as Element, out)
        }
    }

    /** 直下の子要素 `<name>` のテキスト（空なら null）。 */
    private fun firstChildText(parent: Element, name: String): String? {
        val children = parent.childNodes
        for (i in 0 until children.length) {
            val node = children.item(i)
            if (node.nodeType != Node.ELEMENT_NODE) continue
            val child = node as Element
            if (localName(child) == name) {
                return child.textContent?.trim()?.takeIf { it.isNotEmpty() }
            }
        }
        return null
    }
}
