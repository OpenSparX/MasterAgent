package com.opensparx.agent.ui

import android.os.Bundle
import android.view.View
import android.widget.LinearLayout
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.opensparx.agent.R

/**
 * Architecture Diagram — interactive layered view of the OpenSparX Agent OS stack.
 *
 * Each layer card expands on tap to reveal implementation details.
 * Designed for developer and investor audiences.
 */
class ArchitectureActivity : AppCompatActivity() {

    private data class LayerBinding(val container: View, val detail: TextView)

    private val layers = mutableListOf<LayerBinding>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_architecture)

        setupLayers()
    }

    private fun setupLayers() {
        val layerIds = listOf(
            R.id.layer_ux to R.id.detail_ux,
            R.id.layer_intelligence to R.id.detail_intelligence,
            R.id.layer_coordination to R.id.detail_coordination,
            R.id.layer_inference to R.id.detail_inference,
            R.id.layer_hardware to R.id.detail_hardware
        )

        for ((containerId, detailId) in layerIds) {
            val container = findViewById<LinearLayout>(containerId)
            val detail = findViewById<TextView>(detailId)
            layers.add(LayerBinding(container, detail))

            container.setOnClickListener {
                toggleDetail(detail)
            }
        }
    }

    private fun toggleDetail(detail: TextView) {
        if (detail.visibility == View.VISIBLE) {
            detail.visibility = View.GONE
        } else {
            detail.visibility = View.VISIBLE
        }
    }
}
