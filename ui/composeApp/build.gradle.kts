import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    alias(libs.plugins.kotlinMultiplatform)
    alias(libs.plugins.jetbrainsCompose)
    alias(libs.plugins.compose.compiler)
    alias(libs.plugins.kotlin.serialization)
}

kotlin {
    jvm("desktop")

    sourceSets {
        val commonMain by getting {
            dependencies {
                implementation(compose.runtime)
                implementation(compose.foundation)
                implementation(compose.material3)
                implementation(compose.materialIconsExtended)
                implementation(compose.ui)
                implementation(compose.components.resources)
                implementation(libs.coroutines.core)
                implementation(libs.serialization.json)
                implementation(libs.kotlinx.datetime)
            }
        }

        val desktopMain by getting {
            dependencies {
                implementation(compose.desktop.currentOs)
                implementation(libs.coroutines.swing)
                implementation(libs.jna)
                implementation(libs.jna.platform)
            }
        }
    }
}

/* Resolve the engine exe relative to this build file's directory. */
val engineExe = if (System.getProperty("os.name").contains("Windows", ignoreCase = true))
    "wavrec_engine.exe" else "wavrec_engine"

val enginePath = listOf(
    projectDir.resolve("../../build/engine/Debug/$engineExe"),
    projectDir.resolve("../../build/engine/Release/$engineExe"),
).firstOrNull { it.exists() }?.absolutePath ?: ""

compose.desktop {
    application {
        mainClass = "com.wavrec.MainKt"

        jvmArgs += listOf("-Dwavrec.engine.path=$enginePath")

        nativeDistributions {
            targetFormats(TargetFormat.Dmg, TargetFormat.Msi, TargetFormat.Deb)
            packageName    = "WavRec"
            packageVersion = "1.0.0"
            description    = "Multi-channel audio recording and annotation"
            copyright      = "© 2026"

            windows {
                menuGroup   = "WavRec"
                upgradeUuid = "wavrec-app-uuid-0001"
            }
            macOS {
                bundleID = "com.wavrec.app"
            }
        }
    }
}
