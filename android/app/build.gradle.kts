plugins {
    id("com.android.application")
}

/*
 * This Gradle project does NOT drive CMake. The native libraries are built
 * separately by CMake + NDK in CI (or locally), then Gradle packages the
 * prebuilt .so files into an installable APK. The guest shader pack is linked
 * straight into libnocturnerecomp.so (see src/gpu/guest_shaders.h), so it needs
 * no separate staging step.
 *
 * Before running Gradle, stage the files:
 *   - .so files → android/app/jniLibs/arm64-v8a/
 *
 * The CI workflow handles this staging automatically.
 */

/*
 * Version comes from the newest v* tag reachable from HEAD, so a CI build and
 * a local build of the same commit agree. Override with -PappVersionName= or
 * NOCTURNE_VERSION_NAME when git history is unavailable (shallow clone, tarball).
 */
val appVersionName: String = run {
    val override = (project.findProperty("appVersionName") as String?)
        ?: System.getenv("NOCTURNE_VERSION_NAME")
    if (!override.isNullOrBlank()) return@run override.removePrefix("v")
    try {
        val proc = ProcessBuilder("git", "describe", "--tags", "--abbrev=0", "--match", "v*")
            .directory(rootProject.projectDir)
            .redirectErrorStream(true)
            .start()
        val out = proc.inputStream.bufferedReader().readText().trim()
        if (proc.waitFor() == 0 && out.isNotEmpty()) out.removePrefix("v") else "0.0.0"
    } catch (e: Exception) {
        "0.0.0"
    }
}

// Packed major/minor/patch so it rises monotonically across releases; Android
// refuses to install an APK whose versionCode is lower than the installed one.
val appVersionCode: Int = run {
    val parts = Regex("""^(\d+)\.(\d+)\.(\d+)""").find(appVersionName)?.destructured
        ?: return@run 1
    val (major, minor, patch) = parts
    maxOf(1, major.toInt() * 1_000_000 + minor.toInt() * 1_000 + patch.toInt())
}

android {
    namespace = "com.birabittoh.nocturnerecomp"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.birabittoh.nocturnerecomp"
        minSdk = 28        // matches CMAKE_ANDROID_API in CMakePresets.json
        targetSdk = 35
        versionCode = appVersionCode
        versionName = appVersionName

        ndk {
            // Only arm64 is supported right now.
            abiFilters += "arm64-v8a"
        }
    }

    /*
     * Stable sideload signing key.
     *
     * Signing with the machine's debug keystore makes every build a different
     * signer, so installing over an existing copy fails with
     * INSTALL_FAILED_UPDATE_INCOMPATIBLE and the app has to be uninstalled
     * first, taking its saves and extracted game data with it.
     *
     * The keystore is deliberately not in the repository (see .gitignore).
     * Create one once with:
     *
     *   keytool -genkeypair -keystore android/keystore/sideload.jks \
     *     -storetype PKCS12 -storepass nocturnerecomp -keypass nocturnerecomp \
     *     -alias nocturnerecomp -keyalg RSA -keysize 2048 -validity 10950 \
     *     -dname "CN=NocturneRecomp Sideload"
     *
     * Path, alias and passwords are overridable through gradle properties
     * (-PsideloadKeystore=...) or NOCTURNE_KEYSTORE / NOCTURNE_KEYSTORE_PASSWORD /
     * NOCTURNE_KEY_ALIAS / NOCTURNE_KEY_PASSWORD in the environment. CI restores the same
     * keystore from the NOCTURNE_KEYSTORE_BASE64 secret so its APKs and local ones
     * are interchangeable. When no keystore is found the build falls back to
     * debug signing.
     */
    // CI exports these from repository secrets, which arrive as empty strings
    // rather than unset when the secret does not exist, so blanks have to fall
    // through to the default too.
    fun signingSetting(property: String, env: String, fallback: String): String =
        listOf(project.findProperty(property) as String?, System.getenv(env), fallback)
            .firstOrNull { !it.isNullOrBlank() }!!

    // Resolved against the android/ directory, not app/, so the keystore sits
    // next to the Gradle build rather than inside the module.
    val sideloadKeystore = rootProject.file(
        signingSetting("sideloadKeystore", "NOCTURNE_KEYSTORE", "keystore/sideload.jks")
    )

    signingConfigs {
        if (sideloadKeystore.isFile) {
            create("sideload") {
                storeFile = sideloadKeystore
                storePassword = signingSetting(
                    "sideloadKeystorePassword", "NOCTURNE_KEYSTORE_PASSWORD", "nocturnerecomp")
                keyAlias = signingSetting(
                    "sideloadKeyAlias", "NOCTURNE_KEY_ALIAS", "nocturnerecomp")
                keyPassword = signingSetting(
                    "sideloadKeyPassword", "NOCTURNE_KEY_PASSWORD", "nocturnerecomp")
            }
        }
    }

    // Prebuilt native libraries, staged by CI into jniLibs/arm64-v8a/.
    sourceSets["main"].jniLibs.srcDirs(
        layout.projectDirectory.dir("jniLibs")
    )

    /*
     * SDL3's Android Java glue (org.libsdl.app.*) comes from the SDK, not from
     * this repository. librexruntime.so links SDL3 statically and registers its
     * native methods against these exact classes, so a vendored copy goes stale
     * the moment the SDK updates SDL3 and the app then aborts inside
     * System.loadLibrary with a NoSuchMethodError. Taking them from the SDK the
     * .so was built against keeps the signatures in lockstep.
     *
     * Override with -PsdkJavaDir=... or NOCTURNE_SDK_JAVA_DIR when the SDK lives
     * somewhere other than sdk/android-arm64.
     */
    val sdkJavaDir = rootProject.file(
        (project.findProperty("sdkJavaDir") as String?)
            ?: System.getenv("NOCTURNE_SDK_JAVA_DIR")
            ?: "../sdk/android-arm64/share/rexglue/android/java"
    )
    require(sdkJavaDir.isDirectory) {
        "SDL Java glue not found at $sdkJavaDir. Fetch the Android SDK first: " +
            "python scripts/download-sdk.py sdk/android-arm64 --pinned --platform android-arm64"
    }
    sourceSets["main"].java.srcDirs(sdkJavaDir)

    buildTypes {
        release {
            isMinifyEnabled = false
            // RenderDoc captures require a debuggable APK. Opt in with
            // -PwithRenderDoc=true so normal sideload builds stay non-debuggable.
            isDebuggable = (project.findProperty("withRenderDoc") as String?) == "true"
            // Sideload key when one is present, otherwise debug-signed as before
            // so a fresh clone (and CI, which has no keystore) still builds.
            signingConfig = signingConfigs.findByName("sideload")
                ?: signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    packaging {
        // Avoid stripping the prebuilt libraries — they are already stripped
        // (release build) or we want symbols for debugging (debug build).
        jniLibs.keepDebugSymbols += "**/*.so"
        // Extract native libs at install time so dlopen() can find siblings
        // by path. Equivalent to android:extractNativeLibs="true" but set
        // through the build system as the AGP lint recommends.
        jniLibs.useLegacyPackaging = true
    }
}

dependencies {
    // FileProvider only; the SDK's AutoUpdater resolves it through the
    // activity's class loader (see auto_updater_android.cpp).
    implementation("androidx.core:core:1.13.1")
}
