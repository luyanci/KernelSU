package me.weishu.kernelsu.ui.kernelFlash

import android.content.Context
import android.net.Uri
import android.os.Environment
import androidx.activity.ComponentActivity
import androidx.activity.compose.BackHandler
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.rounded.Refresh
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.key
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.component.KeyEventBlocker
import me.weishu.kernelsu.ui.util.reboot
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.ui.kernelFlash.state.*
import top.yukonga.miuix.kmp.basic.Card
import top.yukonga.miuix.kmp.basic.FloatingActionButton
import top.yukonga.miuix.kmp.basic.Icon
import top.yukonga.miuix.kmp.basic.IconButton
import top.yukonga.miuix.kmp.basic.LinearProgressIndicator
import top.yukonga.miuix.kmp.basic.Scaffold
import top.yukonga.miuix.kmp.basic.SmallTopAppBar
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.icon.MiuixIcons
import top.yukonga.miuix.kmp.icon.icons.useful.Back
import top.yukonga.miuix.kmp.icon.icons.useful.Save
import top.yukonga.miuix.kmp.theme.MiuixTheme.colorScheme
import top.yukonga.miuix.kmp.utils.scrollEndHaptic
import java.io.File
import java.text.SimpleDateFormat
import java.util.*

/**
 * @author ShirkNeko
 * @date 2025/5/31.
 */
private object KernelFlashStateHolder {
    var currentState: HorizonKernelState? = null
    var currentUri: Uri? = null
    var currentSlot: String? = null
    var isFlashing = false
}

/**
 * Kernel刷写界面
 */
@Destination<RootGraph>
@Composable
fun KernelFlashScreen(
    navigator: DestinationsNavigator,
    kernelUri: Uri,
    selectedSlot: String? = null,
) {
    val context = LocalContext.current

    val shouldAutoExit = remember {
        val sharedPref = context.getSharedPreferences("kernel_flash_prefs", Context.MODE_PRIVATE)
        sharedPref.getBoolean("auto_exit_after_flash", false)
    }

    val scrollState = rememberScrollState()
    val scope = rememberCoroutineScope()
    var logText by rememberSaveable { mutableStateOf("") }
    var showFloatAction by rememberSaveable { mutableStateOf(false) }
    val logContent = rememberSaveable { StringBuilder() }
    val horizonKernelState = remember {
        if (KernelFlashStateHolder.currentState != null &&
            KernelFlashStateHolder.currentUri == kernelUri &&
            KernelFlashStateHolder.currentSlot == selectedSlot) {
            KernelFlashStateHolder.currentState!!
        } else {
            HorizonKernelState().also {
                KernelFlashStateHolder.currentState = it
                KernelFlashStateHolder.currentUri = kernelUri
                KernelFlashStateHolder.currentSlot = selectedSlot
                KernelFlashStateHolder.isFlashing = false
            }
        }
    }

    val flashState by horizonKernelState.state.collectAsState()

    val onFlashComplete = {
        showFloatAction = true
        KernelFlashStateHolder.isFlashing = false

        // 如果需要自动退出，延迟1.5秒后退出
        if (shouldAutoExit) {
            scope.launch {
                delay(1500)
                val sharedPref = context.getSharedPreferences("kernel_flash_prefs", Context.MODE_PRIVATE)
                sharedPref.edit { remove("auto_exit_after_flash") }
                (context as? ComponentActivity)?.finish()
            }
        }
    }

    // 开始刷写
    LaunchedEffect(Unit) {
        if (!KernelFlashStateHolder.isFlashing && !flashState.isCompleted && flashState.error.isEmpty()) {
            withContext(Dispatchers.IO) {
                KernelFlashStateHolder.isFlashing = true
                val worker = HorizonKernelWorker(
                    context = context,
                    state = horizonKernelState,
                    slot = selectedSlot
                )
                worker.uri = kernelUri
                worker.setOnFlashCompleteListener(onFlashComplete)
                worker.start()

                // 监听日志更新
                while (flashState.error.isEmpty()) {
                    if (flashState.logs.isNotEmpty()) {
                        logText = flashState.logs.joinToString("\n")
                        logContent.clear()
                        logContent.append(logText)
                    }
                    delay(100)
                }

                if (flashState.error.isNotEmpty()) {
                    logText += "\n${flashState.error}\n"
                    logContent.append("\n${flashState.error}\n")
                    KernelFlashStateHolder.isFlashing = false
                }
            }
        } else {
            logText = flashState.logs.joinToString("\n")
            if (flashState.error.isNotEmpty()) {
                logText += "\n${flashState.error}\n"
            } else if (flashState.isCompleted) {
                logText += "\n${context.getString(R.string.horizon_flash_complete)}\n\n\n"
                showFloatAction = true
            }
        }
    }

    val onBack: () -> Unit = {
        if (!flashState.isFlashing || flashState.isCompleted || flashState.error.isNotEmpty()) {
            // 清理全局状态
            if (flashState.isCompleted || flashState.error.isNotEmpty()) {
                KernelFlashStateHolder.currentState = null
                KernelFlashStateHolder.currentUri = null
                KernelFlashStateHolder.currentSlot = null
                KernelFlashStateHolder.isFlashing = false
            }
            navigator.popBackStack()
        }
    }

    DisposableEffect(shouldAutoExit) {
        onDispose {
            if (shouldAutoExit) {
                KernelFlashStateHolder.currentState = null
                KernelFlashStateHolder.currentUri = null
                KernelFlashStateHolder.currentSlot = null
                KernelFlashStateHolder.isFlashing = false
            }
        }
    }

    BackHandler {
        onBack()
    }

    KeyEventBlocker {
        it.key == Key.VolumeDown || it.key == Key.VolumeUp
    }

    Scaffold(
        topBar = {
            TopBar(
                flashState = flashState,
                onBack = onBack,
                onSave = {
                    scope.launch {
                        val format = SimpleDateFormat("yyyy-MM-dd-HH-mm-ss", Locale.getDefault())
                        val date = format.format(Date())
                        val file = File(
                            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                            "KernelSU_kernel_flash_log_${date}.log"
                        )
                        file.writeText(logContent.toString())
                    }
                }
            )
        },
        floatingActionButton = {
            if (showFloatAction) {
                FloatingActionButton(
                    onClick = {
                        scope.launch {
                            withContext(Dispatchers.IO) {
                                reboot()
                            }
                        }
                    },
                    modifier = Modifier.padding(bottom = 20.dp, end = 20.dp)
                ) {
                    Icon(
                        Icons.Rounded.Refresh,
                        contentDescription = stringResource(id = R.string.reboot)
                    )
                }
            }
        },
        popupHost = { }
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(it)
                .scrollEndHaptic(),
        ) {
            FlashProgressIndicator(flashState)
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .weight(1f)
                    .verticalScroll(scrollState)
            ) {
                LaunchedEffect(logText) {
                    scrollState.animateScrollTo(scrollState.maxValue)
                }
                Text(
                    modifier = Modifier.padding(16.dp),
                    text = logText,
                    fontFamily = FontFamily.Monospace,
                    color = colorScheme.onSurface
                )
            }
        }
    }
}

@Composable
private fun FlashProgressIndicator(
    flashState: FlashState,
) {
    val progressColor = when {
        flashState.error.isNotEmpty() -> colorScheme.primary
        flashState.isCompleted -> colorScheme.secondary
        else -> colorScheme.primary
    }

    val progress = animateFloatAsState(
        targetValue = flashState.progress,
        label = "FlashProgress"
    )

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 12.dp)
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    text = when {
                        flashState.error.isNotEmpty() -> stringResource(R.string.flash_failed)
                        flashState.isCompleted -> stringResource(R.string.flash_success)
                        else -> stringResource(R.string.flashing)
                    },
                    fontWeight = FontWeight.Bold,
                    color = progressColor
                )

                when {
                    flashState.error.isNotEmpty() -> {
                        Icon(
                            imageVector = Icons.Default.Error,
                            contentDescription = null,
                            tint = colorScheme.primary
                        )
                    }
                    flashState.isCompleted -> {
                        Icon(
                            imageVector = Icons.Default.CheckCircle,
                            contentDescription = null,
                            tint = colorScheme.secondary
                        )
                    }
                }
            }

            if (flashState.currentStep.isNotEmpty()) {
                Text(
                    text = flashState.currentStep,
                    color = colorScheme.onSurfaceVariantSummary
                )

                Spacer(modifier = Modifier.height(8.dp))
            }

            LinearProgressIndicator(
                progress = progress.value,
                modifier = Modifier
                    .fillMaxWidth()
                    .height(8.dp)
            )

            if (flashState.error.isNotEmpty()) {
                Spacer(modifier = Modifier.height(8.dp))

                Row(
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Icon(
                        imageVector = Icons.Default.Error,
                        contentDescription = null,
                        tint = colorScheme.primary,
                        modifier = Modifier.size(16.dp)
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))

                Text(
                    text = flashState.error,
                    color = colorScheme.primary,
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(
                            colorScheme.primaryContainer.copy(alpha = 0.3f)
                        )
                        .padding(8.dp)
                )
            }
        }
    }
}

@Composable
private fun TopBar(
    flashState: FlashState,
    onBack: () -> Unit,
    onSave: () -> Unit = {}
) {
    SmallTopAppBar(
        title = stringResource(
            when {
                flashState.error.isNotEmpty() -> R.string.flash_failed
                flashState.isCompleted -> R.string.flash_success
                else -> R.string.kernel_flashing
            }
        ),
        navigationIcon = {
            IconButton(
                modifier = Modifier.padding(start = 16.dp),
                onClick = onBack
            ) {
                Icon(
                    MiuixIcons.Useful.Back,
                    contentDescription = null,
                    tint = colorScheme.onBackground
                )
            }
        },
        actions = {
            IconButton(
                modifier = Modifier.padding(end = 16.dp),
                onClick = onSave
            ) {
                Icon(
                    imageVector = MiuixIcons.Useful.Save,
                    contentDescription = stringResource(id = R.string.save_log),
                    tint = colorScheme.onBackground
                )
            }
        }
    )
}
