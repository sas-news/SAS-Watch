package dev.sasnews.amoledwatch.ui.screens

import android.Manifest
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import dev.sasnews.amoledwatch.R
import dev.sasnews.amoledwatch.WatchApp
import dev.sasnews.amoledwatch.formatWalkClock
import dev.sasnews.amoledwatch.formatWalkDistance
import dev.sasnews.amoledwatch.formatWalkElapsed
import dev.sasnews.amoledwatch.formatWalkPace
import dev.sasnews.amoledwatch.walk.CheckpointOnRoute
import dev.sasnews.amoledwatch.walk.GeoPoint
import dev.sasnews.amoledwatch.walk.WalkSnapshot
import dev.sasnews.amoledwatch.walk.WalkStatus
import kotlin.math.cos
import kotlin.math.min

/** ルート一覧の表示用 (点列と CP はスケッチに使う)。 */
data class WalkRouteUi(
    val id: String,
    val name: String,
    val totalM: Double,
    val points: List<GeoPoint>,
    val checkpoints: List<CheckpointOnRoute>,
    val selected: Boolean,
)

/** WalkContent が描画に使う全状態（スクショテストしやすいよう分離）。 */
data class WalkUiState(
    val hasPermission: Boolean,
    val routes: List<WalkRouteUi>,
    val snapshot: WalkSnapshot,
    val current: GeoPoint?,
    val busy: Boolean,
    val error: String?,
)

private val MIN_TAP = 48.dp

/**
 * 歩く大会モードの画面（DevicesScreen の「ウォーク」カードから全画面で開く）。
 * ボトムタブは増やさない (docs/walk-event.md §5.3)。
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun WalkScreen(
    onBack: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val context = LocalContext.current
    val app = context.applicationContext as WatchApp
    val walk = app.walk

    val routes by walk.routes.collectAsState()
    val selectedId by walk.selectedRouteId.collectAsState()
    val snapshot by walk.snapshot.collectAsState()
    val current by walk.currentPoint.collectAsState()
    val busy by walk.busy.collectAsState()
    val error by walk.error.collectAsState()

    var hasPermission by remember { mutableStateOf(walk.hasLocationPermission()) }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { hasPermission = walk.hasLocationPermission() }
    // 設定アプリで許可して戻ってきた場合にも追従し、歩行中なら測位を再開する (M2/M3)。
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        hasPermission = walk.hasLocationPermission()
        walk.onScreenResumed()
    }
    // 画面から離れたら省電力測位へ (時計の問い合わせが無ければ GPS を止める)。
    LifecycleEventEffect(Lifecycle.Event.ON_PAUSE) {
        walk.onScreenPaused()
    }
    DisposableEffect(Unit) {
        onDispose { walk.onScreenPaused() }
    }

    val picker = rememberLauncherForActivityResult(
        ActivityResultContracts.OpenDocument(),
    ) { uri ->
        if (uri != null) walk.importRouteFile(uri)
    }

    // 歩行中に画面を作り直したとき、Foreground Service が落ちていれば起こす。
    LaunchedEffect(Unit) { walk.ensureService() }

    BackHandler(onBack = onBack)

    Scaffold(
        modifier = modifier,
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.walk_title)) },
                navigationIcon = {
                    TextButton(onClick = onBack) {
                        Text(stringResource(R.string.walk_back))
                    }
                },
            )
        },
    ) { padding ->
        WalkContent(
            state = WalkUiState(
                hasPermission = hasPermission,
                routes = routes.map { stored ->
                    WalkRouteUi(
                        id = stored.id,
                        name = stored.name,
                        totalM = stored.totalM,
                        points = stored.route.points,
                        checkpoints = stored.route.checkpointsOnRoute,
                        selected = stored.id == selectedId,
                    )
                },
                snapshot = snapshot,
                current = current,
                busy = busy,
                error = error,
            ),
            onRequestPermission = {
                permissionLauncher.launch(
                    arrayOf(
                        Manifest.permission.ACCESS_FINE_LOCATION,
                        Manifest.permission.ACCESS_COARSE_LOCATION,
                    ),
                )
            },
            onImport = {
                picker.launch(
                    arrayOf(
                        "application/gpx+xml",
                        "application/vnd.google-earth.kml+xml",
                        "application/geo+json",
                        "application/json",
                        "text/xml",
                        "*/*",
                    ),
                )
            },
            onSample = walk::importSample,
            onSelectRoute = walk::selectRoute,
            onDeleteRoute = walk::deleteRoute,
            onStart = walk::startWalk,
            onFinish = walk::finishWalk,
            onRetryTracking = walk::retryTracking,
            onDismissError = walk::clearError,
            modifier = Modifier.padding(padding),
        )
    }
}

/** 画面本体（状態を受け取るだけ。スクショテストはこれを直接描く）。 */
@Composable
fun WalkContent(
    state: WalkUiState,
    onRequestPermission: () -> Unit,
    onImport: () -> Unit,
    onSample: () -> Unit,
    onSelectRoute: (String) -> Unit,
    onDeleteRoute: (String) -> Unit,
    onStart: () -> Unit,
    onFinish: () -> Unit,
    onRetryTracking: () -> Unit,
    onDismissError: () -> Unit,
    modifier: Modifier = Modifier,
) {
    var confirmFinish by remember { mutableStateOf(false) }
    var deleteTarget by remember { mutableStateOf<WalkRouteUi?>(null) }
    val selected = state.routes.firstOrNull { it.selected }
    val snapshot = state.snapshot
    val walking = snapshot.state == WalkStatus.WALKING || snapshot.state == WalkStatus.NO_FIX

    Column(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (!state.hasPermission) {
            PermissionCard(onRequestPermission)
        }

        RouteCard(
            state = state,
            onImport = onImport,
            onSample = onSample,
            onSelectRoute = onSelectRoute,
            onDeleteRequest = { deleteTarget = it },
        )

        if (selected != null && selected.points.size >= 2) {
            RouteSketchCard(selected, state.current)
        }

        StatusCard(state)

        if (walking) {
            if (snapshot.state == WalkStatus.NO_FIX) {
                // GPS OFF・権限喪失などで測位が止まったときの復帰手段 (M2)。
                OutlinedButton(
                    onClick = onRetryTracking,
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = MIN_TAP),
                ) {
                    Text(stringResource(R.string.walk_retry))
                }
            }
            Button(
                onClick = { confirmFinish = true },
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = MIN_TAP),
            ) {
                Text(stringResource(R.string.walk_stop))
            }
        } else {
            Button(
                onClick = onStart,
                enabled = state.hasPermission && selected != null && !state.busy,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = MIN_TAP),
            ) {
                Text(stringResource(R.string.walk_start))
            }
            if (!state.hasPermission) {
                Text(
                    stringResource(R.string.walk_permission_needed),
                    style = MaterialTheme.typography.bodySmall,
                )
            } else if (selected == null) {
                Text(
                    stringResource(R.string.walk_not_selected),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }

        if (state.error != null) {
            Text(state.error, color = MaterialTheme.colorScheme.error)
            TextButton(onClick = onDismissError) {
                Text(stringResource(R.string.walk_close))
            }
        }
    }

    val target = deleteTarget
    if (target != null) {
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text(stringResource(R.string.walk_delete_confirm_title)) },
            text = { Text(stringResource(R.string.walk_delete_confirm_body)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        deleteTarget = null
                        onDeleteRoute(target.id)
                    },
                ) {
                    Text(stringResource(R.string.walk_delete_confirm_yes))
                }
            },
            dismissButton = {
                TextButton(onClick = { deleteTarget = null }) {
                    Text(stringResource(R.string.walk_cancel))
                }
            },
        )
    }

    if (confirmFinish) {
        AlertDialog(
            onDismissRequest = { confirmFinish = false },
            title = { Text(stringResource(R.string.walk_stop_confirm_title)) },
            text = { Text(stringResource(R.string.walk_stop_confirm_body)) },
            confirmButton = {
                TextButton(
                    onClick = {
                        confirmFinish = false
                        onFinish()
                    },
                ) {
                    Text(stringResource(R.string.walk_stop_confirm_yes))
                }
            },
            dismissButton = {
                TextButton(onClick = { confirmFinish = false }) {
                    Text(stringResource(R.string.walk_cancel))
                }
            },
        )
    }
}

@Composable
private fun PermissionCard(onRequestPermission: () -> Unit) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(
                stringResource(R.string.walk_perm_title),
                style = MaterialTheme.typography.titleMedium,
            )
            Text(stringResource(R.string.walk_perm_body))
            Button(
                onClick = onRequestPermission,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = MIN_TAP),
            ) {
                Text(stringResource(R.string.walk_perm_grant))
            }
        }
    }
}

@Composable
private fun RouteCard(
    state: WalkUiState,
    onImport: () -> Unit,
    onSample: () -> Unit,
    onSelectRoute: (String) -> Unit,
    onDeleteRequest: (WalkRouteUi) -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.walk_route_title), style = MaterialTheme.typography.titleMedium)
            OutlinedButton(
                onClick = onImport,
                enabled = !state.busy,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = MIN_TAP),
            ) {
                Text(stringResource(R.string.walk_import))
            }
            OutlinedButton(
                onClick = onSample,
                enabled = !state.busy,
                modifier = Modifier
                    .fillMaxWidth()
                    .heightIn(min = MIN_TAP),
            ) {
                Text(stringResource(R.string.walk_sample))
            }
            Text(
                stringResource(R.string.walk_import_hint),
                style = MaterialTheme.typography.bodySmall,
            )
            if (state.busy) {
                Text(stringResource(R.string.walk_importing), style = MaterialTheme.typography.bodySmall)
            }
            if (state.routes.isEmpty()) {
                Text(
                    stringResource(R.string.walk_no_routes),
                    style = MaterialTheme.typography.bodySmall,
                )
            }
            state.routes.forEach { route ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column(Modifier.weight(1f)) {
                        Text(route.name, style = MaterialTheme.typography.titleSmall)
                        Text(
                            stringResource(
                                R.string.walk_route_total,
                                formatWalkDistance(route.totalM.toLong()),
                            ),
                            style = MaterialTheme.typography.bodySmall,
                        )
                    }
                    if (route.selected) {
                        Spacer(Modifier.width(8.dp))
                        Text(
                            stringResource(R.string.walk_selected),
                            color = MaterialTheme.colorScheme.primary,
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    } else {
                        OutlinedButton(
                            onClick = { onSelectRoute(route.id) },
                            enabled = !state.busy,
                            modifier = Modifier.heightIn(min = MIN_TAP),
                        ) {
                            Text(stringResource(R.string.walk_select))
                        }
                    }
                    Spacer(Modifier.width(4.dp))
                    TextButton(
                        onClick = { onDeleteRequest(route) },
                        enabled = !state.busy,
                        modifier = Modifier.heightIn(min = MIN_TAP),
                    ) {
                        Text(stringResource(R.string.walk_delete))
                    }
                }
            }
        }
    }
}

@Composable
private fun RouteSketchCard(route: WalkRouteUi, current: GeoPoint?) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Text(stringResource(R.string.walk_sketch_title), style = MaterialTheme.typography.titleMedium)
            RouteSketch(route, current, Modifier.fillMaxWidth().height(220.dp))
            Text(
                stringResource(R.string.walk_sketch_legend),
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

/**
 * ルートの簡易スケッチ。地図タイルは使わず、緯度経度を局所平面に直して
 * 始点・終点・チェックポイント・現在地を描く (docs/walk-event.md §8)。
 */
@Composable
private fun RouteSketch(route: WalkRouteUi, current: GeoPoint?, modifier: Modifier = Modifier) {
    val lineColor = MaterialTheme.colorScheme.primary
    val dotColor = MaterialTheme.colorScheme.secondary
    val background = MaterialTheme.colorScheme.surfaceVariant
    val startColor = Color(0xFF81C784)
    val endColor = Color(0xFFE57373)
    val cpColor = Color(0xFFFFB74D)

    Canvas(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .background(background),
    ) {
        val points = route.points
        if (points.size < 2) return@Canvas
        val origin = points.first()
        val cosLat = cos(Math.toRadians(origin.lat))
        fun plane(p: GeoPoint) = Offset(
            ((p.lon - origin.lon) * cosLat).toFloat(),
            (p.lat - origin.lat).toFloat(),
        )

        val planePoints = points.map(::plane)
        val extra = current?.let(::plane)
        var minX = planePoints.minOf { it.x }
        var maxX = planePoints.maxOf { it.x }
        var minY = planePoints.minOf { it.y }
        var maxY = planePoints.maxOf { it.y }
        if (extra != null) {
            minX = minOf(minX, extra.x)
            maxX = maxOf(maxX, extra.x)
            minY = minOf(minY, extra.y)
            maxY = maxOf(maxY, extra.y)
        }
        val spanX = (maxX - minX).coerceAtLeast(1e-9f)
        val spanY = (maxY - minY).coerceAtLeast(1e-9f)
        val pad = 16.dp.toPx()
        val scale = min((size.width - 2 * pad) / spanX, (size.height - 2 * pad) / spanY)
        val offsetX = (size.width - spanX * scale) / 2f
        val offsetY = (size.height - spanY * scale) / 2f
        fun screen(p: GeoPoint): Offset {
            val q = plane(p)
            return Offset(
                offsetX + (q.x - minX) * scale,
                size.height - (offsetY + (q.y - minY) * scale),
            )
        }

        val path = Path()
        planePoints.forEachIndexed { i, _ ->
            val o = screen(points[i])
            if (i == 0) path.moveTo(o.x, o.y) else path.lineTo(o.x, o.y)
        }
        drawPath(
            path = path,
            color = lineColor,
            style = Stroke(
                width = 3.dp.toPx(),
                cap = StrokeCap.Round,
                join = StrokeJoin.Round,
            ),
        )

        route.checkpoints.forEach { cp ->
            drawCircle(cpColor, radius = 4.dp.toPx(), center = screen(cp.point))
        }
        drawCircle(startColor, radius = 6.dp.toPx(), center = screen(points.first()))
        drawCircle(endColor, radius = 6.dp.toPx(), center = screen(points.last()))
        current?.let {
            drawCircle(Color.White, radius = 8.dp.toPx(), center = screen(it))
            drawCircle(dotColor, radius = 5.5.dp.toPx(), center = screen(it))
        }
    }
}

@Composable
private fun StatusCard(state: WalkUiState) {
    val snapshot = state.snapshot
    val warning = snapshot.offRouteM > WalkSnapshot.OFF_ROUTE_WARN_M &&
        (snapshot.state == WalkStatus.WALKING || snapshot.state == WalkStatus.NO_FIX)
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(Modifier.padding(16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Column(Modifier.weight(1f)) {
                    Text(
                        stringResource(R.string.walk_status_title),
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Text(
                        stateLabel(snapshot),
                        style = MaterialTheme.typography.bodyMedium,
                    )
                }
                Text(
                    stringResource(R.string.walk_progress, snapshot.permille / 10),
                    style = MaterialTheme.typography.headlineMedium,
                    fontWeight = FontWeight.Bold,
                )
            }

            LinearProgressIndicator(
                progress = { (snapshot.permille / 1000f).coerceIn(0f, 1f) },
                modifier = Modifier.fillMaxWidth(),
            )

            if (snapshot.route.isNotEmpty()) {
                Text(
                    stringResource(
                        R.string.walk_progress_distance,
                        formatWalkDistance(snapshot.doneM),
                        formatWalkDistance(snapshot.totalM),
                    ),
                )
            }

            if (snapshot.state != WalkStatus.NO_ROUTE) {
                Row(modifier = Modifier.fillMaxWidth()) {
                    LabeledValue(
                        stringResource(R.string.walk_remain),
                        formatWalkDistance(snapshot.remainM),
                        Modifier.weight(1f),
                    )
                    LabeledValue(
                        stringResource(R.string.walk_eta),
                        formatWalkClock(snapshot.etaEpoch),
                        Modifier.weight(1f),
                    )
                }
                Row(modifier = Modifier.fillMaxWidth()) {
                    LabeledValue(
                        stringResource(R.string.walk_pace),
                        formatWalkPace(snapshot.paceSPerKm),
                        Modifier.weight(1f),
                    )
                    LabeledValue(
                        stringResource(R.string.walk_elapsed),
                        formatWalkElapsed(snapshot.elapsedS),
                        Modifier.weight(1f),
                    )
                }
                if (snapshot.state == WalkStatus.WALKING || snapshot.state == WalkStatus.NO_FIX) {
                    Text(
                        stringResource(
                            R.string.walk_accuracy,
                            snapshot.accuracyM,
                        ),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                if (snapshot.nextCp.isNotEmpty()) {
                    Text(
                        stringResource(
                            R.string.walk_next_cp,
                            snapshot.nextCp,
                            formatWalkDistance(snapshot.nextCpM),
                        ),
                        style = MaterialTheme.typography.bodySmall,
                    )
                }
                Text(
                    stringResource(R.string.walk_updated, formatWalkClock(snapshot.updatedEpoch)),
                    style = MaterialTheme.typography.bodySmall,
                )
                Text(
                    stringResource(R.string.walk_power_note),
                    style = MaterialTheme.typography.bodySmall,
                )
            }

            if (warning) {
                Text(
                    stringResource(R.string.walk_off_route, snapshot.offRouteM),
                    color = MaterialTheme.colorScheme.error,
                    style = MaterialTheme.typography.bodyMedium,
                )
            }
            if (snapshot.msg.isNotEmpty() &&
                snapshot.state != WalkStatus.WALKING &&
                snapshot.state != WalkStatus.FINISHED
            ) {
                Text(snapshot.msg, style = MaterialTheme.typography.bodySmall)
            }
            if (snapshot.state == WalkStatus.FINISHED) {
                Text(stringResource(R.string.walk_state_finished), style = MaterialTheme.typography.bodyMedium)
            }
        }
    }
}

@Composable
private fun LabeledValue(label: String, value: String, modifier: Modifier = Modifier) {
    Row(modifier = modifier, verticalAlignment = Alignment.CenterVertically) {
        Text(label, style = MaterialTheme.typography.bodySmall)
        Spacer(Modifier.width(6.dp))
        Text(value, style = MaterialTheme.typography.bodyMedium)
    }
}

@Composable
private fun stateLabel(snapshot: WalkSnapshot): String = when (snapshot.state) {
    WalkStatus.IDLE -> stringResource(R.string.walk_state_idle)
    WalkStatus.WALKING -> stringResource(R.string.walk_state_walking)
    WalkStatus.FINISHED -> stringResource(R.string.walk_state_finished)
    WalkStatus.NO_ROUTE -> stringResource(R.string.walk_state_no_route)
    WalkStatus.NO_FIX -> stringResource(R.string.walk_state_no_fix)
}
