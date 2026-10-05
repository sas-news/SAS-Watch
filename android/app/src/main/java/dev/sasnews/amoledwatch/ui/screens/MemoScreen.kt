package dev.sasnews.amoledwatch.ui.screens

import android.content.Intent
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.connection.LinkState
import dev.sasnews.amoledwatch.protocol.MemoEntry
import dev.sasnews.amoledwatch.ui.WatchViewModel
import java.io.File

/** メモ画面の1件 (text メモは text に本文が入る)。 */
data class MemoUi(
    val id: Int,
    val kind: String,
    val sec: Int,
    val text: String?,
)

@Composable
fun MemoScreen(
    vm: WatchViewModel,
    modifier: Modifier = Modifier,
) {
    val link by vm.linkState.collectAsState()
    val memos by vm.memos.collectAsState()
    val texts by vm.memoTexts.collectAsState()
    val playingId by vm.playingMemoId.collectAsState()
    val busyId by vm.memoBusyId.collectAsState()
    val context = LocalContext.current

    MemoContent(
        connected = link is LinkState.Connected,
        memos = memos?.map { m ->
            MemoUi(
                id = m.id,
                kind = m.kind,
                sec = m.sec,
                text = texts[m.id],
            )
        },
        playingId = playingId,
        busyId = busyId,
        onRefresh = vm::refreshMemos,
        onPlay = vm::memoPlayToggle,
        onDelete = vm::memoDelete,
        onShare = { id ->
            vm.memoShare(id) { file ->
                val uri = FileProvider.getUriForFile(
                    context, "${context.packageName}.fileprovider", file,
                )
                val intent = Intent(Intent.ACTION_SEND).apply {
                    type = "audio/wav"
                    putExtra(Intent.EXTRA_STREAM, uri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                context.startActivity(
                    Intent.createChooser(intent, context.getString(R.string.memo_share_title)),
                )
            }
        },
        modifier = modifier,
    )
}

/** 時計から同期したメモ (テキスト/音声) の一覧画面。 */
@Composable
fun MemoContent(
    connected: Boolean,
    memos: List<MemoUi>?,
    playingId: Int?,
    busyId: Int?,
    onRefresh: () -> Unit,
    onPlay: (Int) -> Unit,
    onDelete: (Int) -> Unit,
    onShare: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    Column(
        modifier = modifier
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!connected) {
            Text(
                stringResource(R.string.memo_not_connected),
                style = MaterialTheme.typography.bodyMedium,
            )
            return@Column
        }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                stringResource(R.string.memo_list_title),
                style = MaterialTheme.typography.titleMedium,
            )
            OutlinedButton(onClick = onRefresh) {
                Text(stringResource(R.string.memo_refresh))
            }
        }

        if (memos == null || memos.isEmpty()) {
            Text(
                stringResource(R.string.memo_list_empty),
                style = MaterialTheme.typography.bodyMedium,
            )
        }

        memos?.forEach { m ->
            Card(Modifier.fillMaxWidth()) {
                Column(
                    Modifier.padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    if (m.kind == "voice") {
                        Text(
                            stringResource(R.string.memo_kind_voice, m.sec),
                            style = MaterialTheme.typography.titleSmall,
                        )
                        if (busyId == m.id) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                CircularProgressIndicator(
                                    modifier = Modifier.padding(4.dp),
                                    strokeWidth = 2.dp,
                                )
                                Text(
                                    stringResource(R.string.memo_downloading),
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                        }
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            Button(
                                onClick = { onPlay(m.id) },
                                enabled = busyId == null,
                            ) {
                                Text(
                                    stringResource(
                                        if (playingId == m.id) R.string.memo_stop
                                        else R.string.memo_play,
                                    ),
                                )
                            }
                            OutlinedButton(
                                onClick = { onShare(m.id) },
                                enabled = busyId == null,
                            ) {
                                Text(stringResource(R.string.memo_share))
                            }
                            OutlinedButton(onClick = { onDelete(m.id) }) {
                                Text(stringResource(R.string.memo_delete))
                            }
                        }
                    } else {
                        Text(
                            stringResource(R.string.memo_kind_text),
                            style = MaterialTheme.typography.titleSmall,
                        )
                        Text(
                            m.text ?: "…",
                            style = MaterialTheme.typography.bodyMedium,
                        )
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                            OutlinedButton(onClick = { onDelete(m.id) }) {
                                Text(stringResource(R.string.memo_delete))
                            }
                        }
                    }
                }
            }
        }
    }
}
