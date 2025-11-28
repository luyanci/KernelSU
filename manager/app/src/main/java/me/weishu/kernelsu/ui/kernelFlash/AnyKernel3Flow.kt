package me.weishu.kernelsu.ui.kernelFlash

import android.net.Uri
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Security
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.MutableState
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.core.net.toUri
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.screen.InstallMethod
import top.yukonga.miuix.kmp.basic.Icon
import top.yukonga.miuix.kmp.basic.Text
import top.yukonga.miuix.kmp.basic.TextButton
import top.yukonga.miuix.kmp.extra.SuperArrow
import top.yukonga.miuix.kmp.extra.SuperDialog
import top.yukonga.miuix.kmp.theme.MiuixTheme
import top.yukonga.miuix.kmp.theme.MiuixTheme.colorScheme



@Stable
data class AnyKernel3State(
    val showSlotSelectionDialog: Boolean,
    val onHorizonKernelSelected: (InstallMethod.HorizonKernel) -> Unit,
    val onSlotSelected: (String) -> Unit,
    val onDismissSlotDialog: () -> Unit,
    val onReopenSlotDialog: (InstallMethod.HorizonKernel) -> Unit,
)

@Composable
fun rememberAnyKernel3State(
    installMethodState: MutableState<InstallMethod?>,
    preselectedKernelUri: String?,
    horizonKernelSummary: String,
    isAbDevice: Boolean
): AnyKernel3State {
    var showSlotSelectionDialog by remember { mutableStateOf(false) }
    var tempKernelUri by remember { mutableStateOf<Uri?>(null) }

    val onHorizonKernelSelected: (InstallMethod.HorizonKernel) -> Unit = { method ->
        val uri = method.uri
        if (uri != null) {
            if (isAbDevice && method.slot == null) {
                tempKernelUri = uri
                showSlotSelectionDialog = true
            } else {
                installMethodState.value = method            }
        }
    }
    
    val onReopenSlotDialog: (InstallMethod.HorizonKernel) -> Unit = { method ->
        val uri = method.uri
        if (uri != null && isAbDevice) {
            tempKernelUri = uri
            showSlotSelectionDialog = true
        }
    }

    val onSlotSelected: (String) -> Unit = { slot ->
        val uri = tempKernelUri ?: (installMethodState.value as? InstallMethod.HorizonKernel)?.uri
        if (uri != null) {
            installMethodState.value = InstallMethod.HorizonKernel(
                uri = uri,
                slot = slot,
                summary = horizonKernelSummary
            )
            tempKernelUri = null
            showSlotSelectionDialog = false
        }
    }

    val onDismissSlotDialog = {
        showSlotSelectionDialog = false
    }


    val onDismissPatchDialog = {
    }

    LaunchedEffect(preselectedKernelUri, isAbDevice, horizonKernelSummary) {
        preselectedKernelUri?.let { uriString ->
            runCatching { uriString.toUri() }
                .getOrNull()
                ?.let { preselectedUri ->
                    val method = InstallMethod.HorizonKernel(
                        uri = preselectedUri,
                        summary = horizonKernelSummary
                    )
                    if (isAbDevice) {
                        tempKernelUri = preselectedUri
                        showSlotSelectionDialog = true
                    } else {
                        installMethodState.value = method
                    }
                }
        }
    }

    return AnyKernel3State(
        showSlotSelectionDialog = showSlotSelectionDialog,
        onHorizonKernelSelected = onHorizonKernelSelected,
        onSlotSelected = onSlotSelected,
        onDismissSlotDialog = onDismissSlotDialog,
        onReopenSlotDialog = onReopenSlotDialog,
    )
}


