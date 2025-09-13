package me.weishu.kernelsu.ui.component

import androidx.compose.runtime.Composable
import me.weishu.kernelsu.Natives
import me.weishu.kernelsu.ksuApp

fun KsuGetVersion(): Int? {
    val isManager = Natives.becomeManager(ksuApp.packageName)
    val ksuVersion Int? = if (isManager) Natives.version else null
    return ksuVersion
}

@Composable
fun KsuIsValid(
    content: @Composable () -> Unit
) {

    if (KsuGetVersion() != null) {
        content()
    }
}
