package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.agent.AgentBridge
import dev.sasnews.amoledwatch.agent.MuseApproval
import dev.sasnews.amoledwatch.agent.MuseChatSummary
import dev.sasnews.amoledwatch.ui.WatchViewModel

/**
 * Muse チャット画面 (Devices タブのカードから全画面で開く)。
 *
 * - 会話一覧 (GET /chats) から選択、または「新しい会話」で作成
 * - 日本語 IME でテキスト送信
 * - SSE のストリーミング受信中表示
 * - 承認要求 (approval) は「スマホの Muse アプリで承認」に誘導 (v1 は時計から承認しない)
 * - 時計からの dictation 経由の往復も同じ履歴に出す
 */
@Composable
fun MuseChatScreen(vm: WatchViewModel, onBack: () -> Unit, modifier: Modifier = Modifier) {
    val config by vm.agentConfig.collectAsState()
    val chats by vm.museChats.collectAsState()
    val selectedId by vm.museChatId.collectAsState()
    val messages by vm.museMessages.collectAsState()
    val loading by vm.museLoading.collectAsState()
    val busy by vm.museBusy.collectAsState()
    val approval by vm.museApproval.collectAsState()
    val notice by vm.museNotice.collectAsState()

    LaunchedEffect(Unit) { vm.refreshMuseChats() }

    Column(
        modifier = modifier
            .fillMaxSize()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(onClick = onBack) { Text(stringResource(R.string.muse_back)) }
            Text(
                stringResource(R.string.muse_title),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.weight(1f),
            )
            OutlinedButton(onClick = vm::newMuseChat) {
                Text(stringResource(R.string.muse_new_chat))
            }
        }

        if (!config.museUsable()) {
            Card(Modifier.fillMaxWidth()) {
                Text(
                    stringResource(R.string.muse_no_token),
                    modifier = Modifier.padding(16.dp),
                )
            }
            return@Column
        }

        if (notice != null) {
            Card(
                Modifier.fillMaxWidth(),
                colors = CardDefaults.cardColors(
                    containerColor = MaterialTheme.colorScheme.errorContainer,
                ),
            ) {
                Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    Text(notice!!)
                    TextButton(onClick = vm::consumeMuseNotice) {
                        Text(stringResource(R.string.muse_approval_close))
                    }
                }
            }
        }

        if (approval != null) {
            ApprovalCard(approval!!, onDismiss = vm::dismissMuseApproval)
        }

        ConversationListCard(
            chats = chats,
            selectedId = selectedId,
            loading = loading,
            onRefresh = vm::refreshMuseChats,
            onSelect = vm::openMuseChat,
        )

        if (messages.isEmpty()) {
            Text(
                stringResource(R.string.muse_empty_messages),
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier.fillMaxWidth(),
            )
        } else {
            LazyColumn(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                items(messages) { msg -> MessageRow(msg) }
            }
        }

        if (busy) {
            Text(
                stringResource(R.string.muse_streaming),
                style = MaterialTheme.typography.bodySmall,
            )
        }

        Composer(enabled = !busy) { text -> vm.sendMuseText(text) }

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedButton(
                onClick = vm::clearMuseHistory,
                modifier = Modifier.weight(1f),
            ) {
                Text(stringResource(R.string.muse_clear_history))
            }
        }
    }
}

@Composable
private fun ApprovalCard(approval: MuseApproval, onDismiss: () -> Unit) {
    Card(
        Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.secondaryContainer,
        ),
    ) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(
                stringResource(R.string.muse_approval_title),
                style = MaterialTheme.typography.titleSmall,
            )
            Text(approval.question ?: approval.title ?: "")
            Text(
                stringResource(R.string.muse_approval_body),
                style = MaterialTheme.typography.bodySmall,
            )
            if (approval.options.isNotEmpty()) {
                Text(
                    approval.options.joinToString(" / ") { it.second },
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            TextButton(onClick = onDismiss) {
                Text(stringResource(R.string.muse_approval_close))
            }
        }
    }
}

@Composable
private fun ConversationListCard(
    chats: List<MuseChatSummary>,
    selectedId: String,
    loading: Boolean,
    onRefresh: () -> Unit,
    onSelect: (String) -> Unit,
) {
    Card(Modifier.fillMaxWidth()) {
        Column(Modifier.padding(12.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    stringResource(R.string.muse_chats_title),
                    style = MaterialTheme.typography.titleSmall,
                    modifier = Modifier.weight(1f),
                )
                TextButton(onClick = onRefresh) {
                    Text(stringResource(R.string.muse_refresh_chats))
                }
            }
            if (loading) {
                Text(
                    stringResource(R.string.muse_loading),
                    style = MaterialTheme.typography.bodySmall,
                )
            } else if (chats.isEmpty()) {
                Text(
                    stringResource(R.string.muse_chats_empty),
                    style = MaterialTheme.typography.bodySmall,
                )
            } else {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(max = 160.dp)
                        .verticalScroll(rememberScrollState()),
                ) {
                    chats.forEach { c ->
                        TextButton(
                            onClick = { onSelect(c.chatId) },
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Column(Modifier.fillMaxWidth()) {
                                Text(
                                    (c.title ?: c.chatId).take(60) +
                                        if (c.chatId == selectedId) {
                                            " ★" + stringResource(R.string.muse_selected)
                                        } else {
                                            ""
                                        },
                                    style = MaterialTheme.typography.bodyMedium,
                                )
                                val snippet = c.snippet
                                if (!snippet.isNullOrBlank()) {
                                    Text(
                                        snippet.take(60),
                                        style = MaterialTheme.typography.bodySmall,
                                    )
                                }
                            }
                        }
                        HorizontalDivider()
                    }
                }
            }
        }
    }
}

@Composable
private fun MessageRow(msg: AgentBridge.MuseUiMessage) {
    if (msg.role == "system") {
        Text(
            msg.text,
            style = MaterialTheme.typography.bodySmall,
            fontStyle = FontStyle.Italic,
            modifier = Modifier.fillMaxWidth(),
        )
        return
    }
    val isUser = msg.role == "user"
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = if (isUser) Arrangement.End else Arrangement.Start,
    ) {
        Card(
            colors = CardDefaults.cardColors(
                containerColor = if (isUser) {
                    MaterialTheme.colorScheme.primaryContainer
                } else {
                    MaterialTheme.colorScheme.surfaceVariant
                },
            ),
            modifier = Modifier.fillMaxWidth(0.85f),
        ) {
            Column(Modifier.padding(10.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                val label = when {
                    msg.fromWatch -> stringResource(R.string.muse_from_watch)
                    isUser -> "You"
                    else -> stringResource(R.string.muse_role_system)
                }
                Text(
                    label,
                    style = MaterialTheme.typography.labelSmall,
                )
                Text(
                    msg.text + if (msg.streaming) " …" else "",
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
        }
    }
}

@Composable
private fun Composer(enabled: Boolean, onSend: (String) -> Unit) {
    var text by remember { mutableStateOf("") }
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        OutlinedTextField(
            value = text,
            onValueChange = { if (it.length <= 4000) text = it },
            label = { Text(stringResource(R.string.muse_input_hint)) },
            minLines = 1,
            maxLines = 4,
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = {
                if (text.isNotBlank()) {
                    onSend(text)
                    text = ""
                }
            },
            enabled = enabled && text.isNotBlank(),
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(stringResource(R.string.muse_send))
        }
    }
}
