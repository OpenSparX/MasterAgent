package com.opensparx.agent.ui

import android.content.Context
import android.graphics.Color
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.LinearLayout
import android.widget.TextView
import com.opensparx.agent.R
import com.opensparx.agent.core.AgentPipeline
import com.opensparx.agent.core.AgentPipeline.PipelineEvent
import com.opensparx.agent.core.AgentPipeline.Stage

/**
 * Renders pipeline execution events into a LinearLayout container.
 * Each stage appears as a compact line: [icon] [label] ... [status]
 *
 * Active stage pulses green, completed stages show checkmark, pending show circle
 */
class PipelineVisualizer(private val context: Context) {

    private val stageViews = mutableMapOf<Stage, TextView>()

    fun renderEvent(container: ViewGroup, event: PipelineEvent) {
        when (event) {
            is PipelineEvent.StageStart -> {
                val tv = getOrCreateStageView(container, event.stage)
                tv.text = "${event.stage.icon} ${event.stage.label}  ◉ running..."
                tv.setTextColor(Color.parseColor("#00E676"))
            }
            is PipelineEvent.StageComplete -> {
                val tv = stageViews[event.stage] ?: return
                tv.text = "${event.stage.icon} ${event.stage.label}  ✓ ${event.durationMs}ms"
                tv.setTextColor(Color.parseColor("#66BB6A"))
            }
            is PipelineEvent.StageProgress -> {
                val tv = stageViews[event.stage] ?: return
                tv.text = "${event.stage.icon} ${event.stage.label}  → ${event.detail}"
                tv.setTextColor(Color.parseColor("#00E676"))
            }
            is PipelineEvent.SpeculationHit -> {
                val specView = container.findViewById<TextView>(R.id.text_speculation_status)
                specView?.visibility = View.VISIBLE
                specView?.text = "⚡ Speculation HIT: '${event.predictedIntent}' — saved ${event.savedMs}ms"
            }
            is PipelineEvent.MemoryRecall -> {
                val memView = container.findViewById<TextView>(R.id.text_memory_status)
                memView?.visibility = View.VISIBLE
                val memText = event.memories.take(2).joinToString("\n") { "  🗄️ ${it.content}" }
                memView?.text = "Memory:\n$memText"
            }
            is PipelineEvent.VerificationResult -> {
                val badge = container.findViewById<TextView>(R.id.text_verification_badge)
                badge?.visibility = View.VISIBLE
                badge?.text = if (event.passed) "🔒 Verified: ${event.formula} (${event.timeMs}ms)"
                              else "⚠️ FAILED: ${event.formula}"
                badge?.setTextColor(if (event.passed) Color.parseColor("#00E676") else Color.RED)
            }
            is PipelineEvent.ToolInvoked -> {
                val tv = stageViews[Stage.EXECUTION] ?: return
                tv.text = "${Stage.EXECUTION.icon} Tool: ${event.toolName} 🔧"
                tv.setTextColor(Color.parseColor("#FFD600"))
            }
            is PipelineEvent.LearningRecord -> {
                val tv = stageViews[Stage.LEARNING] ?: return
                tv.text = "${Stage.LEARNING.icon} ${event.what}"
            }
            is PipelineEvent.MeshSync -> {
                val tv = stageViews[Stage.MESH] ?: return
                tv.text = "${Stage.MESH.icon} Synced: ${event.devices} device(s), ${event.rounds} round"
            }
            is PipelineEvent.PipelineComplete -> {
                val summary = container.findViewById<TextView>(R.id.text_pipeline_summary)
                summary?.visibility = View.VISIBLE
                summary?.text = "✅ ${event.stagesCompleted} stages | ${event.totalMs}ms | All verified"
                // Update time
                val timeView = container.findViewById<TextView>(R.id.text_pipeline_time)
                timeView?.text = "${event.totalMs}ms"
            }
            else -> {}
        }
    }

    private fun getOrCreateStageView(container: ViewGroup, stage: Stage): TextView {
        stageViews[stage]?.let { return it }

        val stagesContainer = container.findViewById<LinearLayout>(R.id.pipeline_stages_container)
            ?: container as? LinearLayout ?: return TextView(context)

        val tv = TextView(context).apply {
            text = "${stage.icon} ${stage.label}  ○ pending"
            setTextColor(Color.parseColor("#555555"))
            textSize = 11f
            setPadding(0, 4, 0, 4)
            val lp = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
            layoutParams = lp
        }
        stagesContainer.addView(tv)
        stageViews[stage] = tv
        return tv
    }

    fun reset() {
        stageViews.clear()
    }
}
