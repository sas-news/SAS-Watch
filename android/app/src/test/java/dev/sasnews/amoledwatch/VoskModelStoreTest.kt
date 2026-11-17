package dev.sasnews.amoledwatch

import dev.sasnews.amoledwatch.agent.VoskModelStore
import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Vosk モデルの置き場所/判定 (ネイティブライブラリを読まない範囲)。 */
class VoskModelStoreTest {

    @Test
    fun modelDirRequiresConfModelConf() {
        val dir = File.createTempFile("vosk-model", "").let { f ->
            f.delete()
            File(f.parentFile, "vosk-model-small-ja-0.22").apply { mkdirs() }
        }
        try {
            assertFalse(VoskModelStore.isModelDir(dir))
            File(dir, "conf").mkdirs()
            File(dir, "conf/model.conf").writeText("x")
            assertTrue(VoskModelStore.isModelDir(dir))
        } finally {
            dir.deleteRecursively()
        }
    }

    @Test
    fun constantsMatchDesign() {
        assertEquals("vosk-model-small-ja-0.22", VoskModelStore.MODEL_DIR_NAME)
        assertTrue(VoskModelStore.MODEL_URL.endsWith("vosk-model-small-ja-0.22.zip"))
        assertTrue(VoskModelStore.MODEL_URL.startsWith("https://"))
        assertTrue(VoskModelStore.NO_MODEL_MESSAGE.contains("音声モデル"))
    }
}
