package com.wavrec

import androidx.compose.ui.unit.DpSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.*
import com.wavrec.model.AppViewModel
import com.wavrec.ui.WavRecApp
import java.io.File

fun main() {
    /*  Engine executable: check WAVREC_ENGINE_PATH env var first,
     *  then look alongside this jar, then the CMake build output.   */
    val enginePath = System.getenv("WAVREC_ENGINE_PATH")
        ?: System.getProperty("wavrec.engine.path")?.takeIf { it.isNotBlank() && java.io.File(it).exists() }
        ?: findEngineExe()
        ?: error(
            "Cannot find wavrec_engine. " +
            "Set WAVREC_ENGINE_PATH to the full path of the executable."
        )

    val vm = AppViewModel(engineExePath = enginePath)

    application {
        Window(
            onCloseRequest = {
                vm.shutdown()
                exitApplication()
            },
            title = "WavRec",
            state = rememberWindowState(size = DpSize(1200.dp, 800.dp)),
        ) {
            WavRecApp(vm)
        }

        /* Start engine after window is created */
        vm.start()
    }
}

private fun findEngineExe(): String? {
    val isWindows = System.getProperty("os.name").contains("Windows", ignoreCase = true)
    val exeName   = if (isWindows) "wavrec_engine.exe" else "wavrec_engine"

    /* Standard CMake build locations relative to the UI project */
    val candidates = listOf(
        /* Running from ui/ directory during development */
        "../build/engine/Debug/$exeName",
        "../build/engine/Release/$exeName",
        /* Running from project root */
        "build/engine/Debug/$exeName",
        "build/engine/Release/$exeName",
    )

    return candidates
        .map { File(it).canonicalFile }
        .firstOrNull { it.exists() }
        ?.absolutePath
}
