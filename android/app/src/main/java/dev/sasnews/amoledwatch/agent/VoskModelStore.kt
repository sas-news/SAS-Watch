package dev.sasnews.amoledwatch.agent

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipInputStream
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.vosk.Model
import org.vosk.Recognizer

/**
 * スマホ内のオフライン音声認識 (Vosk 日本語小型モデル) の管理。
 *
 * - モデルは初回だけ HTTPS でダウンロードしてアプリ領域に展開し、以後は再利用する
 *   (無料・Apache-2.0。ネットワークは初回のみ)。
 * - ダウンロード/展開の進捗を [state] で出し、設定画面に表示する。
 * - 失敗は日本語メッセージにして [state] の Failed で返す。
 */
class VoskModelStore(
    private val context: Context,
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.IO),
    private val client: OkHttpClient = OkHttpClient.Builder()
        .connectTimeout(20, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .build(),
) {
    companion object {
        private const val TAG = "VoskModelStore"

        /** 無料の日本語小型モデル (約 50MB, Apache-2.0)。 */
        const val MODEL_URL =
            "https://alphacephei.com/vosk/models/vosk-model-small-ja-0.22.zip"

        const val MODEL_DIR_NAME = "vosk-model-small-ja-0.22"

        /** Vosk モデルとして妥当か (conf/model.conf がある)。 */
        fun isModelDir(dir: File): Boolean = File(dir, "conf/model.conf").isFile

        const val NO_MODEL_MESSAGE = "音声モデルがありません。設定でダウンロードしてください"
        const val DOWNLOAD_FAILED_MESSAGE = "音声モデルをダウンロードできませんでした"
        const val EXTRACT_FAILED_MESSAGE = "音声モデルを展開できませんでした"
    }

    /** ダウンロード/展開の状態。 */
    sealed interface State {
        data object NotDownloaded : State

        /** @param total 0 はサイズ不明。 */
        data class Downloading(val received: Long, val total: Long) : State

        data object Extracting : State

        data object Ready : State

        data class Failed(val message: String) : State
    }

    private val filesDir = File(context.filesDir, MODEL_DIR_NAME)
    private val _state = MutableStateFlow<State>(
        if (isModelDir(filesDir)) State.Ready else State.NotDownloaded,
    )
    val state: StateFlow<State> = _state

    private val downloading = AtomicBoolean(false)

    @Volatile private var model: Model? = null

    fun isReady(): Boolean = state.value is State.Ready || isModelDir(filesDir)

    /** モデルを読み込む (2 回目以降はキャッシュ)。失敗なら null。 */
    @Synchronized
    fun model(): Model? {
        model?.let { return it }
        if (!isModelDir(filesDir)) return null
        return try {
            Model(filesDir.absolutePath).also {
                model = it
                _state.value = State.Ready
            }
        } catch (e: Exception) {
            Log.w(TAG, "Model load failed: ${e.message}")
            _state.value = State.Failed(NO_MODEL_MESSAGE)
            null
        }
    }

    /** セッション用の Recognizer を作る (16kHz mono)。準備できていなければ null。 */
    fun newRecognizer(): Recognizer? {
        val m = model() ?: return null
        return try {
            Recognizer(m, 16_000.0f).also { it.setWords(false) }
        } catch (e: Exception) {
            Log.w(TAG, "Recognizer create failed: ${e.message}")
            null
        }
    }

    /** 未ダウンロードならダウンロードして展開する。二重起動しない。 */
    fun download() {
        if (!downloading.compareAndSet(false, true)) return
        if (isReady()) {
            _state.value = State.Ready
            downloading.set(false)
            return
        }
        scope.launch {
            try {
                downloadAndExtract()
            } catch (e: Exception) {
                Log.w(TAG, "download failed: ${e.message}")
                _state.value = State.Failed(
                    if (e is ModelDownloadException) e.japanese else DOWNLOAD_FAILED_MESSAGE,
                )
            } finally {
                downloading.set(false)
            }
        }
    }

    private class ModelDownloadException(val japanese: String) : Exception(japanese)

    private suspend fun downloadAndExtract() = withContext(Dispatchers.IO) {
        val zip = File(context.cacheDir, "vosk-model.zip")
        if (zip.exists()) zip.delete()
        _state.value = State.Downloading(0, 0)

        // 1) ダウンロード
        val req = Request.Builder().url(MODEL_URL).get().build()
        try {
            client.newCall(req).execute().use { res ->
                if (!res.isSuccessful) {
                    throw ModelDownloadException("$DOWNLOAD_FAILED_MESSAGE (HTTP ${res.code})")
                }
                val body = res.body ?: throw ModelDownloadException(DOWNLOAD_FAILED_MESSAGE)
                val total = body.contentLength()
                var received = 0L
                body.byteStream().use { input ->
                    FileOutputStream(zip).use { out ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n <= 0) break
                            out.write(buf, 0, n)
                            received += n
                            _state.value = State.Downloading(received, total)
                        }
                    }
                }
            }
        } catch (e: ModelDownloadException) {
            throw e
        } catch (e: Exception) {
            throw ModelDownloadException(DOWNLOAD_FAILED_MESSAGE)
        }

        // 2) 展開 (zip slip 対策: 親ディレクトリの外へは書かない)
        _state.value = State.Extracting
        val base = context.filesDir.canonicalFile
        try {
            ZipInputStream(zip.inputStream().buffered()).use { zin ->
                while (true) {
                    val entry = zin.nextEntry ?: break
                    val target = File(base, entry.name).canonicalFile
                    if (!target.path.startsWith(base.path + File.separator)) {
                        throw ModelDownloadException(EXTRACT_FAILED_MESSAGE)
                    }
                    if (entry.isDirectory) {
                        target.mkdirs()
                    } else {
                        target.parentFile?.mkdirs()
                        FileOutputStream(target).use { out -> zin.copyTo(out) }
                    }
                    zin.closeEntry()
                }
            }
        } catch (e: ModelDownloadException) {
            throw e
        } catch (e: Exception) {
            Log.w(TAG, "unzip failed: ${e.message}")
            throw ModelDownloadException(EXTRACT_FAILED_MESSAGE)
        } finally {
            zip.delete()
        }

        if (!isModelDir(File(base, MODEL_DIR_NAME))) {
            throw ModelDownloadException(EXTRACT_FAILED_MESSAGE)
        }
        _state.value = State.Ready
        // 次の「話しかける」で待たされないようバックグラウンドでモデルを読み込む。
        scope.launch(Dispatchers.IO) { model() }
        Log.i(TAG, "Vosk model ready: ${File(base, MODEL_DIR_NAME).absolutePath}")
    }
}
