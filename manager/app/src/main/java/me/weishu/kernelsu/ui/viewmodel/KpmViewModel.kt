package me.weishu.kernelsu.ui.viewmodel

import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import me.weishu.kernelsu.ui.util.*

class KpmViewModel : ViewModel() {
    var moduleList by mutableStateOf(emptyList<ModuleInfo>())
        private set


    var search by mutableStateOf("")
        internal set

    var isRefreshing by mutableStateOf(false)
        private set

    var currentModuleDetail by mutableStateOf("")
        private set

    fun loadModuleDetail(moduleId: String) {
        viewModelScope.launch {
            currentModuleDetail = withContext(Dispatchers.IO) {
                try {
                    getKpmModuleInfo(moduleId)
                } catch (e: Exception) {
                    "无法获取模块详细信息: ${e.message}"
                }
            }
            Log.d("KsuCli", "Module detail: $currentModuleDetail")
        }
    }

    fun fetchModuleList() {
        viewModelScope.launch {
            isRefreshing = true
            try {
                val moduleCount = getKpmModuleCount()
                Log.d("KsuCli", "Module count: $moduleCount")

                val moduleInfo = listKpmModules()
                Log.d("KsuCli", "Module info: $moduleInfo")

                val modules = parseModuleList(moduleInfo)
                moduleList = modules
            } finally {
                isRefreshing = false
            }
        }
    }

    private fun getInstalledKernelPatches(): List<ModuleInfo> {
        return try {
            val output = printKpmModules()
            parseModuleList(output)
        } catch (e: Exception) {
            emptyList()
        }
    }

    private fun parseModuleList(output: String): List<ModuleInfo> {
        return output.split("\n").mapNotNull { line ->
            if (line.isBlank()) return@mapNotNull null
            val parts = line.split("|")
            if (parts.size < 7) return@mapNotNull null

            ModuleInfo(
                id = parts[0].trim(),
                name = parts[1].trim(),
                version = parts[2].trim(),
                author = parts[3].trim(),
                description = parts[4].trim(),
                args = parts[6].trim(),
                enabled = true,
                hasAction = controlKpmModule(parts[0].trim()).isNotBlank()
            )
        }
    }


    data class ModuleInfo(
        val id: String,
        val name: String,
        val version: String,
        val author: String,
        val description: String,
        val args: String,
        val enabled: Boolean,
        val hasAction: Boolean
    )
}