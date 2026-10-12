package dev.sasnews.amoledwatch.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material3.Card
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import dev.sasnews.amoledwatch.InstalledApp
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.media.MediaBridge
import dev.sasnews.amoledwatch.ui.WatchViewModel

@Composable
fun NotifyScreen(
    vm: WatchViewModel,
    onOpenNotifSettings: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val hasAccess by vm.notifAccess.collectAsState()
    val enabled by vm.enabledPackages.collectAsState()
    val nowPlaying by vm.nowPlaying.collectAsState()
    val apps by vm.installedApps.collectAsState()
    // 未取得 or 更新があったときだけ再スキャンが走る (ensureLoaded は軽い)。
    LaunchedEffect(Unit) { vm.ensureInstalledApps() }
    NotifyContent(
        hasAccess = hasAccess,
        enabledPackages = enabled,
        apps = apps,
        nowPlaying = nowPlaying,
        onToggle = vm::togglePackage,
        onOpenNotifSettings = onOpenNotifSettings,
        modifier = modifier,
    )
}

/**
 * 通知転送アプリの絞り込み (純関数)。
 * - アプリ名・パッケージ名の大文字小文字無視の部分一致。
 * - `onlyEnabled` の判定は「現在の enabled」(チェック切替で即反映)。
 * - 並び替えは「画面を開いた時点の enabled スナップショット」(`sortEnabled`) を
 *   基準に ON を上へ。安定ソートなので同じ枠内では名前順が維持され、
 *   チェックを切り替えても行が上下に飛ばない。
 */
fun filterInstalledApps(
    apps: List<InstalledApp>,
    sortEnabled: Set<String>,
    query: String,
    onlyEnabled: Boolean,
    enabledNow: Set<String> = sortEnabled,
): List<InstalledApp> {
    val q = query.trim()
    return apps
        .filter { app ->
            (!onlyEnabled || app.pkg in enabledNow) &&
                (
                    q.isEmpty() ||
                        app.label.contains(q, ignoreCase = true) ||
                        app.pkg.contains(q, ignoreCase = true)
                    )
        }
        .sortedByDescending { it.pkg in sortEnabled }
}

/** 通知転送とメディア連携の設定画面。 */
@Composable
fun NotifyContent(
    hasAccess: Boolean,
    enabledPackages: Set<String>,
    apps: List<InstalledApp>?,
    nowPlaying: MediaBridge.NowPlaying?,
    onToggle: (String, Boolean) -> Unit,
    onOpenNotifSettings: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var query by rememberSaveable { mutableStateOf("") }
    var onlyEnabled by rememberSaveable { mutableStateOf(false) }
    // 並び替えの基準はこの画面を開いた時点のスナップショット。
    val sortEnabled = remember { enabledPackages }
    val filtered = if (apps == null) {
        null
    } else {
        filterInstalledApps(
            apps, sortEnabled, query, onlyEnabled, enabledPackages,
        )
    }

    LazyColumn(
        modifier = modifier,
        contentPadding = PaddingValues(16.dp),
    ) {
        item(key = "access") {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.notif_access_title), style = MaterialTheme.typography.titleMedium)
                    if (hasAccess) {
                        Text(stringResource(R.string.notif_access_granted))
                    } else {
                        Text(stringResource(R.string.notif_access_body))
                        OutlinedButton(onClick = onOpenNotifSettings, modifier = Modifier.fillMaxWidth()) {
                            Text(stringResource(R.string.notif_access_open))
                        }
                    }
                }
            }
        }
        item(key = "access_gap") { Spacer(Modifier.height(12.dp)) }

        item(key = "media") {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.media_title), style = MaterialTheme.typography.titleMedium)
                    Text(stringResource(R.string.media_body), style = MaterialTheme.typography.bodySmall)
                    if (!hasAccess) {
                        Text(stringResource(R.string.media_no_access), style = MaterialTheme.typography.bodySmall)
                    } else if (nowPlaying != null) {
                        Text(
                            stringResource(R.string.media_now_playing, nowPlaying.title, nowPlaying.artist),
                        )
                    } else {
                        Text(stringResource(R.string.media_idle), style = MaterialTheme.typography.bodySmall)
                    }
                }
            }
        }
        item(key = "media_gap") { Spacer(Modifier.height(12.dp)) }

        item(key = "apps") {
            Card(Modifier.fillMaxWidth()) {
                Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(stringResource(R.string.notif_apps_title), style = MaterialTheme.typography.titleMedium)
                    if (apps == null) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            CircularProgressIndicator(Modifier.size(20.dp))
                            Spacer(Modifier.width(8.dp))
                            Text(
                                stringResource(R.string.notif_apps_loading),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                    } else {
                        if (enabledPackages.isEmpty()) {
                            Text(
                                stringResource(R.string.notif_apps_none),
                                style = MaterialTheme.typography.bodySmall,
                            )
                        }
                        OutlinedTextField(
                            value = query,
                            onValueChange = { query = it },
                            label = { Text(stringResource(R.string.notif_search_hint)) },
                            singleLine = true,
                            modifier = Modifier.fillMaxWidth(),
                        )
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            Text(
                                stringResource(R.string.notif_only_enabled),
                                modifier = Modifier.weight(1f),
                            )
                            Switch(
                                checked = onlyEnabled,
                                onCheckedChange = { onlyEnabled = it },
                            )
                        }
                    }
                }
            }
        }

        if (filtered != null) {
            if (filtered.isEmpty()) {
                item(key = "apps_empty") {
                    Text(
                        stringResource(R.string.notif_apps_empty),
                        style = MaterialTheme.typography.bodySmall,
                        modifier = Modifier.padding(start = 16.dp, top = 8.dp),
                    )
                }
            } else {
                items(filtered, key = { it.pkg }) { app ->
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Checkbox(
                            checked = enabledPackages.contains(app.pkg),
                            onCheckedChange = { onToggle(app.pkg, it) },
                        )
                        Column {
                            Text(app.label)
                            Text(app.pkg, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                }
            }
        }
    }
}
