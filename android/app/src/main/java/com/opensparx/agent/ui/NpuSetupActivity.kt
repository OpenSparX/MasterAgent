package com.opensparx.agent.ui

import android.os.Bundle
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.opensparx.agent.R
import com.opensparx.agent.inference.BackendFactory
import com.opensparx.agent.inference.NpuModelPreparer

/**
 * Shows device-specific NPU setup instructions.
 * Accessed from Model Library → "NPU Setup Guide" button.
 */
class NpuSetupActivity : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_npu_setup)

        val vendor = BackendFactory.detectChipVendor()
        val guide = NpuModelPreparer.getPreparationGuide(vendor)

        findViewById<TextView>(R.id.text_npu_title).text = guide.title
        findViewById<TextView>(R.id.text_device_req).text = "要求: ${guide.deviceRequirements}"

        val stepsView = findViewById<TextView>(R.id.text_steps)
        val sb = StringBuilder()

        guide.steps.forEachIndexed { index, step ->
            sb.appendLine("━━━ Step ${index + 1}: ${step.title} ━━━")
            sb.appendLine(step.description)
            sb.appendLine()
            step.command?.let { cmd ->
                sb.appendLine("```")
                sb.appendLine(cmd)
                sb.appendLine("```")
                sb.appendLine()
            }
            step.note?.let { note ->
                sb.appendLine("⚠️ $note")
                sb.appendLine()
            }
        }

        sb.appendLine("━━━ 支持的模型 ━━━")
        guide.supportedModels.forEach { model ->
            sb.appendLine("• ${model.name} (${model.estimatedSize})")
            sb.appendLine("  用途: ${model.useCase}")
        }

        stepsView.text = sb.toString()
    }
}
