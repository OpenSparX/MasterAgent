package com.opensparx.agent.inference

import android.content.Context
import android.util.Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File

/**
 * NPU model preparation pipeline.
 *
 * Converts standard model formats (GGUF, ONNX, SafeTensors) into
 * NPU-optimized binaries for Qualcomm GenieX or MediaTek NeuroPilot.
 *
 * On-device flow:
 * 1. User downloads a base model (GGUF/ONNX) via ModelManager
 * 2. NpuModelPreparer converts it to NPU format (if not pre-converted)
 * 3. GenieXBackend loads the NPU binary for inference
 *
 * Pre-converted flow (recommended):
 * 1. Developer runs qai_hub (Qualcomm AI Hub) on cloud to produce .bin
 * 2. .bin is hosted for direct download via ModelManager
 * 3. No on-device conversion needed
 *
 * Model format matrix:
 * ┌────────────────────┬──────────────┬─────────────────────────────┐
 * │ Backend            │ File format  │ How to prepare              │
 * ├────────────────────┼──────────────┼─────────────────────────────┤
 * │ GenieX (NPU)       │ .bin         │ qai_hub compile --target 8g3│
 * │ QNN dlopen (NPU)   │ .bin         │ qnn-model-prepare (offline) │
 * │ CPU (llama.cpp)    │ .gguf        │ Ready to use as-is          │
 * │ NeuroPilot (MTK)   │ .tflite      │ mtk_converter (NeuroPilot)  │
 * └────────────────────┴──────────────┴─────────────────────────────┘
 */
object NpuModelPreparer {

    private const val TAG = "NpuModelPreparer"

    /**
     * Model preparation status for UI display.
     */
    enum class PrepStatus {
        NOT_NEEDED,     // Already in correct format
        CONVERTING,     // On-device conversion in progress
        READY,          // Converted and ready
        FAILED,         // Conversion failed
        UNSUPPORTED,    // Cannot convert this format on-device
    }

    data class PrepResult(
        val status: PrepStatus,
        val outputPath: String? = null,
        val errorMessage: String? = null,
        val estimatedTimeMs: Long = 0,
    )

    /**
     * Check if a model file needs NPU conversion for the current device.
     */
    fun checkModelFormat(modelPath: String, targetBackend: String): PrepStatus {
        val file = File(modelPath)
        if (!file.exists()) return PrepStatus.FAILED

        return when (targetBackend) {
            "geniex" -> {
                // GenieX needs .bin produced by qai_hub or qnn-model-prepare
                if (modelPath.endsWith(".bin")) PrepStatus.NOT_NEEDED
                else if (modelPath.endsWith(".gguf")) PrepStatus.UNSUPPORTED // Cannot convert GGUF→bin on device
                else PrepStatus.UNSUPPORTED
            }
            "gguf" -> {
                // CPU backend accepts GGUF directly
                if (modelPath.endsWith(".gguf")) PrepStatus.NOT_NEEDED
                else PrepStatus.UNSUPPORTED
            }
            "neuropilot" -> {
                if (modelPath.endsWith(".tflite")) PrepStatus.NOT_NEEDED
                else PrepStatus.UNSUPPORTED
            }
            else -> PrepStatus.UNSUPPORTED
        }
    }

    /**
     * Get instructions for the user on how to prepare models for their device.
     */
    fun getPreparationGuide(vendor: BackendFactory.ChipVendor): NpuGuide {
        return when (vendor) {
            BackendFactory.ChipVendor.QUALCOMM -> NpuGuide(
                title = "Qualcomm GenieX NPU Setup",
                steps = listOf(
                    GuideStep(
                        title = "Option A: Download Pre-converted Model (Recommended)",
                        description = "Download a .bin file that's already optimized for Snapdragon NPU.",
                        command = null,
                        note = "Pre-converted models are available in the Model Library for supported devices."
                    ),
                    GuideStep(
                        title = "Option B: Convert via Qualcomm AI Hub (Cloud)",
                        description = "Use qai_hub to compile your model for Snapdragon 8 Gen 3/4.",
                        command = """
                            # Install qai_hub CLI
                            pip install qai-hub

                            # Login to Qualcomm AI Hub
                            qai-hub configure --api-token YOUR_TOKEN

                            # Compile model for target device
                            qai-hub model compile \
                              --model qwen3-0.6b \
                              --target "Samsung Galaxy S24" \
                              --output ./qwen3-0.6b-geniex.bin
                        """.trimIndent(),
                        note = "Requires a Qualcomm AI Hub account (free tier available)."
                    ),
                    GuideStep(
                        title = "Option C: Local QNN Conversion (Advanced)",
                        description = "Use Qualcomm AI Engine Direct SDK on your PC.",
                        command = """
                            # Requires Qualcomm AI Engine Direct SDK (NDA)
                            # Export model to ONNX first:
                            python export_onnx.py --model qwen3-0.6b --output qwen3.onnx

                            # Run QNN model prepare (produces .bin for HTP/NPU):
                            qnn-model-prepare \
                              --input qwen3.onnx \
                              --output qwen3-htp.bin \
                              --target htp \
                              --quantize int8 \
                              --htp_soc sm8650
                        """.trimIndent(),
                        note = "Requires NDA access to Qualcomm AI Engine Direct SDK."
                    ),
                    GuideStep(
                        title = "Push to Device",
                        description = "Transfer the converted model to your phone.",
                        command = """
                            # Push via ADB:
                            adb push qwen3-0.6b-geniex.bin /sdcard/Download/

                            # Then open OpenSparX → Model Library → Import
                            # Or the app will detect it in /sdcard/Download/ automatically
                        """.trimIndent(),
                        note = "The Model Library 'Import' button scans /sdcard/Download/ for .bin and .gguf files."
                    ),
                ),
                supportedModels = listOf(
                    SupportedModel("qwen3-0.6b", "Qwen3 0.6B", "Intent classification, fast response", "~400MB"),
                    SupportedModel("qwen3-4b", "Qwen3 4B", "Conversation, multi-turn dialogue", "~2.5GB"),
                    SupportedModel("deepseek-r1-1.5b", "DeepSeek-R1 1.5B", "Reasoning, chain-of-thought", "~1GB"),
                ),
                deviceRequirements = "Snapdragon 8 Gen 2+ (sm8550/sm8650/sm8750) with 8GB+ RAM"
            )

            BackendFactory.ChipVendor.MEDIATEK -> NpuGuide(
                title = "MediaTek NeuroPilot Setup",
                steps = listOf(
                    GuideStep(
                        title = "Convert to TFLite",
                        description = "NeuroPilot uses TensorFlow Lite models with MTK delegate.",
                        command = """
                            # Export to TFLite with MTK extensions:
                            python mtk_converter.py \
                              --model qwen3-0.6b \
                              --output qwen3-neuropilot.tflite \
                              --quantize int8 \
                              --target dimensity-9300
                        """.trimIndent(),
                        note = "Requires MediaTek NeuroPilot SDK (NDA access via MediaTek partner program)."
                    ),
                ),
                supportedModels = listOf(
                    SupportedModel("qwen3-0.6b", "Qwen3 0.6B", "Intent + lightweight tasks", "~350MB"),
                ),
                deviceRequirements = "Dimensity 9200+ with NeuroPilot 7.0+"
            )

            else -> NpuGuide(
                title = "CPU Fallback (llama.cpp)",
                steps = listOf(
                    GuideStep(
                        title = "Download GGUF Model",
                        description = "GGUF models work on any device via llama.cpp CPU inference.",
                        command = """
                            # Download from HuggingFace:
                            wget https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/qwen3-0.6b-q4_k_m.gguf

                            # Push to device:
                            adb push qwen3-0.6b-q4_k_m.gguf /sdcard/Download/
                        """.trimIndent(),
                        note = "GGUF Q4_K_M is a good balance of quality vs size. Q8 is better but 2x larger."
                    ),
                ),
                supportedModels = listOf(
                    SupportedModel("qwen3-0.6b", "Qwen3 0.6B (Q4_K_M)", "All-purpose, runs anywhere", "~450MB"),
                    SupportedModel("qwen3-4b", "Qwen3 4B (Q4_K_M)", "Better quality, needs 4GB+ free RAM", "~2.8GB"),
                ),
                deviceRequirements = "Any Android 9+ device with 4GB+ RAM"
            )
        }
    }

    data class NpuGuide(
        val title: String,
        val steps: List<GuideStep>,
        val supportedModels: List<SupportedModel>,
        val deviceRequirements: String,
    )

    data class GuideStep(
        val title: String,
        val description: String,
        val command: String?,
        val note: String?,
    )

    data class SupportedModel(
        val id: String,
        val name: String,
        val useCase: String,
        val estimatedSize: String,
    )
}
