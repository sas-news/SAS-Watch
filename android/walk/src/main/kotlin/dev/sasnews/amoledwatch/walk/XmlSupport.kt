package dev.sasnews.amoledwatch.walk

import java.io.ByteArrayInputStream
import javax.xml.XMLConstants
import javax.xml.parsers.DocumentBuilder
import javax.xml.parsers.DocumentBuilderFactory
import org.w3c.dom.Document
import org.w3c.dom.Element
import org.w3c.dom.Node

/**
 * XML (GPX / KML) の安全な読み込みと DOM 走査の共通処理。
 *
 * - DOCTYPE 禁止・外部エンティティ無効 (XXE 対策)
 * - 名前空間プレフィックスを落とした要素名で走査する (gx:Track → Track)
 */
internal object XmlSupport {

    /** XML を解析して DOM を返す。壊れている/DOCTYPE 入りは [GpxException]。 */
    fun parse(bytes: ByteArray): Document {
        val doc = try {
            builder().parse(ByteArrayInputStream(bytes))
        } catch (e: Exception) {
            throw GpxException("XML を解析できません", e)
        }
        // 二重の防御: パーサが DOCTYPE を拒否できない実装でもここで弾く。
        if (doc.doctype != null) throw GpxException("DOCTYPE は使えません")
        return doc
    }

    private fun builder(): DocumentBuilder {
        val factory = DocumentBuilderFactory.newInstance()
        factory.isNamespaceAware = false
        // Android の実装は setXIncludeAware で UnsupportedOperationException を投げる
        // ものがある (JVM では通る)。セキュリティ設定は下の setFeature 側で行う。
        try {
            factory.isXIncludeAware = false
        } catch (e: Exception) {
            // 未対応なら無視 (デフォルトで false)。
        }
        try {
            factory.isExpandEntityReferences = false
        } catch (e: Exception) {
            // 未対応なら無視。
        }
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

    /** 名前空間プレフィックスを落とした要素名。 */
    fun localName(el: Element): String = el.tagName.substringAfterLast(':')

    /** 直下の子要素。 */
    fun children(parent: Element): List<Element> {
        val out = ArrayList<Element>(parent.childNodes.length)
        val nodes = parent.childNodes
        for (i in 0 until nodes.length) {
            val node = nodes.item(i)
            if (node.nodeType == Node.ELEMENT_NODE) out.add(node as Element)
        }
        return out
    }

    /** 自分自身を含む全子孫要素 (文書順)。 */
    fun descendants(root: Element): List<Element> {
        val out = ArrayList<Element>()
        collect(root, out)
        return out
    }

    private fun collect(el: Element, out: MutableList<Element>) {
        out.add(el)
        val nodes = el.childNodes
        for (i in 0 until nodes.length) {
            val node = nodes.item(i)
            if (node.nodeType == Node.ELEMENT_NODE) collect(node as Element, out)
        }
    }

    /** 直下の子要素 `<name>` のテキスト (空なら null)。 */
    fun firstChildText(parent: Element, name: String): String? {
        for (child in children(parent)) {
            if (localName(child) == name) {
                return child.textContent?.trim()?.takeIf { it.isNotEmpty() }
            }
        }
        return null
    }
}
