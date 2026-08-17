package com.opensparx.agent.inference

import android.content.Context
import android.os.Build
import android.util.Log

/**
 * Auto-detects chipset and returns the best available [InferenceBackend].
 *
 * Detection order:
 * 1. Qualcomm Snapdragon → GenieX (if SDK present + NPU available)
 * 2. MediaTek Dimensity  → NeuroPilot (future)
 * 3. Fallback            → CPU (llama.cpp)
 *
 * Usage:
 * ```kotlin
 * val backend = BackendFactory.create(context)
 * backend.initialize(modelPath)
 * val result = backend.generate("Hello")
 * ```
 */
object BackendFactory {

    private const val TAG = "BackendFactory"

    /** Detected chipset vendor. */
    enum class ChipVendor { QUALCOMM, MEDIATEK, SAMSUNG, UNKNOWN }

    /**
     * Create the best inference backend for this device.
     * Does NOT initialize it — caller must call [InferenceBackend.initialize].
     */
    fun create(context: Context): InferenceBackend {
        val vendor = detectChipVendor()
        Log.i(TAG, "Detected chip vendor: $vendor (SOC: ${Build.SOC_MODEL})")

        return when (vendor) {
            ChipVendor.QUALCOMM -> createQualcommBackend(context)
            ChipVendor.MEDIATEK -> createMtkBackend(context)
            else -> {
                Log.w(TAG, "No NPU backend available, using CPU fallback")
                CpuFallbackBackend(context)
            }
        }
    }

    /**
     * Get all available backends ranked by preference.
     * Useful for settings UI: let user override auto-detection.
     */
    fun availableBackends(context: Context): List<InferenceBackend> {
        val backends = mutableListOf<InferenceBackend>()

        // Always try GenieX first on Qualcomm
        if (detectChipVendor() == ChipVendor.QUALCOMM) {
            backends.add(GenieXBackend(context))
        }

        // MTK when available
        if (detectChipVendor() == ChipVendor.MEDIATEK) {
            backends.add(MtkNeuroPilotBackend(context))
        }

        // CPU always available as last resort
        backends.add(CpuFallbackBackend(context))

        return backends
    }

    // ─── Detection ──────────────────────────────────────────────────────

    fun detectChipVendor(): ChipVendor {
        val soc = Build.SOC_MODEL.lowercase()
        val hardware = Build.HARDWARE.lowercase()
        val board = Build.BOARD.lowercase()

        return when {
            // Qualcomm Snapdragon
            soc.contains("sm8") || soc.contains("sm7") || soc.contains("sm6") ||
            hardware.contains("qcom") || hardware.contains("kona") ||
            hardware.contains("taro") || hardware.contains("kalama") ||
            hardware.contains("pineapple") || hardware.contains("sun") ||
            board.contains("msm") || board.contains("sdm") -> ChipVendor.QUALCOMM

            // MediaTek Dimensity
            soc.contains("mt6") || soc.contains("mt8") ||
            hardware.contains("mt6") || hardware.contains("mt8") -> ChipVendor.MEDIATEK

            // Samsung Exynos
            soc.contains("exynos") || hardware.contains("exynos") ||
            soc.contains("s5e") -> ChipVendor.SAMSUNG

            else -> ChipVendor.UNKNOWN
        }
    }

    private fun createQualcommBackend(context: Context): InferenceBackend {
        // Check if GenieX runtime is available on device
        val genieX = GenieXBackend(context)
        return if (genieX.isRuntimePresent()) {
            Log.i(TAG, "GenieX runtime detected — using NPU acceleration")
            genieX
        } else {
            Log.w(TAG, "Qualcomm device but GenieX runtime not present, falling back to QNN dlopen")
            QnnDlopenBackend(context) // Legacy path: raw QNN dlopen
        }
    }

    private fun createMtkBackend(context: Context): InferenceBackend {
        val mtk = MtkNeuroPilotBackend(context)
        return if (mtk.isRuntimePresent()) {
            Log.i(TAG, "NeuroPilot runtime detected — using MTK APU")
            mtk
        } else {
            Log.w(TAG, "MTK device but NeuroPilot not present, using CPU fallback")
            CpuFallbackBackend(context)
        }
    }
}
