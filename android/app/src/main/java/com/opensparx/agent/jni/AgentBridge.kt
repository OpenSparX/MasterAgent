package com.opensparx.agent.jni

/**
 * JNI bridge to the native MasterAgent C++ engine.
 *
 * This class provides Kotlin access to:
 * - On-device NPU inference (via QNN Runtime)
 * - ProactiveEngine (context signal → trigger → DAG)
 * - AgentScheduler (multi-agent orchestration)
 * - FormalVerifier (safety checks)
 */
object AgentBridge {

    init {
        System.loadLibrary("sparx_agent")
    }

    // ─── Engine Lifecycle ────────────────────────────────────────────────

    /** Initialize the agent engine. Call once at app startup. */
    external fun initialize(modelPath: String, npuBackend: String = "qnn"): Boolean

    /** Shutdown and release all resources. */
    external fun shutdown()

    /** Check if NPU runtime is available on this device. */
    external fun isNpuAvailable(): Boolean

    /** Get current NPU load fraction [0.0, 1.0]. */
    external fun getNpuLoad(): Float

    // ─── Reactive Mode (user-triggered) ─────────────────────────────────

    /** Submit a user query → returns a plan ID for tracking. */
    external fun submitQuery(text: String): Long

    /** Get the DAG structure for a plan (JSON format). */
    external fun getPlanDag(planId: Long): String

    /** Get current execution status of a plan. */
    external fun getPlanStatus(planId: Long): String

    // ─── Proactive Mode (context-triggered) ──────────────────────────────

    /** Start the proactive context engine. */
    external fun startProactiveEngine()

    /** Stop the proactive context engine. */
    external fun stopProactiveEngine()

    /** Push a context signal from Android sensors. */
    external fun pushSignal(channel: String, value: Double, confidence: Float)

    /** Push a labeled signal (e.g., face detection result). */
    external fun pushLabelSignal(channel: String, label: String, score: Float)

    /** Register a trigger condition. Returns trigger ID. */
    external fun registerTrigger(
        name: String,
        conditionJson: String,
        priority: Int,
        cooldownSeconds: Int
    ): Long

    /** Get list of active triggers and their states (JSON). */
    external fun getTriggersState(): String

    // ─── Agent Status ────────────────────────────────────────────────────

    /** Get all running agents and their states (JSON). */
    external fun getAgentsState(): String

    /** Get engine statistics (signals processed, triggers fired, etc). */
    external fun getEngineStats(): String

    // ─── Inference ───────────────────────────────────────────────────────

    /** Get current model info (name, size, quantization). */
    external fun getModelInfo(): String

    /** Get inference speed (tokens/sec). */
    external fun getInferenceSpeed(): Float

    /** Get KV cache utilization. */
    external fun getKvCacheUsage(): Float
}
