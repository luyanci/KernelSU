package me.weishu.kernelsu.ui.screen

import android.app.Activity.RESULT_OK
import android.content.Intent
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import com.ramcosta.composedestinations.annotation.Destination
import com.ramcosta.composedestinations.annotation.RootGraph
import com.ramcosta.composedestinations.navigation.DestinationsNavigator
import kotlinx.coroutines.launch
import me.weishu.kernelsu.R
import me.weishu.kernelsu.ui.component.ConfirmResult
import me.weishu.kernelsu.ui.component.SearchAppBar
import me.weishu.kernelsu.ui.component.rememberConfirmDialog
import me.weishu.kernelsu.ui.component.rememberLoadingDialog
import me.weishu.kernelsu.ui.theme.getCardColors
import me.weishu.kernelsu.ui.theme.getCardElevation
import me.weishu.kernelsu.ui.viewmodel.KpmViewModel
import me.weishu.kernelsu.ui.util.loadKpmModule
import me.weishu.kernelsu.ui.util.unloadKpmModule
import java.io.File

@OptIn(ExperimentalMaterial3Api::class)
@Destination<RootGraph>
@Composable
fun KpmScreen(
    navigator: DestinationsNavigator,
    viewModel: KpmViewModel = viewModel()
) {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val snackBarHost = remember { SnackbarHostState() }
    val confirmDialog = rememberConfirmDialog()
    val loadingDialog = rememberLoadingDialog()

    val scrollBehavior = TopAppBarDefaults.pinnedScrollBehavior(rememberTopAppBarState())

    val kpmInstall = stringResource(R.string.kpm_install)
    val kpmInstallConfirm = stringResource(R.string.kpm_install_confirm)
    val kpmInstallSuccess = stringResource(R.string.kpm_install_success)
    val kpmInstallFailed = stringResource(R.string.kpm_install_failed)
    val install = stringResource(R.string.install)
    val cancel = stringResource(R.string.cancel)
    val kpmUninstall = stringResource(R.string.kpm_uninstall)
    val kpmUninstallConfirmTemplate = stringResource(R.string.kpm_uninstall_confirm)
    val uninstall = stringResource(R.string.uninstall)
    val kpmUninstallSuccess = stringResource(R.string.kpm_uninstall_success)
    val kpmUninstallFailed = stringResource(R.string.kpm_uninstall_failed)

    val selectPatchLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != RESULT_OK) return@rememberLauncherForActivityResult

        val uri = result.data?.data ?: return@rememberLauncherForActivityResult

        scope.launch {
            // 复制文件到临时目录
            val tempFile = File(context.cacheDir, "temp_patch.kpm")
            context.contentResolver.openInputStream(uri)?.use { input ->
                tempFile.outputStream().use { output ->
                    input.copyTo(output)
                }
            }

            val confirmResult = confirmDialog.awaitConfirm(
                title = kpmInstall,
                content = kpmInstallConfirm,
                confirm = install,
                dismiss = cancel
            )

            if (confirmResult == ConfirmResult.Confirmed) {
                val success = loadingDialog.withLoading {
                    loadKpmModule(tempFile.absolutePath)
                }

                Log.d("KsuCli", "loadKpmModule result: $success")

                if (success == "success") {
                    viewModel.fetchModuleList()
                    snackBarHost.showSnackbar(
                        message = kpmInstallSuccess,
                        duration = SnackbarDuration.Long
                    )
                } else {
                    // 修正为显示安装失败的消息
                    snackBarHost.showSnackbar(
                        message = kpmInstallFailed,
                        duration = SnackbarDuration.Long
                    )
                }
            }
            tempFile.delete()
        }
    }

    LaunchedEffect(Unit) {
        if (viewModel.moduleList.isEmpty()) {
            viewModel.fetchModuleList()
        }
    }

    Scaffold(
        topBar = {
            SearchAppBar(
                title = { Text(stringResource(R.string.kpm_title)) },
                searchText = viewModel.search,
                onSearchTextChange = { viewModel.search = it },
                onClearClick = { viewModel.search = "" },
                scrollBehavior = scrollBehavior,
                dropdownContent = {
                    IconButton(onClick = { viewModel.fetchModuleList() }) {
                        Icon(
                            imageVector = Icons.Outlined.Refresh,
                            contentDescription = stringResource(R.string.refresh)
                        )
                    }
                }
            )
        },
        floatingActionButton = {
            ExtendedFloatingActionButton(
                onClick = {
                    selectPatchLauncher.launch(
                        Intent(Intent.ACTION_GET_CONTENT).apply {
                            type = "application/*"
                        }
                    )
                },
                icon = {
                    Icon(
                        imageVector = Icons.Outlined.Add,
                        contentDescription = stringResource(R.string.kpm_install)
                    )
                },
                text = { Text(stringResource(R.string.kpm_install)) },
                containerColor = MaterialTheme.colorScheme.secondaryContainer,
                contentColor = MaterialTheme.colorScheme.onSecondaryContainer
            )
        },
        snackbarHost = { SnackbarHost(snackBarHost) }
    ) { padding ->
        PullToRefreshBox(
            onRefresh = { viewModel.fetchModuleList() },
            isRefreshing = viewModel.isRefreshing,
            modifier = Modifier.padding(padding)
        ) {
            if (viewModel.moduleList.isEmpty()) {
                Box(
                    modifier = Modifier.fillMaxSize(),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        stringResource(R.string.kpm_empty),
                        textAlign = TextAlign.Center
                    )
                }
            } else {
                LazyColumn(
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(16.dp),
                    verticalArrangement = Arrangement.spacedBy(16.dp)
                ) {
                    items(viewModel.moduleList) { module ->
                        val kpmUninstallConfirm = String.format(kpmUninstallConfirmTemplate, module.name)
                        KpmModuleItem(
                            module = module,
                            onUninstall = {
                                scope.launch {
                                    val confirmResult = confirmDialog.awaitConfirm(
                                        title = kpmUninstall,
                                        content = kpmUninstallConfirm,
                                        confirm = uninstall,
                                        dismiss = cancel
                                    )
                                    if (confirmResult == ConfirmResult.Confirmed) {
                                        val success = loadingDialog.withLoading {
                                            unloadKpmModule(module.id)
                                        }
                                        Log.d("KsuCli", "unloadKpmModule result: $success")
                                        if (success == "success") {
                                            viewModel.fetchModuleList()
                                            snackBarHost.showSnackbar(
                                                message = kpmUninstallSuccess,
                                                duration = SnackbarDuration.Long
                                            )
                                        } else {
                                            snackBarHost.showSnackbar(
                                                message = kpmUninstallFailed,
                                                duration = SnackbarDuration.Long
                                            )
                                        }
                                    }
                                }
                            },
                            onControl = {
                                viewModel.loadModuleDetail(module.id)
                            }
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun KpmModuleItem(
    module: KpmViewModel.ModuleInfo,
    onUninstall: () -> Unit,
    onControl: () -> Unit
) {
    ElevatedCard(
        colors = getCardColors(MaterialTheme.colorScheme.secondaryContainer),
        elevation = CardDefaults.cardElevation(defaultElevation = getCardElevation())
    ) {
        Column(
            modifier = Modifier.padding(16.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        text = module.name,
                        style = MaterialTheme.typography.titleMedium
                    )
                    Text(
                        text = "${stringResource(R.string.kpm_version)}: ${module.version}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "${stringResource(R.string.kpm_author)}: ${module.author}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                    Text(
                        text = "${stringResource(R.string.kpm_args)}: ${module.args}",
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            }

            Spacer(modifier = Modifier.height(8.dp))

            Text(
                text = module.description,
                style = MaterialTheme.typography.bodyMedium
            )

            Spacer(modifier = Modifier.height(16.dp))

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                FilledTonalButton(
                    onClick = onControl
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Settings,
                        contentDescription = null
                    )
                    Text(stringResource(R.string.kpm_control))
                }

                FilledTonalButton(
                    onClick = onUninstall
                ) {
                    Icon(
                        imageVector = Icons.Outlined.Delete,
                        contentDescription = null
                    )
                    Text(stringResource(R.string.kpm_uninstall))
                }
            }
        }
    }
}